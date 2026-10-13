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
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        s = getenv("VKBRIDGE_TEST_CRASH_PIPELINE");
        test_crash = s && *s && *s != '0';
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
            VKB_WARN("test: faulting on purpose in a %s call", tls_arm ? "guarded" : "UNGUARDED");
            *(volatile int *)(uintptr_t)0x40 = 1;
        }
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
    guard_arm arm;
    arm.prev = tls_arm;
    if (sigsetjmp(arm.jb, 1)) {
        contained(g, "vkCreateGraphicsPipelines", &arm);
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t s = 0; s < infos[i].stageCount; s++) log_stage(&infos[i].pStages[s], i, n);
            if (out) out[i] = VK_NULL_HANDLE;
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    tls_arm = &arm;
    for (uint32_t i = 0; i < n; i++) test_fault(infos[i].pStages, infos[i].stageCount, sizeof(*infos[i].pStages));
    VkResult r = g->gfx(device, cache, n, infos, a, out);
    tls_arm = arm.prev;
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
    guard_arm arm;
    arm.prev = tls_arm;
    if (sigsetjmp(arm.jb, 1)) {
        contained(g, "vkCreateComputePipelines", &arm);
        for (uint32_t i = 0; i < n; i++) {
            log_stage(&infos[i].stage, i, n);
            if (out) out[i] = VK_NULL_HANDLE;
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    tls_arm = &arm;
    if (n) test_fault(&infos[0].stage, n, sizeof(*infos));
    VkResult r = g->compute(device, cache, n, infos, a, out);
    tls_arm = arm.prev;
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
    if (!armable()) return g->module(device, ci, a, out);
    guard_arm arm;
    arm.prev = tls_arm;
    if (sigsetjmp(arm.jb, 1)) {
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
    VkResult r = g->module(device, ci, a, out);
    tls_arm = arm.prev;
    return r;
}

void vkb_guard_device_init(vkb_srv_table *t)
{
    guard_env();
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
