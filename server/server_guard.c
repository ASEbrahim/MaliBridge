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
 * named the thread's current command); one on a driver worker thread still kills the process
 * (a player log: the driver's "mali-compiler" thread). What it can still do: the compiles in
 * flight are written to stuck-pipelines as crash suspects (vkb_guard_crash_suspects, x=1), so the
 * next start gives them their stand-in at once; a later run asks the driver once more (built: off
 * the list; crashed again: the stand-in for good).
 *
 * Compiles that never end. The Mali-G925's compiler (Android 16, 2026-10-14) spun at 100% of a
 * core inside vkCreateGraphicsPipelines when Stray (DXVK) created its first ~25 game pipelines;
 * the game's thread waited for the reply for ever (UE "RenderThread timed out after 120 s"), and
 * no GPU work was involved. A watchdog thread now looks at every guarded call: one running past
 * VKBRIDGE_COMPILE_WARN_S (5) is logged with its stages' SPIR-V hashes (and the SPIR-V saved to
 * <cache dir>/stuck-spirv/), and one running past VKBRIDGE_COMPILE_TIMEOUT_S (by the mode,
 * server_mode.c: 10 in safe mode, 20 in performance mode; 0 = only log) is interrupted with a signal that jumps out of the driver as a contained crash does. Unreal
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
 *
 * A line in that file is a suspect, not a verdict. The watchdog cuts a compile off without knowing
 * whether it would have finished, and a compile can run out of time without being at fault: 29
 * compute pipelines of the S11 Ultra's list timed out once and then built in 0.0-2.5 s (a player log),
 * on a list with a real G925 hang beside them. So what draws wrong or not at all is asked of the
 * driver again in later runs, and what the driver builds stops being stuck:
 *   - the constant-colour stand-in (m=2): the real pipeline again with twice, then four times the
 *     limit (one line per try, written first, so a try that hangs is not repeated for ever); built,
 *     it is kept as m=5. The first try of a run that runs out of time ends that run's tries of
 *     these: a true hang costs one interrupt in each of the next two runs, then none;
 *   - a pipeline with no fallback (m=0), asked again once per run: built, a line m=5 t=1 takes it
 *     off the list for good (is_stuck): a later run compiles it as any other, the same limit, the
 *     same fold and LUT checks;
 *   - a shader module (m=0): refused without a word before; the driver is asked again as a compute
 *     pipeline is (t=1..3), with the same m=5 t=1 when it builds, and a refusal now says so.
 * The fallbacks that draw right (m=1, m=3) and the LUT stand-in (m=4, one try, row 113) stay as they
 * were. No new line format: lines of older builds (v=3) take these tries too, and an older build
 * reads these lines as it reads its own.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "spirv_gsfold.h"
#include "spirv_caps.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <pthread.h>
#include <setjmp.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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
    double limit; /* this device's interrupt (s; < 0: none), chosen at its creation: its game's preset counts */
    int feedback;  /* pipeline creation feedback asked of the driver (cache_feedback) */
    uint64_t built, hits; /* pipelines the driver built with feedback; of those, found in the pipeline cache */
    /* memory calls (a player log): the driver's, and the faults contained in them */
    PFN_vkAllocateMemory alloc;
    PFN_vkBindImageMemory bind_img;
    PFN_vkBindImageMemory2 bind_img2;
    PFN_vkBindBufferMemory bind_buf;
    PFN_vkBindBufferMemory2 bind_buf2;
    int mem_crashes;
    /* the driver's vkEndCommandBuffer of a client's replayed command buffer (a player log) */
    PFN_vkEndCommandBuffer end;
    int end_crashes;
    /* the capability check (cap_*): decided at the device's creation */
    uint32_t lacks;   /* what the driver lacks of what SPIR-V can ask (spirv_caps.h bits) */
    uint32_t refuse;  /* of those, what a pipeline is refused for */
    uint32_t ndyn;    /* dynamic states the driver gave no entry point to set */
    VkDynamicState dyn[24];
} vkb_guard;

#define GUARD_RA 6
typedef struct guard_arm {
    sigjmp_buf jb;
    volatile int sig;
    void *volatile addr;
    volatile int code;        /* si_code of the fault (SEGV_ACCERR 2 for a player log), 0 for the watchdog's interrupt */
    volatile uintptr_t pc;    /* the faulting pc, 0 when unknown */
    volatile int timed_out; /* set by the watchdog before it signals the thread */
    int base;               /* a connection thread's base arm (vkb_guard_serve): any fault no specific arm took */
    volatile unsigned snap; /* set by the handler: the calls in flight at the fault (snap_take), 0 none */
    uintptr_t ra[GUARD_RA];   /* set by the handler: the return addresses of the frame records above the fault, newest first */
    int nra;
    struct guard_arm *prev;
} guard_arm;

static __thread guard_arm *volatile tls_arm;
static __thread int tls_variant; /* inside a fallback compile (float_controls2 stripped / stand-in) */
static vkb_guard *guards[MAX_TABLES];
static int process_crashes;
/* a player log (Maleoon 920): 65 short-lived steamwebhelpers each faulted once in vkGetDeviceQueue over 3.5 min; every
 * fault was contained, but the count only grew, so the 65th ended the server and the session for everyone. Each
 * contained fault is charged to its device (table id); when the device goes (its program destroyed it or ended),
 * its charge leaves the count. PROCESS_MAX now limits faults of programs still running (a storm), not the
 * server's lifetime. A driver lock the fault left held is the memory fault watchdog's to catch, as before. */
static int charged[MAX_TABLES];
static int charge(uint32_t id)
{
    if (id < MAX_TABLES) __atomic_add_fetch(&charged[id], 1, __ATOMIC_RELAXED);
    return __atomic_add_fetch(&process_crashes, 1, __ATOMIC_RELAXED);
}
static int guard_off = -1;
static int test_crash = -1;

static void guard_env(void)
{
    if (guard_off < 0) {
        const char *s = vkb_srv_env_global("VKBRIDGE_NO_CRASH_GUARD");
        guard_off = s && *s && *s != '0';
        /* =spin: marked pipelines spin instead (compile watchdog); =spinfc2: only until their
         * float_controls2 is stripped (the first fallback then compiles); =slow: they take
         * VKBRIDGE_TEST_SLOW_S (3) seconds, then compile (the compute retry); =gs: every pipeline
         * with a geometry stage faults, marked or not ('s r38p1 beta: server_errata_gs.c); =clip:
         * every pipeline a stage of which reaches the driver with a clip or cull distance built-in or
         * capability faults at 0x40, as Mali's compiler did without shaderClipDistance (ULTRAKILL; No
         * Man's Sky, a player log: 0x18 / 0x40); =worker: a marked pipeline's compile has a thread
         * of the "driver" named "mali-compiler" fault (a read at 0x8 inside libc's strlen: the pc outside the
         * server, as a player log's was in libGLES_mali.so) while the call waits for it, as Mali's compile waits on
         * its compiler worker: the guard cannot contain it (no arm on that thread) and the process dies;
         * =workerself: the same thread faults in the server's own code instead (a server bug, not a compile) */
        s = getenv("VKBRIDGE_TEST_CRASH_PIPELINE");
        test_crash = s && *s && *s != '0' ? (!strcmp(s, "spin") ? 2 : !strcmp(s, "spinfc2") ? 3 : !strcmp(s, "slow") ? 4 : !strcmp(s, "gs") ? 5 : !strcmp(s, "clip") ? 6 : !strcmp(s, "worker") ? 7 : !strcmp(s, "workerself") ? 8 : 1) : 0;
        if (guard_off) VKB_INFO("driver crash containment off (VKBRIDGE_NO_CRASH_GUARD)");
        if (test_crash) VKB_WARN("VKBRIDGE_TEST_CRASH_PIPELINE: marked pipelines fault on purpose");
    }
}

static unsigned snap_take(void);
static int snap_text(char *buf, size_t cap, unsigned snap);

/* From the crash handler; async-signal-safe up to the jump (TLS reads, plain stores, the copy of the calls in
 * flight). Returns only when the faulting thread is not inside a guarded call. */
/* In the handler: the return addresses of the frame records above the fault (the caller of the faulting function: the
 * contained line alone gave only the pc, and a function with two callers - PowerVR's GrabScissorDepthBiasBuffers, a player log
 * and the 11-13 Pixel run - could not be told apart). Plain loads of the thread's own stack only: a record is followed
 * while it lies above the stack pointer, within 1 MB of it, aligned, and the next one is higher (no read outside the
 * stack the thread is running on). Pointer-authentication bits are stripped (Android arm64: 39-bit addresses). */
static int fault_ras(void *ucv, uintptr_t *ra, int max)
{
    int n = 0;
#if defined(__aarch64__) || (defined(__x86_64__) && defined(REG_RBP))
    if (!ucv) return 0;
    const ucontext_t *uc = ucv;
#if defined(__aarch64__)
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29], sp = (uintptr_t)uc->uc_mcontext.sp;
#else
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP], sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#endif
    while (n < max && fp >= sp && fp - sp < (1u << 20) && !(fp & 7)) {
        uintptr_t next = ((const uintptr_t *)fp)[0], r = ((const uintptr_t *)fp)[1];
#if defined(__aarch64__) && defined(__ANDROID__)
        r &= 0x7fffffffffull;
#endif
        if (!r) break;
        ra[n++] = r;
        if (next <= fp) break;
        fp = next;
    }
#else
    (void)ucv;
    (void)ra;
    (void)max;
#endif
    return n;
}

void vkb_guard_fault(int sig, void *addr, int code, uintptr_t pc, void *uc)
{
    guard_arm *a = tls_arm;
    if (!a) return;
    /* the base arm stops containing past the process limit, as the specific ones do (armable) */
    if (a->base && (guard_off || __atomic_load_n(&process_crashes, __ATOMIC_RELAXED) >= PROCESS_MAX)) return;
    tls_arm = a->prev;
    a->sig = sig;
    a->addr = addr;
    a->code = code;
    a->pc = pc;
    a->snap = snap_take();
    a->nra = fault_ras(uc, a->ra, GUARD_RA);
    siglongjmp(a->jb, 1);
}

/*
 * After a contained fault (not the watchdog's interrupt): what the uncontained crash line says, which
 * the CONTAINED line alone did not (t12-t15: the next RE7/RE8 log would have carried only "signal 11
 * (address ..)" and could not tell which hypothesis of a player log holds): the fault's code and pc as
 * library + offset (the same driver site, libGLES_mali.so+0xa177bc, or another), what the address lies in
 * (/proc/self/maps: scudo's reservation, /dev/mali0, a dma-buf, nothing), the call's arguments (the
 * allocation's path and dma-buf, the bind's image and memory) and the other programs' calls in flight
 * (an import or a destroy racing it). Runs after the jump, in the thread's own context: the calls in flight
 * AT the fault are the handler's copy (snap_take); the list made here, later, is printed after it as
 * "after recovery" (the only list until then, it named calls that began after the fault).
 */
static void fault_detail(const char *call, const guard_arm *a)
{
    if (a->timed_out || (!a->code && !a->pc)) return;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; /* vkb_crashctx_where has static buffers */
    static char buf[4096];
    pthread_mutex_lock(&lock);
    /* VKBRIDGE_TEST_FAULT_LATE_MS=<ms> (host/faultsnap_test.sh): this account waits that long first, as on the phone,
     * where dladdr, /proc/self/maps and the call's description took 22-30 ms (a player log) */
    static int late_ms = -1;
    if (late_ms < 0) {
        const char *s = getenv("VKBRIDGE_TEST_FAULT_LATE_MS");
        late_ms = s && *s ? atoi(s) : 0;
    }
    if (late_ms > 0) {
        struct timespec ts = {late_ms / 1000, (long)(late_ms % 1000) * 1000000L};
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
    }
    Dl_info di;
    memset(&di, 0, sizeof(di));
    int found = a->pc && dladdr((void *)a->pc, &di) && di.dli_fname;
    const char *lib = found ? strrchr(di.dli_fname, '/') : NULL;
    int n = snprintf(buf, sizeof(buf), "vkbridge-server E   fault: code %d%s, pc %p (%s+0x%lx)\n", a->code,
                     a->sig == SIGSEGV && a->code == SEGV_ACCERR ? " (SEGV_ACCERR)" : a->sig == SIGSEGV && a->code == SEGV_MAPERR ? " (SEGV_MAPERR)" : "",
                     (void *)a->pc, found ? (lib ? lib + 1 : di.dli_fname) : "?", found ? (unsigned long)(a->pc - (uintptr_t)di.dli_fbase) : 0ul);
    for (int i = 0; i < a->nra && n > 0 && n < (int)sizeof(buf) - 120; i++) {
        Dl_info ri;
        memset(&ri, 0, sizeof(ri));
        int rf = dladdr((void *)a->ra[i], &ri) && ri.dli_fname;
        const char *rl = rf ? strrchr(ri.dli_fname, '/') : NULL;
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s+0x%lx%s", i ? " <- " : "vkbridge-server E   called from (frame records, newest first): ",
                      rf ? (rl ? rl + 1 : ri.dli_fname) : "?", rf ? (unsigned long)(a->ra[i] - (uintptr_t)ri.dli_fbase) : (unsigned long)a->ra[i],
                      i == a->nra - 1 ? "\n" : "");
    }
    if (n > 0 && n < (int)sizeof(buf) - 1) n += vkb_crashctx_where(buf + n, sizeof(buf) - (size_t)n, a->addr);
    if (n > 0 && n < (int)sizeof(buf) - 1) n += vkb_crashctx_describe(buf + n, sizeof(buf) - (size_t)n, call);
    if (n > 0 && n < (int)sizeof(buf) - 1) n += snap_text(buf + n, sizeof(buf) - (size_t)n, a->snap);
    /* and again now, after the jump and the account above (on the phone 22-30 ms after the fault: a player log): calls that
     * began after the fault are among these, so this list alone never shows what ran at the fault */
    if (n > 0 && n < (int)sizeof(buf) - 1) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "vkbridge-server E   calls in flight after recovery: ");
        int m = n > 0 && n < (int)sizeof(buf) - 1 ? vkb_srv_crash_others(buf + n, sizeof(buf) - (size_t)n) : 0;
        if (!m && n > 0 && n < (int)sizeof(buf) - 1) m = snprintf(buf + n, sizeof(buf) - (size_t)n, "none");
        n += m;
    }
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    /* one log line per line of the crash handler's text, without its "vkbridge-server E " lead */
    for (char *l = buf; *l;) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        const char *body = !strncmp(l, "vkbridge-server E ", 18) ? l + 18 : l;
        if (*body) VKB_ERR("%s", body);
        if (!e) break;
        l = e + 1;
    }
    pthread_mutex_unlock(&lock);
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
    int total = charge(g->t->id);
    if (mine >= POISON_AT) g->poisoned = 1;
    VKB_ERR("CONTAINED driver crash: signal %d (address %p) in %s on device %u - the call fails, the session lives "
            "(%d on this device, %d in the process%s%s)",
            a->sig, a->addr, call, g->t->id, mine, total, g->poisoned ? "; device poisoned: no more pipelines on it" : "",
            total >= PROCESS_MAX ? "; containment now off, the next fault kills the process" : "");
    fault_detail(call, a);
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
static double mono_s(void);

static uint32_t stage_needs(const VkPipelineShaderStageCreateInfo *st);

/* =worker / =workerself: the "driver's" compiler thread that faults while the call waits for it */
static void *worker_fault_main(void *self)
{
    pthread_setname_np(pthread_self(), "mali-compiler");
    if (self) *(volatile int *)(uintptr_t)0x8 = 1; /* in the server's own code */
    size_t (*volatile sl)(const char *) = (size_t (*)(const char *))dlsym(RTLD_DEFAULT, "strlen");
    if (sl) sl((const char *)(uintptr_t)0x8); /* in libc: outside the server, as in the driver */
    return NULL;
}

static void test_fault(const VkPipelineShaderStageCreateInfo *stages, uint32_t n, size_t stride)
{
    if (!test_crash) return;
    if (test_crash == 7 || test_crash == 8) {
        for (uint32_t i = 0; i < n; i++)
            if (marked((const VkPipelineShaderStageCreateInfo *)((const char *)stages + i * stride))) {
                pthread_t th;
                VKB_WARN("test: the driver's compiler thread faults on purpose (%s) during a %s call", test_crash == 8 ? "in the server's code" : "in libc",
                         tls_arm ? "guarded" : "UNGUARDED");
                if (pthread_create(&th, NULL, worker_fault_main, test_crash == 8 ? (void *)1 : NULL) == 0) pthread_join(th, NULL);
            }
        return;
    }
    if (test_crash == 6) {
        for (uint32_t i = 0; i < n; i++)
            if (stage_needs((const VkPipelineShaderStageCreateInfo *)((const char *)stages + i * stride)) & (VKB_SPVCAP_CLIP | VKB_SPVCAP_CULL)) {
                VKB_WARN("test: a clip/cull distance built-in reached the driver: faulting on purpose (imitated Mali compiler) in a %s call",
                         tls_arm ? "guarded" : "UNGUARDED");
                *(volatile int *)(uintptr_t)0x40 = 1;
            }
        return;
    }
    for (uint32_t i = 0; i < n && test_crash == 5; i++)
        if (((const VkPipelineShaderStageCreateInfo *)((const char *)stages + i * stride))->stage == VK_SHADER_STAGE_GEOMETRY_BIT) {
            VKB_WARN("test: faulting on purpose on a geometry stage in a %s call", tls_arm ? "guarded" : "UNGUARDED");
            *(volatile int *)(uintptr_t)0x40 = 1;
        }
    if (test_crash == 5) return;
    for (uint32_t i = 0; i < n; i++)
        if (marked((const VkPipelineShaderStageCreateInfo *)((const char *)stages + i * stride))) {
            if (test_crash == 3 && tls_variant) continue;
            if (test_crash == 4) {
                const char *v = getenv("VKBRIDGE_TEST_SLOW_S");
                double until = mono_s() + (v && *v ? atof(v) : 3.0);
                VKB_WARN("test: compiling slowly on purpose in a %s call", tls_arm ? "guarded" : "UNGUARDED");
                while (mono_s() < until) {
                }
                continue;
            }
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
    /* for the crash handler (vkb_guard_crash_suspects): the pipeline's stuck-list key, set only for the
     * program's own pipeline compile (not a fallback's, a module's or vkb_guard_run's) */
    int pipe;   /* 1 graphics, 2 compute, 0 none */
    int trying; /* a listed pipeline asked of the driver again (tls_limit) */
    uint64_t key[STAGES_MAX];
    /* for the fault-time copy (snap_take): the thread's kernel id, and the request it serves (vkb_srv_current_cmd: NULL on the
     * server's own threads, "... (in the background)" on the async compile's workers) */
    int tid;
    const char *ctx;
} watch_slot;

typedef struct mod_hash {
    VkShaderModule m;
    uint64_t hash;
    uint32_t needs; /* what the module asks of the device (spirv_caps.h), from its SPIR-V when it was made */
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
/* each slot's change count, outside the slot (watch_begin clears it): odd while watch_begin / watch_end write it under
 * watch_lock, so the signal handler, which takes no lock, can tell a slot it read half-written */
static unsigned slot_seq[WATCH_SLOTS];
static __thread int tls_tid;
extern __thread const char *vkb_srv_current_cmd; /* server_conn.c */

/*
 * The calls in flight AT a contained fault (ledger 297). A player's RE8 logs (a player log, Mali-G615 r44p1) listed the
 * other calls in flight after the fault's jump, 22-30 ms after it, so the calls named had begun after the fault and nothing
 * showed what was in the driver at it. The handler now copies, before the jump, the markers that already exist: each
 * connection's running request and its start (conn->running / running_ns, set for every request) and each guarded compile's
 * watch slot (the async compile's workers, the BC transcoder builds, every pipeline and module). Nothing new on the request
 * path; a compile's slot gains two plain stores of its change count. In the handler: atomic loads and plain copies into a
 * static ring of SNAP_RING copies (a fault takes the next one; no lock, no allocation, no stdio); a marker that changes while
 * it is copied is marked torn, never waited for. fault_detail prints it after the jump.
 * Ages: a request's start is on CLOCK_MONOTONIC_COARSE (kept: it is read for every request; one tick is 4 ms at HZ=250), a
 * compile's on CLOCK_MONOTONIC; each is aged against the handler's reading of the same clock.
 */
#define SNAP_CALLS 24
#define SNAP_RING 8
typedef struct fault_snap {
    unsigned seq; /* 2k+1 while fault k (0-based) writes it, 2k+2 once written */
    int n, more;
    uint64_t at_coarse_ns;
    double at_mono;
    vkb_crash_call c[SNAP_CALLS];
} fault_snap;
static fault_snap snaps[SNAP_RING];
static unsigned snap_count;

static unsigned snap_take(void)
{
    unsigned k = __atomic_fetch_add(&snap_count, 1, __ATOMIC_RELAXED);
    fault_snap *s = &snaps[k % SNAP_RING];
    __atomic_store_n(&s->seq, 2 * k + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    s->at_coarse_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s->at_mono = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    int self = (int)syscall(SYS_gettid), more = 0;
    int n = vkb_srv_crash_snap(s->c, SNAP_CALLS, self, &more), nreq = n;
    for (int i = 0; i < WATCH_SLOTS; i++) {
        unsigned q1 = __atomic_load_n(&slot_seq[i], __ATOMIC_ACQUIRE);
        if (!__atomic_load_n(&slots[i].used, __ATOMIC_RELAXED)) continue;
        int tid = __atomic_load_n(&slots[i].tid, __ATOMIC_RELAXED);
        if (tid == self) continue; /* the faulting call's own */
        int listed = 0;            /* a connection thread's compile: its request is listed already */
        for (int j = 0; j < nreq && !listed; j++) listed = s->c[j].tid == tid;
        if (listed) continue;
        double start = slots[i].start;
        const char *call = slots[i].call, *ctx = slots[i].ctx;
        uint32_t dev = slots[i].device;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        unsigned q2 = __atomic_load_n(&slot_seq[i], __ATOMIC_RELAXED);
        if (n >= SNAP_CALLS) {
            more++;
            continue;
        }
        vkb_crash_call *o = &s->c[n++];
        o->tid = tid;
        o->pid = 0;
        o->cmd = 0;
        o->device = dev;
        o->since_ns = start > 0 ? (uint64_t)(start * 1e9) : 0;
        o->call = call;
        o->ctx = ctx;
        o->comm[0] = 0;
        o->torn = (q1 & 1) || q1 != q2;
    }
    s->n = n;
    s->more = more;
    __atomic_store_n(&s->seq, 2 * k + 2, __ATOMIC_RELEASE);
    return k + 1;
}

/* After the jump: the copy as one line, "vkbridge-server E   calls in flight at the fault: ...\n". */
static int snap_text(char *buf, size_t cap, unsigned snap)
{
    int n = snprintf(buf, cap, "vkbridge-server E   calls in flight at the fault: ");
    if (n < 0 || (size_t)n >= cap) return n < 0 ? 0 : (int)cap - 1;
#define SNAP_ADD(...)                                                                         \
    do {                                                                                      \
        if (n >= 0 && (size_t)n < cap) n += snprintf(buf + n, cap - (size_t)n, __VA_ARGS__); \
    } while (0)
    const fault_snap *s = snap ? &snaps[(snap - 1) % SNAP_RING] : NULL;
    unsigned seq = s ? __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) : 0;
    if (!s) SNAP_ADD("not copied");
    else if (seq != 2 * (snap - 1) + 2) SNAP_ADD("not kept (written over by a later fault's copy)");
    else if (!s->n && !s->more) SNAP_ADD("none");
    else {
        int reqs = 0;
        for (int i = 0; i < s->n; i++) {
            const vkb_crash_call *c = &s->c[i];
            if (c->cmd) {
                reqs++;
                SNAP_ADD("%s%s (pid %d, thread %d) %s %llu ms%s", i ? "; " : "", c->comm[0] ? c->comm : "?", c->pid, c->tid,
                         vkb_cmd_names[c->cmd - 1],
                         (unsigned long long)(s->at_coarse_ns > c->since_ns ? (s->at_coarse_ns - c->since_ns) / 1000000ull : 0),
                         c->torn ? " (changing as it was read)" : "");
            } else {
                double ms = c->since_ns ? (s->at_mono - c->since_ns / 1e9) * 1e3 : 0;
                SNAP_ADD("%scompile %s on device %u (thread %d, %s) %.0f ms%s", i ? "; " : "", c->call ? c->call : "?", c->device, c->tid,
                         c->ctx ? c->ctx : "the server's own", ms > 0 ? ms : 0, c->torn ? " (changing as it was read)" : "");
            }
        }
        if (s->more) SNAP_ADD("; %d more", s->more);
        if (reqs) SNAP_ADD(" (a request's age on the coarse clock: a few ms)");
    }
    SNAP_ADD("\n");
#undef SNAP_ADD
    if (n < 0) n = 0;
    if ((size_t)n >= cap) n = (int)cap - 1;
    return n;
}
/* The G925's normal compiles of Stray's shaders took < 2 s. Performance mode waits longer before
 * it gives a pipeline a fallback (a slow compile there is more likely to finish than to spin). */
#define WARN_S 5.0
#define TIMEOUT_SAFE_S 10.0
#define TIMEOUT_PERF_S 20.0
static double warn_s = WARN_S, timeout_s = TIMEOUT_SAFE_S;
static int watch_started, timeout_sig;
static mod_hash mods[MOD_BUCKETS];
static uint64_t stuck[STUCK_MAX][STAGES_MAX];
static uint32_t stuck_n[STUCK_MAX], nstuck;
static int stuck_mode[STUCK_MAX]; /* FB_*: what this pipeline gets instead */
/* Crash suspects (x= in the file; older builds read past it): 1 = in flight when the driver crashed on a
 * thread the guard cannot contain (written by the crash handler): its stand-in (a compute pipeline: a failed
 * one) at once, not asked of the driver in that run; 2 = given that in a run: a later run asks the driver
 * again once (written first, as every try); built, it leaves the list (m=5 t=1); 3 = crashed the driver again
 * while asked: the stand-in for good (t=RETRY_MAX). 0 = not from a crash. */
static unsigned char stuck_crash[STUCK_MAX];
static char crash_path[700]; /* <cache dir>/stuck-pipelines for the crash handler, set when the list is loaded */

enum { FB_FAIL = 0, FB_RELAXED = 1, FB_STANDIN = 2, FB_NOGS = 3, FB_LUT = 4, FB_SLOW = 5, FB_COUNT };
static const char *const fb_names[] = {"fails", "float_controls2 stripped", "constant-colour stand-in fragment shader",
                                       "pass-through geometry stage folded into the vertex shader",
                                       "neutral colour-grading LUT stand-in (UE4 CombineLUTs)", "built with a longer limit"};
/* A compute pipeline no fallback built is tried again in later runs (once per run), each time with
 * twice the limit (10 s in safe mode: 20, then 40), before it fails at once for good: a slow compile
 * is not a spinning one, and Unreal games draw black without it (Little Nightmares III on the
 * Dimensity 9500, 2026-10-22: three compute pipelines failed at once in every run after one slow
 * start). The try is written down before it starts, so one that hangs the driver is not repeated
 * for ever; one that builds is kept as FB_SLOW, built with that limit from then on. t=<tries>. */
#define RETRY_MAX 3
static int stuck_tries[STUCK_MAX];
static unsigned char stuck_retried[STUCK_MAX]; /* tried again in this run already */
static int retry_cut;                          /* a try of a listed pipeline ran out of time in this run */
static int retry_busy;                         /* a stand-in's try is running (they go one at a time: at most one interrupt) */
static __thread double tls_limit;              /* a retry's own limit (armed_compute) */
static __thread int tls_trying;                /* a listed pipeline is asked of the driver again (for the crash handler) */
/* This driver's compiler spins on UE4's colour-grading LUT shape, not just compiles it slowly: a LUT
 * pipeline's retry with twice the limit ran out of time too (a stuck-pipelines line m=4 t=2, and no
 * graphics stand-in ever built with a longer limit, m=5 t>=2). From then on every LUT pipeline gets
 * its stand-in at once: no further retries, no timeout for a variant not seen before. Stray on the
 * Mali-G925 r49p1 (a player log-11-10): each new CombineLUTs variant froze the game 10 s,
 * and each one's retry in the next run 20 s more - 7 of 7 retries spun to the end, none built.
 * lut_cut: a LUT pipeline ran out of time in this run; other LUT pipelines of this run get the
 * stand-in at once too (each is retried once in a later run until lut_spins is known). */
static int lut_spins, lut_spins_blocked, lut_cut;
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
    return vkb_spv_app_hash(code, size); /* FNV-1a; a module an erratum rewrote by the app's code */
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
    uint32_t needs = vkb_spv_caps(ci->pCode, ci->codeSize); /* the preamble only: before the lock */
    unsigned b = (unsigned)(((uintptr_t)m >> 4) * 2654435761u) % MOD_BUCKETS;
    pthread_mutex_lock(&watch_lock);
    for (unsigned i = 0; i < MOD_BUCKETS; i++) {
        mod_hash *e = &mods[(b + i) % MOD_BUCKETS];
        if (e->m && e->m != m && i < MOD_BUCKETS - 1) continue;
        e->m = m;
        e->hash = h;
        e->needs = needs;
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

/* What the stage's SPIR-V asks of the device (spirv_caps.h): an inline module scanned now, a module
 * by what was found when it was made; 0 if unknown. */
static uint32_t stage_needs(const VkPipelineShaderStageCreateInfo *st)
{
    for (const VkBaseInStructure *b = st->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *ci = (const VkShaderModuleCreateInfo *)b;
            return vkb_spv_caps(ci->pCode, ci->codeSize);
        }
    if (!st->module) return 0;
    unsigned b = (unsigned)(((uintptr_t)st->module >> 4) * 2654435761u) % MOD_BUCKETS;
    uint32_t needs = 0;
    pthread_mutex_lock(&watch_lock);
    for (unsigned i = 0; i < MOD_BUCKETS; i++) {
        mod_hash *e = &mods[(b + i) % MOD_BUCKETS];
        if (!e->m) break;
        if (e->m == st->module) {
            needs = e->needs;
            break;
        }
    }
    pthread_mutex_unlock(&watch_lock);
    return needs;
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
    if (vkb_cache_subdir("stuck-spirv") != 0) { /* the cache directory made again if it was removed */
        vkb_cache_write_failed("stuck-spirv", errno);
        return;
    }
    snprintf(path, sizeof(path), "%s/stuck-spirv/h-%016llx.spv", dir, (unsigned long long)h);
    if (access(path, F_OK) == 0) return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        vkb_cache_write_failed("stuck-spirv", errno);
        return;
    }
    int ok = fwrite(code, 1, size, f) == size;
    int err = errno;
    if (fclose(f) != 0 && ok) {
        ok = 0;
        err = errno;
    }
    if (!ok) {
        unlink(path); /* a short one would be taken for the whole module */
        vkb_cache_write_failed("stuck-spirv", err);
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
        /* m=5 t=1: built when the driver was asked again with the ordinary limit: no longer stuck */
        if (same == n && n) hit = stuck_mode[k] == FB_SLOW && stuck_tries[k] <= 1 ? 0 : stuck_mode[k] + 1;
    }
    pthread_mutex_unlock(&watch_lock);
    return hit;
}

/* For a failed compute pipeline due another try in this run (marked as tried), or a slow one: its
 * tries so far (the limit is the base doubled that many times); 0 otherwise. */
static int stuck_tries_take(const uint64_t *key, uint32_t n, int mode)
{
    int tries = 0;
    pthread_mutex_lock(&watch_lock);
    for (uint32_t k = 0; k < nstuck && !tries; k++) {
        if (stuck_n[k] != n || stuck_mode[k] != mode) continue;
        uint32_t same = 0;
        for (uint32_t i = 0; i < n; i++) same += stuck[k][i] == key[i];
        if (same != n || !n) continue;
        if (stuck_crash[k] == 1) break; /* the run after the crash: its stand-in, no try */
        if (mode == FB_FAIL && (stuck_retried[k] || stuck_tries[k] >= RETRY_MAX)) break;
        /* a graphics pipeline that got the LUT stand-in (UE4 CombineLUTs): one try in a later run, with twice the limit */
        if (mode == FB_LUT && (stuck_retried[k] || stuck_tries[k] >= 2)) break;
        /* the constant-colour stand-in draws wrong: up to the compute limit; one that ran out of time ends this run's */
        if (mode == FB_STANDIN && (stuck_retried[k] || stuck_tries[k] >= RETRY_MAX || __atomic_load_n(&retry_cut, __ATOMIC_RELAXED) ||
                                   __atomic_exchange_n(&retry_busy, 1, __ATOMIC_ACQ_REL)))
            break;
        stuck_retried[k] = 1;
        tries = stuck_tries[k] > 0 ? stuck_tries[k] : 1;
    }
    pthread_mutex_unlock(&watch_lock);
    return tries;
}

/* key: per stage, SPIR-V hash ^ entry/specialization hash. note: a comment for the file (NULL:
 * not written, as when loading it). */
/* Stuck graphics pipelines that no fallback built, asked of the driver again this run (once each) */
#define RETRIED_MAX 256
static uint64_t retried[RETRIED_MAX];
static uint32_t nretried;
static pthread_mutex_t retried_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tried_key(const uint64_t *key, uint32_t nh)
{
    uint64_t k = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < nh; i++) k = (k ^ key[i]) * 0x100000001b3ull;
    return k;
}
/* Stuck pipelines the driver built when asked again: the rest of this run, the driver as usual */
static uint64_t healed[RETRIED_MAX];
static uint32_t nhealed;
static int is_healed(const uint64_t *key, uint32_t nh)
{
    uint64_t k = tried_key(key, nh);
    int found = 0;
    pthread_mutex_lock(&retried_lock);
    for (uint32_t i = 0; i < nhealed && !found; i++) found = healed[i] == k;
    pthread_mutex_unlock(&retried_lock);
    return found;
}
static void heal(const uint64_t *key, uint32_t nh)
{
    uint64_t k = tried_key(key, nh);
    pthread_mutex_lock(&retried_lock);
    if (nhealed < RETRIED_MAX) healed[nhealed++] = k;
    pthread_mutex_unlock(&retried_lock);
}
static __thread int tls_retrying; /* inside the one retry: the stuck list is not read */
static __thread int tls_retry_cut; /* the watchdog cut the retry off: what came back is a fallback */

/* 1: not tried this run (now marked); a pipeline that got stuck in this run counts as tried */
static int retry_once(const uint64_t *key, uint32_t nh)
{
    uint64_t k = tried_key(key, nh);
    int fresh = 1;
    pthread_mutex_lock(&retried_lock);
    for (uint32_t i = 0; i < nretried && fresh; i++) fresh = retried[i] != k;
    if (fresh) {
        if (nretried < RETRIED_MAX) retried[nretried++] = k;
        else fresh = 0; /* past the cap: the fallbacks' answer stands */
    }
    pthread_mutex_unlock(&retried_lock);
    return fresh;
}

/* The crash-suspect state of a listed pipeline (stuck_crash), 0 if none */
static int crash_of(const uint64_t *key, uint32_t n)
{
    int x = 0;
    pthread_mutex_lock(&watch_lock);
    for (uint32_t k = 0; k < nstuck && n; k++) {
        if (stuck_n[k] != n) continue;
        uint32_t same = 0;
        for (uint32_t i = 0; i < n; i++) same += stuck[k][i] == key[i];
        if (same == n) {
            x = stuck_crash[k];
            break;
        }
    }
    pthread_mutex_unlock(&watch_lock);
    return x;
}

static void add_stuck_x(const uint64_t *key, uint32_t n, int mode, int tries, int crash, const char *note);
static void add_stuck(const uint64_t *key, uint32_t n, int mode, int tries, const char *note)
{
    add_stuck_x(key, n, mode, tries, 0, note);
}

/* A crash suspect (x=1) given its stand-in or failure in this run: x=2 (written once), a later run asks again */
static void crash_given(const uint64_t *key, uint32_t n, int mode, const uint64_t *h, const uint32_t *stg, const char *call)
{
    int first = 0;
    pthread_mutex_lock(&watch_lock);
    for (uint32_t k = 0; k < nstuck && n; k++) {
        if (stuck_n[k] != n) continue;
        uint32_t same = 0;
        for (uint32_t i = 0; i < n; i++) same += stuck[k][i] == key[i];
        if (same == n) {
            if (stuck_crash[k] == 1) stuck_crash[k] = 2, stuck_retried[k] = 1, first = 1;
            break;
        }
    }
    pthread_mutex_unlock(&watch_lock);
    if (!first) return;
    char st[400], note[480];
    stages_text(h, stg, n, st, sizeof(st));
    snprintf(note, sizeof(note), "%s, stages %s, given at once after the driver's crash; asked again in a later run", call, st);
    add_stuck_x(key, n, mode, 1, 2, note);
}

static void add_stuck_x(const uint64_t *key, uint32_t n, int mode, int tries, int crash, const char *note)
{
    if (!n) return;
    /* the device's cache wedged the driver (a contained crash, then this): not this pipeline's doing, nothing noted
     * (server_pcache.c set the cache aside; the next run builds it as any other) */
    if (note && vkb_pcache_aside(vkb_srv_current_table())) {
        static int told;
        if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED))
            VKB_WARN("compile watchdog: pipelines that time out on a device whose pipeline cache wedged the driver are not noted as stuck (%s)", note);
        return;
    }
    if (note) retry_once(key, n); /* stuck in this run: not asked again before the next */
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
        stuck_retried[nstuck] = note != NULL; /* one that failed in this run waits for the next */
        stuck_n[nstuck++] = n;
    }
    if (k < (int)nstuck) stuck_mode[k] = mode, stuck_tries[k] = tries, stuck_crash[k] = (unsigned char)crash; /* the last word wins (a later line in the file too) */
    pthread_mutex_unlock(&watch_lock);
    const char *dir = vkb_pcache_dir();
    if (!note || !dir) return;
    char path[700];
    snprintf(path, sizeof(path), "%s/stuck-pipelines", dir);
    int gone = access(path, F_OK) != 0;
    FILE *f = fopen(path, "a");
    if (!f && errno == ENOENT && vkb_cache_dir_ensure()) f = fopen(path, "a"); /* the cache directory was removed */
    if (!f) {
        vkb_cache_write_failed("stuck-pipelines", errno);
        return;
    }
    if (gone) {
        /* A new file: what this run knew from the old one (removed with the cache directory, or
         * never written) goes back in, so the next start does not wait on those compiles again. */
        pthread_mutex_lock(&watch_lock);
        for (int j = 0; j < (int)nstuck; j++) {
            if (j == k) continue;
            for (uint32_t i = 0; i < stuck_n[j]; i++) fprintf(f, "%sk-%016llx", i ? "," : "", (unsigned long long)stuck[j][i]);
            fprintf(f, " m=%d v=%d t=%d", stuck_mode[j], STUCK_VERSION, stuck_tries[j]);
            if (stuck_crash[j]) fprintf(f, " x=%d", stuck_crash[j]);
            fprintf(f, " # %s: known before the file was removed\n", fb_names[stuck_mode[j]]);
        }
        pthread_mutex_unlock(&watch_lock);
    }
    for (uint32_t i = 0; i < n; i++) fprintf(f, "%sk-%016llx", i ? "," : "", (unsigned long long)key[i]);
    fprintf(f, " m=%d v=%d t=%d", mode, STUCK_VERSION, tries);
    if (crash) fprintf(f, " x=%d", crash);
    fprintf(f, " # %s: %s\n", fb_names[mode], note);
    int ok = fflush(f) == 0 && !ferror(f);
    int e = errno;
    if (fclose(f) != 0 && ok) {
        ok = 0;
        e = errno;
    }
    if (!ok) vkb_cache_write_failed("stuck-pipelines", e);
}

static void load_stuck(void)
{
    const char *dir = vkb_pcache_dir();
    if (!dir) return;
    char path[700], line[700];
    snprintf(path, sizeof(path), "%s/stuck-pipelines", dir);
    snprintf(crash_path, sizeof(crash_path), "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *m = strstr(line, "m="), *ver = strstr(line, "v="), *t = strstr(line, "t="), *x = strstr(line, " x=");
        if (!m || !ver || atoi(ver + 2) < STUCK_VERSION) continue; /* an older fallback list chose it: tried again */
        int mode = atoi(m + 2), tries = t ? atoi(t + 2) : 1, crash = x ? atoi(x + 3) : 0;
        if (crash < 0 || crash > 3) crash = 0;
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
        add_stuck_x(h, n, mode, tries, crash, NULL);
    }
    fclose(f);
    for (uint32_t k = 0; k < nstuck; k++) {
        if (stuck_crash[k] != 1 && stuck_crash[k] != 3) continue;
        char keys[200];
        int o = 0;
        keys[0] = 0;
        for (uint32_t i = 0; i < stuck_n[k] && o < (int)sizeof(keys) - 24; i++) o += snprintf(keys + o, sizeof(keys) - (size_t)o, "%sk-%016llx", i ? "," : "", (unsigned long long)stuck[k][i]);
        const char *what = stuck_mode[k] == FB_FAIL ? "fails at once" : "gets its stand-in at once";
        if (stuck_crash[k] == 1)
            VKB_WARN("pipeline %s was being compiled when the driver crashed in an earlier run: it %s in this run (asked of the driver again in a later run)", keys, what);
        else
            VKB_WARN("pipeline %s crashed the driver again when it was asked again: it %s for good (delete stuck-pipelines to retry it)", keys, what);
    }
    uint32_t unbuilt = 0, lut_spun = 0;
    for (uint32_t k = 0; k < nstuck; k++) {
        unbuilt += stuck_mode[k] != FB_SLOW; /* m=5: built, by their longer limit or the ordinary one */
        if (stuck_n[k] > 1 && stuck_mode[k] == FB_LUT && stuck_tries[k] >= 2) lut_spun++;
        if (stuck_n[k] > 1 && stuck_mode[k] == FB_SLOW && stuck_tries[k] >= 2) lut_spins_blocked = 1; /* a stand-in's retry built: slow, not spinning */
    }
    if (lut_spun && !lut_spins_blocked) {
        lut_spins = 1;
        VKB_WARN("UE4 colour-grading LUT pipelines get their neutral stand-in at once: this driver's compiler did not finish %u of "
                 "them with twice the limit either, none built (delete %s to compile them again)", lut_spun, path);
    }
    if (unbuilt) VKB_WARN("%u pipeline(s) the driver never finished compiling get their fallback at once (%s; delete it to retry them)", unbuilt, path);
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

/* glslc -O: layout(location = 0) out vec4 o; void main { o = vec4(0.5, 0.5, 0.5, 1.0); } */
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
    a->code = 0;
    a->pc = 0;
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
                         lim > 0 ? "" : " - VKBRIDGE_COMPILE_TIMEOUT_S=0: not interrupted");
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

/* preset: with the game's preset on a client's thread (a device's own limit), else server-wide */
static double timeout_for(int mode, const char **why, int preset)
{
    int src = VKB_ENV_SRC_NONE;
    const char *v = preset ? vkb_srv_env_src("VKBRIDGE_COMPILE_TIMEOUT_S", &src) : vkb_srv_env_global("VKBRIDGE_COMPILE_TIMEOUT_S");
    if (v && *v) {
        if (why)
            *why = src == VKB_ENV_SRC_PRESET_FILE      ? "VKBRIDGE_COMPILE_TIMEOUT_S, the game's preset file"
                   : src == VKB_ENV_SRC_PRESET_BUILTIN ? "VKBRIDGE_COMPILE_TIMEOUT_S, the game's built-in preset"
                                                       : "VKBRIDGE_COMPILE_TIMEOUT_S";
        return atof(v);
    }
    if (why) *why = mode == VKB_MODE_PERFORMANCE ? "performance mode" : "safe mode";
    return mode == VKB_MODE_PERFORMANCE ? TIMEOUT_PERF_S : TIMEOUT_SAFE_S;
}

double vkb_guard_timeout_s(int mode, const char **why) { return timeout_for(mode, why, 0); }

/* The switches again at each device (and probe): the mode or <cache dir>/env may have changed */
static void watch_env(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_COMPILE_WARN_S");
    warn_s = v && *v ? atof(v) : WARN_S;
    const char *why;
    double t = vkb_guard_timeout_s(vkb_mode(), &why);
    if (watch_started && t != timeout_s) VKB_INFO("compile watchdog: interrupt after %.0f s now (%s)", t, why);
    timeout_s = t;
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
    VKB_INFO("compile watchdog: warn after %.0f s, interrupt after %.0f s (%s; signal %d)", warn_s, timeout_s, why, timeout_sig);
}

static watch_slot *watch_begin(guard_arm *arm, const char *call, uint32_t device, double limit, const uint64_t *h, const uint32_t *stage,
                               uint32_t n, const VkPipelineShaderStageCreateInfo *stages, uint32_t nst, const uint64_t *key, int pipe)
{
    watch_slot *w = NULL;
    pthread_mutex_lock(&watch_lock);
    for (int i = 0; i < WATCH_SLOTS && !w; i++)
        if (!slots[i].used) w = &slots[i];
    unsigned *seq = w ? &slot_seq[w - slots] : NULL;
    if (w) {
        if (!tls_tid) tls_tid = (int)syscall(SYS_gettid);
        __atomic_store_n(seq, *seq + 1, __ATOMIC_RELAXED); /* odd: being written (snap_take) */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        memset(w, 0, sizeof(*w));
        w->tid = tls_tid;
        w->ctx = vkb_srv_current_cmd;
        w->used = 1;
        w->thread = pthread_self();
        w->arm = arm;
        w->start = mono_s();
        w->call = call;
        w->device = device;
        w->limit = limit;
        w->nstages = n < STAGES_MAX ? n : STAGES_MAX;
        if (h) memcpy(w->hash, h, w->nstages * sizeof(*h));
        if (stage) memcpy(w->stage, stage, w->nstages * sizeof(*stage));
        if (key && pipe) {
            memcpy(w->key, key, w->nstages * sizeof(*key));
            w->trying = tls_trying || tls_limit > 0;
            __atomic_store_n(&w->pipe, pipe, __ATOMIC_RELEASE); /* last: the handler reads without the lock */
        }
        for (uint32_t i = 0; stages && i < w->nstages && i < nst; i++)
            for (const VkBaseInStructure *b = stages[i].pNext; b; b = b->pNext)
                if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                    w->code[i] = ((const VkShaderModuleCreateInfo *)b)->pCode;
                    w->size[i] = ((const VkShaderModuleCreateInfo *)b)->codeSize;
                }
        __atomic_store_n(seq, *seq + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&watch_lock);
    return w;
}

/*
 * From the crash handler (server_main.c on_crash), for a fault the guard did not contain: the compiles in flight
 * go in stuck-pipelines as crash suspects (x=1), so the next start gives them their stand-in at once instead of
 * dying at the same spot. a player log (Street Fighter 6, Mali-G1-Ultra r54p1) and a player log (POCO X7 Pro): SIGSEGV
 * on the driver's own "mali-compiler" thread while the game's pipelines compiled; the arm is per thread, so
 * nothing could catch it. Which faults count: SIGSEGV/SIGBUS/SIGILL/SIGFPE with the pc outside the server's own
 * code (in that player log libGLES_mali.so; a fault in our code is our bug, not the compile's), on a thread that is
 * not inside some other Vulkan call (a submit faulting in the driver is not a compile's doing); the faulting
 * thread's own compile (containment off past the process limit) counts alone. Nothing but plain reads of the
 * slots and open/write/close: no lock (the dying thread may hold none of ours, another may hold watch_lock),
 * no fsync (the page cache outlives the process). A slot read half-written gives one wrong suspect, which the
 * next runs clear (one stand-in run, one try). Nothing is written by a run that does not crash, and a kill
 * (SIGKILL: low memory, force stop) runs no handler: those leave no suspect.
 */
void vkb_guard_crash_suspects(int sig, const char *lib, int in_call)
{
    if (sig != SIGSEGV && sig != SIGBUS && sig != SIGILL && sig != SIGFPE) return;
    if (!crash_path[0] || guard_off > 0) return;
    if (lib && strstr(lib, "vkbridge")) return;
    pthread_t self = pthread_self();
    int own = -1;
    for (int i = 0; i < WATCH_SLOTS; i++)
        if (slots[i].used && __atomic_load_n(&slots[i].pipe, __ATOMIC_ACQUIRE) && pthread_equal(slots[i].thread, self)) own = i;
    if (own < 0 && in_call) return;
    int fd = -1, marked = 0, again = 0;
    char said[400];
    int so = 0;
    said[0] = 0;
    for (int i = 0; i < WATCH_SLOTS; i++) {
        watch_slot *w = &slots[i];
        if (own >= 0 && i != own) continue;
        int pipe = __atomic_load_n(&w->pipe, __ATOMIC_ACQUIRE);
        if (!w->used || !pipe) continue;
        uint32_t n = w->nstages < STAGES_MAX ? w->nstages : STAGES_MAX;
        int zero = !n;
        for (uint32_t k = 0; k < n; k++) zero |= !w->key[k];
        if (zero) continue; /* a stage whose SPIR-V is not known: the list cannot match it */
        int confirmed = w->trying;
        int mode = pipe == 1 ? FB_STANDIN : FB_FAIL;
        char line[900];
        int o = 0;
        for (uint32_t k = 0; k < n && o < (int)sizeof(line) - 24; k++) o += snprintf(line + o, sizeof(line) - (size_t)o, "%sk-%016llx", k ? "," : "", (unsigned long long)w->key[k]);
        char st[400];
        stages_text(w->hash, w->stage, n, st, sizeof(st));
        if (o > 0 && o < (int)sizeof(line))
            o += snprintf(line + o, sizeof(line) - (size_t)o, " m=%d v=%d t=%d x=%d # %s: %s when the driver crashed (signal %d, pc in %s), %s, stages %s\n", mode,
                          STUCK_VERSION, confirmed ? RETRY_MAX : 1, confirmed ? 3 : 1, fb_names[mode], confirmed ? "asked again and in flight" : "in flight", sig,
                          lib ? lib : "?", pipe == 1 ? "vkCreateGraphicsPipelines" : "vkCreateComputePipelines", st);
        if (o <= 0 || o >= (int)sizeof(line)) continue;
        if (fd < 0) fd = open(crash_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) break;
        if (write(fd, line, (size_t)o) != o) break;
        marked++;
        again += confirmed;
        if (so < (int)sizeof(said) - 40) so += snprintf(said + so, sizeof(said) - (size_t)so, "%s%s", so ? "; " : "", st);
    }
    if (fd >= 0) close(fd);
    if (!marked) return;
    char msg[700];
    int k = snprintf(msg, sizeof(msg),
                     "vkbridge-server E   %d pipeline compile(s) in flight at this crash marked as suspects in stuck-pipelines: the next start "
                     "gives them the stand-in at once%s (stages %s)\n",
                     marked, again ? "; one asked again after an earlier crash keeps it for good" : "", said);
    vkb_crash_log_raw(msg, k > 0 && k < (int)sizeof(msg) ? (size_t)k : sizeof(msg) - 1);
}

/* Before the arm goes out of scope: a signal already on its way finds timed_out unset on the next
 * arm, or no arm, and returns. */
static void watch_end(watch_slot *w)
{
    if (!w) return;
    pthread_mutex_lock(&watch_lock);
    unsigned *seq = &slot_seq[w - slots];
    __atomic_store_n(seq, *seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    w->pipe = 0;
    w->used = 0;
    __atomic_store_n(seq, *seq + 1, __ATOMIC_RELEASE);
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
    /* after a contained crash on this device: the driver may be wedged by its pipeline cache's data */
    if (__atomic_load_n(&g->crashes, __ATOMIC_RELAXED) > 0) vkb_pcache_wedged(g->t, call);
}

static void remember(const char *call, const uint64_t *h, const uint64_t *key, const uint32_t *stage, uint32_t nh, int mode)
{
    char st[400], note[480];
    stages_text(h, stage, nh, st, sizeof(st));
    snprintf(note, sizeof(note), "%s, stages %s", call, st);
    add_stuck(key, nh, mode, 1, note);
}

/* One guarded driver call; *timed: interrupted by the compile watchdog (the call failed). */
__attribute__((noinline)) static VkResult armed_gfx(vkb_guard *g, VkDevice device, VkPipelineCache cache, uint32_t n,
                                                    const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *a, VkPipeline *out,
                                                    const uint64_t *h, const uint32_t *stg, uint32_t nh, int *timed, const uint64_t *key)
{
    guard_arm arm;
    arm.prev = tls_arm;
    arm.base = 0;
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
    w = watch_begin(&arm, "vkCreateGraphicsPipelines", g->t->id, tls_limit > 0 ? tls_limit : g->limit, h, stg, nh, n ? infos[0].pStages : NULL, n ? infos[0].stageCount : 0, key, 1);
    for (uint32_t i = 0; i < n; i++) test_fault(infos[i].pStages, infos[i].stageCount, sizeof(*infos[i].pStages));
    VkResult r = g->gfx(device, cache, n, infos, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    /* the loss record's pipeline table (server_lostlog.c): the stages' hashes, already made */
    for (uint32_t i = 0; r == VK_SUCCESS && out && i < n; i++) vkb_ll_pipeline(out[i], h, stg, n == 1 ? nh : 0, 0);
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
    VkResult r = armed_gfx(g, device, cache, 1, &ci, a, out, h, stg, nh, timed, NULL);
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

/* ------------------------------------------------------------------ the capability check */

/*
 * A pipeline whose SPIR-V asks for what the driver lacks never reaches the driver's compiler, which
 * has died on such input: ULTRAKILL's Unity vertex shader (gl_ClipDistance in gl_PerVertex, Mali r44p1
 * without shaderClipDistance: SIGSEGV at 0x40) and No Man's Sky's glslang vertex shaders (a player log G925
 * r49p1: 0x18 x3, a player log G615 r44p1: 0x40 x3, the device poisoned, then the server died in a draw).
 * Both times the bridge's clip rewrite (spirv_clip.c) had not taken the built-in out, and nothing
 * stopped the rest. The check sits here, the last layer before the driver: what reaches it is what the
 * emulations made of the program's pipeline, so anything an emulation takes care of never gets here.
 *
 * Decided at the device's creation (vkb_guard_device_init): what the driver lacks of what SPIR-V can
 * ask (its VkPhysicalDeviceFeatures; VKBRIDGE_FAKE_MISSING=clip/cull imitates it on a host), and the
 * dynamic states the driver gave no entry point to set (its vkCmdSetCullMode etc. NULL). Per pipeline:
 * each stage's needs, found when its module was made (spirv_caps.c, the module's preamble). Nothing in
 * any draw or submit.
 *   - A clip or cull distance built-in or capability on a driver without the feature: the pipeline is
 *     refused (VK_ERROR_INITIALIZATION_FAILED, what a contained compile crash answers: DXVK and
 *     vkd3d-proton log a failed pipeline and skip its draws) and said loudly. No working session in the
 *     players' logs shows one (the rewrite's "could not be demoted" is only in NMS's two, both crashed).
 *   - Anything else the driver lacks (64-bit or 16-bit integers, 64-bit floats, cube arrays, sample
 *     shading, storage image formats ..., a dynamic state with no setter): said once per kind and passed
 *     on as before - no crash of a driver is known for those, and a driver that takes them anyway draws.
 * VKBRIDGE_CAP_GATE=strict refuses those too; =log refuses nothing (says what would be); =0 turns the
 * check off. VKBRIDGE_NO_CRASH_GUARD=1 turns it off with the rest of this file.
 */
static int cap_gate = -1; /* 0 off, 1 refuse clip/cull, 2 strict, 3 log only */

static void cap_env(void)
{
    if (cap_gate >= 0) return;
    const char *e = vkb_srv_env_global("VKBRIDGE_CAP_GATE");
    int v = 1;
    if (e && *e == '0') v = 0;
    else if (e && !strcmp(e, "strict")) v = 2;
    else if (e && !strcmp(e, "log")) v = 3;
    if (v != 1) VKB_INFO("capability check: %s (VKBRIDGE_CAP_GATE=%s)", v == 0 ? "off" : v == 2 ? "strict: every lack refused" : "log only", e);
    cap_gate = v;
}

static int cap_active(const vkb_guard *g) { return cap_gate > 0 && (g->lacks || g->ndyn); }

static const struct {
    VkDynamicState state;
    size_t setter; /* offsetof(vkb_dispatch, ...) */
    const char *name;
} cap_dyn[] = {
#define D(s, f) {VK_DYNAMIC_STATE_##s, offsetof(vkb_dispatch, f), #f}
    D(CULL_MODE, vkCmdSetCullMode), D(FRONT_FACE, vkCmdSetFrontFace), D(PRIMITIVE_TOPOLOGY, vkCmdSetPrimitiveTopology),
    D(VIEWPORT_WITH_COUNT, vkCmdSetViewportWithCount), D(SCISSOR_WITH_COUNT, vkCmdSetScissorWithCount),
    D(VERTEX_INPUT_BINDING_STRIDE, vkCmdBindVertexBuffers2), D(DEPTH_TEST_ENABLE, vkCmdSetDepthTestEnable),
    D(DEPTH_WRITE_ENABLE, vkCmdSetDepthWriteEnable), D(DEPTH_COMPARE_OP, vkCmdSetDepthCompareOp),
    D(DEPTH_BOUNDS_TEST_ENABLE, vkCmdSetDepthBoundsTestEnable), D(STENCIL_TEST_ENABLE, vkCmdSetStencilTestEnable),
    D(STENCIL_OP, vkCmdSetStencilOp), D(RASTERIZER_DISCARD_ENABLE, vkCmdSetRasterizerDiscardEnable),
    D(DEPTH_BIAS_ENABLE, vkCmdSetDepthBiasEnable), D(PRIMITIVE_RESTART_ENABLE, vkCmdSetPrimitiveRestartEnable),
    D(PATCH_CONTROL_POINTS_EXT, vkCmdSetPatchControlPointsEXT), D(LOGIC_OP_EXT, vkCmdSetLogicOpEXT),
    D(VERTEX_INPUT_EXT, vkCmdSetVertexInputEXT),
#undef D
};

static void cap_device(vkb_guard *g, vkb_srv_table *t)
{
    cap_env();
    if (!cap_gate) return;
    const vkb_srv_table *inst = t->parent;
    if (inst && inst->real.vkGetPhysicalDeviceFeatures && t->physical) {
        VkPhysicalDeviceFeatures f;
        memset(&f, 0, sizeof(f));
        inst->real.vkGetPhysicalDeviceFeatures(t->physical, &f);
        if (!f.shaderClipDistance || vkb_emu_faked("clip")) g->lacks |= VKB_SPVCAP_CLIP;
        if (!f.shaderCullDistance || vkb_emu_faked("cull")) g->lacks |= VKB_SPVCAP_CULL;
        if (!f.shaderFloat64) g->lacks |= VKB_SPVCAP_F64;
        if (!f.shaderInt64) g->lacks |= VKB_SPVCAP_I64;
        if (!f.shaderInt16) g->lacks |= VKB_SPVCAP_I16;
        if (!f.geometryShader) g->lacks |= VKB_SPVCAP_GEOM;
        if (!f.tessellationShader) g->lacks |= VKB_SPVCAP_TESS;
        if (!f.imageCubeArray) g->lacks |= VKB_SPVCAP_CUBE_ARRAY;
        if (!f.sampleRateShading) g->lacks |= VKB_SPVCAP_SAMPLE_RATE;
        if (!f.shaderImageGatherExtended) g->lacks |= VKB_SPVCAP_GATHER_EXT;
        if (!f.shaderStorageImageMultisample) g->lacks |= VKB_SPVCAP_MS_STORAGE;
        if (!f.shaderStorageImageExtendedFormats) g->lacks |= VKB_SPVCAP_EXT_FORMATS;
        if (!f.shaderStorageImageReadWithoutFormat) g->lacks |= VKB_SPVCAP_READ_NOFMT;
        if (!f.shaderStorageImageWriteWithoutFormat) g->lacks |= VKB_SPVCAP_WRITE_NOFMT;
    }
    g->refuse = cap_gate == 3 ? 0 : cap_gate == 2 ? g->lacks : g->lacks & (VKB_SPVCAP_CLIP | VKB_SPVCAP_CULL);
    for (size_t i = 0; i < sizeof(cap_dyn) / sizeof(cap_dyn[0]) && g->ndyn < sizeof(g->dyn) / sizeof(g->dyn[0]); i++)
        if (!*(void *const *)((const char *)&t->real + cap_dyn[i].setter)) g->dyn[g->ndyn++] = cap_dyn[i].state;
    if (g->lacks) {
        char txt[600];
        vkb_spv_caps_text(g->lacks, txt, sizeof(txt));
        VKB_INFO("capability check: device %u's driver lacks %s; %s", t->id, txt,
                 g->refuse ? "a pipeline that still asks for clip/cull distances at the driver is refused" : "said, not refused");
    }
}

static const char *cap_dyn_name(VkDynamicState d)
{
    for (size_t i = 0; i < sizeof(cap_dyn) / sizeof(cap_dyn[0]); i++)
        if (cap_dyn[i].state == d) return cap_dyn[i].name;
    return "?";
}

/* 1 when the pipeline of these stages (and dynamic states) is refused; says what it lacks (call NULL: quiet) */
static int cap_refuses(vkb_guard *g, const char *call, const VkPipelineShaderStageCreateInfo *stages, uint32_t n, size_t stride,
                       const VkPipelineDynamicStateCreateInfo *ds)
{
    static uint32_t said_bits, said_dyn; /* the kinds passed on that were said already (once per process) */
    static uint64_t refused;
    int refuse = 0;
    for (uint32_t i = 0; i < n; i++) {
        const VkPipelineShaderStageCreateInfo *st = (const VkPipelineShaderStageCreateInfo *)((const char *)stages + i * stride);
        uint32_t miss = stage_needs(st) & g->lacks;
        if (!miss) continue;
        uint32_t bad = miss & g->refuse;
        if (bad) refuse = 1;
        if (!call) continue;
        uint32_t fresh = miss & ~bad & ~__atomic_fetch_or(&said_bits, miss & ~bad, __ATOMIC_RELAXED);
        uint64_t k = bad ? __atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED) : 0;
        if (!fresh && !(bad && (k <= 8 || !(k & (k - 1))))) continue;
        char txt[600];
        vkb_spv_caps_text(bad ? bad : fresh, txt, sizeof(txt));
        const vkb_srv_proc *p = g->t->proc;
        if (bad)
            VKB_ERR("capability check: %s on device %u (%s, pid %d): stage 0x%x (SPIR-V h-%016llx) asks for %s, which the driver lacks "
                    "and no emulation took out - refused before the driver (VK_ERROR_INITIALIZATION_FAILED): Mali's compiler died on "
                    "this (ULTRAKILL, No Man's Sky); %llu refused so far",
                    call, g->t->id, p && p->comm[0] ? p->comm : "?", p ? p->pid : 0, (unsigned)st->stage, (unsigned long long)code_hash(st),
                    txt, (unsigned long long)k);
        else
            VKB_WARN("capability check: %s on device %u (%s, pid %d): stage 0x%x (SPIR-V h-%016llx) asks for %s, which the driver lacks - "
                     "passed on as before (no driver crash known for it; VKBRIDGE_CAP_GATE=strict refuses it; said once)",
                     call, g->t->id, p && p->comm[0] ? p->comm : "?", p ? p->pid : 0, (unsigned)st->stage, (unsigned long long)code_hash(st), txt);
    }
    for (uint32_t i = 0; ds && g->ndyn && i < ds->dynamicStateCount; i++)
        for (uint32_t k = 0; k < g->ndyn; k++) {
            if (ds->pDynamicStates[i] != g->dyn[k]) continue;
            if (cap_gate == 2) refuse = 1;
            uint32_t bit = 1u << (k & 31);
            if (call && (cap_gate == 2 || !(__atomic_fetch_or(&said_dyn, bit, __ATOMIC_RELAXED) & bit)))
                VKB_WARN("capability check: %s on device %u: dynamic state %d asked for, the driver gave no %s to set it - %s",
                         call, g->t->id, (int)g->dyn[k], cap_dyn_name(g->dyn[k]),
                         cap_gate == 2 ? "refused (VKBRIDGE_CAP_GATE=strict)" : "passed on as before (said once)");
        }
    return refuse;
}

static VkResult gfx_guarded(VkDevice device, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *infos,
                            const VkAllocationCallbacks *a, VkPipeline *out);
static VkResult compute_guarded(VkDevice device, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *infos,
                                const VkAllocationCallbacks *a, VkPipeline *out);

static VkResult gfx_guarded(VkDevice device, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *infos,
                            const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_guard *g = find(device);
    if (!g) return VK_ERROR_INITIALIZATION_FAILED;
    if (g->poisoned) {
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_DEVICE_LOST;
    }
    if (cap_active(g)) {
        unsigned char small[16], *ref = n <= 16 ? small : calloc(n, 1);
        uint32_t nref = 0;
        for (uint32_t i = 0; ref && i < n; i++)
            nref += ref[i] = (unsigned char)cap_refuses(g, "vkCreateGraphicsPipelines", infos[i].pStages, infos[i].stageCount,
                                                        sizeof(*infos[i].pStages), infos[i].pDynamicState);
        if (nref) {
            /* the others are built one by one, as asked; the refused get null handles */
            for (uint32_t i = 0; i < n; i++)
                if (ref[i]) out[i] = VK_NULL_HANDLE;
                else if (gfx_guarded(device, cache, 1, &infos[i], a, &out[i]) != VK_SUCCESS) out[i] = VK_NULL_HANDLE;
            if (ref != small) free(ref);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        if (ref != small) free(ref);
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
    int known = tls_retrying || is_healed(key, nh) ? 0 : is_stuck(key, nh);
    if (known) {
        int mode = known - 1;
        /* UE4's CombineLUTs (a colour-grading LUT) that timed out in an earlier run got its neutral stand-in, and
         * the stand-in stayed for ever (a player log, Sifu: two 10 s timeouts, no colour grading in any game after
         * that). As a compute pipeline that did not finish is (above), it is compiled again once in a later run
         * with twice the limit, written down before it starts (a compile that hangs is not repeated): built, it
         * is kept as "built with a longer limit" (m=5) and built with that limit from then on; not built, the
         * stand-in as before (m=4, t=2: no third try). */
        double base = g->limit > 0 ? g->limit : g->limit < 0 ? 0 : timeout_s;
        int slow = mode == FB_SLOW;
        /* in flight when the driver crashed on its own thread (vkb_guard_crash_suspects): 1 its stand-in, 2 one try */
        int cx = crash_of(key, nh);
        int lut_known = mode == FB_LUT && __atomic_load_n(&lut_spins, __ATOMIC_ACQUIRE);
        int tries = (mode == FB_LUT || mode == FB_STANDIN || slow) && !lut_known && n == 1 && (base > 0 || cx) && !tls_retrying
                        ? stuck_tries_take(key, nh, mode) : 0;
        if (tries) {
            int step = slow ? tries : tries + 1;
            double lim = base * (double)(1 << (step - 1)), t0 = mono_s();
            char stx[400], note[480];
            stages_text(h, stg, nh, stx, sizeof(stx));
            if (cx && !slow) {
                VKB_WARN("pipeline (stages %s) was in flight when the driver crashed in an earlier run: asked of the driver again (try %d of %d)",
                         stx, step, RETRY_MAX);
                snprintf(note, sizeof(note), "vkCreateGraphicsPipelines, stages %s, try %d after the driver's crash", stx, step);
                add_stuck_x(key, nh, mode, step, cx, note); /* first: a try that crashes again is not repeated for ever */
            } else if (!slow) {
                if (mode == FB_LUT)
                    VKB_WARN("UE4 colour-grading LUT pipeline (stages %s) got its neutral stand-in in an earlier run: compiled again with %.0f s",
                             stx, lim);
                else
                    VKB_WARN("pipeline (stages %s) got the constant-colour stand-in in an earlier run: compiled again with %.0f s (try %d of %d)",
                             stx, lim, step, RETRY_MAX);
                snprintf(note, sizeof(note), "vkCreateGraphicsPipelines, stages %s, try %d with %.0f s", stx, step, lim);
                add_stuck(key, nh, mode, step, note); /* first: a try that hangs is not repeated */
            }
            tls_limit = lim;
            tls_trying = 1;
            r = armed_gfx(g, device, cache, 1, infos, a, out, h, stg, nh, &timed, key);
            tls_trying = 0;
            tls_limit = 0;
            if (mode == FB_STANDIN) __atomic_store_n(&retry_busy, 0, __ATOMIC_RELEASE);
            if (r == VK_SUCCESS && cx && !slow) {
                VKB_WARN("  built fine, in %.1f s: no longer a crash suspect, off the list (its own fragment shader from now on)", mono_s() - t0);
                snprintf(note, sizeof(note), "vkCreateGraphicsPipelines, stages %s, built after the crash", stx);
                add_stuck(key, nh, FB_SLOW, 1, note); /* m=5 t=1: not stuck (is_stuck) */
                return r;
            }
            if (r == VK_SUCCESS) {
                if (!slow) {
                    VKB_WARN("  built this time, in %.1f s: built with %.0f s from now on, with its %s", mono_s() - t0, lim,
                             mode == FB_LUT ? "colour grading" : "own fragment shader");
                    snprintf(note, sizeof(note), "vkCreateGraphicsPipelines, stages %s, %.1f s of %.0f s", stx, mono_s() - t0, lim);
                    add_stuck(key, nh, FB_SLOW, step, note);
                    if (mode == FB_LUT) __atomic_store_n(&lut_spins_blocked, 1, __ATOMIC_RELAXED); /* slow here, not spinning */
                }
                return r;
            }
            VKB_ERR("  %s with %.0f s: the stand-in again", timed ? "timed out" : "failed", lim);
            if (timed) __atomic_store_n(&retry_cut, 1, __ATOMIC_RELAXED);
            if (timed && mode == FB_LUT && !__atomic_load_n(&lut_spins_blocked, __ATOMIC_RELAXED) &&
                !__atomic_exchange_n(&lut_spins, 1, __ATOMIC_ACQ_REL))
                VKB_WARN("  this driver's compiler spins on the UE4 LUT shape: every LUT pipeline gets its stand-in at once from now on");
            if (slow) {
                /* m=5 came from one of the two: the LUT's own stand-in, or the constant colour */
                mode = is_lut_pipeline(infos, h, nh) ? FB_LUT : FB_STANDIN;
                snprintf(note, sizeof(note), "vkCreateGraphicsPipelines, stages %s, timed out with %.0f s", stx, lim);
                add_stuck(key, nh, mode, mode == FB_LUT ? 2 : step, note);
            }
        }
        r = VK_ERROR_INITIALIZATION_FAILED;
        if (cx == 1) crash_given(key, nh, mode, h, stg, "vkCreateGraphicsPipelines");
        if (mode != FB_FAIL && n == 1) {
            VKB_ONCE(VKB_LOG_WARN, "a pipeline the driver never finished compiling was asked for again: its fallback at once (logged once)");
            r = try_fallback(g, device, cache, infos, a, out, h, stg, nh, mode, &timed);
        }
        if (r == VK_SUCCESS) return r;
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        /* a crash suspect is asked of the driver only by its one written-down try above, never again here */
        if (cx) return r;
        /* No fallback built it: a failed pipeline is worse than a slow one (Zink in Xwayland binds the
         * missing pipeline and the driver faults - the Pixel 6 Pro's G78 at every Steam start, after
         * one hang while Half-Life 2's shaders were pre-compiled beside it). The driver gets it again
         * under the watchdog, once per run: a hang that does not repeat is gone, one that does costs
         * one timeout and the fallbacks below. */
        if (!retry_once(key, nh)) return r;
        VKB_WARN("a pipeline the driver never finished compiling got no fallback: the driver is asked again (once this run)");
        tls_retrying = 1;
        tls_retry_cut = 0;
        r = gfx_guarded(device, cache, n, infos, a, out);
        tls_retrying = 0;
        if (r == VK_SUCCESS && !tls_retry_cut) {
            char stx[400], note[480];
            heal(key, nh);
            VKB_WARN("  the driver built it this time");
            stages_text(h, stg, nh, stx, sizeof(stx));
            snprintf(note, sizeof(note), "vkCreateGraphicsPipelines, stages %s, built when asked again", stx);
            add_stuck(key, nh, FB_SLOW, 1, note); /* a compile that finished is not stuck: off the list */
        }
        return r;
    }
    /* The startup probe saw this driver's compiler spin on the LUT's shape (server_probe.c): its
     * stand-in at once instead of after a timeout (before the
     * geometry fold: the spin is the worse failure). */
    int spins = __atomic_load_n(&lut_spins, __ATOMIC_ACQUIRE), cut = __atomic_load_n(&lut_cut, __ATOMIC_ACQUIRE);
    if (n == 1 && (vkb_probe_flag(VKB_PROBE_FLAG_LUT_NOW) || spins || cut) && is_lut_pipeline(infos, h, nh)) {
        r = try_fallback(g, device, cache, infos, a, out, h, stg, nh, FB_LUT, &timed);
        if (r == VK_SUCCESS) {
            if (spins || cut) {
                VKB_ONCE(VKB_LOG_WARN, "UE4 colour-grading LUT pipeline not seen before built with its neutral stand-in at once: %s (logged once)",
                         spins ? "this driver's compiler spun on the shape with twice the limit" : "another LUT pipeline timed out in this run");
                if (!spins) remember("vkCreateGraphicsPipelines", h, key, stg, nh, FB_LUT); /* retried once in a later run, as a timed-out one */
            } else
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
    r = armed_gfx(g, device, cache, n, infos, a, out, h, stg, nh, &timed, key);
    if (!timed) return r;
    tls_retry_cut = 1;
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
            if (order[k] == FB_LUT) __atomic_store_n(&lut_cut, 1, __ATOMIC_RELEASE);
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
                                                        VkPipeline *out, const uint64_t *h, const uint32_t *stg, uint32_t nh, int *timed,
                                                        const uint64_t *key)
{
    guard_arm arm;
    arm.prev = tls_arm;
    arm.base = 0;
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
    w = watch_begin(&arm, "vkCreateComputePipelines", g->t->id, tls_limit > 0 ? tls_limit : g->limit, h, stg, nh, n ? &infos[0].stage : NULL, 1, key, 2);
    if (n) test_fault(&infos[0].stage, n, sizeof(*infos));
    VkResult r = g->compute(device, cache, n, infos, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    for (uint32_t i = 0; r == VK_SUCCESS && out && i < n; i++) vkb_ll_pipeline(out[i], h, stg, n == 1 ? nh : 0, 1);
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
    VkResult r = armed_compute(g, device, cache, 1, &ci, a, out, h, stg, nh, &timed, NULL);
    tls_variant = 0;
    fb_free(g, device, &b);
    VKB_WARN("compile fallback (%s) for compute stage h-%016llx: %s", fb_names[FB_RELAXED], (unsigned long long)h[0],
             r == VK_SUCCESS ? "built" : timed ? "timed out too" : "failed");
    return r;
}

static VkResult compute_guarded(VkDevice device, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *infos,
                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_guard *g = find(device);
    if (!g) return VK_ERROR_INITIALIZATION_FAILED;
    if (g->poisoned) {
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_DEVICE_LOST;
    }
    if (cap_active(g)) {
        uint32_t nref = 0;
        for (uint32_t i = 0; i < n; i++) nref += cap_refuses(g, "vkCreateComputePipelines", &infos[i].stage, 1, sizeof(infos[i].stage), NULL);
        if (nref) {
            for (uint32_t i = 0; i < n; i++)
                if (cap_refuses(g, NULL, &infos[i].stage, 1, sizeof(infos[i].stage), NULL)) out[i] = VK_NULL_HANDLE;
                else if (compute_guarded(device, cache, 1, &infos[i], a, &out[i]) != VK_SUCCESS) out[i] = VK_NULL_HANDLE;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
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
    int cx = known ? crash_of(key, nh) : 0; /* in flight when the driver crashed on its own thread: as in gfx_guarded */
    double base = g->limit > 0 ? g->limit : g->limit < 0 ? 0 : timeout_s;
    int slow = known - 1 == FB_SLOW, tries = (slow || known - 1 == FB_FAIL) && n == 1 && (base > 0 || cx) ? stuck_tries_take(key, nh, known - 1) : 0;
    if (tries) {
        /* a failed one tries the next limit; a slow one builds with its own */
        int step = slow ? tries : tries + 1;
        double lim = base * (double)(1 << (step - 1)), t0 = mono_s();
        char st[400], note[480];
        stages_text(h, stg, nh, st, sizeof(st));
        if (cx && !slow) {
            VKB_WARN("compute stage h-%016llx was in flight when the driver crashed in an earlier run: asked of the driver again (try %d of %d)",
                     (unsigned long long)h[0], step, RETRY_MAX);
            snprintf(note, sizeof(note), "vkCreateComputePipelines, stages %s, try %d after the driver's crash", st, step);
            add_stuck_x(key, nh, FB_FAIL, step, cx, note); /* first */
        } else if (!slow) {
            VKB_WARN("compute stage h-%016llx failed to build in an earlier run: tried again with %.0f s (try %d of %d)",
                     (unsigned long long)h[0], lim, step, RETRY_MAX);
            snprintf(note, sizeof(note), "vkCreateComputePipelines, stages %s, try %d with %.0f s", st, step, lim);
            add_stuck(key, nh, FB_FAIL, step, note); /* first: a try that hangs is not repeated */
        }
        tls_limit = lim;
        tls_trying = 1;
        r = armed_compute(g, device, cache, n, infos, a, out, h, stg, nh, &timed, key);
        tls_trying = 0;
        tls_limit = 0;
        if (r == VK_SUCCESS && cx && !slow) {
            VKB_WARN("  built fine, in %.1f s: no longer a crash suspect, off the list", mono_s() - t0);
            snprintf(note, sizeof(note), "vkCreateComputePipelines, stages %s, built after the crash", st);
            add_stuck(key, nh, FB_SLOW, 1, note);
            return r;
        }
        if (r == VK_SUCCESS) {
            if (!slow) {
                VKB_WARN("  built this time, in %.1f s: built with %.0f s from now on", mono_s() - t0, lim);
                snprintf(note, sizeof(note), "vkCreateComputePipelines, stages %s, %.1f s of %.0f s", st, mono_s() - t0, lim);
                add_stuck(key, nh, FB_SLOW, step, note);
            }
            return r;
        }
        if (slow && timed && step < RETRY_MAX) {
            snprintf(note, sizeof(note), "vkCreateComputePipelines, stages %s, timed out with %.0f s", st, lim);
            add_stuck(key, nh, FB_FAIL, step, note); /* the next run tries the next limit */
        }
        VKB_ERR("  %s with %.0f s: the call fails", timed ? "timed out" : "failed", lim);
        for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return r;
    }
    if (known && !slow) {
        if (cx == 1) crash_given(key, nh, known - 1, h, stg, "vkCreateComputePipelines");
        VKB_ONCE(VKB_LOG_WARN, "a compute pipeline the driver never finished compiling was asked for again: its fallback at once (logged once)");
        r = known - 1 == FB_RELAXED && n == 1 ? compute_relaxed(g, device, cache, infos, a, out, h, stg, nh) : VK_ERROR_INITIALIZATION_FAILED;
        if (r != VK_SUCCESS)
            for (uint32_t i = 0; out && i < n; i++) out[i] = VK_NULL_HANDLE;
        return r;
    }
    r = armed_compute(g, device, cache, n, infos, a, out, h, stg, nh, &timed, key);
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

/* A listed module the driver built when asked again: off the list */
static void module_built(uint64_t mh)
{
    char note[480];
    VKB_WARN("  the driver built it this time");
    snprintf(note, sizeof(note), "vkCreateShaderModule, stages 0x0=h-%016llx, built when asked again", (unsigned long long)mh);
    add_stuck(&mh, 1, FB_SLOW, 1, note);
}

static VkResult module_guarded(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out)
{
    vkb_guard *g = find(device);
    if (!g) return VK_ERROR_INITIALIZATION_FAILED;
    if (g->poisoned) {
        if (out) *out = VK_NULL_HANDLE;
        return VK_ERROR_DEVICE_LOST;
    }
    uint64_t mh = ci && ci->pCode ? spirv_hash(ci->pCode, ci->codeSize) : 0;
    double base = g->limit > 0 ? g->limit : g->limit < 0 ? 0 : timeout_s, lim = g->limit;
    int asked = 0;
    if (mh && is_stuck(&mh, 1)) {
        /* as a failed compute pipeline: tried again in later runs, written down first, each time with twice the limit */
        int tries = base > 0 ? stuck_tries_take(&mh, 1, FB_FAIL) : 0;
        if (!tries) {
            VKB_ONCE(VKB_LOG_WARN, "a shader module the driver never finished compiling was asked for again: refused (logged once)");
            if (out) *out = VK_NULL_HANDLE;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        char note[480];
        asked = tries + 1;
        lim = base * (double)(1 << tries);
        VKB_WARN("a shader module the driver never finished compiling: the driver is asked again with %.0f s (try %d of %d)", lim, asked, RETRY_MAX);
        snprintf(note, sizeof(note), "vkCreateShaderModule, stages 0x0=h-%016llx, try %d with %.0f s", (unsigned long long)mh, asked, lim);
        add_stuck(&mh, 1, FB_FAIL, asked, note);
    }
    if (!armable()) {
        VkResult r = g->module(device, ci, a, out);
        if (r == VK_SUCCESS && out) mod_remember(*out, ci);
        if (r == VK_SUCCESS && asked) module_built(mh);
        return r;
    }
    guard_arm arm;
    arm.prev = tls_arm;
    arm.base = 0;
    arm.timed_out = 0;
    watch_slot *volatile w = NULL;
    if (sigsetjmp(arm.jb, 1)) {
        watch_end(w);
        if (arm.timed_out) {
            uint64_t h = mh;
            uint32_t stg = 0;
            timed_out(g, "vkCreateShaderModule", &h, &h, &stg, 1, 0);
            if (!asked) remember("vkCreateShaderModule", &h, &h, &stg, 1, FB_FAIL); /* a try is on the list already, with its number */
            else __atomic_store_n(&retry_cut, 1, __ATOMIC_RELAXED);
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
    w = watch_begin(&arm, "vkCreateShaderModule", g->t->id, lim, NULL, NULL, 0, NULL, 0, NULL, 0);
    VkResult r = g->module(device, ci, a, out);
    tls_arm = arm.prev;
    watch_end(w);
    if (r == VK_SUCCESS && out) mod_remember(*out, ci);
    if (r == VK_SUCCESS && asked) module_built(mh);
    return r;
}

/* Every pipeline creation timed for the session report (server_report.c); the slow ones with their
 * stages' hashes. */
static void account(VkDevice device, int compute, uint32_t n, const void *infos, double t0, VkResult r)
{
    double ms = (mono_s() - t0) * 1000.0;
    vkb_guard *g = find(device);
    char st[400];
    st[0] = 0;
    if (ms >= 2000.0 && n) {
        uint64_t h[STAGES_MAX], key[STAGES_MAX];
        uint32_t stg[STAGES_MAX], nh;
        if (compute) {
            const VkComputePipelineCreateInfo *ci = infos;
            h[0] = stage_hash(&ci->stage, &key[0]);
            stg[0] = ci->stage.stage;
            nh = 1;
        } else {
            nh = gfx_hashes(infos, n, h, stg, key);
        }
        stages_text(h, stg, nh, st, sizeof(st));
    }
    vkb_report_compile(g ? g->t : NULL, compute, n, ms, r, st);
    if (g && vkb_hitch_on(g->t->proc)) {
        uint64_t a = (uint64_t)(t0 * 1e9);
        vkb_hitch_compile(g->t->proc, compute ? "compute pipelines" : "graphics pipelines", n, a, a + (uint64_t)(ms * 1e6), r);
    }
    if (compute && r < 0 && g) vkb_spvfix_compute_failed(g->t, n, infos, r); /* once per device */
}

/*
 * Pipeline cache hits. Steam's shader pre-caching (fossilize_replay, keyed steam<id>.dl/.run) fills
 * files the game takes in at its start (server_pcache.c); whether that saves the game any compiling
 * shows only in what the driver says about each pipeline: VkPipelineCreationFeedback's
 * APPLICATION_PIPELINE_CACHE_HIT (core in Vulkan 1.3). Each pipeline gets a feedback structure
 * (its own, if the app chained one, is read instead) and the device counts what was found in the
 * cache; the count goes to the session report and the log when the device goes. Devices below 1.3
 * (fossilize_replay replaying a 1.1 archive among them) are not asked.
 * Off by default: it puts a structure on every pipeline the game makes, and no Mali driver has been
 * run with that yet (a feedback bug in the driver would hit every game, for a number only the
 * report reads). VKBRIDGE_CACHE_FEEDBACK=1 turns it on, VKBRIDGE_NO_CACHE_FEEDBACK=1 off whatever
 * else says; both are preset keys, so a game's preset file can do either.
 */
#define FB_BATCH 64

static int cache_feedback(vkb_srv_table *t)
{
    const char *off = vkb_srv_env("VKBRIDGE_NO_CACHE_FEEDBACK");
    if (off && *off && strcmp(off, "0")) return 0;
    const char *on = vkb_srv_env("VKBRIDGE_CACHE_FEEDBACK");
    if (!on || !*on || !strcmp(on, "0")) return 0;
    uint32_t real = t->pd ? t->pd->info.real_api_version : 0;
    return t->api_version >= VK_API_VERSION_1_3 && real >= VK_API_VERSION_1_3;
}

static VkPipelineCreationFeedbackCreateInfo *chained_feedback(const void *pnext)
{
    for (const VkBaseInStructure *b = pnext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) return (VkPipelineCreationFeedbackCreateInfo *)b;
    return NULL;
}

typedef struct fb_batch {
    VkPipelineCreationFeedbackCreateInfo info[FB_BATCH];
    VkPipelineCreationFeedback fb[FB_BATCH];
    const VkPipelineCreationFeedback *seen[FB_BATCH]; /* where each pipeline's feedback lands */
    uint32_t n;
} fb_batch;

/* Copies of the create infos with a feedback structure in front of each chain that has none; NULL:
 * not asked (the originals go to the driver). */
static void *fb_wrap(vkb_guard *g, fb_batch *b, uint32_t n, const void *infos, size_t isize)
{
    b->n = 0;
    if (!g || !g->feedback || !infos || !n || n > FB_BATCH) return NULL;
    uint8_t *copy = malloc((size_t)n * isize);
    if (!copy) return NULL;
    memcpy(copy, infos, (size_t)n * isize);
    for (uint32_t i = 0; i < n; i++) {
        VkBaseOutStructure *ci = (VkBaseOutStructure *)(copy + (size_t)i * isize);
        VkPipelineCreationFeedbackCreateInfo *own = chained_feedback(ci->pNext);
        if (own) {
            /* the app's own (read, not counted when it has nowhere to write: a second structure
             * of the same type would be invalid) */
            b->seen[i] = own->pPipelineCreationFeedback;
            continue;
        }
        b->fb[i] = (VkPipelineCreationFeedback){0, 0};
        b->info[i] = (VkPipelineCreationFeedbackCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, ci->pNext, &b->fb[i], 0, NULL};
        ci->pNext = (VkBaseOutStructure *)&b->info[i];
        b->seen[i] = &b->fb[i];
    }
    b->n = n;
    return copy;
}

static void fb_count(vkb_guard *g, const fb_batch *b, const VkPipeline *out, VkResult r)
{
    if (!g || !b->n || (r != VK_SUCCESS && r != VK_PIPELINE_COMPILE_REQUIRED)) return;
    uint64_t built = 0, hits = 0;
    for (uint32_t i = 0; i < b->n; i++) {
        if ((out && out[i] == VK_NULL_HANDLE) || !b->seen[i]) continue;
        VkPipelineCreationFeedbackFlags f = b->seen[i]->flags;
        if (!(f & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) continue;
        built++;
        if (f & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) hits++;
    }
    __atomic_add_fetch(&g->built, built, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g->hits, hits, __ATOMIC_RELAXED);
    if (built) vkb_report_cache_hits(g->t, built, hits);
}

/* Timed for the hitch line only when the process has it on (VKBRIDGE_HITCH_MS, server_hitch.c):
 * a module is cheap to make in most drivers, the server's SPIR-V rewrites are not always. */
static VkResult VKAPI_CALL gd_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                VkShaderModule *out)
{
    vkb_guard *g = find(device);
    vkb_srv_proc *p = g ? g->t->proc : NULL;
    if (!vkb_hitch_on(p)) return module_guarded(device, ci, a, out);
    uint64_t t0 = vkb_hitch_now();
    VkResult r = module_guarded(device, ci, a, out);
    vkb_hitch_compile(p, "shader module", 1, t0, vkb_hitch_now(), r);
    return r;
}

static VkResult VKAPI_CALL gd_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                     const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                     VkPipeline *out)
{
    int gate = vkb_compile_gate_enter(n ? infos[0].flags : 0); /* server_compile_gate.c: waits before the driver, not timed */
    double t0 = mono_s();
    vkb_guard *g = find(device);
    fb_batch b;
    VkGraphicsPipelineCreateInfo *w = fb_wrap(g, &b, n, infos, sizeof(*infos));
    VkResult r = gfx_guarded(device, cache, n, w ? w : infos, a, out);
    vkb_compile_gate_leave(gate);
    fb_count(g, &b, out, r);
    account(device, 0, n, infos, t0, r);
    free(w);
    return r;
}

static VkResult VKAPI_CALL gd_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                    const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                    VkPipeline *out)
{
    int gate = vkb_compile_gate_enter(n ? infos[0].flags : 0);
    double t0 = mono_s();
    vkb_guard *g = find(device);
    fb_batch b;
    VkComputePipelineCreateInfo *w = fb_wrap(g, &b, n, infos, sizeof(*infos));
    VkResult r = compute_guarded(device, cache, n, w ? w : infos, a, out);
    vkb_compile_gate_leave(gate);
    fb_count(g, &b, out, r);
    account(device, 1, n, infos, t0, r);
    free(w);
    return r;
}

/* ------------------------------------------------------------------ the startup probe */

/* ------------------------------------------------------------------ memory calls */

/*
 * Faults in the driver's vkAllocateMemory and vkBind*Memory are contained the same way (a player log:
 * Mali-G615 r44p1 faulted in vkBindImageMemory, a player log at the same driver pc in vkAllocateMemory,
 * SEGV_ACCERR both; the cause inside the driver is not known). The arm is a sigsetjmp without the
 * signal mask (no system call: a few stores per call; these calls are made when resources are
 * made, not per frame), and the signal is unblocked on the way back. What the program then sees:
 *   - an allocation fails with VK_ERROR_OUT_OF_DEVICE_MEMORY, which programs handle; a device's
 *     second contained memory fault takes the device as lost (server_lost.c);
 *   - a bind takes its device as lost at once: the image or buffer is left without memory (and a
 *     bind is asynchronous, the program was told VK_SUCCESS), so nothing of it may reach the GPU;
 *     the program's next submission or wait says VK_ERROR_DEVICE_LOST and it ends as after any
 *     loss, the other programs go on.
 * The same caveat as for compiles: a lock the driver held at the fault stays held. The process
 * limit (PROCESS_MAX) and VKBRIDGE_NO_CRASH_GUARD=1 apply.
 *
 * The stall watchdog (started at the first contained memory fault; nothing runs before one): a
 * later driver call that waits on such a lock never comes back, and without it the program, or
 * every program, would freeze with no end screen where the uncontained fault ended the session.
 * Once a second, the requests running for VKBRIDGE_MEM_STALL_S (30) seconds that do not wait by
 * design (fence/semaphore/idle waits, query results with a wait, pipeline compiles: those have
 * their own limits) are looked at:
 *   - one program stuck: its session is ended without touching the driver - its devices are
 *     stopped (refused, never destroyed, as after a driver thread's fault: server_lost.c) and its
 *     connections shut down, so it sees the server gone and ends; the others go on;
 *   - a second program stuck (at once, or after one was ended): the lock is the driver's for every
 *     program; the server ends (abort, with the stuck calls in its crash line), as the fault
 *     itself would have ended it before the containment, and the app shows its end screen.
 * VKBRIDGE_MEM_STALL_S=0 turns the watchdog off.
 */
/* the driver's memory calls by table id, kept past the guard's own end (a layer above may still
 * hold the wrapper): a call without a guard goes to the driver unguarded */
static PFN_vkAllocateMemory mn_alloc[MAX_TABLES];
static PFN_vkBindImageMemory mn_bind_img[MAX_TABLES];
static PFN_vkBindImageMemory2 mn_bind_img2[MAX_TABLES];
static PFN_vkBindBufferMemory mn_bind_buf[MAX_TABLES];
static PFN_vkBindBufferMemory2 mn_bind_buf2[MAX_TABLES];
static PFN_vkEndCommandBuffer mn_end[MAX_TABLES];

static uint32_t cur_id(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < MAX_TABLES ? t->id : 0;
}

#define MEM_ARM(g, call, failed, body) GUARD_ARM(g, call, mem_contained, failed, body)
#define GUARD_ARM(g, call, on_fault, failed, body)                                                        \
    do {                                                                                                  \
        if (!(g) || (tls_arm && !tls_arm->base) || !armable()) {                                          \
            body;                                                                                         \
            break;                                                                                        \
        }                                                                                                 \
        guard_arm arm;                                                                                    \
        arm.prev = tls_arm;                                                                               \
        arm.base = 0;                                                                                     \
        arm.timed_out = 0;                                                                                \
        if (sigsetjmp(arm.jb, 0)) {                                                                       \
            sigset_t un;                                                                                  \
            sigemptyset(&un);                                                                             \
            sigaddset(&un, arm.sig);                                                                      \
            pthread_sigmask(SIG_UNBLOCK, &un, NULL); /* the handler's own block: the jump kept it */     \
            on_fault(g, call, &arm, failed);                                                              \
            failed;                                                                                       \
            break;                                                                                        \
        }                                                                                                 \
        tls_arm = &arm;                                                                                   \
        body;                                                                                             \
        tls_arm = arm.prev;                                                                               \
    } while (0)

static const char *mem_fault_call; /* the last contained memory fault, for the watchdog's lines */
static uint32_t mem_fault_dev;
static double mem_stall_s = 30;

static void *mem_watch_main(void *arg)
{
    (void)arg;
    int ended = 0; /* the program ended for a stall: its stuck thread stays in the driver for good */
    for (;;) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        vkb_srv_stuck_req st[8];
        int n = vkb_srv_stuck(mem_stall_s, st, 8), fresh = 0, k = -1;
        for (int i = 0; i < n; i++)
            if (st[i].pid != ended) fresh++, k = k < 0 ? i : k;
        if (!fresh) continue;
        if (!ended && fresh == 1) {
            VKB_ERR("memory fault watchdog: %s (pid %d) has been in %s for %.0f s, after a driver fault in %s on device %u was "
                    "contained: most likely waiting on a driver lock that fault left held (%s). Its session ends: its devices are left as they are, the "
                    "other programs go on", st[k].comm, st[k].pid, st[k].cmd, st[k].secs, mem_fault_call ? mem_fault_call : "?",
                    mem_fault_dev, st[k].probe[0] ? st[k].probe : "its thread not readable");
            int d = vkb_srv_end_proc(st[k].pid, "a call of its program stuck in the driver after a contained memory fault");
            VKB_ERR("memory fault watchdog: %d device(s) of pid %d stopped, its connections closed", d, st[k].pid);
            ended = st[k].pid;
            continue;
        }
        char msg[800];
        snprintf(msg, sizeof(msg),
                 "memory fault watchdog: %s (pid %d) has been in %s for %.0f s%s, after a driver fault in %s on device %u was "
                 "contained: a second program stuck behind it, so the lock is the driver's for every program; the server ends now, as the fault would "
                 "have ended it",
                 st[k].comm, st[k].pid, st[k].cmd, st[k].secs, ended ? " (a program was ended for this before)" : " (and another program)",
                 mem_fault_call ? mem_fault_call : "?", mem_fault_dev);
        VKB_ERR("%s", msg); /* then the crash line of the abort names the calls still in the driver */
        abort();
    }
    return NULL;
}

static void mem_watch_start(void)
{
    static int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    const char *e = vkb_srv_env_global("VKBRIDGE_MEM_STALL_S");
    if (e && *e) mem_stall_s = atof(e);
    if (mem_stall_s <= 0) {
        VKB_INFO("memory fault watchdog: off (VKBRIDGE_MEM_STALL_S=%s)", e);
        return;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, mem_watch_main, NULL) != 0) {
        VKB_WARN("memory fault watchdog: no thread");
        return;
    }
    pthread_detach(th);
    VKB_INFO("memory fault watchdog: on (a call stuck %.0f s ends its program; a second program stuck ends the server)", mem_stall_s);
}

static void mem_contained(vkb_guard *g, const char *call, const guard_arm *a, ...)
{
    mem_fault_call = call;
    mem_fault_dev = g->t->id;
    mem_watch_start();
    int mine = __atomic_add_fetch(&g->mem_crashes, 1, __ATOMIC_RELAXED);
    int total = charge(g->t->id);
    int bind = strncmp(call, "vkBind", 6) == 0;
    const vkb_srv_proc *p = g->t->proc;
    VKB_ERR("CONTAINED driver crash: signal %d (address %p) in %s on device %u (%s, pid %d) - %s, the other programs go on "
            "(%d memory fault(s) on this device, %d contained in the process%s)",
            a->sig, a->addr, call, g->t->id, p && p->comm[0] ? p->comm : "?", p ? p->pid : 0,
            bind ? "the device is taken as lost" : mine >= 2 ? "the allocation fails and the device is taken as lost"
                                                            : "the allocation fails (VK_ERROR_OUT_OF_DEVICE_MEMORY)",
            mine, total, total >= PROCESS_MAX ? "; containment now off, the next fault kills the process" : "");
    fault_detail(call, a);
    VKB_ERR("  driver locks held at the fault stay held: a hang in a later driver call would be this crash's doing");
    if (bind || mine >= 2) vkb_lost_mark(g->t, bind ? "a driver fault in a memory bind was contained" : "two driver faults in its allocations were contained");
}

static VKAPI_ATTR VkResult VKAPI_CALL gd_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a,
                                                        VkDeviceMemory *out)
{
    vkb_guard *g = find(device);
    __typeof__(mn_alloc[0]) next = g ? g->alloc : mn_alloc[cur_id()];
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    MEM_ARM(g, "vkAllocateMemory", (*out = VK_NULL_HANDLE, r = VK_ERROR_OUT_OF_DEVICE_MEMORY), r = next(device, ai, a, out));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gd_BindImageMemory(VkDevice device, VkImage img, VkDeviceMemory mem, VkDeviceSize off)
{
    vkb_guard *g = find(device);
    __typeof__(mn_bind_img[0]) next = g ? g->bind_img : mn_bind_img[cur_id()];
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = VK_ERROR_DEVICE_LOST;
    MEM_ARM(g, "vkBindImageMemory", r = VK_ERROR_DEVICE_LOST, r = next(device, img, mem, off));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gd_BindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *bi)
{
    vkb_guard *g = find(device);
    __typeof__(mn_bind_img2[0]) next = g ? g->bind_img2 : mn_bind_img2[cur_id()];
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = VK_ERROR_DEVICE_LOST;
    MEM_ARM(g, "vkBindImageMemory2", r = VK_ERROR_DEVICE_LOST, r = next(device, n, bi));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gd_BindBufferMemory(VkDevice device, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off)
{
    vkb_guard *g = find(device);
    __typeof__(mn_bind_buf[0]) next = g ? g->bind_buf : mn_bind_buf[cur_id()];
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = VK_ERROR_DEVICE_LOST;
    MEM_ARM(g, "vkBindBufferMemory", r = VK_ERROR_DEVICE_LOST, r = next(device, buf, mem, off));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gd_BindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo *bi)
{
    vkb_guard *g = find(device);
    __typeof__(mn_bind_buf2[0]) next = g ? g->bind_buf2 : mn_bind_buf2[cur_id()];
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = VK_ERROR_DEVICE_LOST;
    MEM_ARM(g, "vkBindBufferMemory2", r = VK_ERROR_DEVICE_LOST, r = next(device, n, bi));
    return r;
}

/*
 * The driver's vkEndCommandBuffer of a client's command buffer, at the end of its replay (server_conn.c).
 * a player log (Xiaomi XRing O3, Mali-G2-Ultra r56p0 beta, Animal Well through vkd3d-proton): the server died
 * twice at the same driver pc, a read of address 0x18 in the driver's End of vkd3d-proton's query-reset
 * command buffer (begin, vkCmdResetQueryPool, the query mirror's barrier and fill: valid commands, the
 * cause inside the driver is not known), and took Steam with it. Such a fault is now contained: the end
 * fails (submits leave the command buffer out), and the device is stopped (server_lost.c: taken as lost,
 * its requests refused, never reached again nor destroyed) - the driver's state of that command buffer
 * and its pool is broken, and the program's teardown (pool reset and destroy, vkDestroyDevice) would walk
 * it again. Its program sees its device lost and ends; the other programs go on; the device's memory
 * comes back when the session ends. The memory fault watchdog (above) covers a driver lock the fault left
 * held. Only ends inside a client's replay are armed (the server's own command buffers end as before);
 * the arm is the memory calls' (no system call). VKBRIDGE_END_GUARD=0 turns it off, and the process
 * limit and VKBRIDGE_NO_CRASH_GUARD=1 apply.
 */
static void end_contained(vkb_guard *g, const char *call, const guard_arm *a, ...)
{
    mem_fault_call = call;
    mem_fault_dev = g->t->id;
    mem_watch_start();
    int mine = __atomic_add_fetch(&g->end_crashes, 1, __ATOMIC_RELAXED);
    int total = charge(g->t->id);
    const vkb_srv_proc *p = g->t->proc;
    VKB_ERR("CONTAINED driver crash: signal %d (address %p) in %s on device %u (%s, pid %d) - the command buffer is not recorded "
            "and the device is stopped (taken as lost, not reached again), its program ends, the other programs go on "
            "(%d on this device, %d contained in the process%s)",
            a->sig, a->addr, call, g->t->id, p && p->comm[0] ? p->comm : "?", p ? p->pid : 0, mine, total,
            total >= PROCESS_MAX ? "; containment now off, the next fault kills the process" : "");
    char tail[640];
    int n = 0;
    uint32_t done = vkb_srv_replay_n, first = done > VKB_REPLAY_RING ? done - VKB_REPLAY_RING : 0;
    for (uint32_t i = first; i < done && n >= 0 && n < (int)sizeof(tail) - 1; i++) {
        const char *name = vkb_srv_replay_ring[i % VKB_REPLAY_RING];
        n += snprintf(tail + n, sizeof(tail) - (size_t)n, "%s%s", n ? " " : "", name ? name : "?");
    }
    if (n <= 0) snprintf(tail, sizeof(tail), "(no commands)");
    VKB_ERR("  replaying command buffer %p, %u command(s); %s: %s", (void *)vkb_srv_replay_cb, done,
            first ? "the last of the stream" : "the stream", tail);
    fault_detail(call, a);
    VKB_ERR("  driver locks held at the fault stay held: a hang in a later driver call would be this crash's doing");
    vkb_lost_stop(g->t, "a driver fault in vkEndCommandBuffer was contained");
}

static VKAPI_ATTR VkResult VKAPI_CALL gd_EndCommandBuffer(VkCommandBuffer cb)
{
    uint32_t id = cur_id();
    vkb_guard *g = __atomic_load_n(&guards[id], __ATOMIC_ACQUIRE);
    if (g && g->t != vkb_srv_current_table()) g = NULL;
    PFN_vkEndCommandBuffer next = g ? g->end : mn_end[id];
    if (!next) return VK_ERROR_DEVICE_LOST;
    if (!vkb_srv_replay_cb) return next(cb); /* the server's own command buffers */
    if (g && vkb_lost_stopped(g->t)) return VK_ERROR_DEVICE_LOST; /* a fault earlier in this replay stopped it */
    VkResult r = VK_ERROR_DEVICE_LOST;
    GUARD_ARM(g, "vkEndCommandBuffer", end_contained, r = VK_ERROR_DEVICE_LOST, r = next(cb));
    return r;
}

/*
 * One guard around every driver call a connection thread makes for its client (a player log: Mali-G1-Ultra
 * faulted at 0x32 in vkQueueSubmit2; No Man's Sky's last crash, a player log: a near-null read in
 * the driver's vkCmdDraw / vkCmdDrawIndexed inside a replay; the divisor emulation's call through a
 * missing entry point): each ended the one server with Steam, the compositor and every program. The
 * specific arms above cover the calls whose failure a program can take (a pipeline, an allocation, an
 * End); everything else a connection thread does for its client now has this one beneath them.
 *
 * Cost: none per request. The arm is set once when the thread starts serving (and again after a
 * contained fault): the connection's whole loop runs inside it, so no request, submit or draw does
 * anything more than before. The specific arms nest on top of it as before (GUARD_ARM ignores it).
 *
 * A fault it catches (server_conn.c conn_recover then): the program's device is stopped as after a
 * fault in its End (server_lost.c: taken as lost, its requests refused - the client answers
 * VK_ERROR_DEVICE_LOST, as for any refused call - never reached again nor destroyed): the driver's state
 * of what it was doing is broken, and its teardown would walk it again. The request is answered with
 * that refusal (no reply left hanging), the stall watchdog above covers a lock the fault left held, the
 * other programs go on. A fault in an instance-level request (no device) refuses that request only.
 * The crash line's account is logged (the call, inside a replay its command N of M and the stream's
 * tail, the fault's code, pc as library + offset, address, arguments, other calls in flight).
 * VKBRIDGE_REQ_GUARD=0 turns it off (the A/B: the fault ends the server as before); the process limit
 * (PROCESS_MAX) and VKBRIDGE_NO_CRASH_GUARD=1 apply. Bridge code that held a lock of its own at the fault
 * leaves it held too: the same stall watchdog.
 */
int vkb_guard_serve(void (*loop)(void *), void *arg, vkb_guard_caught *f)
{
    static int off = -1;
    guard_env();
    if (off < 0) {
        const char *e = vkb_srv_env_global("VKBRIDGE_REQ_GUARD");
        off = e && *e == '0';
        if (off && !guard_off) VKB_INFO("request guard off (VKBRIDGE_REQ_GUARD=0): a driver fault on a connection thread ends the server");
    }
    if (off || guard_off || tls_arm) {
        loop(arg);
        return 0;
    }
    guard_arm arm;
    memset(&arm, 0, sizeof(arm));
    arm.base = 1;
    arm.prev = NULL;
    if (sigsetjmp(arm.jb, 1)) { /* the mask as it was here: the handler's block of the signal is undone */
        tls_arm = NULL;
        f->sig = arm.sig;
        f->code = arm.code;
        f->addr = arm.addr;
        f->pc = arm.pc;
        f->snap = arm.snap;
        f->nra = arm.nra < 6 ? arm.nra : 6;
        for (int i = 0; i < f->nra; i++) f->ra[i] = arm.ra[i];
        return 1;
    }
    tls_arm = &arm;
    loop(arg);
    tls_arm = NULL;
    return 0;
}

int vkb_guard_request_contained(vkb_srv_table *t, const char *call, const char *where, const vkb_guard_caught *f)
{
    int total = t && t->is_device ? charge(t->id) : __atomic_add_fetch(&process_crashes, 1, __ATOMIC_RELAXED);
    int dev = t && t->is_device;
    const vkb_srv_proc *p = t ? t->proc : vkb_srv_crash_client();
    mem_fault_call = call;
    mem_fault_dev = t ? t->id : 0;
    mem_watch_start();
    VKB_ERR("CONTAINED driver crash: signal %d (address %p) in %s%s%s%s on %s %u (%s, pid %d) - %s, the other programs go on "
            "(%d contained in the process%s)",
            f->sig, f->addr, call ? call : "?", where ? " (" : "", where ? where : "", where ? ")" : "", dev ? "device" : "table",
            t ? t->id : 0, p && p->comm[0] ? p->comm : "?", p ? p->pid : 0,
            dev ? "the device is stopped (taken as lost, not reached again): its program gets VK_ERROR_DEVICE_LOST and ends"
                : "the request is refused",
            total, total >= PROCESS_MAX ? "; containment now off, the next fault kills the process" : "");
    if (vkb_srv_replay_cb) {
        char tail[640];
        int n = 0;
        uint32_t done = vkb_srv_replay_n, first = done > VKB_REPLAY_RING ? done - VKB_REPLAY_RING : 0;
        for (uint32_t i = first; i < done && n >= 0 && n < (int)sizeof(tail) - 1; i++) {
            const char *name = vkb_srv_replay_ring[i % VKB_REPLAY_RING];
            n += snprintf(tail + n, sizeof(tail) - (size_t)n, "%s%s", n ? " " : "", name ? name : "?");
        }
        if (n <= 0) snprintf(tail, sizeof(tail), "(no commands)");
        VKB_ERR("  replaying command buffer %p, command %u of its stream; %s: %s", (void *)vkb_srv_replay_cb, done,
                first ? "the last of the stream" : "the stream", tail);
    }
    guard_arm a;
    memset(&a, 0, sizeof(a));
    a.sig = f->sig;
    a.addr = f->addr;
    a.code = f->code;
    a.pc = f->pc;
    a.snap = f->snap;
    a.nra = f->nra < GUARD_RA ? f->nra : GUARD_RA;
    for (int i = 0; i < a.nra; i++) a.ra[i] = f->ra[i];
    fault_detail(call ? call : "?", &a);
    VKB_ERR("  driver locks held at the fault stay held: a hang in a later driver call would be this crash's doing");
    if (dev) vkb_lost_stop(t, "a driver fault in one of its program's calls was contained");
    return total;
}

const uint32_t *vkb_guard_lut_standin(size_t *size)
{
    *size = sizeof(lut_standin_fs);
    return lut_standin_fs;
}

int vkb_guard_armable(void)
{
    guard_env();
    return armable();
}

void vkb_guard_contained_elsewhere(const char *call, uint32_t dev)
{
    charge(dev);
    mem_fault_call = call;
    mem_fault_dev = dev;
    mem_watch_start();
}

void vkb_guard_watch_start(void)
{
    guard_env();
    if (!guard_off) watch_env();
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
    arm.base = 0;
    arm.timed_out = 0;
    watch_slot *volatile w = NULL;
    if (sigsetjmp(arm.jb, 1)) {
        watch_end(w);
        if (sig) *sig = arm.sig;
        return arm.timed_out ? VKB_GUARD_TIMED_OUT : VKB_GUARD_CRASHED;
    }
    tls_arm = &arm;
    w = watch_begin(&arm, what, 0, 0, NULL, NULL, 0, NULL, 0, NULL, 0);
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
    {
        const char *why;
        double lim = timeout_for(vkb_mode(), &why, 1);
        g->limit = lim > 0 ? lim : -1;
        if (lim != timeout_s) VKB_INFO("compile watchdog: device %u interrupts after %.0f s (%s)", t->id, lim, why);
    }
    g->feedback = cache_feedback(t);
    cap_device(g, t);
    g->gfx = t->real.vkCreateGraphicsPipelines;
    g->compute = t->real.vkCreateComputePipelines;
    g->module = t->real.vkCreateShaderModule;
    __atomic_store_n(&guards[t->id], g, __ATOMIC_RELEASE);
    t->real.vkCreateGraphicsPipelines = t->dt.vkCreateGraphicsPipelines = gd_CreateGraphicsPipelines;
    t->real.vkCreateComputePipelines = t->dt.vkCreateComputePipelines = gd_CreateComputePipelines;
    t->real.vkCreateShaderModule = t->dt.vkCreateShaderModule = gd_CreateShaderModule;
    /* the memory calls (VKBRIDGE_MEM_GUARD=0: not) */
    const char *eg = vkb_srv_env_global("VKBRIDGE_END_GUARD");
    if (!(eg && *eg == '0') && t->real.vkEndCommandBuffer) {
        g->end = mn_end[t->id] = t->real.vkEndCommandBuffer;
        if (t->dt.vkEndCommandBuffer == t->real.vkEndCommandBuffer) t->dt.vkEndCommandBuffer = gd_EndCommandBuffer;
        t->real.vkEndCommandBuffer = gd_EndCommandBuffer;
    }
    const char *mg = vkb_srv_env_global("VKBRIDGE_MEM_GUARD");
    if (mg && *mg == '0') return;
#define MEM_HOOK(field, fn, save)                                                                    \
    do {                                                                                             \
        if (t->real.field) {                                                                         \
            g->save = mn_##save[t->id] = t->real.field;                                              \
            if (t->dt.field == t->real.field) t->dt.field = (fn);                                   \
            t->real.field = (fn);                                                                    \
        }                                                                                            \
    } while (0)
    MEM_HOOK(vkAllocateMemory, gd_AllocateMemory, alloc);
    MEM_HOOK(vkBindImageMemory, gd_BindImageMemory, bind_img);
    MEM_HOOK(vkBindImageMemory2, gd_BindImageMemory2, bind_img2);
    MEM_HOOK(vkBindBufferMemory, gd_BindBufferMemory, bind_buf);
    MEM_HOOK(vkBindBufferMemory2, gd_BindBufferMemory2, bind_buf2);
#undef MEM_HOOK
}

int vkb_guard_mem_faults(const vkb_srv_table *t)
{
    if (!t || t->id >= MAX_TABLES) return 0;
    vkb_guard *g = __atomic_load_n(&guards[t->id], __ATOMIC_ACQUIRE);
    return g && g->t == t ? __atomic_load_n(&g->mem_crashes, __ATOMIC_RELAXED) : 0;
}

void vkb_guard_device_destroy(vkb_srv_table *t)
{
    if (t->id >= MAX_TABLES) return;
    int was = __atomic_exchange_n(&charged[t->id], 0, __ATOMIC_RELAXED);
    if (was > 0) {
        int left = __atomic_sub_fetch(&process_crashes, was, __ATOMIC_RELAXED);
        VKB_INFO("device %u gone: its %d contained crash(es) no longer count toward the limit of %d (%d from programs still running)",
                 t->id, was, PROCESS_MAX, left);
    }
    vkb_guard *g = guards[t->id];
    if (!g || g->t != t) return;
    __atomic_store_n(&guards[t->id], NULL, __ATOMIC_RELEASE);
    if (g->built)
        VKB_INFO("device %u: %llu pipelines built, %llu of them found in the pipeline cache (the driver's creation feedback)", t->id,
                 (unsigned long long)g->built, (unsigned long long)g->hits);
    t->real.vkCreateGraphicsPipelines = g->gfx;
    t->real.vkCreateComputePipelines = g->compute;
    t->real.vkCreateShaderModule = g->module;
#define MEM_UNHOOK(field, fn, save)                                                                  \
    do {                                                                                             \
        if (g->save) {                                                                               \
            if (t->dt.field == (fn)) t->dt.field = g->save;                                         \
            if (t->real.field == (fn)) t->real.field = g->save;                                     \
        }                                                                                            \
    } while (0)
    MEM_UNHOOK(vkAllocateMemory, gd_AllocateMemory, alloc);
    MEM_UNHOOK(vkBindImageMemory, gd_BindImageMemory, bind_img);
    MEM_UNHOOK(vkBindImageMemory2, gd_BindImageMemory2, bind_img2);
    MEM_UNHOOK(vkBindBufferMemory, gd_BindBufferMemory, bind_buf);
    MEM_UNHOOK(vkBindBufferMemory2, gd_BindBufferMemory2, bind_buf2);
    MEM_UNHOOK(vkEndCommandBuffer, gd_EndCommandBuffer, end);
#undef MEM_UNHOOK
    free(g);
}
