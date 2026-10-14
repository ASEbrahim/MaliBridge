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
 * <cache dir>/stuck-spirv/), and one running past VKBRIDGE_COMPILE_TIMEOUT_S (10; 0 = only log)
 * is interrupted with a signal that jumps out of the driver as a contained crash does. Unreal
 * Engine takes a failed pipeline as fatal ("Fatal error!"), and a game that survives it draws
 * black, so the pipeline is then rebuilt with a fallback, each under the same watchdog:
 *   - UE4's colour-grading LUT fragment shader (CombineLUTs; spirv_gsfold.c recognizes it) is
 *     replaced by a neutral LUT (server/shaders/ue4_lut_standin.frag): no grading, a generic
 *     filmic curve, the picture keeps its content. On the G925 this shader itself was the
 *     trigger (Stray, 2026-10-14): the geometry fold and float_controls2 stripping each timed out
 *     too, and a constant colour whited the screen out;
 *   - otherwise the fragment stage becomes a constant-colour stand-in (last resort);
 *   - opt-in, before those (VKBRIDGE_COMPILE_FALLBACKS=gsfold,fc2): a layer-only pass-through
 *     geometry stage folded into the vertex shader (spirv_gsfold.c), and SPV_KHR_float_controls2
 *     stripped from every stage;
 * if none builds, the call fails. What worked is remembered in <cache dir>/stuck-pipelines (keyed
 * by the stages' SPIR-V, entry points and specialization; " m=<fallback> v=<list version>") and
 * used at once from then on, in later runs too; a line from an older fallback list is tried again
 * (delete the file to retry everything). VKBRIDGE_LAYER_GS_FOLD=1 folds every foldable geometry
 * stage up front. The same caveat as for crashes applies: a lock the driver held while it spun
 * stays held.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "spirv_gsfold.h"

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
static __thread int tls_variant; /* inside a fallback compile (float_controls2 stripped / stand-in) */
static vkb_guard *guards[MAX_TABLES];
static int process_crashes;
static int guard_off = -1;
static int test_crash = -1;

static void guard_env(void)
{
    if (guard_off < 0) {
        const char *s = getenv("VKBRIDGE_NO_CRASH_GUARD");
        guard_off = s && *s && *s != '0';
        /* =spin: marked pipelines spin instead (compile watchdog); =spinfc2: only until their
         * float_controls2 is stripped (the first fallback then compiles) */
        s = getenv("VKBRIDGE_TEST_CRASH_PIPELINE");
        test_crash = s && *s && *s != '0' ? (!strcmp(s, "spin") ? 2 : !strcmp(s, "spinfc2") ? 3 : 1) : 0;
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
            if (test_crash == 3 && tls_variant) continue;
            if (test_crash >= 2) {
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
    double limit; /* vkb_guard_run: its own limit (0: VKBRIDGE_COMPILE_TIMEOUT_S) */
} watch_slot;

typedef struct mod_hash {
    VkShaderModule m;
    uint64_t hash;
} mod_hash;

/* Module code by hash, oldest dropped past CODE_BYTES: what a fallback compile rebuilds a stage
 * from (and the stuck-spirv dump). Identical modules are kept once. */
#define CODE_MAX 32768
#define CODE_BYTES (96u << 20)
typedef struct code_entry {
    uint64_t hash;
    uint32_t *code;
    size_t size;
} code_entry;
static code_entry codes[CODE_MAX];
static unsigned code_next, code_count;
static size_t code_bytes;

static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static watch_slot slots[WATCH_SLOTS];
static double warn_s = 5, timeout_s = 10; /* the G925's normal compiles of Stray's shaders took < 2 s */
static int watch_started, timeout_sig;
static mod_hash mods[MOD_BUCKETS];
static uint64_t stuck[STUCK_MAX][STAGES_MAX];
static uint32_t stuck_n[STUCK_MAX], nstuck;
static int stuck_mode[STUCK_MAX]; /* FB_*: what this pipeline gets instead */

enum { FB_FAIL = 0, FB_RELAXED = 1, FB_STANDIN = 2, FB_NOGS = 3, FB_LUT = 4, FB_COUNT };
static const char *const fb_names[] = {"fails", "float_controls2 stripped", "constant-colour stand-in fragment shader",
                                       "pass-through geometry stage folded into the vertex shader",
                                       "neutral colour-grading LUT stand-in (UE4 CombineLUTs)"};
/* stuck-pipelines lines carry the version of the fallback list that chose them: a line from an
 * older list is tried again (2117ebc's constant colour whited out Stray's picture). */
#define STUCK_VERSION 3

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

/* Called with watch_lock held. */
static code_entry *code_find(uint64_t h)
{
    for (unsigned i = 0; h && i < code_count; i++) {
        code_entry *e = &codes[(code_next + CODE_MAX - 1 - i) % CODE_MAX];
        if (e->hash == h && e->code) return e;
    }
    return NULL;
}

/* A copy of the stored code (the caller frees it), or NULL. */
static uint32_t *code_get(uint64_t h, size_t *size)
{
    uint32_t *c = NULL;
    pthread_mutex_lock(&watch_lock);
    code_entry *e = code_find(h);
    if (e && (c = malloc(e->size))) {
        memcpy(c, e->code, e->size);
        *size = e->size;
    }
    pthread_mutex_unlock(&watch_lock);
    return c;
}

/* Modules are remembered by handle (their hash) while the compile watchdog is on; a stale entry is
 * replaced when the driver hands the handle out again. */
static void mod_remember(VkShaderModule m, const VkShaderModuleCreateInfo *ci)
{
    if (!m || !ci || !ci->pCode) return;
    uint64_t h = spirv_hash(ci->pCode, ci->codeSize);
    unsigned b = (unsigned)(((uintptr_t)m >> 4) * 2654435761u) % MOD_BUCKETS;
    pthread_mutex_lock(&watch_lock);
    for (unsigned i = 0; i < MOD_BUCKETS; i++) {
        mod_hash *e = &mods[(b + i) % MOD_BUCKETS];
        if (e->m && e->m != m && i < MOD_BUCKETS - 1) continue;
        e->m = m;
        e->hash = h;
        break;
    }
    if (!code_find(h)) {
        while (code_count && (code_count == CODE_MAX || code_bytes + ci->codeSize > CODE_BYTES)) {
            code_entry *o = &codes[(code_next + CODE_MAX - code_count) % CODE_MAX];
            code_bytes -= o->size;
            free(o->code);
            memset(o, 0, sizeof(*o));
            code_count--;
        }
        uint32_t *copy = malloc(ci->codeSize);
        if (copy) {
            memcpy(copy, ci->pCode, ci->codeSize);
            code_entry *e = &codes[code_next];
            code_next = (code_next + 1) % CODE_MAX;
            e->hash = h;
            e->code = copy;
            e->size = ci->codeSize;
            code_bytes += ci->codeSize;
            code_count++;
        }
    }
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
    code_entry *e = code ? NULL : code_find(h);
    if (e) {
        code = e->code;
        size = e->size;
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

/* 0: not known; else FB_* + 1. */
static int is_stuck(const uint64_t *h, uint32_t n)
{
    int hit = 0;
    pthread_mutex_lock(&watch_lock);
    for (uint32_t k = 0; k < nstuck && !hit; k++) {
        if (stuck_n[k] != n) continue;
        uint32_t same = 0;
        for (uint32_t i = 0; i < n; i++) same += stuck[k][i] == h[i] && h[i];
        if (same == n && n) hit = stuck_mode[k] + 1;
    }
    pthread_mutex_unlock(&watch_lock);
    return hit;
}

/* key: per stage, SPIR-V hash ^ entry/specialization hash. note: a comment for the file (NULL:
 * not written, as when loading it). */
static void add_stuck(const uint64_t *key, uint32_t n, int mode, const char *note)
{
    if (!n) return;
    pthread_mutex_lock(&watch_lock);
    int k;
    for (k = 0; k < (int)nstuck; k++) {
        if (stuck_n[k] != n) continue;
        uint32_t same = 0;
        for (uint32_t i = 0; i < n; i++) same += stuck[k][i] == key[i];
        if (same == n) break;
    }
    if (k == (int)nstuck && nstuck < STUCK_MAX) {
        memcpy(stuck[nstuck], key, n * sizeof(*key));
        stuck_n[nstuck++] = n;
    }
    if (k < (int)nstuck) stuck_mode[k] = mode; /* the last word wins (a later line in the file too) */
    pthread_mutex_unlock(&watch_lock);
    const char *dir = vkb_pcache_dir();
    if (!note || !dir) return;
    char path[700];
    snprintf(path, sizeof(path), "%s/stuck-pipelines", dir);
    FILE *f = fopen(path, "a");
    if (f) {
        for (uint32_t i = 0; i < n; i++) fprintf(f, "%sk-%016llx", i ? "," : "", (unsigned long long)key[i]);
        fprintf(f, " m=%d v=%d # %s: %s\n", mode, STUCK_VERSION, fb_names[mode], note);
        fclose(f);
    }
}

static void load_stuck(void)
{
    const char *dir = vkb_pcache_dir();
    if (!dir) return;
    char path[700], line[700];
    snprintf(path, sizeof(path), "%s/stuck-pipelines", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *m = strstr(line, "m="), *ver = strstr(line, "v=");
        if (!m || !ver || atoi(ver + 2) < STUCK_VERSION) continue; /* an older fallback list chose it: tried again */
        int mode = atoi(m + 2);
        *m = 0;
        if (mode < FB_FAIL || mode >= FB_COUNT) mode = FB_FAIL;
        uint64_t h[STAGES_MAX];
        uint32_t n = 0;
        for (char *p = line; *p && n < STAGES_MAX;) {
            while (*p == ',' || *p == ' ' || *p == 'k' || *p == '-') p++;
            char *end;
            unsigned long long v = strtoull(p, &end, 16);
            if (end == p) break;
            h[n++] = v;
            p = end;
        }
        add_stuck(h, n, mode, NULL);
    }
    fclose(f);
    if (nstuck) VKB_WARN("%u pipeline(s) the driver never finished compiling get their fallback at once (%s; delete it to retry them)", nstuck, path);
}

/* ------------------------------------------------------------------ fallbacks */

#define SPV_OP_EXTENSION 10
#define SPV_OP_CAPABILITY 17
#define SPV_OP_DECORATE 71
#define SPV_OP_EXECUTION_MODE_ID 331
#define SPV_CAP_FLOAT_CONTROLS2 6029
#define SPV_EM_FP_FAST_MATH_DEFAULT 6028
#define SPV_DEC_NO_CONTRACTION 42
#define SPV_DEC_FP_FAST_MATH_MODE 40

/* SPV_KHR_float_controls2 taken out (DXVK and vkd3d-proton emit it where the driver has
 * VK_KHR_shader_float_controls2; the Mali-G925's compiler spun on two of Stray's shaders that use
 * it, the G720 has no such extension). FPFastMathDefault goes (the default without it is what
 * every driver did before); an op marked FPFastMathMode None (exact) becomes NoContraction, the
 * decoration DXVK used for such ops before; other FPFastMathMode masks only allowed more freedom
 * and go. NULL when the module has no float_controls2. */
static uint32_t *spv_relax_fc2(const uint32_t *code, size_t size, size_t *out_size)
{
    size_t n = size / 4;
    if (n < 5 || code[0] != 0x07230203) return NULL;
    int has = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (!wc || i + wc > n) return NULL;
        if (op == SPV_OP_CAPABILITY && wc >= 2 && code[i + 1] == SPV_CAP_FLOAT_CONTROLS2) has = 1;
        i += wc;
    }
    if (!has) return NULL;
    uint32_t *o = malloc(size);
    if (!o) return NULL;
    memcpy(o, code, 20);
    size_t w = 5;
    for (size_t i = 5; i < n;) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        const uint32_t *ins = code + i;
        i += wc;
        if (op == SPV_OP_CAPABILITY && ins[1] == SPV_CAP_FLOAT_CONTROLS2) continue;
        if (op == SPV_OP_EXTENSION && wc >= 2 && !strncmp((const char *)(ins + 1), "SPV_KHR_float_controls2", (wc - 1) * 4)) continue;
        if (op == SPV_OP_EXECUTION_MODE_ID && wc >= 3 && ins[2] == SPV_EM_FP_FAST_MATH_DEFAULT) continue;
        if (op == SPV_OP_DECORATE && wc >= 3 && ins[2] == SPV_DEC_FP_FAST_MATH_MODE) {
            if (wc >= 4 && ins[3] == 0) {
                o[w++] = (3u << 16) | SPV_OP_DECORATE;
                o[w++] = ins[1];
                o[w++] = SPV_DEC_NO_CONTRACTION;
            }
            continue;
        }
        memcpy(o + w, ins, wc * 4);
        w += wc;
    }
    *out_size = w * 4;
    return o;
}

/* glslc -O: layout(location = 0) out vec4 o; void main() { o = vec4(0.5, 0.5, 0.5, 1.0); } */
static const uint32_t standin_fs[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47,
    0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x00030010, 0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0004002b,
    0x00000006, 0x0000000a, 0x3f000000, 0x0004002b, 0x00000006, 0x0000000b, 0x3f800000, 0x0007002c, 0x00000007, 0x0000000c,
    0x0000000a, 0x0000000a, 0x0000000a, 0x0000000b, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0003003e, 0x00000009, 0x0000000c, 0x000100fd, 0x00010038};

/* server/shaders/ue4_lut_standin.frag (glslc -O --target-env=vulkan1.0) */
static const uint32_t lut_standin_fs[] = {
#include "ue4_lut_standin.inc"
};

typedef struct fb_build {
    VkPipelineShaderStageCreateInfo st[STAGES_MAX];
    VkShaderModule tmp[STAGES_MAX];
    uint32_t ntmp;
} fb_build;

static void fb_free(vkb_guard *g, VkDevice device, fb_build *b)
{
    for (uint32_t i = 0; i < b->ntmp; i++) g->t->real.vkDestroyShaderModule(device, b->tmp[i], NULL);
    b->ntmp = 0;
}

static VkShaderModule fb_module(vkb_guard *g, VkDevice device, fb_build *b, const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (g->module(device, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    b->tmp[b->ntmp++] = m;
    return m;
}

static uint32_t *stage_code(const VkPipelineShaderStageCreateInfo *st, uint64_t h, size_t *size, int *owned)
{
    for (const VkBaseInStructure *p = st->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            *size = ((const VkShaderModuleCreateInfo *)p)->codeSize;
            *owned = 0;
            return (uint32_t *)((const VkShaderModuleCreateInfo *)p)->pCode;
        }
    *owned = 1;
    return code_get(h, size);
}

/* The stages of one pipeline with the fallback applied (b->st, *nout of them); 0 when it changes
 * nothing or cannot be applied. */
static int fb_stages(vkb_guard *g, VkDevice device, const VkPipelineShaderStageCreateInfo *st, uint32_t n, const uint64_t *h, int mode,
                     fb_build *b, uint32_t *nout)
{
    if (n > STAGES_MAX) return 0;
    int changed = 0;
    b->ntmp = 0;
    *nout = 0;
    if (mode == FB_NOGS) {
        int vs = -1, gs = -1;
        for (uint32_t i = 0; i < n; i++) {
            if (st[i].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = (int)i;
            if (st[i].stage == VK_SHADER_STAGE_GEOMETRY_BIT) gs = (int)i;
        }
        if (vs < 0 || gs < 0) return 0;
        size_t gsz = 0, vsz = 0, nsz = 0;
        int gown = 0, vown = 0;
        uint32_t *gc = stage_code(&st[gs], h[gs], &gsz, &gown), *vc = stage_code(&st[vs], h[vs], &vsz, &vown);
        uint32_t loc = 0, *nv = NULL;
        if (gc && vc && vkb_gs_layer_passthrough(gc, gsz, &loc)) nv = vkb_vs_write_layer(vc, vsz, loc, &nsz);
        if (gown) free(gc);
        if (vown) free(vc);
        if (!nv) return 0;
        VkShaderModule m = fb_module(g, device, b, nv, nsz);
        free(nv);
        if (!m) return 0;
        for (uint32_t i = 0; i < n; i++) {
            if ((int)i == gs) continue;
            b->st[*nout] = st[i];
            if ((int)i == vs) {
                b->st[*nout].module = m;
                b->st[*nout].pNext = NULL;
            }
            (*nout)++;
        }
        return 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        b->st[i] = st[i];
        if (mode == FB_LUT && st[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            size_t size = 0;
            int own = 0;
            uint32_t *code = stage_code(&st[i], h[i], &size, &own);
            int lut = code && vkb_fs_is_ue4_lut(code, size);
            if (own) free(code);
            if (!lut) break;
            VkShaderModule m = fb_module(g, device, b, lut_standin_fs, sizeof(lut_standin_fs));
            if (!m) break;
            b->st[i].module = m;
            b->st[i].pNext = NULL;
            b->st[i].pName = "main";
            b->st[i].pSpecializationInfo = NULL;
            changed = 1;
            continue;
        }
        if (mode == FB_STANDIN && st[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            VkShaderModule m = fb_module(g, device, b, standin_fs, sizeof(standin_fs));
            if (!m) break;
            b->st[i].module = m;
            b->st[i].pNext = NULL;
            b->st[i].pName = "main";
            b->st[i].pSpecializationInfo = NULL;
            changed = 1;
            continue;
        }
        if (mode != FB_RELAXED) continue;
        size_t size = 0;
        int own = 0;
        uint32_t *code = stage_code(&st[i], h[i], &size, &own);
        size_t rs = 0;
        uint32_t *relaxed = code ? spv_relax_fc2(code, size, &rs) : NULL;
        if (!relaxed && test_crash == 3 && code) { /* host test: shaders without it */
            relaxed = malloc(size);
            if (relaxed) memcpy(relaxed, code, rs = size);
        }
        if (relaxed) {
            VkShaderModule m = fb_module(g, device, b, relaxed, rs);
            if (m) {
                b->st[i].module = m;
                b->st[i].pNext = NULL;
                changed = 1;
            }
            free(relaxed);
        }
        if (own) free(code);
    }
    *nout = n;
    if (!changed) fb_free(g, device, b);
    return changed;
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
            double lim = w->limit > 0 ? w->limit : w->limit < 0 ? 0 : timeout_s;
            if (el >= warn_s && !w->warned) {
                w->warned = 1;
                stages_text(w->hash, w->stage, w->nstages, st, sizeof(st));
                VKB_WARN("%s on device %u still running after %.1f s (the driver's compiler; stages %s)%s", w->call, w->device, el, st,
                         timeout_s > 0 ? "" : " - VKBRIDGE_COMPILE_TIMEOUT_S=0: not interrupted");
                for (uint32_t k = 0; k < w->nstages; k++) save_spirv(w->hash[k], w->code[k], w->size[k]);
            }
            if (lim > 0 && el >= lim && timeout_sig && !w->arm->timed_out) {
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
                               const VkPipelineShaderStageCreateInfo *stages, uint32_t nst)
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
        for (uint32_t i = 0; stages && i < w->nstages && i < nst; i++)
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
    VKB_ERR("TIMED OUT: %s on device %u ran %.1f s in the driver and was interrupted - the session lives "
            "(stages %s; their SPIR-V in stuck-spirv/)", call, g->t->id, el, st);
    VKB_ERR("  driver locks held by the spinning compile stay held: a hang in a later driver call would be this one's doing");
}

static void remember(const char *call, const uint64_t *h, const uint64_t *key, const uint32_t *stage, uint32_t nh, int mode)
{
    char st[400], note[480];
    stages_text(h, stage, nh, st, sizeof(st));
    snprintf(note, sizeof(note), "%s, stages %s", call, st);
    add_stuck(key, nh, mode, note);
}

/* One guarded driver call; *timed: interrupted by the compile watchdog (the call failed). */
__attribute__((noinline)) static VkResult armed_gfx(vkb_guard *g, VkDevice device, VkPipelineCache cache, uint32_t n,
                                                    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *a, VkPipeline *out,
                                                    const uint64_t *h, const uint32_t *stg, uint32_t nh, int *timed)
{
    guard_arm arm;
    arm.prev = tls_arm;
    arm.timed_out = 0;
    watch_slot *volatile w = NULL;
    *timed = 0;
    if (sigsetjmp(arm.jb, 1)) {
        double el = w ? mono_s() - w->start : 0;
        watch_end(w);
        if (arm.timed_out) {
            *timed = 1;
            timed_out(g, tls_variant ? "vkCreateGraphicsPipelines (fallback)" : "vkCreateGraphicsPipelines", h, NULL, stg, nh, el);
        } else {
            contained(g, "vkCreateGraphicsPipelines", &arm);
            for (uint32_t i = 0; i < n; i++)
                for (uint32_t s = 0; s < infos[i].stageCount; s++) log_stage(&infos[i].pStages[s], i, n);
        }
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    tls_arm = &arm;
    w = watch_begin(&arm, "vkCreateGraphicsPipelines", g->t->id, h, stg, nh, n ? infos[0].pStages : NULL, n ? infos[0].stageCount : 0);
    for (uint32_t i = 0; i < n; i++) test_fault(infos[i].pStages, infos[i].stageCount, sizeof(*infos[i].pStages));
    VkResult r = g->gfx(device, cache, n, infos, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    return r;
}

/* The pipeline built with a fallback (one pipeline per call only). */
static VkResult try_fallback(vkb_guard *g, VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *info,
                             const VkAllocationCallbacks *a, VkPipeline *out, const uint64_t *h, const uint32_t *stg, uint32_t nh, int mode,
                             int *timed)
{
    fb_build b;
    uint32_t ns = 0;
    *timed = 0;
    if (!fb_stages(g, device, info->pStages, info->stageCount, h, mode, &b, &ns)) return VK_ERROR_INITIALIZATION_FAILED;
    VkGraphicsPipelineCreateInfo ci = *info;
    ci.stageCount = ns;
    ci.pStages = b.st;
    tls_variant = 1;
    VkResult r = armed_gfx(g, device, cache, 1, &ci, a, out, h, stg, nh, timed);
    tls_variant = 0;
    fb_free(g, device, &b);
    VKB_WARN("compile fallback (%s) for stages h-%016llx...: %s", fb_names[mode], (unsigned long long)(nh ? h[0] : 0),
             r == VK_SUCCESS ? "built" : *timed ? "timed out too" : "failed");
    return r;
}

static int layer_fold_always(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = vkb_srv_env("VKBRIDGE_LAYER_GS_FOLD");
        /* also when the startup probe saw this driver draw a layer-only geometry stage wrong (and
         * a vertex shader's Layer right): server_probe.c; =0 keeps the stage */
        v = e && *e ? *e == '1' : vkb_probe_flag(VKB_PROBE_FLAG_GS_FOLD);
    }
    return v;
}

/* One pipeline whose fragment stage is UE4's colour-grading LUT (vkb_fs_is_ue4_lut). */
static int is_lut_pipeline(const VkGraphicsPipelineCreateInfo *info, const uint64_t *h, uint32_t nh)
{
    for (uint32_t s = 0; s < info->stageCount && s < nh; s++) {
        if (info->pStages[s].stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
        size_t size = 0;
        int own = 0;
        uint32_t *code = stage_code(&info->pStages[s], h[s], &size, &own);
        int lut = code && vkb_fs_is_ue4_lut(code, size);
        if (own) free(code);
        return lut;
    }
    return 0;
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
    int timed = 0;
    VkResult r;
    int known = is_stuck(key, nh);
    if (known) {
        int mode = known - 1;
        VKB_ONCE(VKB_LOG_WARN, "a pipeline the driver never finished compiling was asked for again: its fallback at once (logged once)");
        r = VK_ERROR_INITIALIZATION_FAILED;
        if (mode != FB_FAIL && n == 1) r = try_fallback(g, device, cache, infos, a, out, h, stg, nh, mode, &timed);
        if (r != VK_SUCCESS)
            for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return r;
    }
    /* The startup probe saw this driver's compiler spin on the LUT's shape (server_probe.c): its
     * stand-in at once instead of after a timeout (before the
     * geometry fold: the spin is the worse failure). */
    if (n == 1 && vkb_probe_flag(VKB_PROBE_FLAG_LUT_NOW) && is_lut_pipeline(infos, h, nh)) {
        r = try_fallback(g, device, cache, infos, a, out, h, stg, nh, FB_LUT, &timed);
        if (r == VK_SUCCESS) {
            VKB_WARN("UE4 colour-grading LUT pipeline built with its neutral stand-in at once: the startup probe saw this "
                     "driver's compiler spin on the shape (VKBRIDGE_PROBE_APPLY=0: compile it)");
            return r;
        }
    }
    /* VKBRIDGE_LAYER_GS_FOLD=1: every foldable pass-through geometry stage goes, before any timeout. */
    if (n == 1 && layer_fold_always()) {
        int has_gs = 0;
        for (uint32_t s = 0; s < infos[0].stageCount; s++) has_gs |= infos[0].pStages[s].stage == VK_SHADER_STAGE_GEOMETRY_BIT;
        if (has_gs) {
            r = try_fallback(g, device, cache, infos, a, out, h, stg, nh, FB_NOGS, &timed);
            if (r == VK_SUCCESS) return r;
        }
    }
    r = armed_gfx(g, device, cache, n, infos, a, out, h, stg, nh, &timed);
    if (!timed) return r;
    /* The driver never finished: the fallbacks. By default only fragment-level ones - on the G925
     * the geometry fold and float_controls2 stripping did not help (each costs a timeout) - the
     * best picture first; VKBRIDGE_COMPILE_FALLBACKS=gsfold,fc2 tries those before them. */
    int order[6], norder = 0;
    const char *extra = vkb_srv_env("VKBRIDGE_COMPILE_FALLBACKS");
    if (extra && strstr(extra, "gsfold")) order[norder++] = FB_NOGS;
    if (extra && strstr(extra, "fc2")) order[norder++] = FB_RELAXED;
    order[norder++] = FB_LUT;
    order[norder++] = FB_STANDIN;
    for (int k = 0; n == 1 && k < norder; k++) {
        r = try_fallback(g, device, cache, infos, a, out, h, stg, nh, order[k], &timed);
        if (r == VK_SUCCESS) {
            VKB_ERR("  the pipeline was built with its fallback: %s%s", fb_names[order[k]],
                    order[k] == FB_STANDIN ? " (its draws are wrong; the game goes on)"
                    : order[k] == FB_LUT    ? " (no colour grading; the picture keeps its content)"
                                            : "");
            remember("vkCreateGraphicsPipelines", h, key, stg, nh, order[k]);
            return r;
        }
    }
    VKB_ERR("  no fallback built the pipeline: the call fails");
    remember("vkCreateGraphicsPipelines", h, key, stg, nh, FB_FAIL);
    for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
}

__attribute__((noinline)) static VkResult armed_compute(vkb_guard *g, VkDevice device, VkPipelineCache cache, uint32_t n,
                                                        const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                        VkPipeline *out, const uint64_t *h, const uint32_t *stg, uint32_t nh, int *timed)
{
    guard_arm arm;
    arm.prev = tls_arm;
    arm.timed_out = 0;
    watch_slot *volatile w = NULL;
    *timed = 0;
    if (sigsetjmp(arm.jb, 1)) {
        double el = w ? mono_s() - w->start : 0;
        watch_end(w);
        if (arm.timed_out) {
            *timed = 1;
            timed_out(g, tls_variant ? "vkCreateComputePipelines (fallback)" : "vkCreateComputePipelines", h, NULL, stg, nh, el);
        } else {
            contained(g, "vkCreateComputePipelines", &arm);
            for (uint32_t i = 0; i < n; i++) log_stage(&infos[i].stage, i, n);
        }
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    tls_arm = &arm;
    w = watch_begin(&arm, "vkCreateComputePipelines", g->t->id, h, stg, nh, n ? &infos[0].stage : NULL, 1);
    if (n) test_fault(&infos[0].stage, n, sizeof(*infos));
    VkResult r = g->compute(device, cache, n, infos, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    return r;
}

static VkResult compute_relaxed(vkb_guard *g, VkDevice device, VkPipelineCache cache, const VkComputePipelineCreateInfo *info,
                                const VkAllocationCallbacks *a, VkPipeline *out, const uint64_t *h, const uint32_t *stg, uint32_t nh)
{
    fb_build b;
    uint32_t ns = 0;
    int timed = 0;
    if (!fb_stages(g, device, &info->stage, 1, h, FB_RELAXED, &b, &ns)) return VK_ERROR_INITIALIZATION_FAILED;
    VkComputePipelineCreateInfo ci = *info;
    ci.stage = b.st[0];
    tls_variant = 1;
    VkResult r = armed_compute(g, device, cache, 1, &ci, a, out, h, stg, nh, &timed);
    tls_variant = 0;
    fb_free(g, device, &b);
    VKB_WARN("compile fallback (%s) for compute stage h-%016llx: %s", fb_names[FB_RELAXED], (unsigned long long)h[0],
             r == VK_SUCCESS ? "built" : timed ? "timed out too" : "failed");
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
    int timed = 0;
    VkResult r;
    int known = is_stuck(key, nh);
    if (known) {
        VKB_ONCE(VKB_LOG_WARN, "a compute pipeline the driver never finished compiling was asked for again: its fallback at once (logged once)");
        r = known - 1 == FB_RELAXED && n == 1 ? compute_relaxed(g, device, cache, infos, a, out, h, stg, nh) : VK_ERROR_INITIALIZATION_FAILED;
        if (r != VK_SUCCESS)
            for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return r;
    }
    r = armed_compute(g, device, cache, n, infos, a, out, h, stg, nh, &timed);
    if (!timed) return r;
    if (n == 1 && compute_relaxed(g, device, cache, infos, a, out, h, stg, nh) == VK_SUCCESS) {
        VKB_ERR("  the compute pipeline was built with its fallback: %s", fb_names[FB_RELAXED]);
        remember("vkCreateComputePipelines", h, key, stg, nh, FB_RELAXED);
        return VK_SUCCESS;
    }
    VKB_ERR("  no fallback built the compute pipeline: the call fails");
    remember("vkCreateComputePipelines", h, key, stg, nh, FB_FAIL);
    for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
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
            remember("vkCreateShaderModule", &h, &h, &stg, 1, FB_FAIL);
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
    w = watch_begin(&arm, "vkCreateShaderModule", g->t->id, NULL, NULL, 0, NULL, 0);
    VkResult r = g->module(device, ci, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    if (r == VK_SUCCESS && out) mod_remember(*out, ci);
    return r;
}

/* ------------------------------------------------------------------ the startup probe */

const uint32_t *vkb_guard_lut_standin(size_t *size)
{
    *size = sizeof(lut_standin_fs);
    return lut_standin_fs;
}

int vkb_guard_run(const char *what, double limit_s, void (*fn)(void *), void *arg, int *sig)
{
    guard_env();
    if (sig) *sig = 0;
    if (guard_off) {
        fn(arg);
        return VKB_GUARD_DONE;
    }
    watch_env();
    guard_arm arm;
    arm.prev = tls_arm;
    arm.timed_out = 0;
    watch_slot *volatile w = NULL;
    if (sigsetjmp(arm.jb, 1)) {
        watch_end(w);
        if (sig) *sig = arm.sig;
        return arm.timed_out ? VKB_GUARD_TIMED_OUT : VKB_GUARD_CRASHED;
    }
    tls_arm = &arm;
    w = watch_begin(&arm, what, 0, NULL, NULL, 0, NULL, 0);
    if (w) {
        pthread_mutex_lock(&watch_lock);
        w->limit = limit_s;
        w->warned = 1; /* the probe says what it was doing itself */
        pthread_mutex_unlock(&watch_lock);
    }
    fn(arg);
    tls_arm = arm.prev;
    watch_end(w);
    return VKB_GUARD_DONE;
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
