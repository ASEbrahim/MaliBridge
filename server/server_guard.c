/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contains driver crashes while a shader or pipeline is compiled. Mali r44p1's compiler has died
 * on unusual SPIR-V several times (SIGSEGV at 0x40 in vkCreateGraphicsPipelines, the ULTRAKILL
 * session among them), and since the server lives in the app's process the whole session went
 * with it: one bad game shader ended Steam, the compositor, everything.
 *
 * Each device's real vkCreateGraphicsPipelines / vkCreateComputePipelines / vkCreateShaderModule
 * is wrapped (in t->real, so the emulation layers' own helper pipelines are covered too); the
 * wrapper arms a per-thread sigsetjmp point around the driver call, and the crash handler
 * (server_main.c) jumps back to it when the fault is on an armed thread. The call then fails with
 * VK_ERROR_INITIALIZATION_FAILED and null handles; DXVK and vkd3d-proton log a failed pipeline
 * and skip its draws. Faults anywhere else go on as before (log, then die / debuggerd).
 *
 * The risk: the jump abandons the driver's frames, and any lock the driver held (its compiler
 * mutex, a pipeline cache lock, malloc's arena lock if it died inside malloc) stays held. A later
 * call into the driver that wants it - from any thread, any device, the compositor's included -
 * would hang. That cannot be undone from outside; the policy below only limits how often the
 * driver is re-entered after a fault:
 *   - a device's first two contained crashes fail just that call: games survive a missing
 *     pipeline, while a dead device (or process) ends them;
 *   - the third poisons the device: pipeline and module creation on it return
 *     VK_ERROR_DEVICE_LOST without reaching the driver (a shader corpus that keeps crashing it
 *     would otherwise loop); everything else still works so the app can tear down;
 *   - after 64 in the process (a game that recreates its device after each loss) containment is
 *     off and the next fault dies as before: by then the driver's heap is not worth trusting.
 * VKBRIDGE_NO_CRASH_GUARD=1 turns it off (to get debuggerd's tombstone of the first fault).
 *
 * Only faults on the calling thread are caught. The crashes seen so far were there (the handler
 * named the thread's current command); one on a driver worker thread still kills the process.
 *
 * Compiles that never end. The Mali-G925's compiler (Android 16, 2026-10-14) spun at 100% of a
 * core inside vkCreateGraphicsPipelines when Stray (DXVK) created its first ~25 game pipelines;
 * the game's thread waited for the reply for ever (UE "RenderThread timed out after 120 s"), and
 * no GPU work was involved. A watchdog thread now looks at every guarded call: one running past
 * VKBRIDGE_COMPILE_WARN_S (5) is logged with its stages' SPIR-V hashes (and the SPIR-V saved to
 * <cache dir>/stuck-spirv/), and one running past VKBRIDGE_COMPILE_TIMEOUT_S (20; 0 = only log)
 * is interrupted with a signal that jumps out of the driver as a contained crash does: the call
 * fails with VK_ERROR_INITIALIZATION_FAILED and the game skips that pipeline's draws. The stage
 * hashes go to <cache dir>/stuck-pipelines, and a pipeline with the same stages fails at once
 * from then on, in this run and later ones (delete the file to retry them). The same caveat as
 * for crashes applies: a lock the driver held while it spun stays held.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define POISON_AT 3
/* 8 was hit by one game: ULTRAKILL (Unity, Vulkan) had 7 contained in a second over its devices
 * and the 8th took the session down; the server lives in the app, so dying is the worst outcome. */
#define PROCESS_MAX 64
#define MAX_TABLES 4096

/* VKBRIDGE_TEST_CRASH_PIPELINE=1: a pipeline whose stage carries this specialization constant ID
 * makes the guard fault on purpose inside the armed region (tests/crash_contain_check.c). An ID
 * the shader does not use is legal and ignored by drivers, so the marker changes nothing else. */
#define TEST_CRASH_SPEC_ID 0x7E57C0DEu

typedef struct vkb_guard {
    vkb_srv_table *t;
    PFN_vkCreateGraphicsPipelines gfx;
    PFN_vkCreateComputePipelines compute;
    PFN_vkCreateShaderModule module;
    int crashes;
    volatile int poisoned;
} vkb_guard;

typedef struct guard_arm {
    sigjmp_buf jb;
    volatile int sig;
    void *volatile addr;
    volatile int timed_out; /* set by the watchdog before it signals the thread */
    struct guard_arm *prev;
} guard_arm;

static __thread guard_arm *volatile tls_arm;
static vkb_guard *guards[MAX_TABLES];
static int process_crashes;
static int guard_off = -1;
static int test_crash = -1;

static void guard_env(void)
{
    if (guard_off < 0) {
        const char *s = getenv("VKBRIDGE_NO_CRASH_GUARD");
        guard_off = s && *s && *s != '0';
        s = getenv("VKBRIDGE_TEST_CRASH_PIPELINE"); /* =spin: marked pipelines spin instead (compile watchdog) */
        test_crash = s && *s && *s != '0' ? (!strcmp(s, "spin") ? 2 : 1) : 0;
        if (guard_off) VKB_INFO("driver crash containment off (VKBRIDGE_NO_CRASH_GUARD)");
        if (test_crash) VKB_WARN("VKBRIDGE_TEST_CRASH_PIPELINE: marked pipelines fault on purpose");
    }
}

/* From the crash handler; async-signal-safe up to the jump (TLS reads, plain stores). Returns only
 * when the faulting thread is not inside a guarded call. */
void vkb_guard_fault(int sig, void *addr)
{
    guard_arm *a = tls_arm;
    if (!a) return;
    tls_arm = a->prev;
    a->sig = sig;
    a->addr = addr;
    siglongjmp(a->jb, 1);
}

static vkb_guard *find(VkDevice device)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (t && t->device == device && t->id < MAX_TABLES && guards[t->id]) return guards[t->id];
    for (int i = 0; i < MAX_TABLES; i++) {
        vkb_guard *g = __atomic_load_n(&guards[i], __ATOMIC_ACQUIRE);
        if (g && g->t->device == device) return g;
    }
    return NULL;
}

static int armable(void)
{
    return !guard_off && __atomic_load_n(&process_crashes, __ATOMIC_RELAXED) < PROCESS_MAX;
}

/* After the jump: count, poison, say loudly what died. */
static void contained(vkb_guard *g, const char *call, const guard_arm *a)
{
    int mine = __atomic_add_fetch(&g->crashes, 1, __ATOMIC_RELAXED);
    int total = __atomic_add_fetch(&process_crashes, 1, __ATOMIC_RELAXED);
    if (mine >= POISON_AT) g->poisoned = 1;
    VKB_ERR("CONTAINED driver crash: signal %d (address %p) in %s on device %u - the call fails, the session lives "
            "(%d on this device, %d in the process%s%s)",
            a->sig, a->addr, call, g->t->id, mine, total, g->poisoned ? "; device poisoned: no more pipelines on it" : "",
            total >= PROCESS_MAX ? "; containment now off, the next fault kills the process" : "");
    VKB_ERR("  driver locks held at the fault stay held: a hang in a later driver call would be this crash's doing");
}

static void log_stage(const VkPipelineShaderStageCreateInfo *s, uint32_t i, uint32_t n)
{
    VKB_ERR("  pipeline %u/%u stage 0x%x: module %llx, entry %s%s", i, n, (unsigned)s->stage, (unsigned long long)(uintptr_t)s->module,
            s->pName ? s->pName : "(null)",
            vkb_emu_dumping() ? " (its SPIR-V: dump-spirv/*-<module>.spv)" : "");
}

static int marked(const VkPipelineShaderStageCreateInfo *s)
{
    const VkSpecializationInfo *si = s->pSpecializationInfo;
    for (uint32_t i = 0; si && si->pMapEntries && i < si->mapEntryCount; i++)
        if (si->pMapEntries[i].constantID == TEST_CRASH_SPEC_ID) return 1;
    return 0;
}

/* Also when containment is off: the test then sees the process die as before. */
static void test_fault(const VkPipelineShaderStageCreateInfo *stages, uint32_t n, size_t stride)
{
    if (!test_crash) return;
    for (uint32_t i = 0; i < n; i++)
        if (marked((const VkPipelineShaderStageCreateInfo *)((const char *)stages + i * stride))) {
            if (test_crash == 2) {
                VKB_WARN("test: spinning on purpose in a %s call", tls_arm ? "guarded" : "UNGUARDED");
                for (volatile unsigned long k = 0;; k++) {
                }
            }
            VKB_WARN("test: faulting on purpose in a %s call", tls_arm ? "guarded" : "UNGUARDED");
            *(volatile int *)(uintptr_t)0x40 = 1;
        }
}


/* ------------------------------------------------------------------ compiles that never end */

#define WATCH_SLOTS 64
#define STAGES_MAX 6
#define MOD_BUCKETS 8192
#define STUCK_MAX 256

typedef struct watch_slot {
    int used;
    pthread_t thread;
    guard_arm *arm;
    double start;
    const char *call;
    uint32_t device;
    uint32_t nstages;
    uint64_t hash[STAGES_MAX];
    uint32_t stage[STAGES_MAX];
    const uint32_t *code[STAGES_MAX]; /* inline SPIR-V of the call (valid while the slot is used) */
    size_t size[STAGES_MAX];
    int warned;
} watch_slot;

typedef struct mod_hash {
    VkShaderModule m;
    uint64_t hash;
} mod_hash;

#define RECENT 128 /* the last modules' code, for the stuck-spirv dump (a pipeline follows its modules) */
typedef struct recent_code {
    uint64_t hash;
    uint32_t *code;
    size_t size;
} recent_code;
static recent_code recent[RECENT];
static unsigned recent_next;

static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static watch_slot slots[WATCH_SLOTS];
static double warn_s = 5, timeout_s = 20;
static int watch_started, timeout_sig;
static mod_hash mods[MOD_BUCKETS];
static uint64_t stuck[STUCK_MAX][STAGES_MAX];
static uint32_t stuck_n[STUCK_MAX], nstuck;

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint64_t spirv_hash(const uint32_t *code, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)code;
    for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* Modules are remembered by handle (their hash) while the compile watchdog is on; a stale entry is
 * replaced when the driver hands the handle out again. The code of the last RECENT is kept. */
static void mod_remember(VkShaderModule m, const VkShaderModuleCreateInfo *ci)
{
    if (!m || !ci || !ci->pCode) return;
    uint64_t h = spirv_hash(ci->pCode, ci->codeSize);
    unsigned b = (unsigned)(((uintptr_t)m >> 4) * 2654435761u) % MOD_BUCKETS;
    uint32_t *copy = malloc(ci->codeSize);
    if (copy) memcpy(copy, ci->pCode, ci->codeSize);
    pthread_mutex_lock(&watch_lock);
    for (unsigned i = 0; i < MOD_BUCKETS; i++) {
        mod_hash *e = &mods[(b + i) % MOD_BUCKETS];
        if (e->m && e->m != m && i < MOD_BUCKETS - 1) continue;
        e->m = m;
        e->hash = h;
        break;
    }
    recent_code *r = &recent[recent_next++ % RECENT];
    free(r->code);
    r->hash = h;
    r->code = copy;
    r->size = copy ? ci->codeSize : 0;
    pthread_mutex_unlock(&watch_lock);
}

static uint64_t mix(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* The stage's SPIR-V hash: an inline module (maintenance5) or a remembered one; 0 if unknown. */
static uint64_t code_hash(const VkPipelineShaderStageCreateInfo *st)
{
    for (const VkBaseInStructure *b = st->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *ci = (const VkShaderModuleCreateInfo *)b;
            return ci->pCode ? spirv_hash(ci->pCode, ci->codeSize) : 0;
        }
    if (!st->module) return 0;
    unsigned b = (unsigned)(((uintptr_t)st->module >> 4) * 2654435761u) % MOD_BUCKETS;
    uint64_t h = 0;
    pthread_mutex_lock(&watch_lock);
    for (unsigned i = 0; i < MOD_BUCKETS; i++) {
        mod_hash *e = &mods[(b + i) % MOD_BUCKETS];
        if (!e->m) break;
        if (e->m == st->module) {
            h = e->hash;
            break;
        }
    }
    pthread_mutex_unlock(&watch_lock);
    return h;
}

/* What the stage compiles: the SPIR-V hash (the name of its dump) and, kept apart, the entry
 * point and specialization constants (the same module specialized otherwise is another compile). */
static uint64_t stage_hash(const VkPipelineShaderStageCreateInfo *st, uint64_t *variant)
{
    uint64_t h = code_hash(st), v = 0xcbf29ce484222325ull;
    if (st->pName) v = mix(v, st->pName, strlen(st->pName));
    const VkSpecializationInfo *si = st->pSpecializationInfo;
    if (si) {
        if (si->pMapEntries) v = mix(v, si->pMapEntries, si->mapEntryCount * sizeof(*si->pMapEntries));
        if (si->pData) v = mix(v, si->pData, si->dataSize);
    }
    *variant = v;
    return h;
}

/* Called with watch_lock held; code: the call's inline SPIR-V for this stage, if any. */
static void save_spirv(uint64_t h, const uint32_t *code, size_t size)
{
    const char *dir = vkb_pcache_dir();
    if (!dir || !h) return;
    for (unsigned i = 0; !code && i < RECENT; i++)
        if (recent[i].hash == h && recent[i].code) {
            code = recent[i].code;
            size = recent[i].size;
        }
    if (!code) return;
    char path[700];
    snprintf(path, sizeof(path), "%s/stuck-spirv", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/stuck-spirv/h-%016llx.spv", dir, (unsigned long long)h);
    if (access(path, F_OK) == 0) return;
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(code, 1, size, f);
        fclose(f);
    }
}

static void stages_text(const uint64_t *h, const uint32_t *stage, uint32_t n, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < n && o + 32 < cap; i++)
        o += (size_t)snprintf(out + o, cap - o, "%s0x%x=h-%016llx", i ? " " : "", stage ? stage[i] : 0, (unsigned long long)h[i]);
}

static int is_stuck(const uint64_t *h, uint32_t n)
{
    int hit = 0;
    pthread_mutex_lock(&watch_lock);
    for (uint32_t k = 0; k < nstuck && !hit; k++) {
        if (stuck_n[k] != n) continue;
        uint32_t same = 0;
        for (uint32_t i = 0; i < n; i++) same += stuck[k][i] == h[i] && h[i];
        hit = same == n && n;
    }
    pthread_mutex_unlock(&watch_lock);
    return hit;
}

/* key: per stage, SPIR-V hash ^ entry/specialization hash. note: a comment for the file. */
static void add_stuck(const uint64_t *key, uint32_t n, const char *note)
{
    if (!n || is_stuck(key, n)) return;
    pthread_mutex_lock(&watch_lock);
    if (nstuck < STUCK_MAX) {
        memcpy(stuck[nstuck], key, n * sizeof(*key));
        stuck_n[nstuck++] = n;
    }
    pthread_mutex_unlock(&watch_lock);
    const char *dir = vkb_pcache_dir();
    if (!note || !dir) return;
    char path[700];
    snprintf(path, sizeof(path), "%s/stuck-pipelines", dir);
    FILE *f = fopen(path, "a");
    if (f) {
        for (uint32_t i = 0; i < n; i++) fprintf(f, "%sk-%016llx", i ? "," : "", (unsigned long long)key[i]);
        fprintf(f, " # %s\n", note);
        fclose(f);
    }
}

static void load_stuck(void)
{
    const char *dir = vkb_pcache_dir();
    if (!dir) return;
    char path[700], line[512];
    snprintf(path, sizeof(path), "%s/stuck-pipelines", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        uint64_t h[STAGES_MAX];
        uint32_t n = 0;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        for (char *p = line; *p && n < STAGES_MAX;) {
            while (*p == ',' || *p == ' ' || *p == 'k' || *p == '-') p++;
            char *end;
            unsigned long long v = strtoull(p, &end, 16);
            if (end == p) break;
            h[n++] = v;
            p = end;
        }
        add_stuck(h, n, NULL);
    }
    fclose(f);
    if (nstuck) VKB_WARN("%u pipeline(s) the driver never finished compiling fail at once (%s; delete it to retry them)", nstuck, path);
}

static void on_timeout_sig(int sig)
{
    guard_arm *a = tls_arm;
    if (!a || !a->timed_out) return;
    tls_arm = a->prev;
    a->sig = sig;
    a->addr = NULL;
    siglongjmp(a->jb, 1);
}

static void *watch_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = {0, 250000000};
        nanosleep(&ts, NULL);
        double now = mono_s();
        pthread_mutex_lock(&watch_lock);
        for (int i = 0; i < WATCH_SLOTS; i++) {
            watch_slot *w = &slots[i];
            if (!w->used) continue;
            double el = now - w->start;
            char st[400];
            if (el >= warn_s && !w->warned) {
                w->warned = 1;
                stages_text(w->hash, w->stage, w->nstages, st, sizeof(st));
                VKB_WARN("%s on device %u still running after %.1f s (the driver's compiler; stages %s)%s", w->call, w->device, el, st,
                         timeout_s > 0 ? "" : " - VKBRIDGE_COMPILE_TIMEOUT_S=0: not interrupted");
                for (uint32_t k = 0; k < w->nstages; k++) save_spirv(w->hash[k], w->code[k], w->size[k]);
            }
            if (timeout_s > 0 && el >= timeout_s && timeout_sig && !w->arm->timed_out) {
                w->arm->timed_out = 1;
                pthread_kill(w->thread, timeout_sig);
            }
        }
        pthread_mutex_unlock(&watch_lock);
    }
    return NULL;
}

static void watch_env(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_COMPILE_WARN_S");
    if (v && *v) warn_s = atof(v);
    v = vkb_srv_env("VKBRIDGE_COMPILE_TIMEOUT_S");
    if (v && *v) timeout_s = atof(v);
    if (watch_started) return;
    watch_started = 1;
    /* A real-time signal nobody else handles (ART and bionic keep the first few). */
    for (int s = SIGRTMIN + 4; s <= SIGRTMAX && !timeout_sig; s++) {
        struct sigaction old;
        if (sigaction(s, NULL, &old) != 0) continue;
        if ((old.sa_flags & SA_SIGINFO) || (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN)) continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_timeout_sig;
        sigemptyset(&sa.sa_mask);
        if (sigaction(s, &sa, NULL) == 0) timeout_sig = s;
    }
    load_stuck();
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, watch_main, NULL) != 0) VKB_WARN("compile watchdog: no thread");
    pthread_attr_destroy(&at);
    VKB_INFO("compile watchdog: warn after %.0f s, interrupt after %.0f s (signal %d)", warn_s, timeout_s, timeout_sig);
}

static watch_slot *watch_begin(guard_arm *arm, const char *call, uint32_t device, const uint64_t *h, const uint32_t *stage, uint32_t n,
                               const VkPipelineShaderStageCreateInfo *stages)
{
    watch_slot *w = NULL;
    pthread_mutex_lock(&watch_lock);
    for (int i = 0; i < WATCH_SLOTS && !w; i++)
        if (!slots[i].used) w = &slots[i];
    if (w) {
        memset(w, 0, sizeof(*w));
        w->used = 1;
        w->thread = pthread_self();
        w->arm = arm;
        w->start = mono_s();
        w->call = call;
        w->device = device;
        w->nstages = n < STAGES_MAX ? n : STAGES_MAX;
        if (h) memcpy(w->hash, h, w->nstages * sizeof(*h));
        if (stage) memcpy(w->stage, stage, w->nstages * sizeof(*stage));
        for (uint32_t i = 0; stages && i < w->nstages; i++)
            for (const VkBaseInStructure *b = stages[i].pNext; b; b = b->pNext)
                if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                    w->code[i] = ((const VkShaderModuleCreateInfo *)b)->pCode;
                    w->size[i] = ((const VkShaderModuleCreateInfo *)b)->codeSize;
                }
    }
    pthread_mutex_unlock(&watch_lock);
    return w;
}

/* Before the arm goes out of scope: a signal already on its way finds timed_out unset on the next
 * arm, or no arm, and returns. */
static void watch_end(watch_slot *w)
{
    if (!w) return;
    pthread_mutex_lock(&watch_lock);
    w->used = 0;
    pthread_mutex_unlock(&watch_lock);
}

/* Stage hashes of a batch: one pipeline's stages (DXVK and vkd3d-proton create one at a time);
 * of a larger batch, the first pipeline's. */
__attribute__((noinline)) static uint32_t gfx_hashes(const VkGraphicsPipelineCreateInfo *infos, uint32_t n, uint64_t *h, uint32_t *stage,
                                                     uint64_t *key)
{
    if (!n) return 0;
    uint32_t c = infos[0].stageCount < STAGES_MAX ? infos[0].stageCount : STAGES_MAX;
    for (uint32_t i = 0; i < c; i++) {
        uint64_t v;
        h[i] = stage_hash(&infos[0].pStages[i], &v);
        key[i] = h[i] ? h[i] ^ v : 0;
        stage[i] = infos[0].pStages[i].stage;
    }
    return c;
}

static void timed_out(vkb_guard *g, const char *call, const uint64_t *h, const uint64_t *key, const uint32_t *stage, uint32_t nh,
                      double el)
{
    char st[400];
    stages_text(h, stage, nh, st, sizeof(st));
    VKB_ERR("TIMED OUT: %s on device %u ran %.1f s in the driver and was interrupted - the call fails, the session lives "
            "(stages %s; their SPIR-V in stuck-spirv/, the pipeline fails at once from now on)", call, g->t->id, el, st);
    VKB_ERR("  driver locks held by the spinning compile stay held: a hang in a later driver call would be this one's doing");
    char note[480];
    snprintf(note, sizeof(note), "%s, stages %s", call, st);
    add_stuck(key, nh, note);
}

static VkResult VKAPI_CALL gd_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                     const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                     VkPipeline *out)
{
    vkb_guard *g = find(device);
    if (!g) return VK_ERROR_INITIALIZATION_FAILED;
    if (g->poisoned) {
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_DEVICE_LOST;
    }
    if (!armable()) {
        for (uint32_t i = 0; i < n; i++) test_fault(infos[i].pStages, infos[i].stageCount, sizeof(*infos[i].pStages));
        return g->gfx(device, cache, n, infos, a, out);
    }
    uint64_t h[STAGES_MAX], key[STAGES_MAX];
    uint32_t stg[STAGES_MAX];
    uint32_t nh = gfx_hashes(infos, n, h, stg, key);
    if (is_stuck(key, nh)) {
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        VKB_ONCE(VKB_LOG_WARN, "a pipeline the driver never finished compiling was asked for again: failed at once (logged once)");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    guard_arm arm;
    arm.prev = tls_arm;
    arm.timed_out = 0;
    watch_slot *volatile w = NULL;
    if (sigsetjmp(arm.jb, 1)) {
        double el = w ? mono_s() - w->start : 0;
        watch_end(w);
        if (arm.timed_out) {
            timed_out(g, "vkCreateGraphicsPipelines", h, key, stg, nh, el);
        } else {
            contained(g, "vkCreateGraphicsPipelines", &arm);
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t s = 0; s < infos[i].stageCount; s++) log_stage(&infos[i].pStages[s], i, n);
        }
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    tls_arm = &arm;
    w = watch_begin(&arm, "vkCreateGraphicsPipelines", g->t->id, h, stg, nh, n ? infos[0].pStages : NULL);
    for (uint32_t i = 0; i < n; i++) test_fault(infos[i].pStages, infos[i].stageCount, sizeof(*infos[i].pStages));
    VkResult r = g->gfx(device, cache, n, infos, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    return r;
}

static VkResult VKAPI_CALL gd_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                    const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                    VkPipeline *out)
{
    vkb_guard *g = find(device);
    if (!g) return VK_ERROR_INITIALIZATION_FAILED;
    if (g->poisoned) {
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_DEVICE_LOST;
    }
    if (!armable()) {
        if (n) test_fault(&infos[0].stage, n, sizeof(*infos));
        return g->compute(device, cache, n, infos, a, out);
    }
    uint64_t v = 0, h[1] = {n ? stage_hash(&infos[0].stage, &v) : 0};
    uint64_t key[1] = {h[0] ? h[0] ^ v : 0};
    uint32_t stg[1] = {VK_SHADER_STAGE_COMPUTE_BIT};
    uint32_t nh = n ? 1 : 0;
    if (is_stuck(key, nh)) {
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        VKB_ONCE(VKB_LOG_WARN, "a compute pipeline the driver never finished compiling was asked for again: failed at once (logged once)");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    guard_arm arm;
    arm.prev = tls_arm;
    arm.timed_out = 0;
    watch_slot *volatile w = NULL;
    if (sigsetjmp(arm.jb, 1)) {
        double el = w ? mono_s() - w->start : 0;
        watch_end(w);
        if (arm.timed_out) {
            timed_out(g, "vkCreateComputePipelines", h, key, stg, nh, el);
        } else {
            contained(g, "vkCreateComputePipelines", &arm);
            for (uint32_t i = 0; i < n; i++) log_stage(&infos[i].stage, i, n);
        }
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    tls_arm = &arm;
    w = watch_begin(&arm, "vkCreateComputePipelines", g->t->id, h, stg, nh, n ? &infos[0].stage : NULL);
    if (n) test_fault(&infos[0].stage, n, sizeof(*infos));
    VkResult r = g->compute(device, cache, n, infos, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    return r;
}

static VkResult VKAPI_CALL gd_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                VkShaderModule *out)
{
    vkb_guard *g = find(device);
    if (!g) return VK_ERROR_INITIALIZATION_FAILED;
    if (g->poisoned) {
        if (out) *out = VK_NULL_HANDLE;
        return VK_ERROR_DEVICE_LOST;
    }
    uint64_t mh = ci && ci->pCode ? spirv_hash(ci->pCode, ci->codeSize) : 0;
    if (mh && is_stuck(&mh, 1)) {
        if (out) *out = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!armable()) {
        VkResult r = g->module(device, ci, a, out);
        if (r == VK_SUCCESS && out) mod_remember(*out, ci);
        return r;
    }
    guard_arm arm;
    arm.prev = tls_arm;
    arm.timed_out = 0;
    watch_slot *volatile w = NULL;
    if (sigsetjmp(arm.jb, 1)) {
        watch_end(w);
        if (arm.timed_out) {
            uint64_t h = mh;
            uint32_t stg = 0;
            timed_out(g, "vkCreateShaderModule", &h, &h, &stg, 1, 0);
            if (out) *out = VK_NULL_HANDLE;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        contained(g, "vkCreateShaderModule", &arm);
        if (out) *out = VK_NULL_HANDLE;
        VKB_ERR("  module of %zu bytes", ci->codeSize);
        /* A module the driver died on is saved by nobody else (dumps happen after success). */
        if (vkb_emu_dumping()) {
            static int seq;
            char path[600];
            snprintf(path, sizeof(path), "%s/dump-spirv/crash-%d.spv", vkb_pcache_dir(), __atomic_fetch_add(&seq, 1, __ATOMIC_RELAXED));
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(ci->pCode, 1, ci->codeSize, f);
                fclose(f);
                VKB_ERR("  saved as %s", path);
            }
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    tls_arm = &arm;
    w = watch_begin(&arm, "vkCreateShaderModule", g->t->id, NULL, NULL, 0, NULL);
    VkResult r = g->module(device, ci, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    if (r == VK_SUCCESS && out) mod_remember(*out, ci);
    return r;
}

void vkb_guard_device_init(vkb_srv_table *t)
{
    guard_env();
    if (!guard_off) watch_env();
    if (guard_off || t->id >= MAX_TABLES || !t->real.vkCreateGraphicsPipelines || !t->real.vkCreateComputePipelines ||
        !t->real.vkCreateShaderModule)
        return;
    vkb_guard *g = calloc(1, sizeof(*g));
    if (!g) return;
    g->t = t;
    g->gfx = t->real.vkCreateGraphicsPipelines;
    g->compute = t->real.vkCreateComputePipelines;
    g->module = t->real.vkCreateShaderModule;
    __atomic_store_n(&guards[t->id], g, __ATOMIC_RELEASE);
    t->real.vkCreateGraphicsPipelines = t->dt.vkCreateGraphicsPipelines = gd_CreateGraphicsPipelines;
    t->real.vkCreateComputePipelines = t->dt.vkCreateComputePipelines = gd_CreateComputePipelines;
    t->real.vkCreateShaderModule = t->dt.vkCreateShaderModule = gd_CreateShaderModule;
}

void vkb_guard_device_destroy(vkb_srv_table *t)
{
    if (t->id >= MAX_TABLES) return;
    vkb_guard *g = guards[t->id];
    if (!g || g->t != t) return;
    __atomic_store_n(&guards[t->id], NULL, __ATOMIC_RELEASE);
    t->real.vkCreateGraphicsPipelines = g->gfx;
    t->real.vkCreateComputePipelines = g->compute;
    t->real.vkCreateShaderModule = g->module;
    free(g);
}
