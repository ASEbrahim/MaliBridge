/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Startup capability probe: what the GPU's driver really does, not what it claims.
 *
 * Every failure class the bridge has met so far (docs: failure-catalogue) was found by hand, one
 * tablet run at a time: a feature the driver advertises and gets wrong (the G925's divisor without
 * the zero divisor), an image it refuses, a compiler that never returns (the G925 on UE4's LUT),
 * a wait answered with an error it may not return. Here each tricky capability is exercised once
 * per GPU + driver build with a tiny workload on the driver itself (no bridge layers), and the
 * outcome - pass, fail (wrong result), crash, hang, died (the process ended inside it), skip (not
 * applicable), error (could not set up) - is kept as a device profile in the cache directory:
 *   <cache dir>/probe-<vendor>-<device>-<driverVersion>-<uuid>.json
 * re-run when the driver, the bridge build, the probe list or the imitation switches change.
 *
 * Each probe runs armed like a driver compile (vkb_guard_run, server_guard.c): a fault inside it,
 * or a run past its time limit, jumps back and becomes its result; the probe device is then
 * abandoned (not destroyed: the driver may hold a lock) and a new one made for the next probe.
 * Before each probe the profile is rewritten with "running": <name>, so a process that ends in
 * one leaves that name behind. Ending there is not always the probe's doing: the app ends a
 * session with SIGKILL (Process.killProcess), Android force-stops with it, and a session may end
 * while the background part still runs. So while a probe runs, <profile>.beat holds its name and
 * how long it has run (not counting time the process was stopped), rewritten every 200 ms, and a
 * mark of how the process ended where it could write one; the next start reads it:
 *   stop mark (SIGTERM/SIGINT, the app gone)             interrupted: run again, nothing decided
 *   crash mark (a fatal signal outside the probe's guard, died (on another thread: interrupted)
 *     on the probe's thread)
 *   no mark, still inside past its limit + 2 s           died (the watchdog could not interrupt it)
 *   idle mark (every client process had left, then the   interrupted: the session's own end (the app
 *     process ended without a word)                      kills the server right after its guest)
 *   no mark, within its limit                            a kill or an abort: run again; died only
 *                                                        when it ends so 2 times in a row (3 in
 *                                                        the separate server, which marks aborts
 *                                                        too: there it is almost always a kill)
 * A probe interrupted at its limit after the process was stopped (SIGSTOP) during it is not
 * counted either. A died probe is skipped and its decision applies; the start goes on with the rest.
 *
 * Fed back: a native feature whose probe did not pass gets its emulation instead (logged with the
 * probe's evidence), where the bridge has one - a known bug pattern heals itself on a device
 * nobody has tested. Probes that pass change nothing (the G720's paths stay as they are). Other
 * outcomes set runtime flags: a layer-only geometry stage drawn wrong (and a vertex shader's Layer
 * right) folds such stages into the vertex shader; a compiler spinning on the UE4 LUT shape gives
 * those pipelines their stand-in at once instead of after a 10 s timeout; a BC transcoder that
 * does not build (within VKBRIDGE_PROBE_TX_TIMEOUT_S, 20 s: they are slow, not stuck, on some
 * drivers) leaves its formats decoded; a new variant of the BC converters (one dispatch a run, the palette search) that
 * does not build, hangs or converts one byte differently from the old one is not used (bc_variants: the old one stays,
 * and none is used before it passed). VKBRIDGE_PROBE_APPLY=0 records without applying.
 *
 * The MK probes - viewport_flip, bitwise_vec, dyn_state, vb_strides, view3d_layers, wait_eintr,
 * rendering_churn, fp16_math: one per Mali driver erratum (server_errata.c) a probe can tell, MK-06's only
 * recording - each record their sub-cases (bits, named in the report) and, where they saw the
 * bug's own pattern, a VKB_PROBE_FLAG_MK_* flag. That flag is the "probe failed" the errata
 * decisions read at each vkCreateDevice (errata_source, vkb_errata_set_probe_source); a probe that
 * crashed, hung or failed some other way decides nothing there. With VKBRIDGE_ERRATA_IMITATE naming
 * an erratum (host builds) its imitation goes beneath its probe, which then sees the bug.
 *
 * The profile keeps raw results (status, the flags a probe asked for, sub-cases); what they switch
 * on is decided from them per mode at use time (decide; vkb_probe_decide_mode for a mode not in
 * force), so a mode switch never re-probes. The session report shows each MK probe's result with
 * its erratum's decision and reason in both modes.
 *
 * Cost: bounded at start (VKBRIDGE_PROBE_BUDGET_S, 6 s; the app waits 15 s for a server's socket):
 * what does not fit runs in a background thread after the server listens, and its emulation
 * decisions apply from the next start (its runtime flags at once). Probes that are slow but light
 * (wait_eintr's 1 s of waits, rendering_churn's submissions) always go there. With a profile cached the start
 * only reads it. VKBRIDGE_PROBE=0 turns the probe off, =force re-runs it.
 *
 * Imitation for host tests: VKBRIDGE_PROBE_FAKE=<probe>:<fail|crash|hang|die|abort|stall|slow|error>,...
 * makes a probe that applies fail, fault, spin until its limit, end the process without a word
 * (_exit), abort, spin with the watchdog's signal blocked (only a kill ends it), or take 3 s
 * longer (to stop a session inside it), or end in error without running (could not set up: what
 * safe mode does not take as verified); VKBRIDGE_FAKE_MISSING applies as everywhere (a feature taken as missing is
 * already emulated, so its probe is skipped). VKBRIDGE_PROBE_TX_FAKE=<transcoder>:<ms>,... (BC7->ASTC:5000)
 * makes that BC transcoder's build take ms longer, inside its watchdog; VKBRIDGE_PROBE_BCV_FAKE=<variant>:<differ|fail|slow|hang>,...
 * ("BC1 decode table:differ") changes one byte of that converter variant's output, fails its build, or makes its check
 * 3 s or 600 s longer (bc_variants; a hang is interrupted at VKBRIDGE_PROBE_TX_TIMEOUT_S).
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include "vkb_server.h"
#include "server_emu.h"
#include "server_errata.h"
#include "vkb_ext_alias.h"
#include "probe_spv.h"
#include "spirv_names.h"

#include <dirent.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <vulkan/vulkan_android.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef VKB_GIT
#define VKB_GIT "unknown"
#endif

/* Bump when a probe is added, removed or its pass criterion changes: cached profiles are re-run. */
#define PROBE_VERSION 7
#define MAX_RESULTS 48
#define DETAIL 640

enum { PS_PASS, PS_FAIL, PS_CRASH, PS_HANG, PS_DIED, PS_SKIP, PS_ERROR, PS_COUNT };
static const char *const ps_name[PS_COUNT] = {"pass", "fail", "crash", "hang", "died", "skip", "error"};

typedef struct presult {
    char name[32];
    int status;
    double ms;
    uint32_t emu;    /* emulation switched on because of it (decide: in the mode in force) */
    uint64_t flags;  /* VKB_PROBE_FLAG_* set because of it (decide) */
    uint64_t want;   /* raw: the flags the probe asked for (it saw a failure they answer) */
    uint32_t cases;  /* raw: the sub-cases that ran (bit i: the probe's case i, probe_def.cases) */
    uint32_t failed; /* raw: the sub-cases that did not pass */
    char detail[DETAIL];
} presult;

typedef struct pprofile {
    char path[640];
    char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    char key[160];   /* everything a cached profile must match */
    char driver[160];
    uint32_t vendor, device, driver_version;
    presult res[MAX_RESULTS];
    int n;
    double cost_ms;  /* probing time spent (this start) */
    int cached;      /* results read from the cache file */
    int complete;
    int deferred;    /* probes left to the background thread */
    uint32_t emu_before, emu_after;
    char running[32];
    double start_ms, bg_ms; /* wall time of the start's part and of the background part */
    /* the probe in flight: <path>.beat (probe_begin) */
    int beat_fd, beat_tid;
    double beat_last, beat_active, beat_gap;
    /* a probe an earlier start ended inside without a word, and how many times in a row */
    char suspect[32];
    int suspect_n;
    /* this start's GPU, for the decisions made from the results (vkb_probe_decide_mode, the
     * errata in the report) */
    uint8_t uuid[VK_UUID_SIZE];
    VkPhysicalDeviceProperties props;
    char driver_info[VK_MAX_DRIVER_INFO_SIZE];
    uint32_t possible; /* emulations the bridge can stand in with on this GPU */
    uint32_t missing_before; /* VKB_EMU_* features the GPU lacks (no native path to verify) */
} pprofile;

static pthread_mutex_t plock = PTHREAD_MUTEX_INITIALIZER;
static pprofile profiles[VKB_MAX_PDS];
static int nprofiles;
/* test50: every runtime flag its own bit. The PERF line's BC fast verdicts and the PowerVR line's PV-01 / PV-04 forms were
 * merged onto the same bits (0x400000, 0x1000000); one bit twice makes the sum of the flags differ from their OR. */
_Static_assert(((uint64_t)VKB_PROBE_FLAG_LUT_NOW + (uint64_t)VKB_PROBE_FLAG_GS_FOLD + (uint64_t)VKB_PROBE_FLAG_SPIRV_FIX + (uint64_t)VKB_PROBE_FLAG_TX_ETC2 + (uint64_t)VKB_PROBE_FLAG_TX_EAC + (uint64_t)VKB_PROBE_FLAG_TX_ASTC + (uint64_t)VKB_PROBE_FLAG_TX_ASTC_HDR + (uint64_t)VKB_PROBE_FLAG_TX_BC7 + (uint64_t)VKB_PROBE_FLAG_BC_TAB + (uint64_t)VKB_PROBE_FLAG_BC_PAL_ETC2 + (uint64_t)VKB_PROBE_FLAG_BC_PAL_EAC + (uint64_t)VKB_PROBE_FLAG_BC_FAST_ETC2 + (uint64_t)VKB_PROBE_FLAG_BC_FAST_ASTC + (uint64_t)VKB_PROBE_FLAG_WAIT_LOST + (uint64_t)VKB_PROBE_FLAG_MK_WAIT + (uint64_t)VKB_PROBE_FLAG_MK_FLIP + (uint64_t)VKB_PROBE_FLAG_MK_BITWISE + (uint64_t)VKB_PROBE_FLAG_MK_DYN + (uint64_t)VKB_PROBE_FLAG_MK_VB + (uint64_t)VKB_PROBE_FLAG_MK_VB_SINGLE + (uint64_t)VKB_PROBE_FLAG_MK_CHURN + (uint64_t)VKB_PROBE_FLAG_MK_VIEW3D + (uint64_t)VKB_PROBE_FLAG_MK_FP16 + (uint64_t)VKB_PROBE_FLAG_PV_MUTABLE + (uint64_t)VKB_PROBE_FLAG_PV_NAMES + (uint64_t)VKB_PROBE_FLAG_PV_BGRA_STORE + (uint64_t)VKB_PROBE_FLAG_PV_VIEW3D + (uint64_t)VKB_PROBE_FLAG_PV_V3D_ARRAY1 + (uint64_t)VKB_PROBE_FLAG_PV_V3D_2D1 + (uint64_t)VKB_PROBE_FLAG_PV_V3D_COPY + (uint64_t)VKB_PROBE_FLAG_PV_V3D_FB + (uint64_t)VKB_PROBE_FLAG_PV_V3D_FBCOPY + (uint64_t)VKB_PROBE_FLAG_PV_FBLOOP) == ((uint64_t)VKB_PROBE_FLAG_LUT_NOW | (uint64_t)VKB_PROBE_FLAG_GS_FOLD | (uint64_t)VKB_PROBE_FLAG_SPIRV_FIX | (uint64_t)VKB_PROBE_FLAG_TX_ETC2 | (uint64_t)VKB_PROBE_FLAG_TX_EAC | (uint64_t)VKB_PROBE_FLAG_TX_ASTC | (uint64_t)VKB_PROBE_FLAG_TX_ASTC_HDR | (uint64_t)VKB_PROBE_FLAG_TX_BC7 | (uint64_t)VKB_PROBE_FLAG_BC_TAB | (uint64_t)VKB_PROBE_FLAG_BC_PAL_ETC2 | (uint64_t)VKB_PROBE_FLAG_BC_PAL_EAC | (uint64_t)VKB_PROBE_FLAG_BC_FAST_ETC2 | (uint64_t)VKB_PROBE_FLAG_BC_FAST_ASTC | (uint64_t)VKB_PROBE_FLAG_WAIT_LOST | (uint64_t)VKB_PROBE_FLAG_MK_WAIT | (uint64_t)VKB_PROBE_FLAG_MK_FLIP | (uint64_t)VKB_PROBE_FLAG_MK_BITWISE | (uint64_t)VKB_PROBE_FLAG_MK_DYN | (uint64_t)VKB_PROBE_FLAG_MK_VB | (uint64_t)VKB_PROBE_FLAG_MK_VB_SINGLE | (uint64_t)VKB_PROBE_FLAG_MK_CHURN | (uint64_t)VKB_PROBE_FLAG_MK_VIEW3D | (uint64_t)VKB_PROBE_FLAG_MK_FP16 | (uint64_t)VKB_PROBE_FLAG_PV_MUTABLE | (uint64_t)VKB_PROBE_FLAG_PV_NAMES | (uint64_t)VKB_PROBE_FLAG_PV_BGRA_STORE | (uint64_t)VKB_PROBE_FLAG_PV_VIEW3D | (uint64_t)VKB_PROBE_FLAG_PV_V3D_ARRAY1 | (uint64_t)VKB_PROBE_FLAG_PV_V3D_2D1 | (uint64_t)VKB_PROBE_FLAG_PV_V3D_COPY | (uint64_t)VKB_PROBE_FLAG_PV_V3D_FB | (uint64_t)VKB_PROBE_FLAG_PV_V3D_FBCOPY | (uint64_t)VKB_PROBE_FLAG_PV_FBLOOP), "two VKB_PROBE_FLAG_* share a bit");
static uint64_t pflags; /* runtime flags in force (33 of them: wider than 32 bits since test50) */

int vkb_probe_flag(uint64_t f)
{
    return (__atomic_load_n(&pflags, __ATOMIC_ACQUIRE) & f) != 0;
}

static int bcv_state; /* vkb_probe_bc_variants (atomic) */

/* BC fast converters (VKBRIDGE_BC_FAST): on by default only on the GPUs measured as strong (Mali-G925 / Immortalis-G925: BC7 -62%;
 * Mali-G720 / Immortalis-G720: probe 1.3 s once, then 1.4 ms, RE3 bc hitches 6/4 -> 0/0). A weak Mali-G68 paid a 65 s first-launch probe
 * for them and had a worse first session; unmeasured GPUs stay as before. A model name counts when "G925" / "G720" stands as a whole
 * word in it ("Mali-G925-Immortalis MC12", "Immortalis-G720 MC10"; not "G7205"). VKBRIDGE_TEST_BC_GPU=<name> (host tests, the server's
 * environment only) stands in for the GPU's name. */
int vkb_bc_gpu_measured(const char *name)
{
    static const char *const ok[] = {"G925", "G720"};
    if (!name) return 0;
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        for (const char *h = name; (h = strstr(h, ok[i])) != NULL; h++) {
            char after = h[strlen(ok[i])];
            int before_ok = h == name || !isalnum((unsigned char)h[-1]);
            if (before_ok && !isalnum((unsigned char)after)) return 1;
        }
    return 0;
}

/* 1: the BC fast converters (and their variants in the bc_variants probe) are in play on this GPU: the name is measured, or
 * VKBRIDGE_BC_FAST is set (an explicit setting wins both ways). */
int vkb_bc_fast_gate(const char *deviceName, int *explicit_env)
{
    const char *fv = vkb_srv_env("VKBRIDGE_BC_FAST"), *t = getenv("VKBRIDGE_TEST_BC_GPU");
    int ex = fv && *fv;
    if (explicit_env) *explicit_env = ex;
    if (ex) return 1;
    return vkb_bc_gpu_measured(t && *t ? t : deviceName);
}

int vkb_probe_bc_variants(void) { return __atomic_load_n(&bcv_state, __ATOMIC_ACQUIRE); }

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int env_on(const char *name, int def)
{
    const char *v = vkb_srv_env(name);
    if (!v || !*v) return def;
    return strcmp(v, "0") != 0;
}

/* ------------------------------------------------------------------ the probe device */

enum { O_BUF, O_IMG, O_VIEW, O_MEM, O_SAMPLER, O_DSL, O_PL, O_PIPE, O_SHM, O_RP, O_FB, O_DPOOL, O_SEM, O_FENCE, O_PCACHE, O_BVIEW };

typedef struct pobj {
    int kind;
    uint64_t h;
} pobj;

typedef struct pctx {
    const vkb_dispatch *idt;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures feat;
    VkExtensionProperties *ext;
    uint32_t next;
    uint32_t api;
    uint32_t emu;
    VkPhysicalDeviceMemoryProperties mp;
    /* the probe device and what it has on */
    vkb_dispatch d;
    VkDevice dev;
    VkQueue q;
    uint32_t qf;
    VkCommandPool pool;
    int divisor, divisor_zero, depth_clip, push, null_desc, gs, layer_vs, timeline, clip, mdi, bc, ext_fd, dmabuf, drm_mod,
        compression, bindless;
    int ahb; /* VK_ANDROID_external_memory_android_hardware_buffer on, where the driver has no dma-buf import (ahb_share) */
    int eds, eds3_smask, dynrender, multi_vp; /* the MK probes': extended dynamic state (core 1.3 or EXT), its
                                                 EDS3 sample mask, dynamic rendering, 2 viewports from the VS */
    uint32_t possible;                        /* emulations the bridge can stand in with here (emu_possible) */
    const char *divisor_name;
    /* the running probe */
    int st;
    int gpu_stuck; /* a submission never finished, or the device was lost: the device is abandoned */
    int starved;   /* a build reached its limit while it mostly waited for a CPU (guard_starved): the probe not counted */
    char detail[DETAIL];
    size_t dlen;
    pobj objs[96];
    int nobjs;
    /* flags a probe asks for when it does not pass (bc transcoders: per target) */
    uint64_t want_flags;
    uint32_t cases, failed; /* sub-cases run / not passed (MK probes) */
    int layer_vs_passed;
    int tx_status;      /* bc_transcoders' result this profile (-1: none) */
    int fp16; /* shaderFloat16 on the probe device (fp16_math) */
    int pvr;    /* Imagination's driver (driverID, else vendor 0x1010): PV-04's extra sub-cases and the feedback-loop extension */
    int fbloop; /* attachmentFeedbackLoopLayout on the probe device (view3d_layers' feedback-loop sub-cases) */
} pctx;

static double rq_wait_ms(void);

static int has_ext(const pctx *c, const char *name)
{
    for (uint32_t i = 0; i < c->next; i++)
        if (!strcmp(c->ext[i].extensionName, name)) return 1;
    return 0;
}

static void say(pctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(pctx *c, const char *fmt, ...)
{
    if (c->dlen >= sizeof(c->detail) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(c->detail + c->dlen, sizeof(c->detail) - c->dlen, fmt, ap);
    va_end(ap);
    if (n > 0) c->dlen += (size_t)n;
    if (c->dlen > sizeof(c->detail) - 1) c->dlen = sizeof(c->detail) - 1;
}

/* The worse outcome wins (fail over pass; skip and error only where nothing ran). */
static void outcome(pctx *c, int st)
{
    if (st == PS_FAIL || (c->st == PS_PASS && st != PS_PASS)) c->st = st;
}

static void track(pctx *c, int kind, uint64_t h)
{
    if (h && c->nobjs < (int)(sizeof(c->objs) / sizeof(c->objs[0]))) c->objs[c->nobjs++] = (pobj){kind, h};
}

static void release(pctx *c)
{
    const vkb_dispatch *d = &c->d;
    VkDevice v = c->dev;
    while (c->nobjs > 0) {
        pobj o = c->objs[--c->nobjs];
        switch (o.kind) {
        case O_BUF: d->vkDestroyBuffer(v, (VkBuffer)o.h, NULL); break;
        case O_IMG: d->vkDestroyImage(v, (VkImage)o.h, NULL); break;
        case O_VIEW: d->vkDestroyImageView(v, (VkImageView)o.h, NULL); break;
        case O_MEM: d->vkFreeMemory(v, (VkDeviceMemory)o.h, NULL); break;
        case O_SAMPLER: d->vkDestroySampler(v, (VkSampler)o.h, NULL); break;
        case O_DSL: d->vkDestroyDescriptorSetLayout(v, (VkDescriptorSetLayout)o.h, NULL); break;
        case O_PL: d->vkDestroyPipelineLayout(v, (VkPipelineLayout)o.h, NULL); break;
        case O_PIPE: d->vkDestroyPipeline(v, (VkPipeline)o.h, NULL); break;
        case O_SHM: d->vkDestroyShaderModule(v, (VkShaderModule)o.h, NULL); break;
        case O_RP: d->vkDestroyRenderPass(v, (VkRenderPass)o.h, NULL); break;
        case O_FB: d->vkDestroyFramebuffer(v, (VkFramebuffer)o.h, NULL); break;
        case O_DPOOL: d->vkDestroyDescriptorPool(v, (VkDescriptorPool)o.h, NULL); break;
        case O_SEM: d->vkDestroySemaphore(v, (VkSemaphore)o.h, NULL); break;
        case O_FENCE: d->vkDestroyFence(v, (VkFence)o.h, NULL); break;
        case O_PCACHE: d->vkDestroyPipelineCache(v, (VkPipelineCache)o.h, NULL); break;
        case O_BVIEW: d->vkDestroyBufferView(v, (VkBufferView)o.h, NULL); break;
        }
    }
}

#define H(x) ((uint64_t)(uintptr_t)(x))

static uint32_t mem_type(const pctx *c, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = c->mp.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & want) != want) continue;
        if (f & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT)) continue;
        return i;
    }
    return UINT32_MAX;
}

static VkDeviceMemory alloc(pctx *c, VkMemoryRequirements *mr, VkMemoryPropertyFlags want, const void *next)
{
    uint32_t t = mem_type(c, mr->memoryTypeBits, want);
    if (t == UINT32_MAX) t = mem_type(c, mr->memoryTypeBits, 0);
    if (t == UINT32_MAX) return VK_NULL_HANDLE;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, next, mr->size, t};
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (c->d.vkAllocateMemory(c->dev, &ai, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_MEM, H(m));
    return m;
}

/* A host-visible, coherent buffer, mapped and zeroed. */
static VkBuffer buf(pctx *c, VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b = VK_NULL_HANDLE;
    if (c->d.vkCreateBuffer(c->dev, &bi, NULL, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_BUF, H(b));
    VkMemoryRequirements mr;
    c->d.vkGetBufferMemoryRequirements(c->dev, b, &mr);
    VkDeviceMemory m = alloc(c, &mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL);
    if (!m || c->d.vkBindBufferMemory(c->dev, b, m, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    void *p = NULL;
    if (c->d.vkMapMemory(c->dev, m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    memset(p, 0, (size_t)size);
    if (map) *map = p;
    return b;
}

static VkImage img(pctx *c, const VkImageCreateInfo *ci, VkResult *res)
{
    VkImage i = VK_NULL_HANDLE;
    VkResult r = c->d.vkCreateImage(c->dev, ci, NULL, &i);
    if (res) *res = r;
    if (r != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_IMG, H(i));
    VkMemoryRequirements mr;
    c->d.vkGetImageMemoryRequirements(c->dev, i, &mr);
    VkDeviceMemory m = alloc(c, &mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    if (!m || c->d.vkBindImageMemory(c->dev, i, m, 0) != VK_SUCCESS) {
        if (res) *res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return VK_NULL_HANDLE;
    }
    return i;
}

static VkImageView view(pctx *c, VkImage i, VkImageViewType type, VkFormat f, uint32_t layers)
{
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, i, type, f, {0, 0, 0, 0},
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkImageView v = VK_NULL_HANDLE;
    if (c->d.vkCreateImageView(c->dev, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_VIEW, H(v));
    return v;
}

static VkShaderModule module(pctx *c, const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (c->d.vkCreateShaderModule(c->dev, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_SHM, H(m));
    return m;
}

static VkCommandBuffer cmd(pctx *c)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (c->d.vkAllocateCommandBuffers(c->dev, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    c->d.vkBeginCommandBuffer(cb, &bi);
    return cb;
}

/* Ends, submits and waits (3 s: these are tiny); the command buffer goes back to the pool. */
static VkResult submit(pctx *c, VkCommandBuffer cb)
{
    c->d.vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    VkResult r = c->d.vkCreateFence(c->dev, &fi, NULL, &f);
    if (r == VK_SUCCESS) {
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        r = c->d.vkQueueSubmit(c->q, 1, &si, f);
        if (r == VK_SUCCESS) r = c->d.vkWaitForFences(c->dev, 1, &f, VK_TRUE, 3000000000ull);
        if (r == VK_TIMEOUT || r == VK_ERROR_DEVICE_LOST) {
            /* the GPU never finished: the fence and command buffer stay with the abandoned device */
            say(c, r == VK_TIMEOUT ? " [submission still running after 3 s]" : " [device lost]");
            c->gpu_stuck = 1;
            return r;
        }
        c->d.vkDestroyFence(c->dev, f, NULL);
    }
    c->d.vkFreeCommandBuffers(c->dev, c->pool, 1, &cb);
    return r;
}

static void img_barrier(pctx *c, VkCommandBuffer cb, VkImage i, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                        VkPipelineStageFlags ss, VkPipelineStageFlags ds, uint32_t layers)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                              i, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    c->d.vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static void dev_destroy(pctx *c, int abandon)
{
    if (!c->dev) return;
    if (!abandon) {
        release(c);
        if (c->pool) c->d.vkDestroyCommandPool(c->dev, c->pool, NULL);
        c->d.vkDestroyDevice(c->dev, NULL);
    }
    c->nobjs = 0;
    c->dev = VK_NULL_HANDLE;
    c->pool = VK_NULL_HANDLE;
}

/* The probe device: every feature a probe looks at that the driver claims, and nothing else. */
static int dev_create(pctx *c)
{
    const vkb_dispatch *i = c->idt;
    uint32_t nq = 0;
    i->vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    i->vkGetPhysicalDeviceQueueFamilyProperties(c->pd, &nq, qp);
    c->qf = UINT32_MAX;
    for (uint32_t k = 0; k < nq && c->qf == UINT32_MAX; k++)
        if ((qp[k].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) c->qf = k;
    if (c->qf == UINT32_MAX) return 0;

    VkPhysicalDeviceVulkan12Features v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVertexAttributeDivisorFeatures div = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT dc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT r2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceImageCompressionControlFeaturesEXT cc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeatures dr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    const char *exts[28];
    uint32_t ne = 0;
    void *chain = NULL;
    c->divisor_name = has_ext(c, "VK_KHR_vertex_attribute_divisor")   ? "VK_KHR_vertex_attribute_divisor"
                      : has_ext(c, "VK_EXT_vertex_attribute_divisor") ? "VK_EXT_vertex_attribute_divisor"
                                                                       : NULL;
    const char *r2name = has_ext(c, "VK_EXT_robustness2") ? "VK_EXT_robustness2" : has_ext(c, "VK_KHR_robustness2") ? "VK_KHR_robustness2" : NULL;
    /* what the emulations will stand in for is not probed natively */
    if (c->divisor_name && !(c->emu & VKB_EMU_DIVISOR)) div.pNext = chain, chain = &div, exts[ne++] = c->divisor_name;
    if (has_ext(c, "VK_EXT_depth_clip_enable") && !(c->emu & VKB_EMU_DEPTH_CLIP))
        dc.pNext = chain, chain = &dc, exts[ne++] = "VK_EXT_depth_clip_enable";
    if (r2name && !(c->emu & VKB_EMU_ROBUSTNESS2)) r2.pNext = chain, chain = &r2, exts[ne++] = r2name;
    if (has_ext(c, "VK_EXT_image_compression_control")) cc.pNext = chain, chain = &cc, exts[ne++] = "VK_EXT_image_compression_control";
    if (c->api >= VK_API_VERSION_1_2) v12.pNext = chain, chain = &v12;
    /* the MK probes: dynamic state and dynamic rendering, core in 1.3, else their extensions */
    if (c->api >= VK_API_VERSION_1_3) v13.pNext = chain, chain = &v13;
    else {
        if (has_ext(c, "VK_EXT_extended_dynamic_state")) eds.pNext = chain, chain = &eds, exts[ne++] = "VK_EXT_extended_dynamic_state";
        if (has_ext(c, "VK_KHR_dynamic_rendering")) dr.pNext = chain, chain = &dr, exts[ne++] = "VK_KHR_dynamic_rendering";
    }
    if (has_ext(c, "VK_EXT_extended_dynamic_state3")) eds3.pNext = chain, chain = &eds3, exts[ne++] = "VK_EXT_extended_dynamic_state3";
    /* feedback loops (view3d_layers' sub-cases 5-7, pv_view3d_base): asked for on PowerVR only (and under PV-04's or PV-06's host
     * imitation), so the probe device of every other GPU is made as in test49 */
    const char *v3imi = vkb_srv_env("VKBRIDGE_ERRATA_IMITATE");
    if ((c->pvr || (v3imi && (strstr(v3imi, "pv_view3d_base") || strstr(v3imi, "pv_fbloop_split")))) &&
        has_ext(c, "VK_EXT_attachment_feedback_loop_layout") && c->api >= VK_API_VERSION_1_1)
        fbl.pNext = chain, chain = &fbl, exts[ne++] = "VK_EXT_attachment_feedback_loop_layout";
    f2.pNext = chain;
    if (i->vkGetPhysicalDeviceFeatures2) i->vkGetPhysicalDeviceFeatures2(c->pd, &f2);
    else i->vkGetPhysicalDeviceFeatures(c->pd, &f2.features);
    if (has_ext(c, "VK_KHR_push_descriptor") && !(c->emu & VKB_EMU_PUSH_DESC)) exts[ne++] = "VK_KHR_push_descriptor";
    if (has_ext(c, "VK_KHR_external_memory_fd")) exts[ne++] = "VK_KHR_external_memory_fd";
    if (has_ext(c, "VK_EXT_external_memory_dma_buf") && has_ext(c, "VK_KHR_external_memory_fd")) exts[ne++] = "VK_EXT_external_memory_dma_buf";
    /* Android buffers, only where the driver takes no dma-buf: the ahb_share probe. */
    int want_ahb = 0;
#ifdef __ANDROID__
    want_ahb = !has_ext(c, "VK_EXT_external_memory_dma_buf") && c->api >= VK_API_VERSION_1_1 &&
               has_ext(c, "VK_ANDROID_external_memory_android_hardware_buffer") && has_ext(c, "VK_EXT_queue_family_foreign");
    if (want_ahb) exts[ne++] = "VK_ANDROID_external_memory_android_hardware_buffer", exts[ne++] = "VK_EXT_queue_family_foreign";
#endif
    if (has_ext(c, "VK_EXT_image_drm_format_modifier") && c->api >= VK_API_VERSION_1_1) {
        exts[ne++] = "VK_EXT_image_drm_format_modifier";
        if (c->api < VK_API_VERSION_1_2 && has_ext(c, "VK_KHR_image_format_list")) exts[ne++] = "VK_KHR_image_format_list";
    }

    /* Vulkan 1.0 features: only those looked at. */
    VkPhysicalDeviceFeatures want;
    memset(&want, 0, sizeof(want));
    const VkPhysicalDeviceFeatures *have = &f2.features;
    want.geometryShader = have->geometryShader;
    want.multiDrawIndirect = have->multiDrawIndirect && !(c->emu & VKB_EMU_MULTI_DRAW);
    want.shaderClipDistance = have->shaderClipDistance && !(c->emu & VKB_EMU_CLIP_DISTANCE);
    want.depthClamp = have->depthClamp;
    want.textureCompressionBC = have->textureCompressionBC && !(c->emu & VKB_EMU_BCN);
    want.multiViewport = have->multiViewport;
    want.largePoints = 0;
    f2.features = want;
    /* What each structure keeps on (the rest off). */
    VkBool32 rate = div.vertexAttributeInstanceRateDivisor, zero = div.vertexAttributeInstanceRateZeroDivisor;
    memset((char *)&div + offsetof(VkPhysicalDeviceVertexAttributeDivisorFeatures, vertexAttributeInstanceRateDivisor), 0,
           sizeof(div) - offsetof(VkPhysicalDeviceVertexAttributeDivisorFeatures, vertexAttributeInstanceRateDivisor));
    div.vertexAttributeInstanceRateDivisor = rate;
    div.vertexAttributeInstanceRateZeroDivisor = zero;
    VkBool32 null_desc = r2.nullDescriptor;
    r2.robustBufferAccess2 = r2.robustImageAccess2 = VK_FALSE;
    VkPhysicalDeviceVulkan12Features got12 = v12;
    memset((char *)&v12 + offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge), 0,
           sizeof(v12) - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge));
    v12.timelineSemaphore = got12.timelineSemaphore;
    v12.shaderOutputLayer = got12.shaderOutputLayer;
    v12.shaderOutputViewportIndex = got12.shaderOutputViewportIndex;
    v12.shaderFloat16 = got12.shaderFloat16;
    VkBool32 dyn_rendering = c->api >= VK_API_VERSION_1_3 ? v13.dynamicRendering : dr.dynamicRendering;
    memset((char *)&v13 + offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess), 0,
           sizeof(v13) - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess));
    v13.dynamicRendering = dyn_rendering;
    VkBool32 fbloop = fbl.attachmentFeedbackLoopLayout;
    VkBool32 smask = eds3.extendedDynamicState3SampleMask;
    memset((char *)&eds3 + offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin), 0,
           sizeof(eds3) - offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin));
    eds3.extendedDynamicState3SampleMask = smask;
    int bindless = got12.descriptorIndexing && got12.runtimeDescriptorArray && got12.descriptorBindingPartiallyBound &&
                   got12.descriptorBindingVariableDescriptorCount && got12.descriptorBindingSampledImageUpdateAfterBind;
    if (bindless) {
        v12.descriptorIndexing = v12.runtimeDescriptorArray = v12.descriptorBindingPartiallyBound = VK_TRUE;
        v12.descriptorBindingVariableDescriptorCount = v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, c->qf, 1, &prio};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, NULL, ne, exts, NULL};
    if (!i->vkGetPhysicalDeviceFeatures2) di.pNext = NULL, di.pEnabledFeatures = &want;
    VkResult r = i->vkCreateDevice(c->pd, &di, NULL, &c->dev);
    if (r != VK_SUCCESS) {
        VKB_WARN("probe: the probe device was refused (%d, %u extensions)", r, ne);
        c->dev = VK_NULL_HANDLE;
        return 0;
    }
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vkb_gipa(c->inst, "vkGetDeviceProcAddr");
    c->d = *i;
    vkb_dispatch_load_device(&c->d, gdpa, c->dev);
    c->d.vkGetDeviceQueue(c->dev, c->qf, 0, &c->q);
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, c->qf};
    if (c->d.vkCreateCommandPool(c->dev, &pi, NULL, &c->pool) != VK_SUCCESS) {
        dev_destroy(c, 0);
        return 0;
    }
    c->divisor = c->divisor_name && rate && !(c->emu & VKB_EMU_DIVISOR);
    c->divisor_zero = c->divisor && zero;
    c->depth_clip = dc.depthClipEnable && !(c->emu & VKB_EMU_DEPTH_CLIP);
    c->push = has_ext(c, "VK_KHR_push_descriptor") && !(c->emu & VKB_EMU_PUSH_DESC) && c->d.vkCmdPushDescriptorSet;
    c->null_desc = r2name && null_desc && !(c->emu & VKB_EMU_ROBUSTNESS2);
    c->gs = want.geometryShader;
    c->layer_vs = c->api >= VK_API_VERSION_1_2 && got12.shaderOutputLayer;
    c->timeline = c->api >= VK_API_VERSION_1_2 && got12.timelineSemaphore;
    c->clip = want.shaderClipDistance;
    c->mdi = want.multiDrawIndirect;
    c->bc = want.textureCompressionBC;
    c->ext_fd = has_ext(c, "VK_KHR_external_memory_fd") && c->d.vkGetMemoryFdKHR;
    c->dmabuf = c->ext_fd && has_ext(c, "VK_EXT_external_memory_dma_buf");
    c->drm_mod = has_ext(c, "VK_EXT_image_drm_format_modifier") && c->api >= VK_API_VERSION_1_1;
    c->ahb = want_ahb;
    c->compression = cc.imageCompressionControl && c->d.vkGetImageSubresourceLayout2;
    c->bindless = bindless;
    c->eds = (c->api >= VK_API_VERSION_1_3 || eds.extendedDynamicState) && c->d.vkCmdBindVertexBuffers2 && c->d.vkCmdSetDepthWriteEnable &&
             c->d.vkCmdSetCullMode;
    c->eds3_smask = smask && c->d.vkCmdSetSampleMaskEXT;
    c->dynrender = dyn_rendering && c->d.vkCmdBeginRendering && c->d.vkCmdEndRendering;
    c->multi_vp = want.multiViewport && c->api >= VK_API_VERSION_1_2 && got12.shaderOutputViewportIndex;
    c->fp16 = c->api >= VK_API_VERSION_1_2 && got12.shaderFloat16;
    c->fbloop = fbloop && c->api >= VK_API_VERSION_1_1;
    return 1;
}

/* ------------------------------------------------------------------ drawing into 8x1 pixels */

typedef struct rt {
    VkImage img;
    VkRenderPass rp;
    VkFramebuffer fb;
    VkBuffer rb;
    uint8_t *px;
    uint32_t layers;
} rt;

static int rt_make(pctx *c, rt *t, uint32_t layers)
{
    memset(t, 0, sizeof(*t));
    t->layers = layers;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {8, 1, 1}, 1, layers,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    t->img = img(c, &ci, NULL);
    if (!t->img) return 0;
    VkImageView v = view(c, t->img, layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, layers);
    VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar, NULL, NULL, 0, NULL};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 1, &dep};
    if (!v || c->d.vkCreateRenderPass(c->dev, &rpi, NULL, &t->rp) != VK_SUCCESS) return 0;
    track(c, O_RP, H(t->rp));
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, t->rp, 1, &v, 8, 1, layers};
    if (c->d.vkCreateFramebuffer(c->dev, &fi, NULL, &t->fb) != VK_SUCCESS) return 0;
    track(c, O_FB, H(t->fb));
    void *p = NULL;
    t->rb = buf(c, 32u * layers, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &p);
    t->px = p;
    return t->rb != VK_NULL_HANDLE;
}

typedef void (*rec_fn)(pctx *c, VkCommandBuffer cb, void *arg);

static int rt_draw(pctx *c, rt *t, VkPipeline p, rec_fn rec, void *arg)
{
    memset(t->px, 0xEE, 32u * t->layers);
    VkCommandBuffer cb = cmd(c);
    if (!cb) return 0;
    VkClearValue cv;
    memset(&cv, 0, sizeof(cv));
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, t->rp, t->fb, {{0, 0}, {8, 1}}, 1, &cv};
    c->d.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec(c, cb, arg);
    c->d.vkCmdEndRenderPass(cb);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, t->layers}, {0, 0, 0}, {8, 1, 1}};
    c->d.vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rb, 1, &r);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkResult res = submit(c, cb);
    if (res != VK_SUCCESS) say(c, " draw submission %d;", res);
    return res == VK_SUCCESS;
}

static const uint8_t *pxl(const rt *t, uint32_t layer, uint32_t x)
{
    return t->px + layer * 32u + x * 4u;
}

/* The pixels' red (or green) channels as text: "0 255 0 ...". */
static void row_text(const rt *t, uint32_t layer, int ch, char *out, size_t cap)
{
    size_t len = 0;
    for (uint32_t x = 0; x < 8 && len < cap; x++)
        len += (size_t)snprintf(out + len, cap - len, "%s%u", x ? " " : "", pxl(t, layer, x)[ch]);
}

typedef struct gp_desc {
    const uint32_t *vs, *gs, *fs;
    size_t vs_size, gs_size, fs_size;
    const VkPipelineVertexInputStateCreateInfo *vi;
    VkBool32 depth_clamp;
    const void *rast_next;
} gp_desc;

static VkPipelineLayout layout_push(pctx *c, VkShaderStageFlags stages, uint32_t bytes, uint32_t nsets, const VkDescriptorSetLayout *sets)
{
    VkPushConstantRange pr = {stages, 0, bytes};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, nsets, sets, bytes ? 1u : 0u, &pr};
    VkPipelineLayout l = VK_NULL_HANDLE;
    if (c->d.vkCreatePipelineLayout(c->dev, &li, NULL, &l) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_PL, H(l));
    return l;
}

static VkPipeline gfx(pctx *c, const rt *t, VkPipelineLayout pl, const gp_desc *g, VkPipelineCache cache, VkResult *res)
{
    VkPipelineShaderStageCreateInfo st[3];
    uint32_t n = 0;
    VkShaderModule vs = module(c, g->vs, g->vs_size), fs = module(c, g->fs, g->fs_size);
    VkShaderModule gs = g->gs ? module(c, g->gs, g->gs_size) : VK_NULL_HANDLE;
    if (!vs || !fs || (g->gs && !gs)) {
        *res = VK_ERROR_INITIALIZATION_FAILED;
        return VK_NULL_HANDLE;
    }
    st[n++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL};
    if (gs)
        st[n++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main", NULL};
    st[n++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL};
    VkPipelineVertexInputStateCreateInfo vi0 = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0};
    VkViewport vp = {0, 0, 8, 1, 0, 1};
    VkRect2D sc = {{0, 0}, {8, 1}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, g->rast_next, 0, g->depth_clamp, 0,
                                                 VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {0, 0, 0, 0, 0, 0, 0, 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &ba, {0, 0, 0, 0}};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, n, st, g->vi ? g->vi : &vi0, &ia, NULL, &vps, &rs,
                                       &ms, NULL, &cb, NULL, pl, t->rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    *res = c->d.vkCreateGraphicsPipelines(c->dev, cache, 1, &pi, NULL, &p);
    if (*res != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_PIPE, H(p));
    return p;
}

typedef struct vid_pc {
    float col[4];
    float z;
} vid_pc;

/* ------------------------------------------------------------------ compute */

static VkPipeline comp(pctx *c, const uint32_t *code, size_t size, VkPipelineLayout pl, const VkSpecializationInfo *si, VkResult *res)
{
    VkShaderModule m = module(c, code, size);
    if (!m) {
        *res = VK_ERROR_INITIALIZATION_FAILED;
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", si},
                                      pl, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    *res = c->d.vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (*res != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_PIPE, H(p));
    return p;
}

static VkDescriptorSetLayout dsl(pctx *c, const VkDescriptorSetLayoutBinding *b, uint32_t n, VkDescriptorSetLayoutCreateFlags flags,
                                 const void *next)
{
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, next, flags, n, b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    if (c->d.vkCreateDescriptorSetLayout(c->dev, &ci, NULL, &l) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_DSL, H(l));
    return l;
}

static VkDescriptorSet set_alloc(pctx *c, VkDescriptorSetLayout l)
{
    VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
                                 {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 4, ps};
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (c->d.vkCreateDescriptorPool(c->dev, &pi, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_DPOOL, H(p));
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, p, 1, &l};
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (c->d.vkAllocateDescriptorSets(c->dev, &ai, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

static int dispatch(pctx *c, VkPipeline p, VkPipelineLayout pl, VkDescriptorSet s, const void *pc, uint32_t pc_size)
{
    VkCommandBuffer cb = cmd(c);
    if (!cb) return 0;
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    if (s) c->d.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s, 0, NULL);
    if (pc_size) c->d.vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_size, pc);
    c->d.vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkResult r = submit(c, cb);
    if (r != VK_SUCCESS) say(c, " dispatch submission %d;", r);
    return r == VK_SUCCESS;
}

/* ------------------------------------------------------------------ the probes */

static void p_identity(pctx *c)
{
    VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
    if (c->api >= VK_API_VERSION_1_2 && c->idt->vkGetPhysicalDeviceProperties2) c->idt->vkGetPhysicalDeviceProperties2(c->pd, &p2);
    int pvr = vkb_driver_is_powervr(&c->props, dp.driverID);
    say(c, "%s, Vulkan %u.%u.%u, %s", c->props.deviceName, VK_API_VERSION_MAJOR(c->props.apiVersion),
        VK_API_VERSION_MINOR(c->props.apiVersion), VK_API_VERSION_PATCH(c->props.apiVersion), pvr ? "PowerVR driver" : "driver");
    if (pvr) say(c, " CL %u (0x%08x)", c->props.driverVersion, c->props.driverVersion);
    else say(c, " 0x%08x", c->props.driverVersion);
    if (dp.driverName[0]) say(c, " (%s: %s)", dp.driverName, dp.driverInfo);
    say(c, ", %u device extensions", c->next);
    int any = 0;
    for (size_t i = 0; i < VKB_EXT_ALIAS_COUNT; i++)
        if (!has_ext(c, vkb_ext_aliases[i].app) && has_ext(c, vkb_ext_aliases[i].driver)) {
            say(c, "%s%s only as %s", any ? ", " : "; renamed: ", vkb_ext_aliases[i].app + 3, vkb_ext_aliases[i].driver + 3);
            any = 1;
        }
    outcome(c, PS_PASS);
}

/* Memory types: an allocation in each (mapped and written where host-visible), and where images go. */
static void p_memory_types(pctx *c)
{
    int bad = 0;
    for (uint32_t t = 0; t < c->mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags f = c->mp.memoryTypes[t].propertyFlags;
        say(c, "%s%u:0x%x", t ? " " : "", t, f);
        if (f & VK_MEMORY_PROPERTY_PROTECTED_BIT) {
            say(c, "=protected");
            continue;
        }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 1u << 20, t};
        VkDeviceMemory m = VK_NULL_HANDLE;
        VkResult r = c->d.vkAllocateMemory(c->dev, &ai, NULL, &m);
        if (r != VK_SUCCESS) {
            say(c, "=%d", r);
            if (!(f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) bad = 1;
            continue;
        }
        if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            void *p = NULL;
            r = c->d.vkMapMemory(c->dev, m, 0, VK_WHOLE_SIZE, 0, &p);
            if (r == VK_SUCCESS && p) {
                ((volatile uint32_t *)p)[1000] = 0x5eed;
                say(c, ((volatile uint32_t *)p)[1000] == 0x5eed ? "=ok,mapped" : "=map-readback-wrong");
                c->d.vkUnmapMemory(c->dev, m);
            } else {
                say(c, "=map%d", r);
                bad = 1;
            }
        } else {
            say(c, "=ok");
        }
        c->d.vkFreeMemory(c->dev, m, NULL);
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {256, 256, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage i = VK_NULL_HANDLE;
    if (c->d.vkCreateImage(c->dev, &ci, NULL, &i) == VK_SUCCESS) {
        track(c, O_IMG, H(i));
        VkMemoryRequirements mr;
        c->d.vkGetImageMemoryRequirements(c->dev, i, &mr);
        uint32_t t = mem_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        say(c, "; images: types 0x%x, ordinary device-local %s%u", mr.memoryTypeBits, t == UINT32_MAX ? "NONE " : "", t);
        if (t == UINT32_MAX) bad = 1;
        else {
            VkDeviceMemory m = alloc(c, &mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
            VkResult r = m ? c->d.vkBindImageMemory(c->dev, i, m, 0) : VK_ERROR_OUT_OF_DEVICE_MEMORY;
            if (r != VK_SUCCESS) say(c, " (bind %d)", r), bad = 1;
        }
    }
    outcome(c, bad ? PS_FAIL : PS_PASS);
}

/* dma-heaps the app's context may open (the export emulation and the dmaheap strategy use them). */
static void p_dma_heaps(pctx *c)
{
    DIR *d = opendir("/dev/dma_heap");
    if (!d) {
        say(c, "no /dev/dma_heap (%s)", strerror(errno));
        outcome(c, PS_SKIP);
        return;
    }
    int ok = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[300];
        snprintf(p, sizeof(p), "/dev/dma_heap/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        say(c, "%s%s", c->dlen ? " " : "", e->d_name);
        if (fd >= 0) {
            say(c, "=ok");
            ok++;
            close(fd);
        } else {
            say(c, "=%s", strerror(errno));
        }
    }
    closedir(d);
    say(c, "; the bridge's dma-heap: %s", vkb_mem_dmaheap_available() ? "available" : "NOT available");
    if (!vkb_mem_dmaheap_available() && *vkb_mem_ahb_export_result())
        say(c, " (dma-buf export from AHardwareBuffers: %s)", vkb_mem_ahb_export_result());
    /* needed where the driver exports no dma-bufs (the export emulation, the dmaheap strategy) */
    if (!ok && c->dmabuf) say(c, " (not needed: the driver has dma-buf memory)");
    outcome(c, ok ? PS_PASS : c->dmabuf ? PS_SKIP : PS_FAIL);
}

/* A real export of an image's memory as a dma-buf, where the driver says it can (gamescope's need). */
static void p_dmabuf_export(pctx *c)
{
    VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL,
                                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
                                           VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0};
    VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
    VkResult r = c->idt->vkGetPhysicalDeviceImageFormatProperties2(c->pd, &ii, &ip);
    VkExternalMemoryFeatureFlags ef = r == VK_SUCCESS ? ep.externalMemoryProperties.externalMemoryFeatures : 0;
    say(c, "query (optimal RGBA8): %d, features 0x%x", r, ef);
    if (!(ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        say(c, "; not claimed exportable%s%s", (c->emu & VKB_EMU_DMABUF_EXPORT) ? " (emulated, pages from the " : "",
            (c->emu & VKB_EMU_DMABUF_EXPORT) ? vkb_mem_export_backing() == 2 ? "AHardwareBuffer)" : "dma-heap)" : "");
        outcome(c, PS_SKIP);
        return;
    }
    VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {64, 64, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage i = VK_NULL_HANDLE;
    r = c->d.vkCreateImage(c->dev, &ci, NULL, &i);
    if (r != VK_SUCCESS) {
        say(c, "; image refused: %d", r);
        outcome(c, PS_FAIL);
        return;
    }
    track(c, O_IMG, H(i));
    VkMemoryRequirements mr;
    c->d.vkGetImageMemoryRequirements(c->dev, i, &mr);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, i, VK_NULL_HANDLE};
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkDeviceMemory m = alloc(c, &mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ex);
    if (!m) {
        say(c, "; exportable allocation refused");
        outcome(c, PS_FAIL);
        return;
    }
    c->d.vkBindImageMemory(c->dev, i, m, 0);
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, m, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    r = c->d.vkGetMemoryFdKHR(c->dev, &gi, &fd);
    say(c, "; export: %d (fd %s)", r, fd >= 0 ? "ok" : "none");
    if (fd >= 0) close(fd);
    outcome(c, r == VK_SUCCESS && fd >= 0 ? PS_PASS : PS_FAIL);
}

/* Shared images at LINEAR tiling without a modifier (Zink's; Mali refused them - Skyrim SE), and
 * whether the format query agrees with the create. */
static void p_dmabuf_linear(pctx *c)
{
    static const struct {
        VkImageType type;
        VkFormat f;
        uint32_t w, h;
        const char *what;
    } cases[] = {{VK_IMAGE_TYPE_1D, VK_FORMAT_R32_SFLOAT, 4096, 1, "1D 4096x1 R32F"}, {VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, "2D 8x8 RGBA8"}};
    int refused = 0, disagree = 0;
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL,
                                                      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, cases[k].f, cases[k].type,
                                               VK_IMAGE_TILING_LINEAR, u, 0};
        VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
        VkResult q = c->idt->vkGetPhysicalDeviceImageFormatProperties2(c->pd, &ii, &ip);
        VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, cases[k].type, cases[k].f, {cases[k].w, cases[k].h, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage i = VK_NULL_HANDLE;
        VkResult r = c->d.vkCreateImage(c->dev, &ci, NULL, &i);
        if (r == VK_SUCCESS) track(c, O_IMG, H(i));
        say(c, "%s%s: query %d, create %d", k ? "; " : "", cases[k].what, q, r);
        refused |= r != VK_SUCCESS;
        disagree |= (q == VK_SUCCESS) != (r == VK_SUCCESS);
    }
    if (refused) say(c, " - handled at run time: such images are made without the handle type (server_emu_layer.c)");
    if (disagree) say(c, "; the format query and the create disagree");
    outcome(c, refused ? PS_FAIL : PS_PASS);
}

/* ahb_share: export, import and CPU-read checks of an image over an AHardwareBuffer; recorded only. */
#ifdef __ANDROID__
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} probe_ahb_desc;
typedef struct {
    int version, numFds, numInts;
    int data[0];
} probe_native_handle;
static struct {
    int state;
    int (*allocate)(const probe_ahb_desc *, struct AHardwareBuffer **);
    void (*release)(struct AHardwareBuffer *);
    void (*describe)(const struct AHardwareBuffer *, probe_ahb_desc *);
    const probe_native_handle *(*handle)(const struct AHardwareBuffer *);
} pahb;

static int pahb_load(void)
{
    if (pahb.state) return pahb.state > 0;
    pahb.state = -1;
    void *h = dlopen("libnativewindow.so", RTLD_NOW);
    if (!h) h = dlopen("libandroid.so", RTLD_NOW);
    if (!h) return 0;
    *(void **)&pahb.allocate = dlsym(h, "AHardwareBuffer_allocate");
    *(void **)&pahb.release = dlsym(h, "AHardwareBuffer_release");
    *(void **)&pahb.describe = dlsym(h, "AHardwareBuffer_describe");
    *(void **)&pahb.handle = dlsym(h, "AHardwareBuffer_getNativeHandle");
    if (pahb.allocate && pahb.release && pahb.describe && pahb.handle) pahb.state = 1;
    return pahb.state > 0;
}

#define PAHB_CLEAR 0xffbf8040u /* RGBA8 (0.25, 0.5, 0.75, 1.0) as bytes 40 80 bf ff */

static int near_rgba(uint32_t got)
{
    for (int k = 0; k < 4; k++) {
        int a = (int)((got >> (8 * k)) & 0xff), b = (int)((PAHB_CLEAR >> (8 * k)) & 0xff);
        if (a - b > 1 || b - a > 1) return 0;
    }
    return 1;
}

/* An image over an AHardwareBuffer: imported (ahb set) or exported (ahb NULL, *out_ahb gets it with a reference). */
static VkImage ahb_image(pctx *c, struct AHardwareBuffer *ahb, struct AHardwareBuffer **out_ahb, uint32_t w, uint32_t h, VkResult *res)
{
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vkb_gipa(c->inst, "vkGetDeviceProcAddr");
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID props =
        gdpa ? (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)gdpa(c->dev, "vkGetAndroidHardwareBufferPropertiesANDROID") : NULL;
    PFN_vkGetMemoryAndroidHardwareBufferANDROID get =
        gdpa ? (PFN_vkGetMemoryAndroidHardwareBufferANDROID)gdpa(c->dev, "vkGetMemoryAndroidHardwareBufferANDROID") : NULL;
    *res = VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!props || !get) return VK_NULL_HANDLE;
    VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage i = VK_NULL_HANDLE;
    if ((*res = c->d.vkCreateImage(c->dev, &ci, NULL, &i)) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_IMG, H(i));
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, i, VK_NULL_HANDLE};
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, ahb};
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded,
                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkMemoryRequirements mr;
    c->d.vkGetImageMemoryRequirements(c->dev, i, &mr);
    if (ahb) {
        VkAndroidHardwareBufferPropertiesANDROID hp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        if ((*res = props(c->dev, ahb, &hp)) != VK_SUCCESS) return VK_NULL_HANDLE;
        mr.size = hp.allocationSize;
        mr.memoryTypeBits &= hp.memoryTypeBits;
    }
    /* an exported one: allocationSize 0, the driver sizes the buffer */
    if (!ahb) mr.size = 0;
    VkDeviceMemory m = alloc(c, &mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ahb ? (const void *)&imp : (const void *)&ex);
    if (!m) {
        *res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return VK_NULL_HANDLE;
    }
    if ((*res = c->d.vkBindImageMemory(c->dev, i, m, 0)) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (!ahb) {
        VkMemoryGetAndroidHardwareBufferInfoANDROID gi = {VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, m};
        if ((*res = get(c->dev, &gi, out_ahb)) != VK_SUCCESS || !*out_ahb) return VK_NULL_HANDLE;
    }
    return i;
}

/* Clears i (from UNDEFINED) and releases it to the foreign queue family in GENERAL. */
static VkResult ahb_clear_release(pctx *c, VkImage i)
{
    VkCommandBuffer cb = cmd(c);
    if (!cb) return VK_ERROR_OUT_OF_HOST_MEMORY;
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkClearColorValue col = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}};
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    c->d.vkCmdClearColorImage(cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_GENERAL, c->qf, VK_QUEUE_FAMILY_FOREIGN_EXT, i, rng};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    return submit(c, cb);
}

/* Acquires i from the foreign family and copies its texel (x, y) into a word. */
static VkResult ahb_read(pctx *c, VkImage i, uint32_t x, uint32_t y, uint32_t *out)
{
    void *map = NULL;
    VkBuffer b = buf(c, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &map);
    VkCommandBuffer cb = b ? cmd(c) : VK_NULL_HANDLE;
    if (!cb) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier a = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, c->qf, i, rng};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &a);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(int32_t)x, (int32_t)y, 0}, {1, 1, 1}};
    c->d.vkCmdCopyImageToBuffer(cb, i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    VkResult res = submit(c, cb);
    if (res == VK_SUCCESS) *out = *(volatile uint32_t *)map;
    return res;
}
#endif

static void p_ahb_share(pctx *c)
{
#ifndef __ANDROID__
    say(c, "Android only");
    outcome(c, PS_SKIP);
#else
    if (c->dmabuf || has_ext(c, "VK_EXT_external_memory_dma_buf")) {
        say(c, "not needed: the driver takes dma-bufs");
        outcome(c, PS_SKIP);
        return;
    }
    if (!c->ahb || !pahb_load()) {
        say(c, "no Android buffers here (%s)", c->ahb ? "libnativewindow functions missing" : "no VK_ANDROID_external_memory_android_hardware_buffer");
        outcome(c, PS_SKIP);
        return;
    }
    int bad = 0;
    const uint32_t W = 64, H = 64;
    /* export -> import */
    struct AHardwareBuffer *ahb = NULL;
    VkResult r;
    VkImage a = ahb_image(c, NULL, &ahb, W, H, &r);
    if (!a) {
        say(c, "export: %d", r);
        bad = 1;
    } else {
        probe_ahb_desc d;
        memset(&d, 0, sizeof(d));
        pahb.describe(ahb, &d);
        const probe_native_handle *nh = pahb.handle(ahb);
        say(c, "export: %ux%u format %u stride %u usage 0x%llx, %d fd(s)", d.width, d.height, d.format, d.stride,
            (unsigned long long)d.usage, nh ? nh->numFds : -1);
        r = ahb_clear_release(c, a);
        VkImage b = r == VK_SUCCESS ? ahb_image(c, ahb, NULL, W, H, &r) : VK_NULL_HANDLE;
        uint32_t got = 0;
        if (b) r = ahb_read(c, b, W / 2, H / 2, &got);
        if (!b || r != VK_SUCCESS) say(c, "; import: %d", r), bad = 1;
        else {
            say(c, "; import: texel 0x%08x (want 0x%08x)%s", got, PAHB_CLEAR, near_rgba(got) ? "" : " WRONG");
            bad |= !near_rgba(got);
        }
        pahb.release(ahb);
    }
    /* our own CPU-readable buffer: the layout a copying compositor would read */
    if (!c->gpu_stuck) {
        probe_ahb_desc want = {W, H, 1, 1 /* R8G8B8A8_UNORM */, 3ull /* CPU_READ_OFTEN */ | (1ull << 8) | (1ull << 9), 0, 0, 0};
        struct AHardwareBuffer *own = NULL;
        int ar = pahb.allocate(&want, &own);
        if (ar != 0 || !own) {
            say(c, "; cpu: allocate %d", ar);
            bad = 1;
        } else {
            probe_ahb_desc d;
            memset(&d, 0, sizeof(d));
            pahb.describe(own, &d);
            VkImage i = ahb_image(c, own, NULL, W, H, &r);
            if (i) r = ahb_clear_release(c, i);
            const probe_native_handle *nh = pahb.handle(own);
            uint64_t need = (uint64_t)d.stride * 4u * H;
            int fd = -1;
            for (int k = 0; nh && k < nh->numFds && fd < 0; k++) {
                off_t end = lseek(nh->data[k], 0, SEEK_END);
                if (end > 0 && (uint64_t)end >= need) fd = nh->data[k];
            }
            if (!i || r != VK_SUCCESS) say(c, "; cpu: import/clear %d", r), bad = 1;
            else if (fd < 0) say(c, "; cpu: stride %u, no fd of %llu bytes", d.stride, (unsigned long long)need), bad = 1;
            else {
                void *p = mmap(NULL, (size_t)need, PROT_READ, MAP_SHARED, fd, 0);
                if (p == MAP_FAILED) say(c, "; cpu: mmap %s", strerror(errno)), bad = 1;
                else {
                    struct {
                        uint64_t flags;
                    } sy = {1 /* DMA_BUF_SYNC_READ */ | 0 /* START */};
                    ioctl(fd, 0x40086200 /* DMA_BUF_IOCTL_SYNC */, &sy);
                    uint32_t got = ((volatile uint32_t *)p)[(H / 2) * d.stride + W / 2];
                    sy.flags = 1 | 4 /* END */;
                    ioctl(fd, 0x40086200, &sy);
                    munmap(p, (size_t)need);
                    say(c, "; cpu: stride %u, texel 0x%08x%s", d.stride, got, near_rgba(got) ? "" : " WRONG (not linear?)");
                    bad |= !near_rgba(got);
                }
            }
            pahb.release(own);
        }
    }
    outcome(c, bad ? PS_FAIL : PS_PASS);
#endif
}

/* Layouts of thin LINEAR images: rowPitch 0 broke Chromium (CEF's GPU process). */
static void p_linear_layout(pctx *c)
{
    static const uint32_t ext[][2] = {{1, 1}, {1, 16}, {16, 1}};
    int bad = 0;
    for (size_t k = 0; k < 3; k++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {ext[k][0], ext[k][1], 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkResult r;
        VkImage i = img(c, &ci, &r);
        if (!i) {
            say(c, "%s%ux%u: %d", k ? "; " : "", ext[k][0], ext[k][1], r);
            continue;
        }
        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout l;
        c->d.vkGetImageSubresourceLayout(c->dev, i, &sub, &l);
        say(c, "%s%ux%u: rowPitch %llu size %llu", k ? "; " : "", ext[k][0], ext[k][1], (unsigned long long)l.rowPitch, (unsigned long long)l.size);
        bad |= !l.rowPitch || !l.size;
    }
    if (bad) say(c, " - patched at run time (the server reports the row's size)");
    outcome(c, bad ? PS_FAIL : PS_PASS);
}

/* Compressed formats the GPU samples: what BC textures can be transcoded to. */
static void p_formats(pctx *c)
{
    static const struct {
        VkFormat f;
        const char *n;
    } fs[] = {{VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1"}, {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"},          {VK_FORMAT_BC4_UNORM_BLOCK, "BC4"},
              {VK_FORMAT_BC5_UNORM_BLOCK, "BC5"},     {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H"},       {VK_FORMAT_BC7_UNORM_BLOCK, "BC7"},
              {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, "ETC2"}, {VK_FORMAT_EAC_R11G11_SNORM_BLOCK, "EAC-RG11s"},
              {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC4x4"}, {VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK, "ASTC4x4-HDR"}};
    for (size_t k = 0; k < sizeof(fs) / sizeof(fs[0]); k++) {
        VkFormatProperties fp;
        c->idt->vkGetPhysicalDeviceFormatProperties(c->pd, fs[k].f, &fp);
        VkFormatFeatureFlags o = fp.optimalTilingFeatures;
        say(c, "%s%s:%s%s%s", k ? " " : "", fs[k].n, o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "s" : "-",
            o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? "f" : "-", o & VK_FORMAT_FEATURE_TRANSFER_DST_BIT ? "t" : "-");
    }
    say(c, " (s sampled, f linear filter, t transfer dst)");
    outcome(c, PS_PASS);
}

/* Native BC: one BC1 block (pure red) sampled. */
static void p_bc_native(pctx *c)
{
    if (!c->bc) {
        say(c, "%s", (c->emu & VKB_EMU_BCN) ? "BC emulated" : "no textureCompressionBC");
        outcome(c, PS_SKIP);
        return;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, {4, 4, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r;
    VkImage i = img(c, &ci, &r);
    void *src = NULL, *out = NULL;
    VkBuffer sb = buf(c, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src), ob = buf(c, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out);
    VkImageView v = i ? view(c, i, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 1) : VK_NULL_HANDLE;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler smp = VK_NULL_HANDLE;
    if (!i || !sb || !ob || !v || c->d.vkCreateSampler(c->dev, &si, NULL, &smp) != VK_SUCCESS) {
        say(c, "setup failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    track(c, O_SAMPLER, H(smp));
    /* colour0 = 0xF800 (red), colour1 = 0x001F (blue), every index 0 */
    const uint32_t block[2] = {0x001FF800u, 0};
    memcpy(src, block, sizeof(block));
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayout l = dsl(c, b, 2, 0, NULL);
    VkPipelineLayout pl = l ? layout_push(c, 0, 0, 1, &l) : VK_NULL_HANDLE;
    VkPipeline p = pl ? comp(c, spv_probe_tex, sizeof(spv_probe_tex), pl, NULL, &r) : VK_NULL_HANDLE;
    VkDescriptorSet s = p ? set_alloc(c, l) : VK_NULL_HANDLE;
    if (!s) {
        say(c, "pipeline/set failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    VkDescriptorBufferInfo bi = {ob, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo ii = {smp, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL}};
    c->d.vkUpdateDescriptorSets(c->dev, 2, w, 0, NULL);
    VkCommandBuffer cb = cmd(c);
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
    c->d.vkCmdCopyBufferToImage(cb, sb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
    if (submit(c, cb) != VK_SUCCESS || !dispatch(c, p, pl, s, NULL, 0)) {
        outcome(c, PS_ERROR);
        return;
    }
    const uint32_t *o = out;
    say(c, "BC1 red block read as %u,%u,%u,%u", o[0], o[1], o[2], o[3]);
    outcome(c, o[0] >= 250 && o[1] <= 5 && o[2] <= 5 && o[3] >= 250 ? PS_PASS : PS_FAIL);
}

/* The bridge's own BC transcoders (server_emu_bcn.c), each built on the driver: on a GPU that
 * needs BC emulated these are compiled on a game's first texture upload. */
static double sub_limit_s(void);

typedef struct tx_build {
    pctx *c;
    VkPipelineCache cache;
    int index;
    VkResult r;
    double ms;
    long fake_ms; /* VKBRIDGE_PROBE_TX_FAKE: that much longer (host tests) */
} tx_build;

static void tx_one(void *arg)
{
    tx_build *b = arg;
    struct timespec ts = {b->fake_ms / 1000, (b->fake_ms % 1000) * 1000000L};
    while (b->fake_ms > 0 && nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    b->r = vkb_emu_bcn_build_transcoder(&b->c->d, b->c->dev, b->cache, b->index, &b->ms);
    if (b->fake_ms > 0) b->ms += (double)b->fake_ms;
}

/* The limit of one transcoder's build. Not the probes' 4 s: these are the bridge's own shaders,
 * known to be right, and the question is whether the driver builds them, not how fast. The
 * Mali-G1-Ultra (r54p1) took 1.5-3.3 s over the others where the G720 takes 0.3-0.4 s, and
 * BC7->ASTC (the biggest, about twice the BC3 one everywhere) went past 4 s: it was interrupted
 * as a hang, and with it BC2 and BC3 were decoded too. A build is CPU work only (nothing reaches
 * the GPU, so nothing to reset), in the background part of the probe; but interrupting it jumps
 * out of the driver's compiler, which may then hold a lock: that is worth doing only for a build
 * that is really stuck. VKBRIDGE_PROBE_TX_TIMEOUT_S, at least the probes' own limit. */
#define PROBE_TX_TIMEOUT_S 20.0
static double tx_limit_s(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_PROBE_TX_TIMEOUT_S");
    double t = v && *v ? atof(v) : PROBE_TX_TIMEOUT_S;
    return t > sub_limit_s() ? t : sub_limit_s();
}

/* A guarded build that reached its limit (VKB_GUARD_TIMED_OUT) while this thread mostly waited for a CPU (the background
 * probes run at the lowest priority, a game may hold every core): without that wait it would have ended inside its limit,
 * so the limit did not measure the build. Marks the probe not counted (run_probes: run again at the next start). */
static int guard_starved(pctx *c, double w0, double t0, double limit_s)
{
    double w = w0 >= 0 ? rq_wait_ms() - w0 : 0, ms = mono_ms() - t0;
    if (w < 1000 || ms - w >= 0.75 * limit_s * 1000.0) return 0;
    c->starved = 1;
    say(c, " (%.1f s of it waiting for a CPU)", w / 1000);
    return 1;
}

/* VKBRIDGE_PROBE_TX_FAKE=<name>:<ms>,...: that transcoder's build takes ms longer (host tests). */
static long tx_fake_ms(const char *name)
{
    const char *v = vkb_srv_env("VKBRIDGE_PROBE_TX_FAKE");
    size_t n = strlen(name);
    for (const char *p = v; p && (p = strstr(p, name)); p += n)
        if ((p == v || p[-1] == ',') && p[n] == ':') return atol(p + n + 1);
    return 0;
}

typedef struct bcv_check {
    pctx *c;
    VkPipelineCache cache;
    int index, r;
    double ms;
    char why[160];
} bcv_check;

static void bcv_one(void *arg)
{
    bcv_check *b = arg;
    b->r = vkb_emu_bcn_check_variant(&b->c->d, b->c->dev, b->c->q, b->c->qf, &b->c->mp, b->cache, b->index, &b->ms, b->why, sizeof(b->why));
}

static void p_bc_transcoders(pctx *c)
{
    if (!(c->emu & VKB_EMU_BCN)) {
        say(c, "BC native: no transcoding");
        outcome(c, PS_SKIP);
        return;
    }
    if (!vkb_emu_bcn_transcode_wanted()) {
        say(c, "VKBRIDGE_BC_TRANSCODE=0");
        outcome(c, PS_SKIP);
        return;
    }
    /* through a persistent cache of the probe's own: about 1.9 s of compiles on the G720 without
     * it, at every probe (a re-probe after a stopped session included) */
    char path[512];
    VkPipelineCache cache = vkb_pcache_own_open(&c->d, &c->props, c->dev, "probe", path, sizeof(path));
    int n = vkb_emu_bcn_transcoders(), stuck = 0, slow = 0;
    double limit = tx_limit_s();
    for (int k = 0; k < n; k++) {
        uint32_t flag = 0;
        const char *name = vkb_emu_bcn_transcoder_name(k, &flag);
        tx_build b = {c, cache, k, VK_ERROR_UNKNOWN, 0, tx_fake_ms(name)};
        int sig = 0;
        double w0 = rq_wait_ms(), t0 = mono_ms();
        int g = vkb_guard_run("probe: BC transcoder", limit, tx_one, &b, &sig);
        if (g == VKB_GUARD_DONE && b.r == VK_SUCCESS) {
            int s = b.ms > sub_limit_s() * 1000.0;
            slow += s;
            say(c, "%s%s %.0f ms%s", k ? ", " : "", name, b.ms, s ? " (slow)" : "");
            continue;
        }
        if (g == VKB_GUARD_TIMED_OUT) {
            say(c, "%s%s HANG (still compiling after %.0f s, interrupted; a compile, no GPU work)", k ? ", " : "", name, limit);
            guard_starved(c, w0, t0, limit);
        }
        else
            say(c, "%s%s %s", k ? ", " : "", name, g == VKB_GUARD_CRASHED ? "CRASH" : "FAILED");
        c->want_flags |= flag;
        outcome(c, g == VKB_GUARD_TIMED_OUT ? PS_HANG : g == VKB_GUARD_CRASHED ? PS_CRASH : PS_FAIL);
        if (g != VKB_GUARD_DONE) { /* the driver may hold a lock now: no more compiles here */
            stuck = 1;
            /* and those not tried are decoded too (they went on to be used untested before) */
            for (int j = k + 1; j < n; j++) {
                uint32_t f = 0;
                const char *rest = vkb_emu_bcn_transcoder_name(j, &f);
                say(c, ", %s not tried", rest);
                c->want_flags |= f;
            }
            break;
        }
    }
    if (slow && !stuck) say(c, " (slow: over %.0f s, under the %.0f s limit)", sub_limit_s(), limit);
    /* after a hang or a crash the cache is left as it is (the build may still be in it) */
    if (!stuck) vkb_pcache_own_close(&c->d, c->dev, cache, path);
    if (c->want_flags) say(c, " - formats of a failed transcoder are decoded instead");
    outcome(c, PS_PASS);
}

/* The bc_variants verdict, kept apart from the device profile: the profile's key holds the bridge build (VKB_GIT), so every update
 * of the bridge probed again - ~64 s of the driver compiling ~100 converter pipelines in the background of the first game session on
 * a Galaxy A35 (Mali-G68). The compile work is the cost (the blocks checked are a few hundred bytes), so the answer is kept for
 * what it depends on: the GPU, the driver build and its pipeline-cache UUID, and the converters compared (a hash of their SPIR-V).
 * Only a complete answer is kept (every variant built, ran and passed, or ran and differed: both are deterministic); a hang, a
 * crash, a build or setup failure, VKBRIDGE_PROBE=force and the imitation switches leave no file and are never read from one.
 * No file, a damaged one or another key: the probe runs as before; nothing passed: the old converters (vkb_probe_bc_variants). */
static int bcv_bg; /* the probe runs in the background thread, after the server listens */
static int clients_gone;

static int bcv_verdict_path(pctx *c, char *key, size_t kcap, char *path, size_t pcap)
{
    const char *dir = vkb_pcache_dir(), *salt = vkb_srv_env("VKBRIDGE_TEST_BCV_SALT"), *mode = vkb_srv_env("VKBRIDGE_PROBE"), *fake = vkb_srv_env("VKBRIDGE_PROBE_BCV_FAKE");
    if (!dir || (mode && !strcmp(mode, "force")) || (fake && *fake) || vkb_srv_env("VKBRIDGE_PROBE_BCV_NOCACHE")) return 0;
    char u[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(u + 2 * i, 3, "%02x", c->props.pipelineCacheUUID[i]);
    snprintf(key, kcap, "bcv1:%04x:%04x:%08x:%s:%016llx:%s%s", c->props.vendorID, c->props.deviceID, c->props.driverVersion, u,
             (unsigned long long)vkb_emu_bcn_variants_hash(), vkb_bc_fast_gate(c->props.deviceName, NULL) ? "" : "nofast:", salt ? salt : "");
    snprintf(path, pcap, "%s/bcv-%04x-%04x-%08x-%.16s.txt", dir, c->props.vendorID, c->props.deviceID, c->props.driverVersion, u);
    return 1;
}

/* 1: a verdict for exactly this GPU, driver and converter set is on file (*flags: what it turned off); 0: probe. */
static int bcv_verdict_read(pctx *c, uint32_t *flags)
{
    char key[160], path[600], buf[400];
    if (!bcv_verdict_path(c, key, sizeof(key), path, sizeof(path))) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *nl = strchr(buf, '\n');
    unsigned fl = 0;
    if (!nl) return 0;
    *nl = 0;
    if (strcmp(buf, key) || sscanf(nl + 1, "flags 0x%x", &fl) != 1 || (fl & ~(uint32_t)VKB_PROBE_FLAG_BC_ALL)) return 0;
    *flags = fl;
    return 1;
}

static void bcv_verdict_write(pctx *c, uint32_t flags)
{
    char key[160], path[600], tmp[620];
    if (!bcv_verdict_path(c, key, sizeof(key), path, sizeof(path))) return;
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f && errno == ENOENT && vkb_cache_dir_ensure()) f = fopen(tmp, "w");
    if (!f) {
        vkb_cache_write_failed("the BC converter verdict", errno);
        return; /* not kept: the next start probes again */
    }
    fprintf(f, "%s\nflags 0x%x\n", key, flags);
    int ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) unlink(tmp);
}

/* While a game is connected the background probe leaves the driver's compiler half the time (a pause as long as the variant took,
 * 1.5 s at most; VKBRIDGE_PROBE_BCV_DUTY=0: none): the verdict comes later, the old converters serve meanwhile, as before it. */
static void bcv_pace(double took_ms)
{
    const char *v = vkb_srv_env("VKBRIDGE_PROBE_BCV_DUTY");
    if ((v && *v == '0') || !__atomic_load_n(&bcv_bg, __ATOMIC_ACQUIRE) || __atomic_load_n(&clients_gone, __ATOMIC_ACQUIRE)) return;
    double ms = took_ms > 1500 ? 1500 : took_ms;
    struct timespec ts = {(time_t)(ms / 1000), (long)(((long long)ms % 1000) * 1000000)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* The BC converters' new variants (one dispatch a run, the palette search; server_emu_bcn.c vkb_emu_bcn_check_variant):
 * each built and run on this driver beside the old variant on the same blocks, under the compile watchdog one at a time
 * (VKBRIDGE_PROBE_TX_TIMEOUT_S, 20 s, as the transcoders: a driver's compiler may be slow but must not be stuck). A variant
 * that does not build, hangs or gives one byte that differs turns its part off for this driver build (VKB_PROBE_FLAG_BC_*:
 * one dispatch a run, or the palette search of BC1 -> ETC2 or BC4 / BC5 -> EAC); the old variants stay, so a game only
 * misses the speed-up. A game never uses a new variant before this probe has passed it (vkb_probe_bc_variants). After a
 * hang or a crash nothing more is compiled here: the rest counts as failed. Transcoders whose target the GPU does not
 * sample are never used on it: skipped. */
static void p_bc_variants(pctx *c)
{
    if (!(c->emu & VKB_EMU_BCN)) {
        say(c, "BC native: no converters");
        outcome(c, PS_SKIP);
        return;
    }
    /* PowerVR: not run. On the Pixel 10 Pro XL (DXT 25.1) its one-dispatch BC2->ASTC table check never finished (2026-11-10
     * 22:27:09 'GPU HANG (the GPU did not finish in 5 s)', the GPU firmware's only core dump at 22:27:02, then SystemUI's fence ANR
     * and three system_server restarts), and two later runs hit the limit in BC7->ASTC's: on this GPU a hang stops the whole phone, not
     * just the probe. The old variants are what PowerVR has run; a game only misses the speed-up. VKBRIDGE_BCV_PROBE=1 runs it. */
    if (c->pvr && !env_on("VKBRIDGE_BCV_PROBE", 0)) {
        say(c, "PowerVR: not run (its GPU hung in this probe on the Pixel 10 Pro XL, and a hang there stops the phone); old variants only");
        outcome(c, PS_SKIP);
        return;
    }
    if (c->tx_status == PS_HANG || c->tx_status == PS_CRASH || c->tx_status == PS_DIED) {
        say(c, "bc_transcoders %s: not tried (the driver's compiler may be stuck); old variants only", ps_name[c->tx_status]);
        c->want_flags |= VKB_PROBE_FLAG_BC_ALL;
        outcome(c, PS_FAIL);
        return;
    }
    uint32_t kept = 0;
    if (bcv_verdict_read(c, &kept)) {
        c->want_flags |= kept;
        say(c, "kept verdict for this GPU, driver and converter set (no builds)");
        if (kept)
            say(c, " - off:%s%s%s%s%s", kept & VKB_PROBE_FLAG_BC_TAB ? " one dispatch a run" : "", kept & VKB_PROBE_FLAG_BC_PAL_ETC2 ? " BC1->ETC2 palette" : "",
                kept & VKB_PROBE_FLAG_BC_PAL_EAC ? " BC4/BC5->EAC palette" : "", kept & VKB_PROBE_FLAG_BC_FAST_ETC2 ? " BC1->ETC2 palette fast" : "",
                kept & VKB_PROBE_FLAG_BC_FAST_ASTC ? " BC7->ASTC fast" : "");
        if (kept) VKB_WARN("probe: bc_variants: a kept verdict says these converter variants differed on this driver; the old ones stay (flags 0x%x)", kept);
        outcome(c, PS_PASS);
        return;
    }
    uint32_t before = c->want_flags & VKB_PROBE_FLAG_BC_ALL;
    int uncached = 0; /* a failure that is not a deterministic answer: no verdict kept */
    char path[512];
    VkPipelineCache cache = vkb_pcache_own_open(&c->d, &c->props, c->dev, "probe", path, sizeof(path));
    int n = vkb_emu_bcn_variants(), stuck = 0, done = 0, skipped = 0, tx = vkb_emu_bcn_transcode_wanted();
    int fast_set = vkb_bc_fast_gate(c->props.deviceName, NULL);
    double limit = tx_limit_s();
    for (int k = 0; k < n; k++) {
        uint32_t flag = 0;
        VkFormat target;
        const char *name = vkb_emu_bcn_variant_name(k, &flag, &target);
        /* the BC_FAST variants (new since test47) are checked only where they may be used (the measured GPUs, or VKBRIDGE_BC_FAST set):
         * elsewhere the probe is test47's (table and palette variants), at its cost */
        if ((flag & (VKB_PROBE_FLAG_BC_FAST_ETC2 | VKB_PROBE_FLAG_BC_FAST_ASTC)) && !fast_set) continue;
        if (target != VK_FORMAT_UNDEFINED) {
            VkFormatProperties fp = {0};
            c->idt->vkGetPhysicalDeviceFormatProperties(c->pd, target, &fp);
            if (!tx || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
                skipped++;
                continue;
            }
        }
        if (c->want_flags & flag) continue; /* its part is off already */
        bcv_check b = {c, cache, k, -1, 0, ""};
        int sig = 0;
        double w0 = rq_wait_ms(), t0 = mono_ms();
        int g = vkb_guard_run("probe: BC variant", limit, bcv_one, &b, &sig);
        if (g == VKB_GUARD_DONE && b.r == 0) {
            done++;
            bcv_pace(mono_ms() - t0);
            continue;
        }
        if (g != VKB_GUARD_DONE || b.r != 1) uncached = 1;
        say(c, "%s%s: %s", c->dlen ? "; " : "", name,
            g == VKB_GUARD_TIMED_OUT ? "HANG (interrupted at the limit)" : g == VKB_GUARD_CRASHED ? "CRASH" : b.r == 1 ? "DIFFERS" : b.r == -2 ? "GPU HANG" : "FAILED");
        if (g == VKB_GUARD_DONE && b.why[0]) say(c, " (%s)", b.why);
        if (g == VKB_GUARD_TIMED_OUT) guard_starved(c, w0, t0, limit);
        c->want_flags |= flag;
        outcome(c, g == VKB_GUARD_TIMED_OUT || b.r == -2 ? PS_HANG : g == VKB_GUARD_CRASHED ? PS_CRASH : PS_FAIL);
        if (g != VKB_GUARD_DONE || b.r == -2) {
            stuck = 1;
            if (b.r == -2) c->gpu_stuck = 1;
            c->want_flags |= VKB_PROBE_FLAG_BC_ALL; /* the rest not tried */
            say(c, "; the rest not tried");
            break;
        }
    }
    say(c, "%s%d same as the old variants", c->dlen ? "; " : "", done);
    if (skipped) say(c, ", %d skipped (target not sampled here)", skipped);
    if (c->want_flags & VKB_PROBE_FLAG_BC_ALL)
        say(c, " - off:%s%s%s%s%s", c->want_flags & VKB_PROBE_FLAG_BC_TAB ? " one dispatch a run" : "",
            c->want_flags & VKB_PROBE_FLAG_BC_PAL_ETC2 ? " BC1->ETC2 palette" : "", c->want_flags & VKB_PROBE_FLAG_BC_PAL_EAC ? " BC4/BC5->EAC palette" : "",
            c->want_flags & VKB_PROBE_FLAG_BC_FAST_ETC2 ? " BC1->ETC2 palette fast" : "", c->want_flags & VKB_PROBE_FLAG_BC_FAST_ASTC ? " BC7->ASTC fast" : "");
    if (!stuck) vkb_pcache_own_close(&c->d, c->dev, cache, path);
    if (!stuck && !uncached && !before && !c->gpu_stuck) bcv_verdict_write(c, c->want_flags & VKB_PROBE_FLAG_BC_ALL);
    outcome(c, PS_PASS);
}

/* Framebuffer compression (AFBC) of a plain and a STORAGE image (STORAGE loses it on Mali). */
static void p_compression(pctx *c)
{
    if (!c->compression) {
        say(c, "no VK_EXT_image_compression_control");
        outcome(c, PS_SKIP);
        return;
    }
    static const VkImageUsageFlags us[] = {VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
    for (int k = 0; k < 2; k++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {256, 256, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, us[k], VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkResult r;
        VkImage i = img(c, &ci, &r);
        if (!i) {
            say(c, "%s%s: %d", k ? "; " : "", k ? "storage" : "plain", r);
            continue;
        }
        VkImageCompressionPropertiesEXT cp = {VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT};
        VkSubresourceLayout2 l = {VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2, &cp};
        VkImageSubresource2 sub = {VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2, NULL, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}};
        c->d.vkGetImageSubresourceLayout2(c->dev, i, &sub, &l);
        say(c, "%s%s: compression 0x%x", k ? "; " : "", k ? "storage" : "plain", cp.imageCompressionFlags);
    }
    say(c, " (0x0 default/compressed, 0x4 disabled)");
    outcome(c, PS_PASS);
}

/* Over-allocation from a descriptor pool (DXVK/Hades allocate past what a pool was made for). */
static void p_descriptor_pool(pctx *c)
{
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayout l = dsl(c, &b, 1, 0, NULL);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 16, 1, &ps};
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (!l || c->d.vkCreateDescriptorPool(c->dev, &pi, NULL, &p) != VK_SUCCESS) {
        outcome(c, PS_ERROR);
        return;
    }
    track(c, O_DPOOL, H(p));
    int got = 0;
    VkResult r = VK_SUCCESS;
    for (; got < 32; got++) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, p, 1, &l};
        VkDescriptorSet s;
        if ((r = c->d.vkAllocateDescriptorSets(c->dev, &ai, &s)) != VK_SUCCESS) break;
    }
    say(c, "pool for 16 sets gave %d, then %d", got, got < 32 ? r : 0);
    outcome(c, PS_PASS);
}

/* D3D12 bindless: 1M update-after-bind sampled images, as the layout-support query answers it and
 * as a create does (Mali said unsupported for layouts it then made). */
static void p_bindless(pctx *c)
{
    if (!c->bindless || !c->d.vkGetDescriptorSetLayoutSupport) {
        say(c, "no descriptor indexing");
        outcome(c, PS_SKIP);
        return;
    }
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    c->idt->vkGetPhysicalDeviceProperties2(c->pd, &p2);
    say(c, "UAB per stage: sampled %u storage img %u storage buf %u", p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
        p12.maxPerStageDescriptorUpdateAfterBindStorageImages, p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
    static const uint32_t counts[] = {500000, 1000000};
    for (int k = 0; k < 2; k++) {
        VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                      VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 1, &bf};
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, counts[k], VK_SHADER_STAGE_ALL, NULL};
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi,
                                              VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &b};
        VkDescriptorSetLayoutSupport sup = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
        c->d.vkGetDescriptorSetLayoutSupport(c->dev, &ci, &sup);
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        VkResult r = c->d.vkCreateDescriptorSetLayout(c->dev, &ci, NULL, &l);
        if (r == VK_SUCCESS) track(c, O_DSL, H(l));
        say(c, "; %u: support %u, create %d", counts[k], sup.supported, r);
    }
    outcome(c, PS_PASS);
}

/* Null descriptors (robustness2): reads through null buffers, images, texel buffers read zero. */
static void p_null_descriptor(pctx *c)
{
    if (!c->null_desc) {
        say(c, "%s", (c->emu & VKB_EMU_ROBUSTNESS2) ? "robustness2 emulated (null stand-ins)" : "no nullDescriptor");
        outcome(c, PS_SKIP);
        return;
    }
    void *out = NULL;
    VkBuffer ob = buf(c, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out);
    VkDescriptorSetLayoutBinding b[5] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayout l = dsl(c, b, 5, 0, NULL);
    VkPipelineLayout pl = l ? layout_push(c, 0, 0, 1, &l) : VK_NULL_HANDLE;
    VkResult r = VK_ERROR_UNKNOWN;
    VkPipeline p = pl ? comp(c, spv_probe_null, sizeof(spv_probe_null), pl, NULL, &r) : VK_NULL_HANDLE;
    VkDescriptorSet s = p ? set_alloc(c, l) : VK_NULL_HANDLE;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler smp = VK_NULL_HANDLE;
    if (!ob || !s || c->d.vkCreateSampler(c->dev, &si, NULL, &smp) != VK_SUCCESS) {
        say(c, "setup failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    track(c, O_SAMPLER, H(smp));
    VkDescriptorBufferInfo bo = {ob, 0, VK_WHOLE_SIZE}, bn = {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo in = {smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkBufferView tn = VK_NULL_HANDLE;
    VkWriteDescriptorSet w[5] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bo, NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bn, NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in, NULL, NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 3, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &tn},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bn, NULL}};
    c->d.vkUpdateDescriptorSets(c->dev, 5, w, 0, NULL);
    if (!dispatch(c, p, pl, s, NULL, 0)) {
        outcome(c, PS_FAIL);
        return;
    }
    const uint32_t *o = out;
    say(c, "null reads: buffer 0x%x image 0x%x size %u texel 0x%x ubo 0x%x (ran: %s)", o[0], o[1], o[2], o[3], o[4], o[5] == 0xc0de ? "yes" : "NO");
    outcome(c, o[5] == 0xc0de && !o[0] && !o[1] && !o[2] && !o[3] && !o[4] ? PS_PASS : PS_FAIL);
}

/* Native push descriptors: a buffer pushed, written by a dispatch. */
static void p_push_descriptor(pctx *c)
{
    if (!c->push) {
        say(c, "%s", (c->emu & VKB_EMU_PUSH_DESC) ? "push descriptors emulated" : "no VK_KHR_push_descriptor");
        outcome(c, PS_SKIP);
        return;
    }
    void *out = NULL;
    VkBuffer ob = buf(c, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out);
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayout l = dsl(c, &b, 1, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT, NULL);
    VkPipelineLayout pl = l ? layout_push(c, VK_SHADER_STAGE_COMPUTE_BIT, 4, 1, &l) : VK_NULL_HANDLE;
    VkResult r = VK_ERROR_UNKNOWN;
    VkPipeline p = pl ? comp(c, spv_probe_push, sizeof(spv_probe_push), pl, NULL, &r) : VK_NULL_HANDLE;
    if (!ob || !p) {
        say(c, "setup failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    VkCommandBuffer cb = cmd(c);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    VkDescriptorBufferInfo bi = {ob, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
    c->d.vkCmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &w);
    uint32_t marker = 0x600dcafe;
    c->d.vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &marker);
    c->d.vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (submit(c, cb) != VK_SUCCESS) {
        outcome(c, PS_FAIL);
        return;
    }
    const uint32_t *o = out;
    say(c, "pushed buffer written: 0x%08x 0x%08x", o[0], o[1]);
    outcome(c, o[0] == marker && o[1] == ~marker ? PS_PASS : PS_FAIL);
}

typedef struct inst_draw {
    VkBuffer vb[2];
    uint32_t instances;
} inst_draw;

static void rec_inst(pctx *c, VkCommandBuffer cb, void *arg)
{
    inst_draw *d = arg;
    VkDeviceSize off[2] = {0, 0};
    c->d.vkCmdBindVertexBuffers(cb, 0, 2, d->vb, off);
    c->d.vkCmdDraw(cb, 1, d->instances, 0, 0);
}

/* Vertex attribute divisor as vkd3d-proton uses it: divisor 2 and divisor 0 (the G925 lacked 0). */
static void p_divisor(pctx *c)
{
    if (!c->divisor) {
        say(c, "%s", (c->emu & VKB_EMU_DIVISOR) ? "divisor emulated" : "no divisor");
        outcome(c, PS_SKIP);
        return;
    }
    rt t;
    void *a = NULL, *b = NULL;
    VkBuffer va = buf(c, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &a), vb = buf(c, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &b);
    VkPipelineLayout pl = layout_push(c, 0, 0, 0, NULL);
    if (!rt_make(c, &t, 1) || !va || !vb || !pl) {
        outcome(c, PS_ERROR);
        return;
    }
    const float ea[4] = {10 / 255.0f, 20 / 255.0f, 30 / 255.0f, 40 / 255.0f}, eb[4] = {77 / 255.0f, 99 / 255.0f, 99 / 255.0f, 99 / 255.0f};
    memcpy(a, ea, sizeof(ea));
    memcpy(b, eb, sizeof(eb));
    uint32_t zero = c->divisor_zero ? 0 : 1;
    VkVertexInputBindingDescription bd[2] = {{0, 4, VK_VERTEX_INPUT_RATE_INSTANCE}, {1, 4, VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription ad[2] = {{0, 0, VK_FORMAT_R32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32_SFLOAT, 0}};
    VkVertexInputBindingDivisorDescription dd[2] = {{0, 2}, {1, zero}};
    VkPipelineVertexInputDivisorStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO, NULL, 2, dd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, &ds, 0, 2, bd, 2, ad};
    gp_desc g = {spv_probe_inst, NULL, spv_probe_col, sizeof(spv_probe_inst), 0, sizeof(spv_probe_col), &vi, 0, NULL};
    VkResult r;
    VkPipeline p = gfx(c, &t, pl, &g, VK_NULL_HANDLE, &r);
    if (!p) {
        say(c, "pipeline refused (%d)", r);
        outcome(c, PS_FAIL);
        return;
    }
    inst_draw d = {{va, vb}, 6};
    if (!rt_draw(c, &t, p, rec_inst, &d)) {
        outcome(c, PS_FAIL);
        return;
    }
    static const uint8_t want_a[6] = {10, 10, 20, 20, 30, 30};
    int bad = 0;
    for (uint32_t x = 0; x < 6; x++) {
        const uint8_t *q = pxl(&t, 0, x);
        uint8_t wb = zero ? (x == 0 ? 77 : 99) : 77;
        bad |= abs(q[0] - want_a[x]) > 1 || abs(q[1] - wb) > 1;
    }
    char ra[64], rb[64];
    row_text(&t, 0, 0, ra, sizeof(ra));
    row_text(&t, 0, 1, rb, sizeof(rb));
    say(c, "%s; divisor 2: %s (want 10 10 20 20 30 30); divisor %u: %s", c->divisor_name + 3, ra, zero, rb);
    if (!c->divisor_zero) say(c, " (no zero divisor: tested 1)");
    outcome(c, bad ? PS_FAIL : PS_PASS);
}

typedef struct pts {
    VkPipelineLayout pl;
    uint32_t count;
    float z;
} pts;

static void rec_pts(pctx *c, VkCommandBuffer cb, void *arg)
{
    pts *p = arg;
    vid_pc pc = {{1, 1, 1, 1}, p->z};
    c->d.vkCmdPushConstants(cb, p->pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, sizeof(pc), &pc);
    c->d.vkCmdDraw(cb, p->count, 1, 0, 0);
}

/* Depth clip off (VK_EXT_depth_clip_enable, D3D11's DepthClipEnable = FALSE): points at z = 1.5
 * must be drawn (clamped), and clipped with it on. */
static void p_depth_clip(pctx *c)
{
    if (!c->depth_clip || !c->feat.depthClamp) {
        say(c, "%s", (c->emu & VKB_EMU_DEPTH_CLIP) ? "depth clip emulated" : "no VK_EXT_depth_clip_enable");
        outcome(c, PS_SKIP);
        return;
    }
    rt t;
    VkPipelineLayout pl = layout_push(c, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, sizeof(vid_pc), 0, NULL);
    if (!rt_make(c, &t, 1) || !pl) {
        outcome(c, PS_ERROR);
        return;
    }
    int lit[2] = {0, 0};
    for (int on = 0; on < 2; on++) {
        VkPipelineRasterizationDepthClipStateCreateInfoEXT dc = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT, NULL, 0,
                                                                 (VkBool32)on};
        gp_desc g = {spv_probe_vid, NULL, spv_probe_col, sizeof(spv_probe_vid), 0, sizeof(spv_probe_col), NULL, VK_TRUE, &dc};
        VkResult r;
        VkPipeline p = gfx(c, &t, pl, &g, VK_NULL_HANDLE, &r);
        pts a = {pl, 4, 1.5f};
        if (!p || !rt_draw(c, &t, p, rec_pts, &a)) {
            say(c, "%spipeline/draw failed (%d)", on ? "; " : "", r);
            outcome(c, PS_FAIL);
            return;
        }
        for (uint32_t x = 0; x < 4; x++) lit[on] += pxl(&t, 0, x)[0] > 200;
    }
    say(c, "z = 1.5 points drawn: clip off %d/4 (want 4), clip on %d/4 (want 0)", lit[0], lit[1]);
    outcome(c, lit[0] == 4 && lit[1] == 0 ? PS_PASS : PS_FAIL);
}

/* gl_ClipDistance: odd points clipped. */
static void p_clip_distance(pctx *c)
{
    if (!c->clip) {
        say(c, "%s", (c->emu & VKB_EMU_CLIP_DISTANCE) ? "clip distance emulated" : "no shaderClipDistance");
        outcome(c, PS_SKIP);
        return;
    }
    rt t;
    VkPipelineLayout pl = layout_push(c, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, sizeof(vid_pc), 0, NULL);
    if (!rt_make(c, &t, 1) || !pl) {
        outcome(c, PS_ERROR);
        return;
    }
    gp_desc g = {spv_probe_clip, NULL, spv_probe_col, sizeof(spv_probe_clip), 0, sizeof(spv_probe_col), NULL, 0, NULL};
    VkResult r;
    VkPipeline p = gfx(c, &t, pl, &g, VK_NULL_HANDLE, &r);
    pts a = {pl, 8, 0.5f};
    if (!p || !rt_draw(c, &t, p, rec_pts, &a)) {
        say(c, "pipeline/draw failed (%d)", r);
        outcome(c, PS_FAIL);
        return;
    }
    char row[64];
    row_text(&t, 0, 0, row, sizeof(row));
    int bad = 0;
    for (uint32_t x = 0; x < 8; x++) bad |= (pxl(&t, 0, x)[0] > 200) != !(x & 1);
    say(c, "points with ClipDistance -1 on odd columns: %s (want 255 0 255 0 ...)", row);
    outcome(c, bad ? PS_FAIL : PS_PASS);
}

typedef struct mdi {
    VkPipelineLayout pl;
    VkBuffer ib;
} mdi;

static void rec_mdi(pctx *c, VkCommandBuffer cb, void *arg)
{
    mdi *m = arg;
    vid_pc pc = {{1, 1, 1, 1}, 0.5f};
    c->d.vkCmdPushConstants(cb, m->pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, sizeof(pc), &pc);
    c->d.vkCmdDrawIndirect(cb, m->ib, 0, 2, sizeof(VkDrawIndirectCommand));
}

/* multiDrawIndirect: one indirect call, two draws (columns 2 and 5). */
static void p_multi_draw(pctx *c)
{
    if (!c->mdi) {
        say(c, "%s", (c->emu & VKB_EMU_MULTI_DRAW) ? "multi-draw emulated" : "no multiDrawIndirect");
        outcome(c, PS_SKIP);
        return;
    }
    rt t;
    void *ip = NULL;
    VkBuffer ib = buf(c, 64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &ip);
    VkPipelineLayout pl = layout_push(c, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, sizeof(vid_pc), 0, NULL);
    if (!rt_make(c, &t, 1) || !ib || !pl) {
        outcome(c, PS_ERROR);
        return;
    }
    const VkDrawIndirectCommand dc[2] = {{1, 1, 2, 0}, {1, 1, 5, 0}};
    memcpy(ip, dc, sizeof(dc));
    gp_desc g = {spv_probe_vid, NULL, spv_probe_col, sizeof(spv_probe_vid), 0, sizeof(spv_probe_col), NULL, 0, NULL};
    VkResult r;
    VkPipeline p = gfx(c, &t, pl, &g, VK_NULL_HANDLE, &r);
    mdi m = {pl, ib};
    if (!p || !rt_draw(c, &t, p, rec_mdi, &m)) {
        say(c, "pipeline/draw failed (%d)", r);
        outcome(c, PS_FAIL);
        return;
    }
    char row[64];
    row_text(&t, 0, 0, row, sizeof(row));
    int bad = 0;
    for (uint32_t x = 0; x < 8; x++) bad |= (pxl(&t, 0, x)[0] > 200) != (x == 2 || x == 5);
    say(c, "2 draws in one indirect call: %s (want columns 2 and 5)", row);
    outcome(c, bad ? PS_FAIL : PS_PASS);
}

/* Layered rendering, by the vertex shader (shaderOutputLayer) and by a pass-through geometry stage. */
static void layer_draw(pctx *c, int with_gs)
{
    rt t;
    VkPipelineLayout pl = layout_push(c, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, sizeof(vid_pc), 0, NULL);
    if (!rt_make(c, &t, 2) || !pl) {
        outcome(c, PS_ERROR);
        return;
    }
    gp_desc g = with_gs ? (gp_desc){spv_probe_vid, spv_probe_layer_gs, spv_probe_col, sizeof(spv_probe_vid), sizeof(spv_probe_layer_gs),
                                    sizeof(spv_probe_col), NULL, 0, NULL}
                        : (gp_desc){spv_probe_layer_vs, NULL, spv_probe_col, sizeof(spv_probe_layer_vs), 0, sizeof(spv_probe_col), NULL, 0, NULL};
    double t0 = mono_ms();
    VkResult r;
    VkPipeline p = gfx(c, &t, pl, &g, VK_NULL_HANDLE, &r);
    double ms = mono_ms() - t0;
    pts a = {pl, 8, 0.5f};
    if (!p || !rt_draw(c, &t, p, rec_pts, &a)) {
        say(c, "pipeline/draw failed (%d)", r);
        outcome(c, PS_FAIL);
        return;
    }
    int l0 = 0, l1 = 0;
    for (uint32_t x = 0; x < 8; x++) l0 += pxl(&t, 0, x)[0] > 200, l1 += pxl(&t, 1, x)[0] > 200;
    say(c, "8 points into layer 1: layer 0 has %d, layer 1 has %d (compiled in %.1f ms)", l0, l1, ms);
    outcome(c, l0 == 0 && l1 == 8 ? PS_PASS : PS_FAIL);
}

static void p_layer_vs(pctx *c)
{
    if (!c->layer_vs) {
        say(c, "no shaderOutputLayer");
        outcome(c, PS_SKIP);
        return;
    }
    layer_draw(c, 0);
}

static void p_layer_gs(pctx *c)
{
    if (!c->gs) {
        say(c, "no geometryShader");
        outcome(c, PS_SKIP);
        return;
    }
    layer_draw(c, 1);
    /* the fold makes the vertex shader write Layer: only where that works */
    if (c->layer_vs_passed) c->want_flags |= VKB_PROBE_FLAG_GS_FOLD;
    else say(c, "; no fold (the vertex shader's Layer did not pass)");
}

/* DXVK's guard: OpSelect over a composite of specialization constants (declared a plain constant
 * composite, as DXVK emits it), specialized to true. Mali took the defaults: zeros. */
static void p_spec_select(pctx *c)
{
    void *out = NULL;
    VkBuffer ob = buf(c, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out);
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayout l = dsl(c, &b, 1, 0, NULL);
    VkPipelineLayout pl = l ? layout_push(c, 0, 0, 1, &l) : VK_NULL_HANDLE;
    VkBool32 on = VK_TRUE;
    VkSpecializationMapEntry me = {0, 0, sizeof(VkBool32)};
    VkSpecializationInfo si = {1, &me, sizeof(on), &on};
    VkResult r = VK_ERROR_UNKNOWN;
    VkPipeline p = pl ? comp(c, spv_probe_spec, sizeof(spv_probe_spec), pl, &si, &r) : VK_NULL_HANDLE;
    VkDescriptorSet s = p ? set_alloc(c, l) : VK_NULL_HANDLE;
    if (!ob || !s) {
        say(c, "setup failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    VkDescriptorBufferInfo bi = {ob, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
    c->d.vkUpdateDescriptorSets(c->dev, 1, &w, 0, NULL);
    if (!dispatch(c, p, pl, s, NULL, 0)) {
        outcome(c, PS_FAIL);
        return;
    }
    const uint32_t *o = out;
    say(c, "selected %u %u %u %u (want 11 22 33 44; zeros: folded at the defaults)", o[0], o[1], o[2], o[3]);
    int ok = o[0] == 11 && o[1] == 22 && o[2] == 33 && o[3] == 44;
    if (!ok) {
        say(c, " - the SPIR-V fix (server_spvfix.c) %s", vkb_driver_is_arm(&c->props, 0) ? "is on for Arm's driver" : "is turned on");
        c->want_flags |= VKB_PROBE_FLAG_SPIRV_FIX;
    }
    outcome(c, ok ? PS_PASS : PS_FAIL);
}

/* A fragment shader of the shape the G925's compiler never finished (UE4's colour-grading LUT):
 * built against a render pass; a spin here is caught at the limit. */
static void p_compile_aces(pctx *c)
{
    rt t;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayout l = dsl(c, &b, 1, 0, NULL);
    VkPipelineLayout pl = l ? layout_push(c, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, sizeof(vid_pc), 1, &l) : VK_NULL_HANDLE;
    if (!rt_make(c, &t, 1) || !pl) {
        outcome(c, PS_ERROR);
        return;
    }
    /* vid.vert has no uv output: the fragment shader's input reads undefined values, which a
     * compile does not care about */
    gp_desc g = {spv_probe_vid, NULL, spv_probe_aces, sizeof(spv_probe_vid), 0, sizeof(spv_probe_aces), NULL, 0, NULL};
    double t0 = mono_ms();
    VkResult r;
    VkPipeline p = gfx(c, &t, pl, &g, VK_NULL_HANDLE, &r);
    double ms = mono_ms() - t0;
    say(c, "UE4-LUT-shaped fragment shader (%zu bytes of SPIR-V): %s in %.0f ms", sizeof(spv_probe_aces), p ? "built" : "refused", ms);
    if (!p) say(c, " (%d)", r);
    c->want_flags |= VKB_PROBE_FLAG_LUT_NOW; /* applied only if this hangs */
    outcome(c, p ? PS_PASS : PS_FAIL);
}

/* The neutral LUT stand-in the compile watchdog falls back to must build. */
static void p_compile_lut_standin(pctx *c)
{
    rt t;
    VkPipelineLayout pl = layout_push(c, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, sizeof(vid_pc), 0, NULL);
    if (!rt_make(c, &t, 1) || !pl) {
        outcome(c, PS_ERROR);
        return;
    }
    size_t size = 0;
    const uint32_t *code = vkb_guard_lut_standin(&size);
    /* the stand-in reads Layer: with a geometry stage writing it where there is one */
    gp_desc g = {spv_probe_vid, c->gs ? spv_probe_layer_gs : NULL, code, sizeof(spv_probe_vid), c->gs ? sizeof(spv_probe_layer_gs) : 0, size,
                 NULL, 0, NULL};
    double t0 = mono_ms();
    VkResult r;
    VkPipeline p = gfx(c, &t, pl, &g, VK_NULL_HANDLE, &r);
    say(c, "the compile watchdog's LUT stand-in: %s in %.0f ms", p ? "built" : "refused", mono_ms() - t0);
    if (!p) say(c, " (%d) - a stuck UE4 LUT pipeline would get the constant colour", r);
    outcome(c, p ? PS_PASS : PS_FAIL);
}

/* What a pipeline cache saves on this driver: the same pipeline cold and from the cache's data. */
static void p_pipeline_cache(pctx *c)
{
    rt t;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayout l = dsl(c, &b, 1, 0, NULL);
    VkPipelineLayout pl = l ? layout_push(c, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, sizeof(vid_pc), 1, &l) : VK_NULL_HANDLE;
    if (!rt_make(c, &t, 1) || !pl) {
        outcome(c, PS_ERROR);
        return;
    }
    gp_desc g = {spv_probe_clip, NULL, spv_probe_col, sizeof(spv_probe_clip), 0, sizeof(spv_probe_col), NULL, 0, NULL};
    if (!c->clip) g.vs = spv_probe_vid, g.vs_size = sizeof(spv_probe_vid);
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache pc = VK_NULL_HANDLE, pc2 = VK_NULL_HANDLE;
    if (c->d.vkCreatePipelineCache(c->dev, &ci, NULL, &pc) != VK_SUCCESS) {
        outcome(c, PS_ERROR);
        return;
    }
    track(c, O_PCACHE, H(pc));
    VkResult r;
    double t0 = mono_ms();
    VkPipeline p = gfx(c, &t, pl, &g, pc, &r);
    double cold = mono_ms() - t0;
    size_t size = 0;
    c->d.vkGetPipelineCacheData(c->dev, pc, &size, NULL);
    void *data = size ? malloc(size) : NULL;
    if (data) c->d.vkGetPipelineCacheData(c->dev, pc, &size, data);
    ci.initialDataSize = data ? size : 0;
    ci.pInitialData = data;
    VkResult r2 = c->d.vkCreatePipelineCache(c->dev, &ci, NULL, &pc2);
    free(data);
    if (r2 == VK_SUCCESS) track(c, O_PCACHE, H(pc2));
    t0 = mono_ms();
    VkPipeline p2 = r2 == VK_SUCCESS ? gfx(c, &t, pl, &g, pc2, &r) : VK_NULL_HANDLE;
    double warm = mono_ms() - t0;
    say(c, "cold %.1f ms, from %zu bytes of cache data %.1f ms (cache from data: %d)", cold, size, warm, r2);
    outcome(c, p && p2 ? PS_PASS : PS_FAIL);
}

/* Waits: every result outside the legal set (the G925 answered -3) is evidence. */
static void p_waits(pctx *c)
{
    int bad = 0;
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    if (c->d.vkCreateFence(c->dev, &fi, NULL, &f) == VK_SUCCESS) {
        track(c, O_FENCE, H(f));
        VkResult a = c->d.vkWaitForFences(c->dev, 1, &f, VK_TRUE, 0);
        VkResult b = c->d.vkWaitForFences(c->dev, 1, &f, VK_TRUE, 1000000);
        say(c, "unsignalled fence: wait 0 -> %d, 1 ms -> %d", a, b);
        bad |= a != VK_TIMEOUT || b != VK_TIMEOUT;
    }
    if (!c->timeline || !c->d.vkWaitSemaphores) {
        say(c, "; no timeline semaphores");
        outcome(c, bad ? PS_FAIL : PS_PASS);
        return;
    }
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    VkSemaphore s = VK_NULL_HANDLE;
    if (c->d.vkCreateSemaphore(c->dev, &si, NULL, &s) != VK_SUCCESS) {
        outcome(c, PS_ERROR);
        return;
    }
    track(c, O_SEM, H(s));
    uint64_t v = 1;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s, &v};
    VkResult a = c->d.vkWaitSemaphores(c->dev, &wi, 0);
    double t0 = mono_ms();
    VkResult b = c->d.vkWaitSemaphores(c->dev, &wi, 2000000);
    double el = mono_ms() - t0;
    say(c, "; timeline at 0, wait for 1: 0 -> %d, 2 ms -> %d after %.1f ms", a, b, el);
    bad |= a != VK_TIMEOUT || b != VK_TIMEOUT;
    /* the timed-out wait answered VK_ERROR_DEVICE_LOST while the poll says VK_TIMEOUT */
    int lost_at_timeout = a == VK_TIMEOUT && b == VK_ERROR_DEVICE_LOST;
    /* signalled from the queue (wait-before-signal on the host side) */
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &v};
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 0, NULL, NULL, 0, NULL, 1, &s};
    VkResult q = c->d.vkQueueSubmit(c->q, 1, &sub, VK_NULL_HANDLE);
    VkResult w = c->d.vkWaitSemaphores(c->dev, &wi, 1000000000ull);
    uint64_t now = 0;
    if (c->d.vkGetSemaphoreCounterValue) c->d.vkGetSemaphoreCounterValue(c->dev, s, &now);
    say(c, "; signalled by a submission (%d): wait -> %d, value %llu", q, w, (unsigned long long)now);
    bad |= q != VK_SUCCESS || w != VK_SUCCESS || now != 1;
    if (lost_at_timeout && q == VK_SUCCESS && w == VK_SUCCESS && now == 1) {
        c->want_flags |= VKB_PROBE_FLAG_WAIT_LOST;
        say(c, " - a timed-out wait answered VK_ERROR_DEVICE_LOST with the device fine");
    } else if (bad)
        say(c, " - illegal results are retried at run time (server_wait.c)");
    c->d.vkQueueWaitIdle(c->q);
    outcome(c, bad ? PS_FAIL : PS_PASS);
}

/* ------------------------------------------------------------------ the MK probes (Mali driver errata) */

/* One probe per Mali driver erratum the bridge fixes (server_errata.c) where a probe can tell it:
 * each records its sub-cases (cases / failed bits, named in probe_def.cases) and, where it saw the
 * bug's own pattern, a VKB_PROBE_FLAG_MK_* flag - the "probe failed" the errata decisions read.
 * With VKBRIDGE_ERRATA_IMITATE naming the erratum (host builds), its imitation goes beneath the
 * probe's calls (probe_one: vkb_errata_probe_begin). */

static void sub(pctx *c, int i, int ok)
{
    c->cases |= 1u << i;
    if (!ok) c->failed |= 1u << i;
}

/* A w x h RGBA8 target, with a depth/stencil attachment where dsf is a format, read back after the
 * render pass (colour, and the stencil aspect where there is one) */
typedef struct rt2 {
    VkImage img, dsi;
    VkRenderPass rp;
    VkFramebuffer fb;
    VkBuffer rb, sb;
    uint8_t *px, *st;
    uint32_t w, h;
    VkFormat dsf;
} rt2;

static int has_stencil(VkFormat f)
{
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

static VkFormat ds_format(const pctx *c)
{
    static const VkFormat fs[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT};
    for (size_t i = 0; i < sizeof(fs) / sizeof(fs[0]); i++) {
        VkFormatProperties fp;
        c->idt->vkGetPhysicalDeviceFormatProperties(c->pd, fs[i], &fp);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((fp.optimalTilingFeatures & want) == want) return fs[i];
    }
    return VK_FORMAT_UNDEFINED;
}

static int rt2_make(pctx *c, rt2 *t, uint32_t w, uint32_t h, VkFormat dsf)
{
    memset(t, 0, sizeof(*t));
    t->w = w, t->h = h, t->dsf = dsf;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    t->img = img(c, &ci, NULL);
    VkImageView v[2] = {t->img ? view(c, t->img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1) : VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (!v[0]) return 0;
    if (dsf) {
        ci.format = dsf;
        ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        t->dsi = img(c, &ci, NULL);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, t->dsi, VK_IMAGE_VIEW_TYPE_2D, dsf, {0, 0, 0, 0},
                                    {VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(dsf) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0, 1, 0, 1}};
        if (!t->dsi || c->d.vkCreateImageView(c->dev, &vi, NULL, &v[1]) != VK_SUCCESS) return 0;
        track(c, O_VIEW, H(v[1]));
    }
    VkAttachmentDescription ad[2] = {
        {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {0, dsf, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_CLEAR,
         VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar, NULL, dsf ? &dr : NULL, 0, NULL};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, dsf ? 2u : 1u, ad, 1, &sd, 1, &dep};
    if (c->d.vkCreateRenderPass(c->dev, &rpi, NULL, &t->rp) != VK_SUCCESS) return 0;
    track(c, O_RP, H(t->rp));
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, t->rp, dsf ? 2u : 1u, v, w, h, 1};
    if (c->d.vkCreateFramebuffer(c->dev, &fi, NULL, &t->fb) != VK_SUCCESS) return 0;
    track(c, O_FB, H(t->fb));
    void *p = NULL, *q = NULL;
    t->rb = buf(c, 4u * w * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &p);
    t->px = p;
    if (has_stencil(dsf)) {
        t->sb = buf(c, (w * h + 3u) & ~3u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &q);
        t->st = q;
        if (!t->sb) return 0;
    }
    return t->rb != VK_NULL_HANDLE;
}

/* The render pass (cleared: colour 0, depth 1, stencil 0) with rec's commands, then read back */
static int rt2_draw(pctx *c, rt2 *t, rec_fn rec, void *arg)
{
    memset(t->px, 0xEE, 4u * t->w * t->h);
    if (t->st) memset(t->st, 0xEE, t->w * t->h);
    VkCommandBuffer cb = cmd(c);
    if (!cb) return 0;
    VkClearValue cv[2];
    memset(cv, 0, sizeof(cv));
    cv[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, t->rp, t->fb, {{0, 0}, {t->w, t->h}}, t->dsf ? 2u : 1u, cv};
    c->d.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    rec(c, cb, arg);
    c->d.vkCmdEndRenderPass(cb);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {t->w, t->h, 1}};
    c->d.vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rb, 1, &r);
    if (t->st) {
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        c->d.vkCmdCopyImageToBuffer(cb, t->dsi, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->sb, 1, &r);
    }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkResult res = submit(c, cb);
    if (res != VK_SUCCESS) say(c, " draw submission %d;", res);
    return res == VK_SUCCESS;
}

static const uint8_t *px2(const rt2 *t, uint32_t x, uint32_t y)
{
    return t->px + (y * t->w + x) * 4u;
}

/* A graphics pipeline with what the MK probes vary: dynamic states, viewports, cull mode, depth
 * and stencil, sample mask (the rest as gfx) */
typedef struct gx {
    const uint32_t *vs, *fs;
    size_t vs_size, fs_size;
    const VkPipelineVertexInputStateCreateInfo *vi;
    VkPrimitiveTopology topo;
    uint32_t w, h, viewports;
    const VkDynamicState *dyn;
    uint32_t ndyn;
    VkCullModeFlags cull;
    const VkPipelineDepthStencilStateCreateInfo *ds;
    const VkSampleMask *smask;
} gx;

static VkPipeline gfx_ex(pctx *c, VkRenderPass rp, VkPipelineLayout pl, const gx *g, VkResult *res)
{
    VkShaderModule vs = module(c, g->vs, g->vs_size), fs = module(c, g->fs, g->fs_size);
    if (!vs || !fs) {
        *res = VK_ERROR_INITIALIZATION_FAILED;
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi0 = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, g->topo, 0};
    VkViewport vp[2];
    VkRect2D sc[2];
    uint32_t nv = g->viewports ? g->viewports : 1;
    for (uint32_t i = 0; i < nv && i < 2; i++) {
        vp[i] = (VkViewport){0, 0, (float)g->w, (float)g->h, 0, 1};
        sc[i] = (VkRect2D){{0, 0}, {g->w, g->h}};
    }
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, nv, vp, nv, sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL,
                                                 g->cull, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT, 0, 0,
                                               g->smask, 0, 0};
    VkPipelineColorBlendAttachmentState ba = {0, 0, 0, 0, 0, 0, 0, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &ba, {0, 0, 0, 0}};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, g->ndyn, g->dyn};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, g->vi ? g->vi : &vi0, &ia, NULL, &vps, &rs,
                                       &ms, g->ds, &cbs, g->ndyn ? &dy : NULL, pl, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    *res = c->d.vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, &p);
    if (*res != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_PIPE, H(p));
    return p;
}

/* quad.vert's push constants: the rectangle (x0, y0, x1, y1 in NDC), the colour, the depth */
typedef struct quad_pc {
    float rect[4];
    float col[4];
    float z;
} quad_pc;

static void quad(pctx *c, VkCommandBuffer cb, VkPipelineLayout pl, float x0, float y0, float x1, float y1, float r, float g, float b, float z,
                 uint32_t instances)
{
    quad_pc pc = {{x0, y0, x1, y1}, {r, g, b, 1}, z};
    c->d.vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    c->d.vkCmdDraw(cb, 6, instances, 0, 0);
}

/* One quarter of a 8x4 target: "0011" - which of its 4 rows are lit (1), clear (0) or partly (?) */
static void rows_of(const rt2 *t, uint32_t x0, char out[5])
{
    for (uint32_t y = 0; y < 4; y++) {
        int n = 0;
        for (uint32_t x = x0; x < x0 + 4; x++) n += px2(t, x, y)[0] > 200;
        out[y] = n == 4 ? '1' : n == 0 ? '0' : '?';
    }
    out[4] = 0;
}

/* MK-02: negative viewport height (y + h, -h) ignored. An 8x4 target; the rectangle NDC
 * y in [-1, 0] (quad.vert, face.frag: green is gl_FrontFacing).
 *   rows        viewport (0, 4, 4, -4) on the left half: the bottom two rows lit (0011); the bug
 *               draws the top two (1100), upside down. The right half, viewport (4, 0, 4, 4), must
 *               draw the top two (the target and a positive viewport work).
 *   facing      the same rectangle, flipped on the left and plain on the right, faces the other way
 *               (a negative height flips the facing computed in framebuffer space)
 *   two_viewports  with multiViewport and the VS's ViewportIndex: viewports (0, 4, 4, -4) and
 *               (4, 4, 4, -4), one instance each - both halves 0011 (a driver flipping only
 *               viewport 0 draws the right half upside down)
 * The bug's flag only for an image upside down: another wrong picture is a fail that turns
 * nothing on (a flip on a correct driver would turn every frame upside down). */
typedef struct flip_rec {
    VkPipelineLayout pl;
    VkPipeline p;
    int two;
} flip_rec;

static void rec_flip(pctx *c, VkCommandBuffer cb, void *arg)
{
    flip_rec *f = arg;
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f->p);
    if (f->two) {
        VkViewport v[2] = {{0, 4, 4, -4, 0, 1}, {4, 4, 4, -4, 0, 1}};
        c->d.vkCmdSetViewport(cb, 0, 2, v);
        quad(c, cb, f->pl, -1, -1, 1, 0, 1, 0, 0, 0.5f, 2);
        return;
    }
    VkViewport flipped = {0, 4, 4, -4, 0, 1}, plain = {4, 0, 4, 4, 0, 1};
    c->d.vkCmdSetViewport(cb, 0, 1, &flipped);
    quad(c, cb, f->pl, -1, -1, 1, 0, 1, 0, 0, 0.5f, 1);
    c->d.vkCmdSetViewport(cb, 0, 1, &plain);
    quad(c, cb, f->pl, -1, -1, 1, 0, 1, 0, 0, 0.5f, 1);
}

static void p_viewport_flip(pctx *c)
{
    rt2 t;
    VkPipelineLayout pl = layout_push(c, VK_SHADER_STAGE_VERTEX_BIT, sizeof(quad_pc), 0, NULL);
    if (!pl || !rt2_make(c, &t, 8, 4, VK_FORMAT_UNDEFINED)) {
        outcome(c, PS_ERROR);
        return;
    }
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT};
    gx g = {spv_probe_quad, spv_probe_face, sizeof(spv_probe_quad), sizeof(spv_probe_face), NULL, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 8, 4, 1, dyn,
            1, VK_CULL_MODE_NONE, NULL, NULL};
    VkResult r;
    flip_rec f = {pl, gfx_ex(c, t.rp, pl, &g, &r), 0};
    if (!f.p || !rt2_draw(c, &t, rec_flip, &f)) {
        say(c, "pipeline/draw failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    char left[5], right[5];
    rows_of(&t, 0, left);
    rows_of(&t, 4, right);
    int plain_ok = !strcmp(right, "1100"), upside = !strcmp(left, "1100");
    sub(c, 0, plain_ok && !strcmp(left, "0011"));
    int face_l = px2(&t, 0, 3)[1] > 127, face_r = px2(&t, 4, 0)[1] > 127;
    sub(c, 1, plain_ok && left[3] == '1' && face_l != face_r);
    say(c, "viewport (0,4,4,-4) rows lit %s (want 0011), (4,0,4,4) %s (want 1100); facing %s / %s (want different)", left, right,
        face_l ? "front" : "back", face_r ? "front" : "back");
    int upside2 = 0;
    if (c->multi_vp) {
        g.vs = spv_probe_quad_vp, g.vs_size = sizeof(spv_probe_quad_vp), g.viewports = 2;
        flip_rec f2 = {pl, gfx_ex(c, t.rp, pl, &g, &r), 1};
        if (!f2.p || !rt2_draw(c, &t, rec_flip, &f2)) {
            say(c, "; two viewports: pipeline/draw failed (%d)", r);
            sub(c, 2, 0);
        } else {
            rows_of(&t, 0, left);
            rows_of(&t, 4, right);
            upside2 = !strcmp(left, "1100") || !strcmp(right, "1100");
            sub(c, 2, !strcmp(left, "0011") && !strcmp(right, "0011"));
            say(c, "; two flipped viewports %s %s (want 0011 0011)", left, right);
        }
    } else
        say(c, "; no multiViewport with the VS's ViewportIndex: two viewports not tried");
    if (plain_ok && (upside || upside2)) {
        c->want_flags |= VKB_PROBE_FLAG_MK_FLIP;
        say(c, " - upside down: the driver ignores the negative height");
    } else if (c->failed)
        say(c, " - wrong, but not the flipped picture: viewport_flip stays off");
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* MK-03 (Arm erratum 3843237): integer vector bitwise ops and shifts on a lane swizzle or a vector
 * built lane by lane (bitwise.comp, -O0), x = 0x10 from a push constant, against the CPU. May pass
 * on an affected driver: the miscompile is pattern-specific. */
static void p_bitwise_vec(pctx *c)
{
    void *out = NULL;
    VkBuffer ob = buf(c, 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out);
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayout l = dsl(c, &b, 1, 0, NULL);
    VkPipelineLayout pl = l ? layout_push(c, VK_SHADER_STAGE_COMPUTE_BIT, 4, 1, &l) : VK_NULL_HANDLE;
    VkResult r = VK_ERROR_UNKNOWN;
    VkPipeline p = pl ? comp(c, spv_probe_bitwise, sizeof(spv_probe_bitwise), pl, NULL, &r) : VK_NULL_HANDLE;
    VkDescriptorSet s = p ? set_alloc(c, l) : VK_NULL_HANDLE;
    if (!ob || !s) {
        say(c, "setup failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    VkDescriptorBufferInfo bi = {ob, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
    c->d.vkUpdateDescriptorSets(c->dev, 1, &w, 0, NULL);
    const uint32_t x = 0x10;
    if (!dispatch(c, p, pl, s, &x, 4)) {
        outcome(c, PS_ERROR);
        return;
    }
    const uint32_t *o = out;
    const uint32_t want[15] = {x | 1u, 2u | 1u,
                               (x + 3u) & 0xF0u, (x + 2u) & 0x0Fu, (x + 1u) & 0xFFu, x & 0x01u,
                               (x + 1u) ^ 3u, x ^ 3u,
                               ~2u, ~1u, ~x,
                               (uint32_t)(-(int32_t)x >> 1), (uint32_t)((int32_t)x >> 2),
                               (x + 5u) << 1, x << 3};
    /* the expressions' lanes in b.v (the probe's cases: or, and, xor, not, sra, shl) */
    static const int first[7] = {0, 2, 6, 8, 11, 13, 15};
    if (o[15] != 0xc0de) {
        say(c, "the shader did not run (marker 0x%x)", o[15]);
        outcome(c, PS_ERROR);
        return;
    }
    for (int k = 0; k < 6; k++) {
        int ok = 1;
        for (int i = first[k]; i < first[k + 1]; i++) ok &= o[i] == want[i];
        sub(c, k, ok);
        if (!ok) {
            say(c, "%scase %d lanes", c->dlen ? "; " : "", k);
            for (int i = first[k]; i < first[k + 1]; i++) say(c, " 0x%x", o[i]);
            say(c, " (want");
            for (int i = first[k]; i < first[k + 1]; i++) say(c, " 0x%x", want[i]);
            say(c, ")");
        }
    }
    if (c->failed) {
        c->want_flags |= VKB_PROBE_FLAG_MK_BITWISE;
        say(c, " - integer vector bitwise ops miscompiled");
    } else
        say(c, "6 swizzle- and composite-fed vector bitwise ops and shifts right");
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* MK-20 (ncnn#7054: wrong 16-bit float results on the Mali-G715's r54 drivers): one dispatch of
 * fp16.comp, 16-bit float arithmetic on push-constant inputs, its results converted to 32 bits.
 * Every input, partial result and result is exact in 16 bits, so a right driver gives exact
 * values; one 16-bit ulp is allowed (two for relaxed precision). Cases: fma (fused, mul + add,
 * dot), range (the smallest normal, the top of the range; one product through a denormal, which
 * may flush to zero), convert (from 32 bits, to and from integers, a vector), relaxed (mediump
 * 32-bit maths a driver may run in 16 bits), chain (a 16-bit accumulation over a loop, scalar
 * and vector). Runs only where the driver offers shaderFloat16 (applies): one small pipeline. */
static float ulp16(float x)
{
    float a = x < 0 ? -x : x, u = 1.0f / 16777216.0f; /* 2^-24: a 16-bit denormal's step */
    while (u * 1024.0f * 2.0f <= a && u < 32.0f) u *= 2.0f;
    return u;
}

static void p_fp16_math(pctx *c)
{
    void *out = NULL;
    VkBuffer ob = buf(c, 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &out);
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayout l = dsl(c, &b, 1, 0, NULL);
    struct {
        float a[4], c[4];
        int32_t n;
    } pc = {{1.5f, 2.25f, -0.125f, 0.25f}, {1.0f / 16384.0f, 1.0f / 3.0f, 65280.0f, -2.75f}, 16};
    VkPipelineLayout pl = l ? layout_push(c, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pc), 1, &l) : VK_NULL_HANDLE;
    VkResult r = VK_ERROR_UNKNOWN;
    VkPipeline p = pl ? comp(c, spv_probe_fp16, sizeof(spv_probe_fp16), pl, NULL, &r) : VK_NULL_HANDLE;
    VkDescriptorSet s = p ? set_alloc(c, l) : VK_NULL_HANDLE;
    if (!ob || !s) {
        say(c, "setup failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    memset(out, 0, 128);
    VkDescriptorBufferInfo bi = {ob, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
    c->d.vkUpdateDescriptorSets(c->dev, 1, &w, 0, NULL);
    if (!dispatch(c, p, pl, s, &pc, sizeof(pc))) {
        outcome(c, PS_ERROR);
        return;
    }
    const float *o = out;
    /* lane 4 may also be 0 (its denormal flushed) */
    static const float want[18] = {3.25f, 3.25f, 6.6875f,
                                   3.0f / 16384.0f, 1.0f / 4096.0f, 16320.0f, 65280.0f,
                                   0.333251953125f, -2.0f, 2048.0f, -2.75f,
                                   3.25f, 30.0f,
                                   30.0f, 180.0f, 270.0f, -15.0f, 30.0f};
    static const int first[6] = {0, 3, 7, 11, 13, 18};
    if (o[18] != 12345.0f) {
        say(c, "the shader did not run (marker %g)", o[18]);
        outcome(c, PS_ERROR);
        return;
    }
    for (int k = 0; k < 5; k++) {
        int ok = 1;
        for (int i = first[k]; i < first[k + 1]; i++) {
            float d = o[i] - want[i], tol = ulp16(want[i]) * (k == 3 ? 2.0f : 1.0f);
            int good = (d <= tol && d >= -tol) || (i == 4 && o[i] == 0.0f);
            ok &= good;
        }
        sub(c, k, ok);
        if (!ok) {
            say(c, "%scase %d lanes", c->dlen ? "; " : "", k);
            for (int i = first[k]; i < first[k + 1]; i++) say(c, " %.9g", o[i]);
            say(c, " (want");
            for (int i = first[k]; i < first[k + 1]; i++) say(c, " %.9g", want[i]);
            say(c, ")");
        }
    }
    if (c->failed) {
        c->want_flags |= VKB_PROBE_FLAG_MK_FP16;
        say(c, " - 16-bit float arithmetic wrong");
    } else
        say(c, "fma, range, conversions, relaxed precision and a 16-bit accumulation right%s", o[4] == 0.0f ? " (16-bit denormals flushed)" : "");
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* MK-04 (Arm errata 3809538, 3893905, 3893903): dynamic state set before a pipeline bind lost at
 * the next draw. An 8x1 target with depth/stencil; pipelines A and B made alike with depth write
 * on, cull NONE, stencil write mask 0xFF (REPLACE, reference 1) and these dynamic: depth write,
 * cull mode, stencil write mask (and the sample mask with EDS3); C has none dynamic. The state is
 * set, then A bound, then B (nothing set again):
 *   depth_write    write off: red at z 0.5 (A), green 0.7 (B), blue 0.8 (B) - column 0 ends blue;
 *                  green means B wrote depth
 *   cull           cull FRONT_AND_BACK: column 1 stays clear
 *   stencil_wmask  write mask 0: column 2's stencil stays 0
 *   sample_mask    (EDS3) mask 0: column 3 stays clear
 *   static_sample_mask  then C (static mask: all) draws column 4 - not written means the dynamic
 *                  0 leaked into a static pipeline (3893903) */
typedef struct dyn_rec {
    VkPipelineLayout pl;
    VkPipeline a, b, cst;
    int stencil, smask;
} dyn_rec;

static float colx(int k)
{
    return (float)k / 4.0f - 1.0f;
}

static void rec_dyn(pctx *c, VkCommandBuffer cb, void *arg)
{
    dyn_rec *d = arg;
    const VkSampleMask all = ~0u, none = 0;
    c->d.vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    c->d.vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    c->d.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    if (d->smask) c->d.vkCmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &all);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->a);
    quad(c, cb, d->pl, colx(0), -1, colx(1), 1, 1, 0, 0, 0.5f, 1);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->b);
    quad(c, cb, d->pl, colx(0), -1, colx(1), 1, 0, 1, 0, 0.7f, 1);
    quad(c, cb, d->pl, colx(0), -1, colx(1), 1, 0, 0, 1, 0.8f, 1);
    quad(c, cb, d->pl, colx(2), -1, colx(3), 1, 1, 1, 1, 0.5f, 1);
    c->d.vkCmdSetCullMode(cb, VK_CULL_MODE_FRONT_AND_BACK);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->a);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->b);
    quad(c, cb, d->pl, colx(1), -1, colx(2), 1, 1, 1, 1, 0.5f, 1);
    if (!d->smask) return;
    c->d.vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    c->d.vkCmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &none);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->a);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->b);
    quad(c, cb, d->pl, colx(3), -1, colx(4), 1, 1, 1, 1, 0.5f, 1);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->cst);
    quad(c, cb, d->pl, colx(4), -1, colx(5), 1, 1, 1, 1, 0.5f, 1);
}

static void p_dyn_state(pctx *c)
{
    if (!c->eds) {
        say(c, "no extended dynamic state");
        outcome(c, PS_SKIP);
        return;
    }
    VkFormat dsf = ds_format(c);
    rt2 t;
    VkPipelineLayout pl = layout_push(c, VK_SHADER_STAGE_VERTEX_BIT, sizeof(quad_pc), 0, NULL);
    if (!dsf || !pl || !rt2_make(c, &t, 8, 1, dsf)) {
        say(c, "setup failed (depth format %d)", dsf);
        outcome(c, PS_ERROR);
        return;
    }
    dyn_rec d = {pl, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, has_stencil(dsf), c->eds3_smask};
    VkStencilOpState so = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 1};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_TRUE,
                                                VK_COMPARE_OP_LESS, VK_FALSE, (VkBool32)d.stencil, so, so, 0, 1};
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                             VK_DYNAMIC_STATE_SAMPLE_MASK_EXT};
    const VkSampleMask all = ~0u;
    gx g = {spv_probe_quad, spv_probe_col, sizeof(spv_probe_quad), sizeof(spv_probe_col), NULL, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 8, 1, 1, dyn,
            d.smask ? 4u : 3u, VK_CULL_MODE_NONE, &ds, &all};
    VkResult r = VK_SUCCESS;
    d.a = gfx_ex(c, t.rp, pl, &g, &r);
    d.b = d.a ? gfx_ex(c, t.rp, pl, &g, &r) : VK_NULL_HANDLE;
    if (d.smask && d.b) {
        g.ndyn = 0;
        d.cst = gfx_ex(c, t.rp, pl, &g, &r);
    }
    if (!d.b || (d.smask && !d.cst) || !rt2_draw(c, &t, rec_dyn, &d)) {
        say(c, "pipeline/draw failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    const uint8_t *p0 = px2(&t, 0, 0);
    int lit1 = px2(&t, 1, 0)[0] > 200, lit3 = px2(&t, 3, 0)[0] > 200, lit4 = px2(&t, 4, 0)[0] > 200;
    sub(c, 0, p0[2] > 200 && p0[1] < 50);
    sub(c, 1, !lit1);
    if (d.stencil) sub(c, 2, t.st[2] == 0);
    if (d.smask) sub(c, 3, !lit3), sub(c, 4, lit4);
    say(c, "after a bind: depth write off %s (rgb %u %u %u), cull FRONT_AND_BACK %s", p0[2] > 200 && p0[1] < 50 ? "kept" : "LOST", p0[0], p0[1],
        p0[2], lit1 ? "LOST (drawn)" : "kept");
    if (d.stencil) say(c, ", stencil write mask 0 %s (stencil %u)", t.st[2] == 0 ? "kept" : "LOST", t.st[2]);
    else say(c, "; no stencil format");
    if (d.smask) say(c, ", sample mask 0 %s, static mask after it %s", lit3 ? "LOST (drawn)" : "kept", lit4 ? "drawn" : "NOT drawn (stale dynamic 0)");
    else say(c, "; no EDS3 sample mask");
    if (c->failed) c->want_flags |= VKB_PROBE_FLAG_MK_DYN;
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* MK-05 (Arm erratum 3786496): vkCmdBindVertexBuffers2's pStrides applied by array index, not by
 * binding number. One pipeline, strides dynamic (made 4), three instance-rate R32_UINT attributes
 * from bindings 0, 2, 3 (1 bound, unused); every buffer word holds its own index, so the read-back
 * says which word instance i fetched (want 4i, 8i, 2i: strides 16 / 32 / 8). Bound three ways:
 *   from_0   one call from binding 0, four bindings (16, 4, 32, 8): array index = binding number
 *   from_1   binding 0 alone, then one call from binding 1, three bindings (4, 32, 8)
 *   single   single-binding calls only, binding 2 first with 12 then again with 32
 * from_1 wrong alone: the split (vb_split, option A) fixes it; single wrong too: it cannot (option
 * B: strides baked into pipeline variants). */
typedef struct vbs_rec {
    VkBuffer b;
    int how;
} vbs_rec;

static void rec_vbs(pctx *c, VkCommandBuffer cb, void *arg)
{
    vbs_rec *v = arg;
    VkBuffer bs[4] = {v->b, v->b, v->b, v->b};
    VkDeviceSize off[4] = {0, 0, 0, 0};
    if (v->how == 0) {
        VkDeviceSize s[4] = {16, 4, 32, 8};
        c->d.vkCmdBindVertexBuffers2(cb, 0, 4, bs, off, NULL, s);
    } else if (v->how == 1) {
        VkDeviceSize s0 = 16, s[3] = {4, 32, 8};
        c->d.vkCmdBindVertexBuffers2(cb, 0, 1, bs, off, NULL, &s0);
        c->d.vkCmdBindVertexBuffers2(cb, 1, 3, bs, off, NULL, s);
    } else {
        static const uint32_t bind[5] = {0, 2, 3, 2, 1};
        static const VkDeviceSize s[5] = {16, 12, 8, 32, 4};
        for (int i = 0; i < 5; i++) c->d.vkCmdBindVertexBuffers2(cb, bind[i], 1, bs, off, NULL, &s[i]);
    }
    c->d.vkCmdDraw(cb, 1, 4, 0, 0);
}

static void p_vb_strides(pctx *c)
{
    if (!c->eds) {
        say(c, "no extended dynamic state");
        outcome(c, PS_SKIP);
        return;
    }
    rt t;
    void *m = NULL;
    VkBuffer vb = buf(c, 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m);
    VkPipelineLayout pl = layout_push(c, 0, 0, 0, NULL);
    if (!vb || !pl || !rt_make(c, &t, 1)) {
        outcome(c, PS_ERROR);
        return;
    }
    for (uint32_t i = 0; i < 64; i++) ((uint32_t *)m)[i] = i;
    VkVertexInputBindingDescription bd[4];
    for (uint32_t i = 0; i < 4; i++) bd[i] = (VkVertexInputBindingDescription){i, 4, VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription ad[3] = {{0, 0, VK_FORMAT_R32_UINT, 0}, {1, 2, VK_FORMAT_R32_UINT, 0}, {2, 3, VK_FORMAT_R32_UINT, 0}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 4, bd, 3, ad};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    gx g = {spv_probe_vbs, spv_probe_col, sizeof(spv_probe_vbs), sizeof(spv_probe_col), &vi, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 8, 1, 1, dyn, 1,
            VK_CULL_MODE_NONE, NULL, NULL};
    VkResult r;
    VkPipeline p = gfx_ex(c, t.rp, pl, &g, &r);
    if (!p) {
        say(c, "pipeline refused (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    static const char *const how[3] = {"one call from binding 0", "one call from binding 1", "single-binding calls"};
    for (int k = 0; k < 3; k++) {
        vbs_rec v = {vb, k};
        if (!rt_draw(c, &t, p, rec_vbs, &v)) {
            outcome(c, PS_ERROR);
            return;
        }
        int ok = 1;
        for (uint32_t i = 0; i < 4; i++) {
            const uint8_t *q = pxl(&t, 0, i);
            ok &= q[0] == 4 * i && q[1] == 8 * i && q[2] == 2 * i;
        }
        sub(c, k, ok);
        say(c, "%s%s: %s", k ? "; " : "", how[k], ok ? "right" : "WRONG");
        if (!ok) say(c, " (instance 3 fetched words %u %u %u, want 12 24 6)", pxl(&t, 0, 3)[0], pxl(&t, 0, 3)[1], pxl(&t, 0, 3)[2]);
    }
    if (c->failed & 4) {
        c->want_flags |= VKB_PROBE_FLAG_MK_VB | VKB_PROBE_FLAG_MK_VB_SINGLE;
        say(c, " - single-binding calls wrong too: splitting cannot fix it (option B, strides baked into pipeline variants, is not built)");
    } else if (c->failed) {
        c->want_flags |= VKB_PROBE_FLAG_MK_VB;
        say(c, " - strides by array index in multi-binding calls: the split (vb_split, option A) fixes it");
    }
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* MK-07: a 2D-array view of a 3D image (4x4x4 RGBA8, 2D_ARRAY_COMPATIBLE) with base layer 1 and
 * VK_REMAINING_ARRAY_LAYERS, cleared as a 3-layer framebuffer: slices 1-3 written, slice 0 keeps
 * what it had. Wrong slices, or a refused view, fail; a device loss is a hang as usual. Record
 * only: view_counts is on everywhere. */
/* One view of type vt (base layer 1, `layers` layers) of the 3D image i cleared as a fb_layers-layer
 * framebuffer and read back into out through ob: 1 the slices are as `want` ("-WWW" form: W cleared by the
 * view, - untouched), 0 wrong (sl: what was written, or "refused"), -1 could not run.
 * copy: the same slices rendered through a 2D-array image of the bridge's own, copied in before and out after the
 * pass (pv_view3d_base's copy form, server_errata_view3d.c: vkb_pv3d_begin / vkb_pv3d_end, the code the fix records) */
static int view3d_run(pctx *c, VkImage i, VkBuffer ob, void *out, VkImageViewType vt, uint32_t layers, uint32_t fb_layers, const char *want,
                      char sl[8], int copy)
{
    VkResult r;
    VkImageView v = VK_NULL_HANDLE;
    vkb_pv3d_pair pair = {i, VK_NULL_HANDLE, 0, 1, fb_layers, {{0, 0}, {4, 4}}, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    if (copy) {
        VkImageCreateInfo si = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {4, 4, 1}, 1, layers,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        pair.shadow = img(c, &si, &r);
        if (pair.shadow) v = view(c, pair.shadow, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, layers);
        if (!v) {
            snprintf(sl, 8, "refused");
            return 0;
        }
    } else {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, i, vt, VK_FORMAT_R8G8B8A8_UNORM,
                                    {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, layers}};
        r = c->d.vkCreateImageView(c->dev, &vi, NULL, &v);
        if (r != VK_SUCCESS) {
            snprintf(sl, 8, "refused");
            return 0;
        }
        track(c, O_VIEW, H(v));
    }
    VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar, NULL, NULL, 0, NULL};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 1, &dep};
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (c->d.vkCreateRenderPass(c->dev, &rpi, NULL, &rp) != VK_SUCCESS) {
        return -1;
    }
    track(c, O_RP, H(rp));
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &v, 4, 4, fb_layers};
    if (c->d.vkCreateFramebuffer(c->dev, &fi, NULL, &fb) != VK_SUCCESS) {
        return -1;
    }
    track(c, O_FB, H(fb));
    VkCommandBuffer cb = cmd(c);
    if (!cb) {
        return -1;
    }
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkClearColorValue before = {{0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    c->d.vkCmdClearColorImage(cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &before, 1, &all);
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 1);
    VkClearValue cv;
    cv.color = (VkClearColorValue){{0xcc / 255.0f, 0xcc / 255.0f, 0xcc / 255.0f, 0xcc / 255.0f}};
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {4, 4}}, 1, &cv};
    if (copy) vkb_pv3d_begin(&c->d, cb, &pair, 0); /* loadOp CLEAR: nothing to copy in */
    c->d.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    c->d.vkCmdEndRenderPass(cb);
    if (copy) vkb_pv3d_end(&c->d, cb, &pair, 1);
    VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 4}};
    c->d.vkCmdCopyImageToBuffer(cb, i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob, 1, &rg);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (submit(c, cb) != VK_SUCCESS) {
        return -1;
    }
    const uint8_t *o = out;
    for (int z = 0; z < 4; z++) {
        int n11 = 0, ncc = 0;
        for (int k = 0; k < 64; k++) n11 += o[z * 64 + k] == 0x11, ncc += o[z * 64 + k] == 0xcc;
        sl[z] = n11 == 64 ? '-' : ncc == 64 ? 'W' : '?';
    }
    sl[4] = 0;
    return !strcmp(sl, want);
}

/* view3d_layers on every driver but PowerVR: test49's probe as it was, one sub-case (MK-07), so no other GPU runs
 * PV-04's extra views and renders and its result is the same as before */
static void p_view3d_layers_one(pctx *c)
{
    if (c->api < VK_API_VERSION_1_1) {
        say(c, "Vulkan 1.0: no 2D array views of 3D images");
        outcome(c, PS_SKIP);
        return;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, VK_IMAGE_TYPE_3D,
                            VK_FORMAT_R8G8B8A8_UNORM, {4, 4, 4}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r;
    VkImage i = img(c, &ci, &r);
    void *out = NULL;
    VkBuffer ob = buf(c, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out);
    if (!i || !ob) {
        say(c, "3D image refused (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, i, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM,
                                {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_REMAINING_ARRAY_LAYERS}};
    VkImageView v = VK_NULL_HANDLE;
    r = c->d.vkCreateImageView(c->dev, &vi, NULL, &v);
    if (r != VK_SUCCESS) {
        say(c, "the 2D array view (base 1, VK_REMAINING_ARRAY_LAYERS) refused: %d", r);
        c->want_flags |= VKB_PROBE_FLAG_MK_VIEW3D;
        sub(c, 0, 0);
        outcome(c, PS_FAIL);
        return;
    }
    track(c, O_VIEW, H(v));
    VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar, NULL, NULL, 0, NULL};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 1, &dep};
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (c->d.vkCreateRenderPass(c->dev, &rpi, NULL, &rp) != VK_SUCCESS) {
        outcome(c, PS_ERROR);
        return;
    }
    track(c, O_RP, H(rp));
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &v, 4, 4, 3};
    if (c->d.vkCreateFramebuffer(c->dev, &fi, NULL, &fb) != VK_SUCCESS) {
        outcome(c, PS_ERROR);
        return;
    }
    track(c, O_FB, H(fb));
    VkCommandBuffer cb = cmd(c);
    if (!cb) {
        outcome(c, PS_ERROR);
        return;
    }
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkClearColorValue before = {{0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    c->d.vkCmdClearColorImage(cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &before, 1, &all);
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 1);
    VkClearValue cv;
    cv.color = (VkClearColorValue){{0xcc / 255.0f, 0xcc / 255.0f, 0xcc / 255.0f, 0xcc / 255.0f}};
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {4, 4}}, 1, &cv};
    c->d.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    c->d.vkCmdEndRenderPass(cb);
    VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 4}};
    c->d.vkCmdCopyImageToBuffer(cb, i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob, 1, &rg);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (submit(c, cb) != VK_SUCCESS) {
        outcome(c, PS_ERROR);
        return;
    }
    const uint8_t *o = out;
    char sl[8];
    for (int z = 0; z < 4; z++) {
        int n11 = 0, ncc = 0;
        for (int k = 0; k < 64; k++) n11 += o[z * 64 + k] == 0x11, ncc += o[z * 64 + k] == 0xcc;
        sl[z] = n11 == 64 ? '-' : ncc == 64 ? 'W' : '?';
    }
    sl[4] = 0;
    int ok = !strcmp(sl, "-WWW");
    sub(c, 0, ok);
    say(c, "slices written: %s (want -WWW: 1-3 cleared by the view, 0 untouched)", sl);
    if (!ok) c->want_flags |= VKB_PROBE_FLAG_MK_VIEW3D;
    outcome(c, ok ? PS_PASS : PS_FAIL);
}

/* PV-04's other pattern (PowerVR CXTP 25.3, Pixel 11): a feedback loop - ATTACHMENT_FEEDBACK_LOOP_OPTIMAL, draws of one
 * colour per layer, a pipeline barrier inside the pass, then draws that sample the 3D image at their own texel and
 * write it red and blue swapped - into a 3-slice 2D-array view (base 1) of a 4x4x4 3D image: the sampling reads wrong
 * values. mode 0 the view as the app makes it; 1 the same through a 2D-array image of the bridge's own, copied in and out
 * with the pass split at the barrier (what the fix records: vkb_pv3d_begin / vkb_pv3d_copy_sync / vkb_pv3d_end); 2 the
 * control, a plain 3-layer 2D-array image (no 3D image in it). Layers by gl_Layer from the vertex shader, or from a
 * geometry shader where the device has no shaderOutputLayer. mode 3 is the control's loop with the pass split at the barrier:
 * ended there, begun again with loadOp LOAD (a render pass of the same attachment that loads what the first stored, starts
 * where it ended), no copy at all - the form pv_fbloop_split (PV-06, server_errata_view3d.c) records. 1 the slices are as wanted ("-bbb": b drawn, read back and
 * swapped; - untouched; the control "bbb"), 0 wrong (sl: what they hold, ? other), -1 could not run. */
#define FB_W 4
#define FB_BASE 1
#define FB_CNT 3

static uint32_t fb_col(uint32_t l)
{
    return (20u + 40u * l) | (200u - 30u * l) << 8 | 85u << 16 | 255u << 24;
}

static uint32_t fb_swap(uint32_t c)
{
    return (c & 0xff00ff00u) | (c & 0xffu) << 16 | (c >> 16 & 0xffu);
}

static VkPipeline fb_pipe(pctx *c, VkRenderPass rp, VkPipelineLayout pl, int rd, int rd3, int loop)
{
    int gs = !c->layer_vs;
    VkShaderModule vs = gs ? module(c, spv_probe_fbl_gsvs, sizeof(spv_probe_fbl_gsvs)) : module(c, spv_probe_fbl_vs, sizeof(spv_probe_fbl_vs));
    VkShaderModule gm = gs ? module(c, spv_probe_fbl_gs, sizeof(spv_probe_fbl_gs)) : VK_NULL_HANDLE;
    VkShaderModule fs = !rd ? module(c, spv_probe_fbl_col, sizeof(spv_probe_fbl_col))
                        : rd3 ? module(c, spv_probe_fbl_rd3, sizeof(spv_probe_fbl_rd3)) : module(c, spv_probe_fbl_rd2a, sizeof(spv_probe_fbl_rd2a));
    if (!vs || !fs || (gs && !gm)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[3] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gm, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkViewport vp = {0, 0, FB_W, FB_W, 0, 1};
    VkRect2D sc = {{0, 0}, {FB_W, FB_W}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL,
                                                 VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {0, 0, 0, 0, 0, 0, 0, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &ba, {0, 0, 0, 0}};
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL,
                                       loop ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0, gs ? 3u : 2u, st, &vi, &ia, NULL, &vps, &rs,
                                       &ms, NULL, &cbs, NULL, pl, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    if (c->d.vkCreateGraphicsPipelines(c->dev, VK_NULL_HANDLE, 1, &pi, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_PIPE, H(p));
    return p;
}

/* a view of img (type vt, layers from base) that is only used as usage u (a 2D-array view of a 3D image takes the attachment usage only) */
static VkImageView fb_view(pctx *c, VkImage i, VkImageViewType vt, uint32_t base, uint32_t layers, VkImageUsageFlags u)
{
    VkImageViewUsageCreateInfo ui = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, u};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, u ? &ui : NULL, 0, i, vt, VK_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0},
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, base, layers}};
    VkImageView v = VK_NULL_HANDLE;
    if (c->d.vkCreateImageView(c->dev, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    track(c, O_VIEW, H(v));
    return v;
}

static int fb_run(pctx *c, int mode, char sl[8])
{
    static const VkImageUsageFlags fbu = VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    const VkImageLayout lay = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
    int is3d = mode < 2, nsl = is3d ? FB_W : FB_CNT;
    int split = mode == 1 || mode == 3; /* the pass ended at the barrier and begun again */
    VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | fbu;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0u,
                            is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {FB_W, FB_W, is3d ? FB_W : 1u}, 1, is3d ? 1u : FB_CNT,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r;
    VkImage im = img(c, &ci, &r);
    VkImage sh = VK_NULL_HANDLE;
    if (mode == 1) {
        VkImageCreateInfo si = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {FB_W, FB_W, 1}, 1, FB_CNT,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | fbu,
                                VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        sh = img(c, &si, &r);
    }
    VkImageView att = !im ? VK_NULL_HANDLE
                      : mode == 1 ? (sh ? fb_view(c, sh, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, FB_CNT, 0) : VK_NULL_HANDLE)
                      : mode == 0 ? fb_view(c, im, VK_IMAGE_VIEW_TYPE_2D_ARRAY, FB_BASE, FB_CNT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | fbu)
                                  : fb_view(c, im, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, FB_CNT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | fbu);
    VkImageView smp = !im ? VK_NULL_HANDLE
                      : is3d ? fb_view(c, im, VK_IMAGE_VIEW_TYPE_3D, 0, 1, VK_IMAGE_USAGE_SAMPLED_BIT)
                             : fb_view(c, im, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, FB_CNT, VK_IMAGE_USAGE_SAMPLED_BIT);
    void *out = NULL;
    VkBuffer ob = buf(c, 4u * FB_W * FB_W * FB_W, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler smpl = VK_NULL_HANDLE;
    if (c->d.vkCreateSampler(c->dev, &sci, NULL, &smpl) == VK_SUCCESS) track(c, O_SAMPLER, H(smpl));
    if (!im || !att || !smp || !ob || !smpl) return -1;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayout dl = dsl(c, &b, 1, 0, NULL);
    VkPipelineLayout pl = dl ? layout_push(c, VK_SHADER_STAGE_FRAGMENT_BIT, 4, 1, &dl) : VK_NULL_HANDLE;
    VkDescriptorSet set = dl ? set_alloc(c, dl) : VK_NULL_HANDLE;
    if (!pl || !set) return -1;
    VkDescriptorImageInfo dii = {smpl, smp, lay};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, NULL, NULL};
    c->d.vkUpdateDescriptorSets(c->dev, 1, &w, 0, NULL);
    /* the app's pass, and the copy form's second part (loads what the first stored, starts where it ended) */
    VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ar = {0, lay};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar, NULL, NULL, 0, NULL};
    VkSubpassDependency dep[2] = {
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_TRANSFER_READ_BIT, 0},
        {0, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_SHADER_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT}};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 2, dep};
    VkRenderPass rp = VK_NULL_HANDLE, rp2 = VK_NULL_HANDLE;
    if (c->d.vkCreateRenderPass(c->dev, &rpi, NULL, &rp) != VK_SUCCESS) return -1;
    track(c, O_RP, H(rp));
    if (split) {
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ad.initialLayout = ad.finalLayout;
        if (c->d.vkCreateRenderPass(c->dev, &rpi, NULL, &rp2) != VK_SUCCESS) return -1;
        track(c, O_RP, H(rp2));
    }
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &att, FB_W, FB_W, FB_CNT};
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (c->d.vkCreateFramebuffer(c->dev, &fi, NULL, &fb) != VK_SUCCESS) return -1;
    track(c, O_FB, H(fb));
    VkPipeline p1 = fb_pipe(c, rp, pl, 0, 0, 0), p2 = fb_pipe(c, rp, pl, 1, is3d, 1);
    if (!p1 || !p2) return -1;
    vkb_pv3d_fns fns = {c->d.vkCmdPipelineBarrier, c->d.vkCmdCopyImage};
    vkb_pv3d_pair pair = {im, sh, 0, FB_BASE, FB_CNT, {{0, 0}, {FB_W, FB_W}}, lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lay,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, FB_CNT};
    VkCommandBuffer cb = cmd(c);
    if (!cb) return -1;
    img_barrier(c, cb, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, is3d ? 1 : FB_CNT);
    VkClearColorValue c0 = {{0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, is3d ? 1u : FB_CNT};
    c->d.vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c0, 1, &all);
    img_barrier(c, cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, lay, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, is3d ? 1 : FB_CNT);
    if (mode == 1) vkb_pv3d_begin(&c->d, cb, &pair, 0); /* loadOp CLEAR: nothing to copy in */
    VkClearValue cv;
    cv.color = (VkClearColorValue){{0xcc / 255.0f, 0xcc / 255.0f, 0xcc / 255.0f, 0xcc / 255.0f}};
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {FB_W, FB_W}}, 1, &cv};
    int32_t base = is3d ? FB_BASE : 0;
    c->d.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    c->d.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    c->d.vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &base);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p1);
    c->d.vkCmdDraw(cb, 3, FB_CNT, 0, 0);
    if (split) { /* the split: the pass ended, (the slices copied,) begun again where it was */
        c->d.vkCmdEndRenderPass(cb);
        if (mode == 1) vkb_pv3d_copy_sync(&fns, cb, &pair, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lay, lay);
        else { /* no copy: what the first part wrote is made visible to the second's loads and reads */
            VkMemoryBarrier sb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
            c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &sb, 0, NULL, 0, NULL);
        }
        bi.renderPass = rp2;
        c->d.vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        c->d.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
        c->d.vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &base);
    }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT, 1, &mb, 0, NULL, 0, NULL);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p2);
    c->d.vkCmdDraw(cb, 3, FB_CNT, 0, 0);
    c->d.vkCmdEndRenderPass(cb);
    if (mode == 1) vkb_pv3d_end(&c->d, cb, &pair, 1);
    VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, is3d ? 1u : FB_CNT}, {0, 0, 0}, {FB_W, FB_W, is3d ? FB_W : 1u}};
    c->d.vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob, 1, &rg);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    if (submit(c, cb) != VK_SUCCESS) return -1;
    const uint32_t *o = out;
    int ok = 1;
    for (int z = 0; z < nsl; z++) {
        int l = is3d ? z - FB_BASE : z;
        int drawn = l >= 0 && l < FB_CNT;
        uint32_t want = drawn ? fb_swap(fb_col((uint32_t)l)) : 0x11111111u, got_bad = 0;
        for (int k = 0; k < FB_W * FB_W; k++) got_bad += o[z * FB_W * FB_W + k] != want;
        sl[z] = !got_bad ? (drawn ? 'b' : '-') : '?';
        if (got_bad) ok = 0;
    }
    sl[nsl] = 0;
    return ok;
}

static void p_view3d_layers(pctx *c)
{
    /* the five sub-cases on PowerVR (and under PV-04's or PV-06's host imitation, which needs them to show the pattern); every other
     * driver keeps the one sub-case it had */
    const char *imi = vkb_srv_env("VKBRIDGE_ERRATA_IMITATE");
    if (!c->pvr && !(imi && (strstr(imi, "pv_view3d_base") || strstr(imi, "pv_fbloop_split")))) {
        p_view3d_layers_one(c);
        return;
    }
    if (c->api < VK_API_VERSION_1_1) {
        say(c, "Vulkan 1.0: no 2D array views of 3D images");
        outcome(c, PS_SKIP);
        return;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, VK_IMAGE_TYPE_3D,
                            VK_FORMAT_R8G8B8A8_UNORM, {4, 4, 4}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r;
    VkImage i = img(c, &ci, &r);
    void *out = NULL;
    VkBuffer ob = buf(c, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out);
    if (!i || !ob) {
        say(c, "3D image refused (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    /* 0 the app's form, 1 view_counts' form; 2 and 3 (one slice) say what else the driver gets right, for pv_view3d_base's
     * one-slice forms; 4 renders the slices of 1 through a 2D-array image of the bridge's own (its copy form) */
    static const struct {
        const char *how, *want;
        VkImageViewType vt;
        uint32_t layers, fb;
        int copy;
    } v3[5] = {{"VK_REMAINING_ARRAY_LAYERS", "-WWW", VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_REMAINING_ARRAY_LAYERS, 3, 0},
               {"explicit count 3 (view_counts' form)", "-WWW", VK_IMAGE_VIEW_TYPE_2D_ARRAY, 3, 3, 0},
               {"one-slice 2D-array view", "-W--", VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 1, 0},
               {"2D view of slice 1", "-W--", VK_IMAGE_VIEW_TYPE_2D, 1, 1, 0},
               {"3 slices through a copied image", "-WWW", VK_IMAGE_VIEW_TYPE_2D_ARRAY, 3, 3, 1}};
    char sl[5][8];
    int res[5];
    for (int k = 0; k < 5; k++) {
        res[k] = view3d_run(c, i, ob, out, v3[k].vt, v3[k].layers, v3[k].fb, v3[k].want, sl[k], v3[k].copy);
        if (res[k] < 0) {
            outcome(c, PS_ERROR);
            return;
        }
        sub(c, k, k == 4 ? res[k] || res[0] || res[1] : res[k]); /* the copy form only matters where the pattern is */
        say(c, "%s%s: slices %s (want %s)", k ? "; " : "", v3[k].how, sl[k], v3[k].want);
    }
    say(c, " (W cleared by the view, - untouched)");
    /* 5-8: feedback loops into the 3-slice view (PowerVR CXTP 25.3), raw, through the copy form, the control into a plain
     * 2D-array image, and the control's loop with the pass split at the barrier (no copy: pv_fbloop_split's form) */
    int fbr[4] = {-1, -1, -1, -1};
    char fsl[4][8] = {"", "", "", ""};
    if (c->fbloop && (c->layer_vs || c->gs)) {
        release(c); /* the first sub-cases' objects (the probe keeps 96) */
        static const int order[4] = {0, 2, 3, 1};
        for (int k = 0; k < 4; k++) {
            fbr[order[k]] = fb_run(c, order[k], fsl[order[k]]);
            release(c);
            if (fbr[order[k]] < 0) break;
        }
    }
    int fb_ran = fbr[0] >= 0 && fbr[1] >= 0 && fbr[2] >= 0 && fbr[3] >= 0;
    if (fb_ran) {
        sub(c, 5, fbr[0]);
        sub(c, 6, fbr[1] || fbr[0]); /* the copy form only matters where the 3D view's loop is wrong */
        sub(c, 7, fbr[2]);           /* the plain control: wrong is PV-06's pattern (the flags are in force only for a failed probe) */
        sub(c, 8, fbr[3] || fbr[2]); /* the split form only matters where the plain loop is wrong */
        say(c, "; feedback loop: 3D view %s, copied %s, 2D array %s, 2D array split %s (want -bbb -bbb bbb bbb: b read and swapped)", fsl[0], fsl[1], fsl[2],
            fsl[3]);
        if (!fbr[2] && fbr[3]) { /* PV-06: the plain loop is wrong and the pass ended at the barrier and begun again is right */
            c->want_flags |= VKB_PROBE_FLAG_PV_FBLOOP;
            say(c, " - pv_fbloop_split (PV-06): feedback-loop passes are split at their barriers");
        } else if (!fbr[2])
            say(c, " - plain loop wrong, split form wrong too: no form of PV-06 proven");
        if (!fbr[0] && fbr[2] && res[1]) { /* (not where the base layer bug explains it: that one's fix copies those views) */
            c->want_flags |= VKB_PROBE_FLAG_PV_V3D_FB;
            if (fbr[1]) c->want_flags |= VKB_PROBE_FLAG_PV_V3D_FBCOPY;
            say(c, " - pv_view3d_base (PV-04): feedback loops in such views are %s", fbr[1] ? "copied" : "wrong with the copy too: no form proven");
        }
    } else if (c->fbloop)
        say(c, "; feedback loop: %s", c->layer_vs || c->gs ? "not run (could not be set up)" : "no layered rendering here");
    if (!res[0]) {
        c->want_flags |= VKB_PROBE_FLAG_MK_VIEW3D;
        say(c, res[1] ? " - the explicit count is right: view_counts fixes it" : " - the explicit count is wrong too: view_counts does not fix it here");
    }
    if (!res[0] && !res[1]) {
        /* PV-04: the pattern, and the forms of its fix the same run proved (server_errata_view3d.c goes by these flags) */
        c->want_flags |= VKB_PROBE_FLAG_PV_VIEW3D;
        if (res[2]) c->want_flags |= VKB_PROBE_FLAG_PV_V3D_ARRAY1;
        if (res[3]) c->want_flags |= VKB_PROBE_FLAG_PV_V3D_2D1;
        if (res[4]) c->want_flags |= VKB_PROBE_FLAG_PV_V3D_COPY;
        say(c, " - pv_view3d_base (PV-04): %s; %s", res[2] ? "one-slice views need nothing" : res[3] ? "one-slice views are rendered through a 2D view of the slice"
                                                    : res[4] ? "one-slice views are copied" : "one-slice views: no form proven",
            res[4] ? "multi-slice views are copied" : "multi-slice views: no form proven");
    }
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* MK-06 (record only; view_quarantine goes by the driver's version, nothing probes it reliably):
 * views made, rendered to with vkCmdBeginRendering (a clear, alternately into an RGBA8 and a BGRA8
 * image, a new colour each time), read back and destroyed, CHURN times - a driver that keys its
 * state by the view's handle value may render a re-made view with a stale one. */
#define CHURN 256

static void p_rendering_churn(pctx *c)
{
    if (!c->dynrender) {
        say(c, "no dynamic rendering");
        outcome(c, PS_SKIP);
        return;
    }
    static const VkFormat fmt[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    VkImage im[2];
    for (int k = 0; k < 2; k++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt[k], {1, 1, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        im[k] = img(c, &ci, NULL);
    }
    void *out = NULL;
    VkBuffer ob = buf(c, 4 * CHURN, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out);
    if (!im[0] || !im[1] || !ob) {
        outcome(c, PS_ERROR);
        return;
    }
    int wrong = 0, first_wrong = -1;
    for (int i = 0; i < CHURN; i++) {
        int k = i & 1;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im[k], VK_IMAGE_VIEW_TYPE_2D, fmt[k], {0, 0, 0, 0},
                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView v = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (c->d.vkCreateImageView(c->dev, &vi, NULL, &v) != VK_SUCCESS || !(cb = cmd(c))) {
            if (v) c->d.vkDestroyImageView(c->dev, v, NULL);
            say(c, "setup failed at %d; ", i);
            outcome(c, PS_ERROR);
            return;
        }
        uint8_t rgba[4] = {(uint8_t)(i * 37 + 11), (uint8_t)(i * 59 + 3), (uint8_t)(200 - i % 97), 255};
        img_barrier(c, cb, im[k], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 1);
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, v, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         VK_ATTACHMENT_STORE_OP_STORE};
        att.clearValue.color = (VkClearColorValue){{rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, 1.0f}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {1, 1}}, 1, 0, 1, &att, NULL, NULL};
        c->d.vkCmdBeginRendering(cb, &ri);
        c->d.vkCmdEndRendering(cb);
        img_barrier(c, cb, im[k], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
        VkBufferImageCopy rg = {(VkDeviceSize)i * 4, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}};
        c->d.vkCmdCopyImageToBuffer(cb, im[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob, 1, &rg);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        c->d.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkResult r = submit(c, cb);
        if (r != VK_SUCCESS) {
            say(c, "iteration %d: submission %d", i, r);
            outcome(c, PS_ERROR); /* the view stays with the device (abandoned on a hang) */
            return;
        }
        c->d.vkDestroyImageView(c->dev, v, NULL);
        const uint8_t *q = (const uint8_t *)out + i * 4;
        uint8_t got[3] = {k ? q[2] : q[0], q[1], k ? q[0] : q[2]};
        int ok = 1;
        for (int ch = 0; ch < 3; ch++) ok &= abs(got[ch] - rgba[ch]) <= 1;
        if (!ok && !wrong++) first_wrong = i;
    }
    sub(c, 0, !wrong);
    say(c, "%d views made, rendered to, read back and destroyed: %d wrong", CHURN, wrong);
    if (wrong) {
        say(c, " (the first: iteration %d) - a re-made view rendered with stale state", first_wrong);
        c->want_flags |= VKB_PROBE_FLAG_MK_CHURN;
    }
    outcome(c, wrong ? PS_FAIL : PS_PASS);
}

/* MK-01: a wait interrupted by a signal must go on (Arm's blob waits with ppoll and fails it with
 * VK_ERROR_INITIALIZATION_FAILED instead). A real-time signal nobody handles gets a no-op handler
 * without SA_RESTART; a second thread sends it to this one at 100, 200 and 300 ms into a 500 ms
 * wait on an unsignalled fence (fence), then a timeline semaphore (semaphore). Pass: VK_TIMEOUT
 * after the full 500 ms. Fail: an error, or any earlier return. Record only: wait_retry is on
 * everywhere (a fail says the mechanism is real on this driver). In the background: 1 s of waits. */
#define EINTR_WAIT_MS 500

static int eintr_sig;
static pthread_once_t eintr_once = PTHREAD_ONCE_INIT;

static void eintr_noop(int sig)
{
    (void)sig;
}

/* From the top down: the compile watchdog takes the first free one from SIGRTMIN + 4 up. Kept
 * installed (a no-op on a signal nobody else handles), so a late one is harmless. */
static void eintr_pick(void)
{
    for (int s = SIGRTMAX; s >= SIGRTMIN + 5 && !eintr_sig; s--) {
        struct sigaction old;
        if (sigaction(s, NULL, &old) != 0) continue;
        if ((old.sa_flags & SA_SIGINFO) || (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN)) continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = eintr_noop;
        sigemptyset(&sa.sa_mask);
        if (sigaction(s, &sa, NULL) == 0) eintr_sig = s;
    }
}

/* static: a probe ended by its guard leaves the kicker running against it */
static struct {
    pthread_t target;
    int sent;
    volatile int stop;
} kick;

static void *kick_main(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3 && !kick.stop; i++) {
        struct timespec ts = {0, 100000000};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        }
        if (kick.stop) break;
        if (pthread_kill(kick.target, eintr_sig) == 0) kick.sent++;
    }
    return NULL;
}

/* One wait with the signals sent into it: its result, *ms how long it took */
static VkResult eintr_wait(pctx *c, VkFence f, VkSemaphore s, double *ms)
{
    kick.target = pthread_self();
    kick.sent = 0;
    kick.stop = 0;
    pthread_t th;
    int kicking = pthread_create(&th, NULL, kick_main, NULL) == 0;
    double t0 = mono_ms();
    VkResult r;
    if (f) r = c->d.vkWaitForFences(c->dev, 1, &f, VK_TRUE, EINTR_WAIT_MS * 1000000ull);
    else {
        uint64_t v = 1;
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s, &v};
        r = c->d.vkWaitSemaphores(c->dev, &wi, EINTR_WAIT_MS * 1000000ull);
    }
    *ms = mono_ms() - t0;
    kick.stop = 1;
    if (kicking) pthread_join(th, NULL);
    return r;
}

static void p_wait_eintr(pctx *c)
{
    pthread_once(&eintr_once, eintr_pick);
    if (!eintr_sig) {
        say(c, "no free real-time signal");
        outcome(c, PS_SKIP);
        return;
    }
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    if (c->d.vkCreateFence(c->dev, &fi, NULL, &f) != VK_SUCCESS) {
        outcome(c, PS_ERROR);
        return;
    }
    track(c, O_FENCE, H(f));
    VkSemaphore s = VK_NULL_HANDLE;
    if (c->timeline && c->d.vkWaitSemaphores) {
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        if (c->d.vkCreateSemaphore(c->dev, &si, NULL, &s) == VK_SUCCESS) track(c, O_SEM, H(s));
    }
    static const char *const what[2] = {"fence", "timeline semaphore"};
    for (int k = 0; k < 2; k++) {
        if (k == 1 && !s) {
            say(c, "; no timeline semaphores");
            break;
        }
        double ms = 0;
        VkResult r = eintr_wait(c, k ? VK_NULL_HANDLE : f, s, &ms);
        int ok = r == VK_TIMEOUT && ms >= EINTR_WAIT_MS - 5;
        sub(c, k, ok);
        say(c, "%s%s, %d ms wait, %d signal(s) into it: %d after %.0f ms", k ? "; " : "", what[k], EINTR_WAIT_MS, kick.sent, r, ms);
        if (!ok) say(c, r == VK_TIMEOUT ? " (early)" : " (an error a wait may not return)");
    }
    if (c->failed) {
        c->want_flags |= VKB_PROBE_FLAG_MK_WAIT;
        say(c, " - the driver's waits do not survive signals (wait_retry retries them)");
    }
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* ------------------------------------------------------------------ the list */

/* PV-mutable-linear (PowerVR; PV-02): the image-format query for a LINEAR, dma-buf shareable,
 * mutable-format RGBA8 / BGRA8 image (a UNORM and an sRGB view format listed, the form gamescope
 * makes its flippable images in) is answered NOT_SUPPORTED, yet the image is made, exported,
 * imported and viewed. Per format (case 0: RGBA8, case 1: BGRA8) the query, then the same image:
 * created with the LINEAR modifier, its memory exported as a dma-buf, a second image made with the
 * exporter's layout (the explicit modifier) importing that dma-buf in a type the fd allows, bound
 * and viewed in both formats. The bug's flag only for query no, everything else works; a driver
 * that refuses both honestly passes; the query saying yes and the image failing is a fail without
 * the flag (the fix could not help). Not sampled from: the views are made, not drawn through. */
static VkResult pv_query(pctx *c, const VkPhysicalDeviceImageFormatInfo2 *ii)
{
    VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
    if (vkb_errata_pvmutable_probe_refuses(ii)) return VK_ERROR_FORMAT_NOT_SUPPORTED; /* the bug imitated */
    return c->idt->vkGetPhysicalDeviceImageFormatProperties2(c->pd, ii, &ip);
}

/* The image made, exported, imported, bound and viewed: 0 when it all worked, else which step failed (1 + step) */
static int pv_image_roundtrip(pctx *c, VkFormat f, VkFormat view_f, VkResult *res)
{
    const VkFormat list[2] = {f, view_f};
    const uint64_t linear = 0;
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, list};
    VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, &fl, 1, &linear};
    VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ml, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, f, {16, 16, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage a = VK_NULL_HANDLE;
    *res = c->d.vkCreateImage(c->dev, &ci, NULL, &a);
    if (*res != VK_SUCCESS) return 1;
    track(c, O_IMG, H(a));
    VkMemoryRequirements mr;
    c->d.vkGetImageMemoryRequirements(c->dev, a, &mr);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, a, VK_NULL_HANDLE};
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkDeviceMemory m = alloc(c, &mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ex);
    if (!m) return *res = VK_ERROR_OUT_OF_DEVICE_MEMORY, 2;
    if ((*res = c->d.vkBindImageMemory(c->dev, a, m, 0)) != VK_SUCCESS) return 3;
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, m, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    if ((*res = c->d.vkGetMemoryFdKHR(c->dev, &gi, &fd)) != VK_SUCCESS || fd < 0) return 4;
    /* the exporter's layout, imported as an explicit modifier image */
    VkImageDrmFormatModifierPropertiesEXT mp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    VkImageSubresource sr = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
    VkSubresourceLayout lay;
    if (!c->d.vkGetImageDrmFormatModifierPropertiesEXT || !c->d.vkGetImageSubresourceLayout) {
        close(fd);
        return *res = VK_ERROR_EXTENSION_NOT_PRESENT, 5;
    }
    c->d.vkGetImageDrmFormatModifierPropertiesEXT(c->dev, a, &mp);
    c->d.vkGetImageSubresourceLayout(c->dev, a, &sr, &lay);
    VkImageDrmFormatModifierExplicitCreateInfoEXT xi = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, &fl, mp.drmFormatModifier, 1, &lay};
    emi.pNext = &xi;
    VkImage b = VK_NULL_HANDLE;
    *res = c->d.vkCreateImage(c->dev, &ci, NULL, &b);
    if (*res != VK_SUCCESS) {
        close(fd);
        return 6;
    }
    track(c, O_IMG, H(b));
    c->d.vkGetImageMemoryRequirements(c->dev, b, &mr);
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    uint32_t bits = mr.memoryTypeBits;
    if (c->d.vkGetMemoryFdPropertiesKHR &&
        c->d.vkGetMemoryFdPropertiesKHR(c->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) == VK_SUCCESS && fp.memoryTypeBits)
        bits &= fp.memoryTypeBits;
    ded.image = b;
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd};
    uint32_t type = mem_type(c, bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = mem_type(c, bits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, type};
    VkDeviceMemory im = VK_NULL_HANDLE;
    *res = type == UINT32_MAX ? VK_ERROR_INVALID_EXTERNAL_HANDLE : c->d.vkAllocateMemory(c->dev, &ai, NULL, &im);
    if (*res != VK_SUCCESS) {
        close(fd);
        return 7;
    }
    track(c, O_MEM, H(im));
    if ((*res = c->d.vkBindImageMemory(c->dev, b, im, 0)) != VK_SUCCESS) return 8;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, b, VK_IMAGE_VIEW_TYPE_2D, f, {0, 0, 0, 0},
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    for (int k = 0; k < 2; k++) {
        vi.format = list[k];
        VkImageView v;
        if ((*res = c->d.vkCreateImageView(c->dev, &vi, NULL, &v)) != VK_SUCCESS) return 9;
        track(c, O_VIEW, H(v));
    }
    return 0;
}

static void p_pv_mutable_linear(pctx *c)
{
    if (!c->drm_mod || !c->dmabuf || !c->d.vkGetMemoryFdKHR || !c->idt->vkGetPhysicalDeviceImageFormatProperties2) {
        say(c, "no VK_EXT_image_drm_format_modifier with dma-bufs");
        outcome(c, PS_SKIP);
        return;
    }
    static const struct {
        VkFormat f, view;
        const char *what;
    } cases[2] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, "RGBA8"}, {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, "BGRA8"}};
    static const char *const stage[10] = {"", "image", "export memory", "bind", "export fd", "layout", "import image", "import memory", "import bind", "view"};
    int bug = 0;
    for (int k = 0; k < 2; k++) {
        const VkFormat list[2] = {cases[k].f, cases[k].view};
        VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, list};
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, &fl,
                                                            0 /* DRM_FORMAT_MOD_LINEAR */, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi,
                                                      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, cases[k].f, VK_IMAGE_TYPE_2D,
                                               VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                               VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
        VkResult q = pv_query(c, &ii), cr = VK_SUCCESS;
        /* gamescope's own form of the query too (its flippable image: usage TRANSFER_DST | SAMPLED, 0x6; a player log
         * session.log:99): the query counts as refused when either form is */
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkResult qg = pv_query(c, &ii);
        if (q == VK_SUCCESS && qg != VK_SUCCESS) q = qg;
        int step = pv_image_roundtrip(c, cases[k].f, cases[k].view, &cr);
        int q_ok = q == VK_SUCCESS, ok_img = step == 0;
        /* agree (both or neither) is fine; the query's no beside a working image is the bug */
        sub(c, k, q_ok == ok_img);
        bug |= !q_ok && ok_img;
        say(c, "%s%s: query %d, image %s", k ? "; " : "", cases[k].what, q, ok_img ? "made, exported, imported, viewed" : stage[step]);
        if (!ok_img) say(c, " (%d)", cr);
    }
    if (bug) {
        c->want_flags |= VKB_PROBE_FLAG_PV_MUTABLE;
        say(c, " - the query refuses an image the driver makes and shares: answered with the base format's");
    } else if (c->failed)
        say(c, " - the query says yes and the image fails: not the refusal pattern, pv_mutable_linear stays off");
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* PV-07 (PowerVR): a B8G8R8A8 image written by storage writes holds R8G8B8A8 byte order (gamescope composites into
 * such an image and its dma-buf reaches the screen: red and blue exchanged). Steam's blue (26,159,255) written by an
 * rgba8-declared compute store (pvstore.comp, gamescope's form) through a B8G8R8A8_UNORM view of a 4x4 B8G8R8A8 image
 * with STORAGE | TRANSFER_SRC, OPTIMAL and LINEAR, copied to a buffer: right = bytes FF 9F 1A FF; the bug = 1A 9F FF FF
 * exactly. PowerVR has no STORAGE feature on B8G8R8A8; the image is made anyway, as gamescope makes it, and a refusal
 * is nothing to fix. Sub-case 2 writes the same through an R8G8B8A8 image (the fix's form): it must read 1A 9F FF FF.
 * The bug's pattern: a B8G8R8A8 case exactly swapped and the R8G8B8A8 case right. */
static int pvb_write(pctx *c, VkFormat f, VkImageTiling tiling, VkDescriptorSetLayout l, VkPipelineLayout pl, VkPipeline p, unsigned char px[4],
                     VkResult *res)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {4, 4, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, tiling,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage i = img(c, &ci, res);
    if (!i) return 1; /* not made */
    VkImageView v = view(c, i, VK_IMAGE_VIEW_TYPE_2D, f, 1);
    void *out = NULL;
    VkBuffer ob = buf(c, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp = VK_NULL_HANDLE;
    if (!v || !ob || (*res = c->d.vkCreateDescriptorPool(c->dev, &dpi, NULL, &dp)) != VK_SUCCESS) return 2;
    track(c, O_DPOOL, H(dp));
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &l};
    VkDescriptorSet s = VK_NULL_HANDLE;
    if ((*res = c->d.vkAllocateDescriptorSets(c->dev, &ai, &s)) != VK_SUCCESS) return 2;
    VkDescriptorImageInfo ii = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, NULL, NULL};
    c->d.vkUpdateDescriptorSets(c->dev, 1, &w, 0, NULL);
    VkCommandBuffer cb = cmd(c);
    if (!cb) return 2;
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
    c->d.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    c->d.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s, 0, NULL);
    c->d.vkCmdDispatch(cb, 1, 1, 1);
    img_barrier(c, cb, i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
    c->d.vkCmdCopyImageToBuffer(cb, i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob, 1, &rg);
    if ((*res = submit(c, cb)) != VK_SUCCESS) return 3;
    const unsigned char *b = out;
    for (int k = 4; k < 64; k++)
        if (b[k] != b[k & 3]) return 4; /* not every texel the same: not a byte-order question */
    memcpy(px, b, 4);
    return 0;
}

/* The PowerVR-only probes (pv_bgra_store PV-07, pv_names PV-01) run on PowerVR's driver, under their own host imitation, or
 * with VKBRIDGE_PV_PROBES=1; every other GPU skips them, so its startup is test49's (no extra pipelines, the same profile
 * results). */
static int pv_probe_skipped(pctx *c, const char *erratum)
{
    const char *imi = vkb_srv_env("VKBRIDGE_ERRATA_IMITATE");
    if (c->pvr || (imi && strstr(imi, erratum)) || env_on("VKBRIDGE_PV_PROBES", 0)) return 0;
    say(c, "not a PowerVR driver: not run");
    outcome(c, PS_SKIP);
    return 1;
}

static void p_pv_bgra_store(pctx *c)
{
    if (pv_probe_skipped(c, "pv_bgra_store")) return;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayout l = dsl(c, &b, 1, 0, NULL);
    VkPipelineLayout pl = l ? layout_push(c, 0, 0, 1, &l) : VK_NULL_HANDLE;
    VkResult r = VK_SUCCESS;
    VkPipeline p = pl ? comp(c, spv_probe_pvstore, sizeof(spv_probe_pvstore), pl, NULL, &r) : VK_NULL_HANDLE;
    if (!p) {
        say(c, "pipeline failed (%d)", r);
        outcome(c, PS_ERROR);
        return;
    }
    static const unsigned char right_bgra[4] = {0xff, 0x9f, 0x1a, 0xff}, right_rgba[4] = {0x1a, 0x9f, 0xff, 0xff};
    static const struct {
        VkFormat f;
        VkImageTiling t;
        const char *what;
    } cases[3] = {{VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, "BGRA8 optimal"},
                  {VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_LINEAR, "BGRA8 linear"},
                  {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL, "RGBA8"}};
    static const char *const stage[5] = {"", "not made", "set up failed", "submit failed", "texels differ"};
    int swapped = 0, rgba_ok = 0;
    for (int k = 0; k < 3; k++) {
        unsigned char px[4] = {0};
        VkResult cr = VK_SUCCESS;
        int st = pvb_write(c, cases[k].f, cases[k].t, l, pl, p, px, &cr);
        int bgra = k < 2;
        const unsigned char *right = bgra ? right_bgra : right_rgba;
        int ok = st == 0 && !memcmp(px, right, 4);
        int swap = bgra && st == 0 && !memcmp(px, right_rgba, 4);
        swapped |= swap;
        if (!bgra) rgba_ok = ok;
        /* a B8G8R8A8 storage image the driver will not make is nothing to fix (gamescope's own image would fail first) */
        sub(c, k, ok || (bgra && st == 1));
        if (st == 0)
            say(c, "%s%s: %02X %02X %02X %02X%s", k ? "; " : "", cases[k].what, px[0], px[1], px[2], px[3],
                ok ? " right" : swap ? " (R and B exchanged)" : " (wrong)");
        else
            say(c, "%s%s: %s (%d)", k ? "; " : "", cases[k].what, stage[st], cr);
    }
    if (swapped && rgba_ok) {
        c->want_flags |= VKB_PROBE_FLAG_PV_BGRA_STORE;
        say(c, " - B8G8R8A8 storage writes land in R8G8B8A8 order: such images made R8G8B8A8");
    } else if (c->failed)
        say(c, " - wrong, but not the exchanged pattern with R8G8B8A8 right: pv_bgra_store stays off");
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

/* PV-names (PowerVR DXT 25.3, Android 17): a vertex input and the output that passes it on share an
 * OpName (DXVK names a passed-through attribute and its varying alike, "TEXCOORD") and the input
 * reads zero. pvnames.vert draws one triangle whose colour is that attribute, (64, 128, 191).
 *   shared    the module as DXVK writes it
 *   stripped  the same module without its names (vkb_spv_strip_names: what the fix sends): a
 *             failure here is no bug of this kind, and the fix could not help
 * The flag only for shared wrong and stripped right: the fix is then known to work on this driver. */
typedef struct {
    VkBuffer b;
} pv_rec;

static void rec_pv(pctx *c, VkCommandBuffer cb, void *arg)
{
    pv_rec *v = arg;
    VkDeviceSize off = 0;
    c->d.vkCmdBindVertexBuffers(cb, 0, 1, &v->b, &off);
    c->d.vkCmdDraw(cb, 3, 1, 0, 0);
}

static void p_pv_names(pctx *c)
{
    if (pv_probe_skipped(c, "pv_names")) return;
    rt t;
    void *m = NULL;
    VkBuffer vb = buf(c, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &m);
    VkPipelineLayout pl = layout_push(c, 0, 0, 0, NULL);
    if (!vb || !pl || !rt_make(c, &t, 1)) {
        outcome(c, PS_ERROR);
        return;
    }
    for (int i = 0; i < 3; i++) {
        float *v = (float *)m + 4 * i;
        v[0] = 0.25f, v[1] = 0.5f, v[2] = 0.75f, v[3] = 1.0f;
    }
    VkVertexInputBindingDescription bd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription ad = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &bd, 1, &ad};
    uint32_t *bare = NULL;
    size_t bare_size = 0;
    if (vkb_spv_strip_names(spv_probe_pvnames_vs, sizeof(spv_probe_pvnames_vs), &bare, &bare_size) != 1) {
        say(c, "the stripped module could not be made");
        outcome(c, PS_ERROR);
        return;
    }
    static const char *const how[2] = {"shared names", "names stripped"};
    for (int k = 0; k < 2; k++) {
        gx g = {k ? bare : spv_probe_pvnames_vs, spv_probe_pvnames_fs, k ? bare_size : sizeof(spv_probe_pvnames_vs), sizeof(spv_probe_pvnames_fs),
                &vi, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 8, 1, 1, NULL, 0, VK_CULL_MODE_NONE, NULL, NULL};
        VkResult r;
        VkPipeline p = gfx_ex(c, t.rp, pl, &g, &r);
        if (!p) {
            say(c, "%s: pipeline refused (%d)", how[k], r);
            free(bare);
            outcome(c, PS_ERROR);
            return;
        }
        pv_rec v = {vb};
        if (!rt_draw(c, &t, p, rec_pv, &v)) {
            free(bare);
            outcome(c, PS_ERROR);
            return;
        }
        const uint8_t *q = pxl(&t, 0, 3);
        int ok = q[0] >= 62 && q[0] <= 66 && q[1] >= 126 && q[1] <= 130 && q[2] >= 189 && q[2] <= 193 && q[3] == 255;
        sub(c, k, ok);
        say(c, "%s%s: %s", k ? "; " : "", how[k], ok ? "right" : "WRONG");
        if (!ok) say(c, " (%u %u %u %u, want 64 128 191 255)", q[0], q[1], q[2], q[3]);
    }
    free(bare);
    if ((c->failed & 1) && !(c->failed & 2)) {
        c->want_flags |= VKB_PROBE_FLAG_PV_NAMES;
        say(c, " - the input reads wrong beside an output of its name, right without names");
    } else if (c->failed)
        say(c, " - wrong, but not the shared-name pattern (the stripped module %s): pv_names stays off", (c->failed & 2) ? "fails too" : "is right");
    outcome(c, c->failed ? PS_FAIL : PS_PASS);
}

typedef struct probe_def {
    const char *name;
    void (*fn)(pctx *c);
    uint32_t emu;      /* emulation that stands in when it does not pass */
    uint64_t flags;    /* runtime flags it can set (the probe says which in want_flags) */
    int heavy;         /* compiles big shaders: a longer limit, last in line, first to be deferred */
    const char *about; /* the failure class (docs: failure-catalogue) */
    /* the MK probes */
    int mk;            /* 1: it tests erratum "erratum" (its imitation beneath it, VKBRIDGE_ERRATA_IMITATE) */
    int erratum;       /* VKB_ERRATUM_* */
    int bg;            /* slow but light (waits, many small submissions): always in the background
                          part when the start has a budget */
    const char *cases; /* its sub-cases, bit 0 first */
} probe_def;

static const probe_def probes[] = {
    {"identity", p_identity, 0, 0, 0, "F18/F46"},
    {"memory_types", p_memory_types, 0, 0, 0, "F01"},
    {"dma_heaps", p_dma_heaps, 0, 0, 0, "F02"},
    {"dmabuf_export", p_dmabuf_export, VKB_EMU_DMABUF_EXPORT, 0, 0, "F03"},
    {"dmabuf_linear_image", p_dmabuf_linear, 0, 0, 0, "F05"},
    {"ahb_share", p_ahb_share, 0, 0, 0, "F03"},
    {"linear_layout", p_linear_layout, 0, 0, 0, "F06"},
    {"compressed_formats", p_formats, 0, 0, 0, "F08"},
    {"bc_native", p_bc_native, VKB_EMU_BCN, 0, 0, "F08"},
    {"compression", p_compression, 0, 0, 0, "F11"},
    {"descriptor_pool", p_descriptor_pool, 0, 0, 0, "F12"},
    {"bindless_limits", p_bindless, 0, 0, 0, "F13"},
    {"null_descriptor", p_null_descriptor, VKB_EMU_ROBUSTNESS2, 0, 0, "F14"},
    {"push_descriptor", p_push_descriptor, VKB_EMU_PUSH_DESC, 0, 0, "F15"},
    {"multi_draw_indirect", p_multi_draw, VKB_EMU_MULTI_DRAW, 0, 0, "F16"},
    {"divisor", p_divisor, VKB_EMU_DIVISOR, 0, 0, "F17"},
    {"depth_clip", p_depth_clip, VKB_EMU_DEPTH_CLIP, 0, 0, "F19"},
    {"clip_distance", p_clip_distance, VKB_EMU_CLIP_DISTANCE, 0, 0, "F21"},
    {"spec_select", p_spec_select, 0, VKB_PROBE_FLAG_SPIRV_FIX, 0, "F23"},
    {"waits", p_waits, 0, VKB_PROBE_FLAG_WAIT_LOST, 0, "F30"},
    {"layer_vs", p_layer_vs, 0, 0, 0, "F22"},
    {"layer_gs", p_layer_gs, 0, VKB_PROBE_FLAG_GS_FOLD, 0, "F22"},
    {"pipeline_cache", p_pipeline_cache, 0, 0, 0, "F37"},
    {"viewport_flip", p_viewport_flip, 0, VKB_PROBE_FLAG_MK_FLIP, 0, "MK-02", 1, VKB_ERRATUM_VIEWPORT_FLIP, 0, "rows,facing,two_viewports"},
    {"bitwise_vec", p_bitwise_vec, 0, VKB_PROBE_FLAG_MK_BITWISE, 0, "MK-03", 1, VKB_ERRATUM_BITWISE_SCALAR, 0, "or,and,xor,not,sra,shl"},
    {"dyn_state", p_dyn_state, 0, VKB_PROBE_FLAG_MK_DYN, 0, "MK-04", 1, VKB_ERRATUM_DYN_REEMIT, 0,
     "depth_write,cull,stencil_wmask,sample_mask,static_sample_mask"},
    {"vb_strides", p_vb_strides, 0, VKB_PROBE_FLAG_MK_VB | VKB_PROBE_FLAG_MK_VB_SINGLE, 0, "MK-05", 1, VKB_ERRATUM_VB_SPLIT, 0,
     "from_0,from_1,single"},
    {"view3d_layers", p_view3d_layers, 0, VKB_PROBE_FLAG_MK_VIEW3D | VKB_PROBE_FLAG_PV_VIEW3D | VKB_PROBE_FLAG_PV_V3D_ALL | VKB_PROBE_FLAG_PV_FBLOOP, 0, "MK-07", 1,
     VKB_ERRATUM_VIEW_COUNTS, 0, "slices,explicit,one_slice_array,one_slice_2d,copy,fb_loop,fb_copy,fb_control,fb_split"},
    {"pv_mutable_linear", p_pv_mutable_linear, 0, VKB_PROBE_FLAG_PV_MUTABLE, 0, "PV-02", 1, VKB_ERRATUM_PV_MUTABLE_LINEAR, 0, "rgba8,bgra8"},
    {"pv_bgra_store", p_pv_bgra_store, 0, VKB_PROBE_FLAG_PV_BGRA_STORE, 0, "PV-07", 1, VKB_ERRATUM_PV_BGRA_STORE, 0, "bgra8_optimal,bgra8_linear,rgba8"},
    {"pv_names", p_pv_names, 0, VKB_PROBE_FLAG_PV_NAMES, 0, "PV-01", 1, VKB_ERRATUM_PV_NAMES, 0, "shared,stripped"},
    {"fp16_math", p_fp16_math, 0, VKB_PROBE_FLAG_MK_FP16, 0, "MK-20", 1, VKB_ERRATUM_FP16_HIDE, 0, "fma,range,convert,relaxed,chain"},
    {"wait_eintr", p_wait_eintr, 0, VKB_PROBE_FLAG_MK_WAIT, 0, "MK-01", 1, VKB_ERRATUM_WAIT_RETRY, 1, "fence,semaphore"},
    {"rendering_churn", p_rendering_churn, 0, VKB_PROBE_FLAG_MK_CHURN, 0, "MK-06", 1, VKB_ERRATUM_VIEW_QUARANTINE, 1, "views"},
    {"compile_lut_standin", p_compile_lut_standin, 0, 0, 1, "F26"},
    {"compile_aces", p_compile_aces, 0, VKB_PROBE_FLAG_LUT_NOW, 1, "F26"},
    {"bc_transcoders", p_bc_transcoders, 0, VKB_PROBE_FLAG_TX_ALL, 1, "F09"},
    {"bc_variants", p_bc_variants, 0, VKB_PROBE_FLAG_BC_ALL, 1, "F09"},
};
#define NPROBES (sizeof(probes) / sizeof(probes[0]))

static const struct {
    uint32_t bit;
    const char *name, *env;
} emu_names[] = {
    {VKB_EMU_BCN, "BC textures", "VKBRIDGE_EMU_BCN"},
    {VKB_EMU_DIVISOR, "vertex attribute divisor", "VKBRIDGE_EMU_DIVISOR"},
    {VKB_EMU_CLIP_DISTANCE, "clip distance", "VKBRIDGE_EMU_CLIP"},
    {VKB_EMU_DEPTH_CLIP, "depth clip enable", "VKBRIDGE_EMU_DEPTHCLIP"},
    {VKB_EMU_ROBUSTNESS2, "robustness2 null descriptors", "VKBRIDGE_EMU_ROBUSTNESS2"},
    {VKB_EMU_DMABUF_EXPORT, "dma-buf export", "VKBRIDGE_EMU_DMABUF_EXPORT"},
    {VKB_EMU_PUSH_DESC, "push descriptors", "VKBRIDGE_EMU_PUSH"},
    {VKB_EMU_MULTI_DRAW, "multiDrawIndirect", "VKBRIDGE_EMU_MULTIDRAW"},
};

static const char *emu_name(uint32_t bit, const char **env)
{
    for (size_t i = 0; i < sizeof(emu_names) / sizeof(emu_names[0]); i++)
        if (emu_names[i].bit == bit) {
            if (env) *env = emu_names[i].env;
            return emu_names[i].name;
        }
    return "?";
}

static const char *flag_text(uint64_t f)
{
    if (f & VKB_PROBE_FLAG_MK_FLIP) return "the bug's pattern (MK-02): negative viewport height ignored";
    if (f & VKB_PROBE_FLAG_MK_BITWISE) return "the bug's pattern (MK-03): vector bitwise ops miscompiled";
    if (f & VKB_PROBE_FLAG_MK_DYN) return "the bug's pattern (MK-04): dynamic state lost across a bind";
    if (f & VKB_PROBE_FLAG_MK_VB_SINGLE) return "the bug's pattern (MK-05), single-binding calls too: the split cannot fix it";
    if (f & VKB_PROBE_FLAG_MK_VB) return "the bug's pattern (MK-05): strides by array index";
    if (f & VKB_PROBE_FLAG_PV_VIEW3D)
        return f & VKB_PROBE_FLAG_PV_V3D_ARRAY1 ? "the bug's pattern (PV-04): a 3D image's 2D-array view renders from slice 0 with any base layer, explicit counts too; "
                                                  "one-slice views need nothing, multi-slice views are copied through a 2D-array image of the bridge's own"
               : f & VKB_PROBE_FLAG_PV_V3D_2D1 ? "the bug's pattern (PV-04): a 3D image's 2D-array view renders from slice 0 with any base layer, explicit counts too; "
                                                  "one-slice views are rendered through a 2D view of the slice, multi-slice views are copied through a 2D-array image of the bridge's own"
               : f & VKB_PROBE_FLAG_PV_V3D_COPY ? "the bug's pattern (PV-04): a 3D image's 2D-array view renders from slice 0 with any base layer, explicit counts too; "
                                                  "such views are copied through a 2D-array image of the bridge's own"
                                                : "the bug's pattern (PV-04): a 3D image's 2D-array view renders from slice 0 with any base layer, explicit counts too; "
                                                  "no form of the fix works here";
    if (f & VKB_PROBE_FLAG_PV_V3D_FB)
        return f & VKB_PROBE_FLAG_PV_V3D_FBCOPY ? "the bug's pattern (PV-04): a feedback loop in a 3D image's 2D-array view reads wrong, in a plain 2D-array image it is right; "
                                                  "such passes are copied through a 2D-array image of the bridge's own and split at their barriers"
                                                : "the bug's pattern (PV-04): a feedback loop in a 3D image's 2D-array view reads wrong, in a plain 2D-array image it is right; "
                                                  "no form of the fix works here";
    if (f & VKB_PROBE_FLAG_PV_FBLOOP)
        return "the bug's pattern (PV-06): a feedback loop in a plain 2D-array image reads wrong; right with the pass ended at the barrier and begun again, "
               "so such passes are split at their barriers";
    if (f & VKB_PROBE_FLAG_MK_VIEW3D) return "the bug's pattern (MK-07): wrong slices";
    if (f & VKB_PROBE_FLAG_MK_WAIT) return "the bug's pattern (MK-01): waits fail after a signal";
    if (f & VKB_PROBE_FLAG_PV_BGRA_STORE) return "the bug's pattern (PV-07): B8G8R8A8 storage writes land in R8G8B8A8 order; such images made R8G8B8A8";
    if (f & VKB_PROBE_FLAG_PV_MUTABLE) return "the bug's pattern (PV-02): a mutable-format LINEAR dma-buf image query refused though the image works; answered with the base format's";
    if (f & VKB_PROBE_FLAG_PV_NAMES) return "the bug's pattern (PV-01): an input reads zero beside an output of its name; shader names stripped";
    if (f & VKB_PROBE_FLAG_MK_FP16) return "the bug's pattern (MK-20): 16-bit float arithmetic wrong; shaderFloat16 hidden from clients";
    if (f & VKB_PROBE_FLAG_MK_CHURN) return "the bug's pattern (MK-06): a re-made view rendered stale";
    if (f & VKB_PROBE_FLAG_LUT_NOW) return "UE4 LUT pipelines get their stand-in at once";
    if (f & VKB_PROBE_FLAG_GS_FOLD) return "layer-only geometry stages folded into the vertex shader";
    if (f & VKB_PROBE_FLAG_WAIT_LOST) return "timed-out semaphore waits the driver answers with VK_ERROR_DEVICE_LOST are checked, and given as VK_TIMEOUT when the device is fine";
    if (f & VKB_PROBE_FLAG_SPIRV_FIX) return "SPIR-V spec-constant fix on";
    if (f & VKB_PROBE_FLAG_TX_ALL) return "formats of the failed BC transcoder decoded";
    if (f & VKB_PROBE_FLAG_BC_ALL) return "the BC converters' old variants kept where a new one failed";
    return "?";
}

/* ------------------------------------------------------------------ imitation (host tests) */

static int fake_mode(const char *probe)
{
    const char *v = vkb_srv_env("VKBRIDGE_PROBE_FAKE");
    if (!v) return 0;
    size_t n = strlen(probe);
    for (const char *p = v; (p = strstr(p, probe)); p += n) {
        if ((p != v && p[-1] != ',') || p[n] != ':') continue;
        const char *m = p + n + 1;
        if (!strncmp(m, "fail", 4)) return 1;
        if (!strncmp(m, "crash", 5)) return 2;
        if (!strncmp(m, "hang", 4)) return 3;
        if (!strncmp(m, "die", 3)) return 4;
        if (!strncmp(m, "abort", 5)) return 5;
        if (!strncmp(m, "stall", 5)) return 6;
        if (!strncmp(m, "slow", 4)) return 7;
        if (!strncmp(m, "error", 5)) return 8;
    }
    return 0;
}

/* ------------------------------------------------------------------ the profile file */

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

/* "key": "value" on one line; 0 when absent. */
static int jget(const char *line, const char *key, char *out, size_t cap)
{
    char k[64];
    snprintf(k, sizeof(k), "\"%s\": ", key);
    const char *p = strstr(line, k);
    if (!p) return 0;
    p += strlen(k);
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < cap) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                    char hex[5] = {p[1], p[2], p[3], p[4], 0};
                    out[n++] = (char)strtol(hex, NULL, 16);
                    p += 5;
                    continue;
                }
            }
            out[n++] = *p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != '\n' && n + 1 < cap) out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

static int status_of(const char *s)
{
    for (int i = 0; i < PS_COUNT; i++)
        if (!strcmp(ps_name[i], s)) return i;
    return PS_ERROR;
}

static void write_profile(pprofile *pf)
{
    if (!pf->path[0]) return;
    char tmp[700];
    snprintf(tmp, sizeof(tmp), "%s.tmp", pf->path);
    FILE *f = fopen(tmp, "we");
    if (!f && errno == ENOENT && vkb_cache_dir_ensure()) f = fopen(tmp, "we"); /* the cache directory was removed */
    if (!f) {
        if (vkb_cache_write_failed("the probe profile", errno)) VKB_WARN("probe: cannot write %s: %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "{\n  \"malibridge_probe\": 1,\n  \"key\": ");
    json_str(f, pf->key);
    fprintf(f, ",\n  \"gpu\": ");
    json_str(f, pf->gpu);
    fprintf(f, ",\n  \"driver\": ");
    json_str(f, pf->driver);
    fprintf(f, ",\n  \"vendor_id\": %u,\n  \"device_id\": %u,\n  \"driver_version\": %u,\n", pf->vendor, pf->device, pf->driver_version);
    fprintf(f, "  \"bridge\": ");
    json_str(f, VKB_GIT);
    fprintf(f, ",\n  \"probe_version\": %d,\n  \"protocol\": \"%016llx\",\n", PROBE_VERSION, (unsigned long long)VKB_PROTOCOL_HASH);
    fprintf(f, "  \"complete\": %s,\n  \"running\": ", pf->complete ? "true" : "false");
    json_str(f, pf->running);
    fprintf(f, ",\n  \"suspect\": ");
    json_str(f, pf->suspect);
    fprintf(f, ",\n  \"suspect_n\": %d", pf->suspect_n);
    fprintf(f, ",\n  \"cost_ms\": %.1f,\n  \"emu_before\": \"0x%x\",\n  \"emu_after\": \"0x%x\",\n  \"results\": [\n", pf->cost_ms, pf->emu_before,
            pf->emu_after);
    for (int i = 0; i < pf->n; i++) {
        const presult *r = &pf->res[i];
        fprintf(f, "    {\"name\": ");
        json_str(f, r->name);
        fprintf(f, ", \"status\": \"%s\", \"ms\": %.1f, \"emu\": \"0x%x\", \"flags\": \"0x%" PRIx64 "\", \"want\": \"0x%" PRIx64 "\", \"cases\": \"0x%x\", \"failed\": \"0x%x\", \"detail\": ",
                ps_name[r->status], r->ms, r->emu, r->flags, r->want, r->cases, r->failed);
        json_str(f, r->detail);
        fprintf(f, "}%s\n", i + 1 < pf->n ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    /* A short write (a full storage) must not replace the profile it was meant to update. */
    int ok = fflush(f) == 0 && !ferror(f);
    if (ok) fsync(fileno(f));
    int e = errno;
    if (fclose(f) != 0 && ok) {
        ok = 0;
        e = errno;
    }
    if (!ok) {
        unlink(tmp);
        if (vkb_cache_write_failed("the probe profile", e)) VKB_WARN("probe: cannot write %s: %s", tmp, strerror(e));
        return;
    }
    if (rename(tmp, pf->path) != 0) VKB_WARN("probe: cannot rename %s: %s", tmp, strerror(errno));
    else vkb_cache_write_ok();
}

/* ------------------------------------------------------------------ how a process ended in a probe */

#define BEAT_MARK_AT 64 /* the end mark's place in the .beat file, after the beat line */
#define BEAT_BOOT_AT 96 /* "boot <boot_id>\n": the boot the probe started in (a reboot is not a kill, judge_end) */
static int crash_marks_all; /* every fatal signal but SIGKILL reaches vkb_probe_mark_end (the separate server) */
static pthread_once_t beat_once = PTHREAD_ONCE_INIT;

void vkb_probe_crash_marks(int all)
{
    crash_marks_all = all;
}

static double sub_limit_s(void);

/* A probe's own time limit in s (< 0: none - the transcoders arm each build themselves). */
static double probe_limit_s(const probe_def *p)
{
    if (p->fn == p_bc_transcoders || p->fn == p_bc_variants) return -1;
    if (p->fn == p_wait_eintr) return sub_limit_s() + 2.0 * EINTR_WAIT_MS / 1000.0; /* it waits by design */
    return p->heavy ? 2 * sub_limit_s() : sub_limit_s();
}

/* How long this thread has waited for a CPU while runnable, ms (/proc/thread-self/schedstat, second field); -1: not known. */
static double rq_wait_ms(void)
{
    char b[96];
    int fd = open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    unsigned long long run, wait;
    return sscanf(b, "%llu %llu", &run, &wait) == 2 ? wait / 1e6 : -1;
}

/* With plock held. */
static void beat_write(pprofile *pf)
{
    char line[BEAT_MARK_AT];
    int n = snprintf(line, sizeof(line), "beat %-31s %9.0f %9.0f %d\n", pf->running, pf->beat_active, pf->beat_gap, crash_marks_all);
    if (n > 0 && n < (int)sizeof(line) && pwrite(pf->beat_fd, line, (size_t)n, 0) < 0) {}
}

/* With plock held: time since the last beat counts as run, unless it is so long the process (all
 * its threads) must have been stopped meanwhile. */
static void beat_tick(pprofile *pf, double now)
{
    double d = now - pf->beat_last;
    pf->beat_last = now;
    if (d > 2000) pf->beat_gap += d;
    else pf->beat_active += d;
}

static void *beat_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = {0, 200000000};
        nanosleep(&ts, NULL);
        double now = mono_ms();
        pthread_mutex_lock(&plock);
        for (int i = 0; i < nprofiles; i++)
            if (profiles[i].beat_fd >= 0) {
                beat_tick(&profiles[i], now);
                beat_write(&profiles[i]);
            }
        pthread_mutex_unlock(&plock);
    }
    return NULL;
}

static void beat_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, beat_main, NULL) != 0) VKB_WARN("probe: no heartbeat thread; a start that ends inside a probe is judged without it");
    pthread_attr_destroy(&a);
}

/* Every client process has left (1) or one is connected (0). A session's end looks so: the guest
 * goes, its processes' connections close, and the app kills the server (SIGKILL, no mark) a few ms
 * later. On the Retroid Pocket 4 Pro every gamescope start ended within 800 ms, the
 * kill came inside the background "bc_transcoders" probe three times, and it was recorded as died:
 * BC transcoding off and its MK-20 line wrong. A probe ended so is run again instead. Only after a
 * client has been: a process that dies before any client connects is judged as before. */

static void idle_write(int fd, int idle)
{
    if (fd >= 0 && pwrite(fd, idle ? "idle\n" : "busy\n", 5, BEAT_MARK_AT) < 0) {}
}

void vkb_probe_clients_gone(int gone)
{
    pthread_mutex_lock(&plock);
    if (__atomic_exchange_n(&clients_gone, gone, __ATOMIC_ACQ_REL) != gone)
        for (int i = 0; i < nprofiles; i++) idle_write(profiles[i].beat_fd, gone);
    pthread_mutex_unlock(&plock);
}

/* This boot's id (/proc/sys/kernel/random/boot_id; VKBRIDGE_BOOT_ID imitates another boot, host
 * tests); "" where it cannot be read, which makes a reboot unknowable and the judging as before. */
static char boot_id_buf[48];
static pthread_once_t boot_once = PTHREAD_ONCE_INIT;

static void boot_id_read(void)
{
    const char *e = vkb_srv_env_global("VKBRIDGE_BOOT_ID");
    if (e && *e) snprintf(boot_id_buf, sizeof(boot_id_buf), "%.40s", e);
    else {
        FILE *f = fopen("/proc/sys/kernel/random/boot_id", "re");
        if (f) {
            if (!fgets(boot_id_buf, sizeof(boot_id_buf), f)) boot_id_buf[0] = 0;
            fclose(f);
        }
    }
    for (char *q = boot_id_buf; *q; q++)
        if (*q == '\n' || *q == ' ') *q = 0;
}

static const char *boot_id(void)
{
    pthread_once(&boot_once, boot_id_read);
    return boot_id_buf;
}

/* After the profile names pf->running: its heartbeat starts. */
static void probe_begin(pprofile *pf)
{
    if (!pf->path[0]) return;
    pthread_once(&beat_once, beat_start);
    char path[700];
    snprintf(path, sizeof(path), "%s.beat", pf->path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 && errno == ENOENT && vkb_cache_dir_ensure()) fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) vkb_cache_write_failed("a probe heartbeat", errno); /* without it the probe is judged as before (judge_end) */
    const char *boot = boot_id();
    pthread_mutex_lock(&plock);
    pf->beat_last = mono_ms();
    pf->beat_active = pf->beat_gap = 0;
    pf->beat_tid = (int)gettid();
    if (fd >= 0) {
        __atomic_store_n(&pf->beat_fd, fd, __ATOMIC_RELEASE);
        beat_write(pf);
        if (*boot) {
            char bl[56];
            int bn = snprintf(bl, sizeof(bl), "boot %.40s\n", boot);
            if (bn > 0 && pwrite(fd, bl, (size_t)bn, BEAT_BOOT_AT) < 0) {}
        }
        if (__atomic_load_n(&clients_gone, __ATOMIC_ACQUIRE)) idle_write(fd, 1);
    }
    pthread_mutex_unlock(&plock);
}

/* The probe returned: "done" (a process that ends before the profile has its result runs it
 * again). Returns the time the process was stopped during it (ms). */
static double probe_end(pprofile *pf)
{
    pthread_mutex_lock(&plock);
    beat_tick(pf, mono_ms());
    double gap = pf->beat_gap;
    int fd = pf->beat_fd;
    __atomic_store_n(&pf->beat_fd, -1, __ATOMIC_RELEASE);
    if (fd >= 0) {
        beat_write(pf);
        if (pwrite(fd, "done\n", 5, BEAT_MARK_AT) < 0) {}
        close(fd);
    }
    pthread_mutex_unlock(&plock);
    return gap;
}

/* A crash counts against the probe only on the probe's own thread: one elsewhere (a game's
 * compile in the same process) only interrupts it. */
void vkb_probe_mark_end(int sig, int crashed)
{
    int tid = crashed ? (int)gettid() : 0;
    int np = __atomic_load_n(&nprofiles, __ATOMIC_ACQUIRE);
    for (int i = 0; i < np && i < VKB_MAX_PDS; i++) {
        int fd = __atomic_load_n(&profiles[i].beat_fd, __ATOMIC_ACQUIRE);
        if (fd < 0) continue;
        char m[24];
        const char *w = !crashed ? "stop " : tid == profiles[i].beat_tid ? "crash " : "away ";
        size_t n = 0;
        while (*w) m[n++] = *w++;
        char d[8];
        int k = 0, v = sig < 0 ? 0 : sig;
        do d[k++] = (char)('0' + v % 10);
        while ((v /= 10) && k < 7);
        while (k) m[n++] = d[--k];
        m[n++] = '\n';
        if (pwrite(fd, m, n, BEAT_MARK_AT) < 0) {}
    }
}

enum { END_INTERRUPTED, END_UNEXPLAINED, END_DIED };

/* How an earlier start ended inside the probe the profile names as running; the .beat file is
 * consumed. *detail: why. */
static int judge_end(pprofile *pf, const char *name, char *detail, size_t cap)
{
    char path[700], buf[200] = "";
    snprintf(path, sizeof(path), "%s.beat", pf->path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t got = fd >= 0 ? pread(fd, buf, sizeof(buf) - 1, 0) : -1;
    if (fd >= 0) close(fd);
    unlink(path);
    buf[got > 0 ? got : 0] = 0;
    char bname[40] = "";
    double ms = -1, gap = 0;
    int all = crash_marks_all, sig = 0;
    int have = got > 0 && sscanf(buf, "beat %39s %lf %lf %d", bname, &ms, &gap, &all) >= 2 && !strcmp(bname, name);
    const char *mark = got > BEAT_MARK_AT ? buf + BEAT_MARK_AT : "";
    const probe_def *p = NULL;
    for (size_t i = 0; i < NPROBES; i++)
        if (!strcmp(probes[i].name, name)) p = &probes[i];
    double limit = p ? probe_limit_s(p) : -1;
    int need = all ? 3 : 2;
    if (got > 0 && !have) {
        /* the beat names the probe before: this one had not started */
        snprintf(detail, cap, "before it started");
        return END_INTERRUPTED;
    }
    if (have && !strncmp(mark, "done", 4)) {
        snprintf(detail, cap, "after it returned (%.0f ms), before its result was saved", ms);
        return END_INTERRUPTED;
    }
    if (have && !strncmp(mark, "stop", 4)) {
        sig = atoi(mark + 5);
        snprintf(detail, cap, "stopped after %.0f ms (%s)", ms, sig ? strsignal(sig) : "the app was gone");
        return END_INTERRUPTED;
    }
    if (have && !strncmp(mark, "away", 4)) {
        sig = atoi(mark + 5);
        snprintf(detail, cap, "the process crashed elsewhere (signal %d) after %.0f ms", sig, ms);
        return END_INTERRUPTED;
    }
    if (have && !strncmp(mark, "crash", 5)) {
        sig = atoi(mark + 6);
        snprintf(detail, cap, "the process ended with signal %d (%s) inside this probe, outside its guard, after %.0f ms on an earlier start", sig,
                 strsignal(sig), ms);
        return END_DIED;
    }
    if (have && limit > 0 && ms >= limit * 1000 + 2000) {
        snprintf(detail, cap, "the process was still inside this probe %.1f s after it began (its limit: %.0f s; the watchdog could not interrupt it) when it was ended on an earlier start",
                 ms / 1000, limit);
        return END_DIED;
    }
    if (have && !strncmp(mark, "idle", 4)) {
        snprintf(detail, cap, "ended after %.0f ms, after every client process had left (the session's end)", ms);
        return END_INTERRUPTED;
    }
    /* Ended without a word and the device rebooted since: the reboot is what ended it (a GPU
     * reset or hang that took the phone down, a power loss), and a probe that reboots the device is
     * a failed probe at once, not one to try again: the next start would reboot it the same way, in
     * a loop. Marks above are read first, so a session that ended on its own before a clean
     * reboot is not blamed. */
    char was[48] = "";
    if (have && got > BEAT_BOOT_AT + 5 && !strncmp(buf + BEAT_BOOT_AT, "boot ", 5)) sscanf(buf + BEAT_BOOT_AT + 5, "%47s", was);
    const char *now = boot_id();
    if (have && *was && *now && strcmp(was, now)) {
        snprintf(detail, cap, "the device rebooted while it ran (boot %.8s, now %.8s), after %.0f ms on an earlier start", was, now, ms);
        return END_DIED;
    }
    int n = !strcmp(pf->suspect, name) ? pf->suspect_n + 1 : 1;
    snprintf(pf->suspect, sizeof(pf->suspect), "%s", name);
    pf->suspect_n = n;
    if (n >= need) {
        snprintf(detail, cap, "the process ended inside this probe without a word (a driver abort or a kill) on %d starts in a row, the last after %s", n,
                 have ? "" : "an unknown time");
        if (have) snprintf(detail + strlen(detail), cap - strlen(detail), "%.0f ms", ms);
        return END_DIED;
    }
    if (have) snprintf(detail, cap, "ended after %.0f ms without a word (a kill from outside, or an abort): %d of %d in a row before it counts as died", ms, n, need);
    else snprintf(detail, cap, "ended without a word and without a heartbeat (a kill, or an abort): %d of %d in a row before it counts as died", n, need);
    return END_UNEXPLAINED;
}

/* The cached profile, if it is this driver's and this bridge's; results of a run that ended in a
 * probe are kept, and that probe is judged (judge_end). Returns 1 when complete. */
static int read_profile(pprofile *pf)
{
    FILE *f = fopen(pf->path, "re");
    if (!f) {
        /* None yet, or it went (the cache directory emptied under a full storage): probed again
         * from the start. A heartbeat left without its profile belongs to no run any more. */
        char beat[700];
        snprintf(beat, sizeof(beat), "%s.beat", pf->path);
        if (unlink(beat) == 0) VKB_INFO("probe: %s had no profile beside it; removed, probing again", beat);
        return 0;
    }
    char line[1600], v[DETAIL];
    int key_ok = 0, complete = 0;
    char running[32] = "";
    pf->n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (jget(line, "key", v, sizeof(v))) key_ok = !strcmp(v, pf->key);
        else if (jget(line, "complete", v, sizeof(v))) complete = !strcmp(v, "true");
        else if (!strstr(line, "\"name\"") && jget(line, "running", v, sizeof(v))) snprintf(running, sizeof(running), "%.31s", v);
        else if (!strstr(line, "\"name\"") && jget(line, "suspect", v, sizeof(v))) snprintf(pf->suspect, sizeof(pf->suspect), "%.31s", v);
        else if (!strstr(line, "\"name\"") && jget(line, "suspect_n", v, sizeof(v))) pf->suspect_n = atoi(v);
        else if (key_ok && pf->n < MAX_RESULTS && jget(line, "name", v, sizeof(v))) {
            presult *r = &pf->res[pf->n++];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%.31s", v);
            if (jget(line, "status", v, sizeof(v))) r->status = status_of(v);
            if (jget(line, "ms", v, sizeof(v))) r->ms = atof(v);
            if (jget(line, "emu", v, sizeof(v))) r->emu = (uint32_t)strtoul(v, NULL, 16);
            if (jget(line, "flags", v, sizeof(v))) r->flags = (uint64_t)strtoull(v, NULL, 16);
            if (jget(line, "want", v, sizeof(v))) r->want = (uint64_t)strtoull(v, NULL, 16);
            if (jget(line, "cases", v, sizeof(v))) r->cases = (uint32_t)strtoul(v, NULL, 16);
            if (jget(line, "failed", v, sizeof(v))) r->failed = (uint32_t)strtoul(v, NULL, 16);
            jget(line, "detail", r->detail, sizeof(r->detail));
        }
    }
    fclose(f);
    if (!key_ok) {
        pf->n = 0;
        pf->suspect[0] = 0;
        pf->suspect_n = 0;
        VKB_INFO("probe: %s is for another driver or bridge build; probing again", pf->path);
        return 0;
    }
    if (running[0] && pf->n < MAX_RESULTS) {
        char why[DETAIL];
        int end = judge_end(pf, running, why, sizeof(why));
        if (end == END_DIED) {
            presult *r = &pf->res[pf->n++];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%s", running);
            r->status = PS_DIED;
            snprintf(r->detail, sizeof(r->detail), "%s", why);
            pf->suspect[0] = 0;
            pf->suspect_n = 0;
            VKB_ERR("probe: an earlier start died inside the \"%s\" probe (%s); recorded as died, not run again", running, why);
        } else {
            VKB_WARN("probe: an earlier start %s inside the \"%s\" probe (%s); running it again, nothing decided from it",
                     end == END_INTERRUPTED ? "was stopped" : "ended", running, why);
        }
        complete = 0;
    }
    return complete;
}

static presult *find_result(pprofile *pf, const char *name)
{
    for (int i = 0; i < pf->n; i++)
        if (!strcmp(pf->res[i].name, name)) return &pf->res[i];
    return NULL;
}

/* ------------------------------------------------------------------ running */

static double sub_limit_s(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_PROBE_TIMEOUT_S");
    double t = v && *v ? atof(v) : 4.0;
    return t > 0 ? t : 4.0;
}

typedef struct run_arg {
    pctx *c;
    const probe_def *p;
    int fake;
} run_arg;

static void run_one(void *arg)
{
    run_arg *a = arg;
    if (a->fake == 2) {
        VKB_WARN("probe: %s faults on purpose (VKBRIDGE_PROBE_FAKE)", a->p->name);
        *(volatile int *)(uintptr_t)0x40 = 1;
    }
    if (a->fake == 3) {
        VKB_WARN("probe: %s spins on purpose (VKBRIDGE_PROBE_FAKE)", a->p->name);
        for (volatile unsigned long k = 0;; k++) {
        }
    }
    if (a->fake == 4) {
        VKB_WARN("probe: %s ends the process on purpose (VKBRIDGE_PROBE_FAKE)", a->p->name);
        _exit(3);
    }
    if (a->fake == 5) {
        VKB_WARN("probe: %s aborts on purpose (VKBRIDGE_PROBE_FAKE)", a->p->name);
        abort();
    }
    if (a->fake == 6) {
        /* a hang the watchdog cannot interrupt: only the app's kill ends it */
        VKB_WARN("probe: %s spins past its watchdog on purpose (VKBRIDGE_PROBE_FAKE)", a->p->name);
        sigset_t all;
        sigfillset(&all);
        sigdelset(&all, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &all, NULL);
        for (volatile unsigned long k = 0;; k++) {
        }
    }
    if (a->fake == 7) {
        VKB_WARN("probe: %s takes 3 s longer on purpose (VKBRIDGE_PROBE_FAKE)", a->p->name);
        struct timespec ts = {3, 0};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        }
    }
    a->p->fn(a->c);
}

/* Is the probe applicable? (the cheap part of each: what it skips on) - run as the probe with the
 * fake replaced by nothing; the imitation acts only on probes that would run. */
static int applies(pctx *c, const probe_def *p)
{
    if (p->fn == p_bc_native) return c->bc;
    if (p->fn == p_null_descriptor) return c->null_desc;
    if (p->fn == p_push_descriptor) return c->push;
    if (p->fn == p_multi_draw) return c->mdi;
    if (p->fn == p_divisor) return c->divisor;
    if (p->fn == p_depth_clip) return c->depth_clip && c->feat.depthClamp;
    if (p->fn == p_clip_distance) return c->clip;
    if (p->fn == p_layer_vs) return c->layer_vs;
    if (p->fn == p_layer_gs) return c->gs;
    if (p->fn == p_bc_transcoders) return (c->emu & VKB_EMU_BCN) && vkb_emu_bcn_transcode_wanted();
    if (p->fn == p_bc_variants) return (c->emu & VKB_EMU_BCN) != 0;
    if (p->fn == p_dma_heaps) return 1;
    if (p->fn == p_dmabuf_export) return c->dmabuf;
    if (p->fn == p_dmabuf_linear) return c->dmabuf;
    if (p->fn == p_compression) return c->compression;
    if (p->fn == p_bindless) return c->bindless;
    if (p->fn == p_dyn_state || p->fn == p_vb_strides) return c->eds;
    if (p->fn == p_rendering_churn) return c->dynrender;
    if (p->fn == p_view3d_layers) return c->api >= VK_API_VERSION_1_1;
    if (p->fn == p_fp16_math) return c->fp16;
    return 1;
}

/* One probe into r; 0 when the device is gone (crash/hang: abandoned). */
static int probe_one(pctx *c, const probe_def *p, presult *r, int *dev_lost)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", p->name);
    c->st = PS_PASS;
    c->dlen = 0;
    c->detail[0] = 0;
    c->want_flags = 0;
    c->cases = c->failed = 0;
    c->gpu_stuck = 0;
    *dev_lost = 0;
    if (!c->dev && !dev_create(c)) {
        r->status = PS_ERROR;
        snprintf(r->detail, sizeof(r->detail), "no probe device");
        return 0;
    }
    int app = applies(c, p);
    run_arg a = {c, p, app ? fake_mode(p->name) : 0};
    /* an MK probe sees its erratum's imitation (host builds, VKBRIDGE_ERRATA_IMITATE) */
    vkb_dispatch saved = c->d;
    void *imi = p->mk && app ? vkb_errata_probe_begin(p->erratum, &c->d, c->dev, c->pd, c->api) : NULL;
    /* view3d_layers also sees pv_view3d_base's imitation (one at a time: view_counts' wins when both are named) */
    if (!imi && app && p->fn == p_view3d_layers) imi = vkb_errata_probe_begin(VKB_ERRATUM_PV_VIEW3D_BASE, &c->d, c->dev, c->pd, c->api);
    /* ... and pv_fbloop_split's (plain feedback loops read stale inside the pass) */
    if (!imi && app && p->fn == p_view3d_layers) imi = vkb_errata_probe_begin(VKB_ERRATUM_PV_FBLOOP_SPLIT, &c->d, c->dev, c->pd, c->api);
    /* waits sees VKBRIDGE_WAIT_FAKE_LOST's imitation (a timed-out wait answered VK_ERROR_DEVICE_LOST) */
    if (!imi && app && p->fn == p_waits && vkb_wait_fake_lost())
        imi = vkb_errata_probe_begin(VKB_ERRATUM_WAIT_RETRY, &c->d, c->dev, c->pd, c->api);
    /* the transcoders arm each build themselves (a hang there must not end the whole probe) */
    double limit = probe_limit_s(p);
    int sig = 0;
    double t0 = mono_ms();
    int g = VKB_GUARD_DONE;
    if (a.fake == 8) {
        c->st = PS_ERROR;
        say(c, "could not set up [imitated: VKBRIDGE_PROBE_FAKE]");
    } else {
        g = vkb_guard_run(p->name, limit, run_one, &a, &sig);
    }
    r->ms = mono_ms() - t0;
    if (imi) {
        c->d = saved;
        vkb_errata_probe_end(imi);
        say(c, " [the bug imitated beneath: VKBRIDGE_ERRATA_IMITATE]");
    }
    if (g == VKB_GUARD_DONE && c->gpu_stuck) {
        /* a GPU hang (or loss) inside the probe's own work */
        r->status = PS_HANG;
        say(c, "; the probe device is abandoned");
        dev_destroy(c, 1);
        *dev_lost = 1;
    } else if (g == VKB_GUARD_DONE) {
        r->status = c->st;
        if (a.fake == 1 && r->status != PS_SKIP) {
            r->status = PS_FAIL;
            /* an MK probe failed as if it had seen its bug (VKB_PROBE_FLAG_MK_VB_SINGLE: no) */
            if (p->mk) c->want_flags |= p->flags & ~(VKB_PROBE_FLAG_MK_VB_SINGLE | VKB_PROBE_FLAG_PV_V3D_ALL | VKB_PROBE_FLAG_PV_VIEW3D | VKB_PROBE_FLAG_PV_FBLOOP);
            if (p->fn == p_bc_variants) c->want_flags |= VKB_PROBE_FLAG_BC_ALL;
            say(c, " [failure imitated: VKBRIDGE_PROBE_FAKE]");
        }
        release(c);
    } else {
        r->status = g == VKB_GUARD_TIMED_OUT ? PS_HANG : PS_CRASH;
        say(c, "%s%s", c->dlen ? "; " : "", g == VKB_GUARD_TIMED_OUT ? "interrupted at its time limit" : "the driver faulted");
        if (g == VKB_GUARD_CRASHED) say(c, " (signal %d)", sig);
        say(c, "; the probe device is abandoned");
        dev_destroy(c, 1);
        *dev_lost = 1;
    }
    snprintf(r->detail, sizeof(r->detail), "%s", c->detail);
    r->want = c->want_flags; /* raw; the flags in force are decided from it (decide) */
    r->cases = c->cases;
    r->failed = c->failed;
    return 1;
}

/* The emulations the bridge can stand in with on this GPU (a probe that does not pass switches its
 * own on only where it can) */
static uint32_t emu_possible(const pctx *c)
{
    uint32_t m = VKB_EMU_IMPLEMENTED;
    if (!c->feat.depthClamp) m &= ~VKB_EMU_DEPTH_CLIP;
    if (!vkb_emu_bcn_supported(c->idt, c->pd)) m &= ~VKB_EMU_BCN;
    if (!vkb_mem_export_backing()) m &= ~VKB_EMU_DMABUF_EXPORT;
    if (vkb_dab_on()) m &= ~VKB_EMU_DMABUF_EXPORT; /* dma-bufs over AHardwareBuffers instead */
    return m;
}

/* Safe mode keeps a probe-gated emulation on until its probe has verified the native path: where
 * the GPU has that path (its feature not missing; dma-buf export, whose native side only its probe
 * tells, is left to the probe) and the probe has not run yet or could not run (error). A skipped
 * probe found no native path to verify: what the bridge does without one stands (vkb_emu_detect). */
static int safe_gated(const probe_def *p, uint32_t missing, int mode)
{
    return mode == VKB_MODE_SAFE && p->emu && p->emu != VKB_EMU_DMABUF_EXPORT && !(missing & p->emu);
}

/* What a result switches on in a mode: emulation bits and runtime flags, from the raw result
 * (status, want) alone - re-run on cached results, so a mode switch never re-probes. Both modes:
 * a probe that failed, crashed, hung or died gets its emulation and its runtime flags. Safe mode
 * also: one that ended in error keeps its emulation (safe_gated; one not run yet: decide_unrun).
 * The MK probes' flags reach the errata decisions (errata_source), whose own per-mode rules are in
 * server_errata.c. */
static void decide(const probe_def *p, presult *r, uint32_t *emu, uint32_t possible, uint32_t missing, int mode, int log)
{
    int bad = r->status == PS_FAIL || r->status == PS_CRASH || r->status == PS_HANG || r->status == PS_DIED;
    uint64_t flags = r->want & p->flags;
    if (p->fn == p_compile_aces) flags = (r->status == PS_HANG || r->status == PS_DIED) ? VKB_PROBE_FLAG_LUT_NOW : 0;
    else if (!bad) flags = 0;
    if (p->fn == p_bc_transcoders && r->status == PS_DIED) flags = VKB_PROBE_FLAG_TX_ALL;
    if (p->fn == p_bc_variants && (r->status == PS_DIED || r->status == PS_HANG || r->status == PS_CRASH)) flags = VKB_PROBE_FLAG_BC_ALL;
    r->flags = flags;
    r->emu = 0;
    int unverified = !bad && r->status == PS_ERROR && safe_gated(p, missing, mode);
    if (!bad && !unverified) return;
    int apply = env_on("VKBRIDGE_PROBE_APPLY", 1) && env_on("VKBRIDGE_EMULATE", 1);
    if (p->emu && !(*emu & p->emu)) {
        const char *env = NULL;
        const char *what = emu_name(p->emu, &env);
        int ok = (possible & p->emu) && env_on(env, 1);
        if (ok && apply) {
            r->emu = p->emu;
            *emu |= p->emu;
        }
        if (log)
            VKB_WARN("probe: %s %s (%s): %s %s%s", p->name, ps_name[r->status], r->detail, what,
                     !ok ? "cannot be emulated here" : apply ? "EMULATED instead of the driver's" : "would be emulated (VKBRIDGE_PROBE_APPLY=0)",
                     unverified ? " (not verified: safe mode)" : "");
    }
    if (flags) {
        if (log)
            VKB_WARN("probe: %s %s (%s): %s%s", p->name, ps_name[r->status], r->detail, flag_text(flags), apply ? "" : " - not applied (VKBRIDGE_PROBE_APPLY=0)");
        if (!apply) r->flags = 0;
    }
    if (log && !p->emu && !flags) VKB_WARN("probe: %s %s: %s", p->name, ps_name[r->status], r->detail);
}

/* Safe mode: the probe-gated emulations whose probe has no result yet (left to the background
 * part, or a start that ended before it) stay on until it passes. Returns the bits added. */
static uint32_t decide_unrun(pprofile *pf, uint32_t *emu, int mode, int log)
{
    uint32_t added = 0;
    if (mode != VKB_MODE_SAFE) return 0;
    int apply = env_on("VKBRIDGE_PROBE_APPLY", 1) && env_on("VKBRIDGE_EMULATE", 1);
    for (size_t i = 0; i < NPROBES; i++) {
        const probe_def *p = &probes[i];
        if (!safe_gated(p, pf->missing_before, mode) || (*emu & p->emu) || find_result(pf, p->name)) continue;
        const char *env = NULL;
        const char *what = emu_name(p->emu, &env);
        int ok = (pf->possible & p->emu) && env_on(env, 1);
        if (ok && apply) {
            *emu |= p->emu;
            added |= p->emu;
        }
        if (log)
            VKB_INFO("probe: %s not run yet: %s %s", p->name, what,
                     !ok ? "cannot be emulated here" : apply ? "EMULATED until it passes (safe mode)" : "would be emulated (VKBRIDGE_PROBE_APPLY=0)");
    }
    return added;
}

/* The decisions of every result in pf in a mode, on copies (the raw results stay): the emulation
 * mask from what the GPU had before the probe and the runtime flags; out (pf->n entries, may be
 * NULL) gets each result as decided. */
static void decide_all(pprofile *pf, int mode, uint32_t *emu, uint64_t *flags, presult *out)
{
    uint32_t e = pf->emu_before;
    uint64_t f = 0;
    for (int k = 0; k < pf->n; k++) {
        presult r = pf->res[k];
        const probe_def *p = NULL;
        for (size_t j = 0; j < NPROBES && !p; j++)
            if (!strcmp(probes[j].name, r.name)) p = &probes[j];
        if (p) decide(p, &r, &e, pf->possible, pf->missing_before, mode, 0);
        f |= r.flags;
        if (out) out[k] = r;
    }
    decide_unrun(pf, &e, mode, 0);
    if (emu) *emu = e;
    if (flags) *flags = f;
}

static uint64_t apply_flags(pprofile *pf)
{
    uint64_t f = 0;
    int bcv = 0;
    for (int i = 0; i < pf->n; i++) {
        f |= pf->res[i].flags;
        if (!strcmp(pf->res[i].name, "bc_variants")) bcv = pf->res[i].status == PS_SKIP || pf->res[i].status == PS_ERROR ? -1 : 1;
    }
    __atomic_or_fetch(&pflags, f, __ATOMIC_ACQ_REL);
    /* after the flags: a reader that sees 1 sees what the probe turned off */
    if (bcv) __atomic_store_n(&bcv_state, bcv, __ATOMIC_RELEASE);
    return f;
}

static uint64_t fnv(uint64_t h, const char *s)
{
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return (h ^ 0xff) * 0x100000001b3ull;
}

/* The profile's identity: driver build, bridge build, probe list, imitation switches. */
static void profile_key(pprofile *pf, const VkPhysicalDeviceProperties *props, const uint8_t *uuid)
{
    static const char *const envs[] = {"VKBRIDGE_FAKE_MISSING", "VKBRIDGE_SERVER_HIDE_EXTS", "VKBRIDGE_PROBE_FAKE", "VKBRIDGE_EMULATE",
                                       "VKBRIDGE_PROBE_APPLY", "VKBRIDGE_BC_TRANSCODE", "VKBRIDGE_ERRATA_IMITATE",
                                       "VKBRIDGE_ERRATA_IMITATE_VB", "VKBRIDGE_WAIT_FAKE_ERROR",
                                       /* a transcoder hang imitated, or a shorter limit, records what a real run must not reuse */
                                       "VKBRIDGE_PROBE_TX_FAKE", "VKBRIDGE_PROBE_TX_TIMEOUT_S", "VKBRIDGE_PROBE_BCV_FAKE"};
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]); i++) h = fnv(h, vkb_srv_env(envs[i]));
    char u[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(u + 2 * i, 3, "%02x", uuid[i]);
    snprintf(pf->key, sizeof(pf->key), "%04x:%04x:%08x:%s:p%d:%s:%016llx:%016llx", props->vendorID, props->deviceID, props->driverVersion, u,
             PROBE_VERSION, VKB_GIT, (unsigned long long)VKB_PROTOCOL_HASH, (unsigned long long)h);
    const char *dir = vkb_pcache_dir();
    if (dir) snprintf(pf->path, sizeof(pf->path), "%s/probe-%04x-%04x-%08x-%.16s.json", dir, props->vendorID, props->deviceID, props->driverVersion, u);
}

typedef struct bg_job {
    int pf_index;
    uint8_t uuid[VK_UUID_SIZE];
    uint32_t emu;
} bg_job;

static bg_job *pending[VKB_MAX_PDS];

static double budget_s(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_PROBE_BUDGET_S");
    return v && *v ? atof(v) : 6.0;
}

/* Runs the probes pf has no result for, within budget_ms (<= 0: no limit). Returns how many were
 * left for later. */
static int run_probes(pctx *c, pprofile *pf, uint32_t *emu, double budget_ms, int log)
{
    double start = mono_ms();
    int left = 0, busy = 0;
    for (size_t i = 0; i < NPROBES; i++) {
        const probe_def *p = &probes[i];
        if (find_result(pf, p->name)) continue;
        if (busy) { /* a probe starved before: the device is busy, and an interrupted compile may hold the driver's lock */
            left++;
            continue;
        }
        double spent = mono_ms() - start;
        double need = (p->heavy ? 2 : 1) * sub_limit_s() * 1000.0;
        if (budget_ms > 0 && (p->bg || spent + (p->heavy ? need : 0) > budget_ms)) {
            left++;
            continue;
        }
        if (pf->n >= MAX_RESULTS) break;
        c->emu = *emu;
        c->layer_vs_passed = 0;
        presult *vs = find_result(pf, "layer_vs");
        c->layer_vs_passed = vs && vs->status == PS_PASS;
        presult *txr = find_result(pf, "bc_transcoders");
        c->tx_status = txr ? txr->status : -1;
        snprintf(pf->running, sizeof(pf->running), "%s", p->name);
        write_profile(pf);
        probe_begin(pf);
        presult r;
        int lost = 0;
        double w0 = rq_wait_ms();
        c->starved = 0;
        probe_one(c, p, &r, &lost);
        double gap = probe_end(pf), starved = w0 >= 0 ? rq_wait_ms() - w0 : 0;
        double lim = probe_limit_s(p) * 1000.0; /* < 0: the transcoders time each build themselves (guard_starved) */
        if (r.status == PS_HANG && gap <= 0 && (c->starved || (lim > 0 && starved >= 1000 && r.ms - starved < 0.75 * lim))) {
            /* runnable but not run (the background thread is at the lowest priority and the device was busy): without that
             * wait it would have ended inside its limit, so the limit did not measure the probe */
            VKB_WARN("probe: %s reached its time limit after %.1f s, %.1f s of them waiting for a CPU; not counted, run again at the next "
                     "start, with the probes after it", p->name, r.ms / 1000, starved / 1000);
            pthread_mutex_lock(&plock);
            pf->running[0] = 0;
            pthread_mutex_unlock(&plock);
            left++;
            busy = 1;
            continue;
        }
        if (r.status == PS_HANG && gap > 0) {
            /* the watchdog's clock ran on while the process was stopped (a suspended session) */
            VKB_WARN("probe: %s reached its time limit after %.1f s, %.1f s of them with the process stopped; not counted, run again at the next start",
                     p->name, r.ms / 1000, gap / 1000);
            pthread_mutex_lock(&plock);
            pf->running[0] = 0;
            pthread_mutex_unlock(&plock);
            left++;
            continue;
        }
        decide(p, &r, emu, c->possible, pf->missing_before, vkb_mode(), log);
        pthread_mutex_lock(&plock);
        pf->res[pf->n++] = r;
        pf->running[0] = 0;
        if (!strcmp(pf->suspect, r.name)) {
            pf->suspect[0] = 0;
            pf->suspect_n = 0;
        }
        pthread_mutex_unlock(&plock);
        if (log && r.status != PS_SKIP) VKB_INFO("probe: %-20s %-5s %6.1f ms  %s", r.name, ps_name[r.status], r.ms, r.detail);
        else VKB_DBG("probe: %s %s %s", r.name, ps_name[r.status], r.detail);
    }
    pf->cost_ms += mono_ms() - start;
    return left;
}

static void ctx_init(pctx *c, const vkb_dispatch *idt, VkInstance inst, VkPhysicalDevice pd, uint32_t emu)
{
    memset(c, 0, sizeof(*c));
    c->idt = idt;
    c->inst = inst;
    c->pd = pd;
    c->emu = emu;
    idt->vkGetPhysicalDeviceProperties(pd, &c->props);
    idt->vkGetPhysicalDeviceFeatures(pd, &c->feat);
    idt->vkGetPhysicalDeviceMemoryProperties(pd, &c->mp);
    uint32_t n = 0;
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    c->ext = calloc(n ? n : 1, sizeof(*c->ext));
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, c->ext);
    c->next = n;
    uint32_t api = VK_API_VERSION_1_0;
    if (vkb_global_dt.vkEnumerateInstanceVersion) vkb_global_dt.vkEnumerateInstanceVersion(&api);
    if (api > VK_API_VERSION_1_3) api = VK_API_VERSION_1_3;
    c->api = c->props.apiVersion < api ? c->props.apiVersion : api;
    c->pvr = vkb_driver_is_powervr(&c->props, vkb_driver_id(idt, pd, c->api));
    c->possible = emu_possible(c);
}

static void ctx_free(pctx *c)
{
    dev_destroy(c, 0);
    free(c->ext);
    c->ext = NULL;
}

/* The background probes' thread at the lowest CPU priority (SCHED_IDLE, and nice 19 where that is refused too): on a weak
 * phone they took 52-100 s at the first start of a driver build (bc_variants 43-82 s, A35 G68 / A17 G57) and competed
 * with a game started meanwhile; a game's threads now go first. Threads this one starts inherit it, so the long-lived
 * helpers it could start first (the probe heartbeat, the compile watchdog the games' compiles use too) are started before.
 * A probe that reaches its time limit while it mostly waited for a CPU is not counted (run_probes: run again at the next
 * start), so a busy device never turns a converter off. Logged which one took effect; a warning if neither. */
static void bg_lowest_priority(void)
{
    pthread_once(&beat_once, beat_start);
    vkb_guard_watch_start();
    pthread_setname_np(pthread_self(), "vkb-probe-bg");
    pid_t tid = gettid();
    struct sched_param sp = {0};
    int ie = sched_setscheduler(0, SCHED_IDLE, &sp) == 0 ? 0 : errno; /* 0: this thread (Linux: a thread is a task) */
    int ne = setpriority(PRIO_PROCESS, (id_t)tid, 19) == 0 ? 0 : errno;
    errno = 0;
    int nv = getpriority(PRIO_PROCESS, (id_t)tid), pol = sched_getscheduler(0);
    if (pol == SCHED_IDLE || nv == 19)
        VKB_INFO("probe: background probes at the lowest CPU priority (%s, nice %d%s%s), a game's threads go first", pol == SCHED_IDLE ? "SCHED_IDLE" : "SCHED_IDLE refused",
                 nv, ie && pol != SCHED_IDLE ? ": " : "", ie && pol != SCHED_IDLE ? strerror(ie) : "");
    else
        VKB_WARN("probe: background probes at normal CPU priority (SCHED_IDLE: %s; nice 19: %s): they compete with a game started meanwhile",
                 ie ? strerror(ie) : "?", ne ? strerror(ne) : "not kept");
}

static void *background(void *arg)
{
    bg_job *j = arg;
    bg_lowest_priority();
    pprofile *pf = &profiles[j->pf_index];
    double t0 = mono_ms();
    uint32_t api = VK_API_VERSION_1_0;
    if (vkb_global_dt.vkEnumerateInstanceVersion) vkb_global_dt.vkEnumerateInstanceVersion(&api);
    if (api > VK_API_VERSION_1_3) api = VK_API_VERSION_1_3;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-probe", 1, "vkbridge", 1, api};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkb_global_dt.vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) {
        free(j);
        return NULL;
    }
    vkb_dispatch dt;
    vkb_dispatch_load_instance(&dt, vkb_gipa, inst);
    uint32_t n = 0;
    dt.vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pds[VKB_MAX_PDS];
    if (n > VKB_MAX_PDS) n = VKB_MAX_PDS;
    dt.vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        dt.vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (memcmp(id.deviceUUID, j->uuid, VK_UUID_SIZE)) continue;
        pctx c;
        ctx_init(&c, &dt, inst, pds[i], j->emu);
        uint32_t emu = j->emu;
        __atomic_store_n(&bcv_bg, 1, __ATOMIC_RELEASE);
        VKB_INFO("probe: %d probe(s) continue in the background (their emulation decisions apply from the next start)", pf->deferred);
        int left = run_probes(&c, pf, &emu, 0, 1);
        pthread_mutex_lock(&plock);
        pf->complete = !left;
        pf->deferred = 0;
        pf->emu_after = emu; /* for the next start; this one keeps what clients were told */
        pf->bg_ms = mono_ms() - t0;
        pthread_mutex_unlock(&plock);
        uint64_t fl = apply_flags(pf);
        write_profile(pf);
        VKB_INFO("probe: background probes done (%.0f ms background, %.0f ms in all)%s; runtime flags 0x%" PRIx64, pf->bg_ms, pf->start_ms + pf->bg_ms,
                 left ? "; some left for the next start" : "", fl);
        ctx_free(&c);
        break;
    }
    dt.vkDestroyInstance(inst, NULL);
    free(j);
    return NULL;
}

static const probe_def *def_of(const char *name)
{
    for (size_t i = 0; i < NPROBES; i++)
        if (!strcmp(probes[i].name, name)) return &probes[i];
    return NULL;
}

/* The errata decisions' view of an MK probe (server_errata.c, at each vkCreateDevice and for the
 * report): fail where the probe saw the bug's own pattern (its VKB_PROBE_FLAG_MK_* decided on),
 * pass where it passed, else not run - a probe that crashed, hung, died, skipped or is still to run
 * in the background, or failed some other way, decides nothing. Over all GPUs, the worst. */
static int errata_source(const char *probe)
{
    const probe_def *p = def_of(probe);
    if (!p || !p->mk) return VKB_ERRATA_PROBE_NOT_RUN;
    int res = VKB_ERRATA_PROBE_NOT_RUN;
    pthread_mutex_lock(&plock);
    for (int i = 0; i < nprofiles; i++) {
        presult *r = find_result(&profiles[i], probe);
        if (!r) continue;
        if (r->flags & p->flags & (VKB_PROBE_FLAG_MK_ALL | VKB_PROBE_FLAG_PV_ALL | VKB_PROBE_FLAG_PV_V3D_FB | VKB_PROBE_FLAG_PV_FBLOOP))
            res = VKB_ERRATA_PROBE_FAIL; /* (PV-04's loop pattern and PV-06's stand alone) */
        else if (r->status == PS_PASS && res == VKB_ERRATA_PROBE_NOT_RUN) res = VKB_ERRATA_PROBE_PASS;
    }
    pthread_mutex_unlock(&plock);
    return res;
}

int vkb_probe_decide_mode(const uint8_t uuid[VK_UUID_SIZE], int mode, uint32_t *emu, uint64_t *flags)
{
    int found = 0;
    pthread_mutex_lock(&plock);
    for (int i = 0; i < nprofiles && !found; i++) {
        pprofile *pf = &profiles[i];
        if (memcmp(pf->uuid, uuid, VK_UUID_SIZE)) continue;
        found = 1;
        decide_all(pf, mode, emu, flags, NULL);
    }
    pthread_mutex_unlock(&plock);
    return found;
}

void vkb_probe_device(vkb_pd_knowledge *k, VkInstance inst, const vkb_dispatch *idt, VkPhysicalDevice pd)
{
    const char *mode = vkb_srv_env("VKBRIDGE_PROBE");
    if (mode && !strcmp(mode, "0")) {
        VKB_INFO("probe: off (VKBRIDGE_PROBE=0)");
        __atomic_store_n(&bcv_state, -1, __ATOMIC_RELEASE);
        return;
    }
    if (!vkb_pcache_dir() && !(mode && !strcmp(mode, "force"))) {
        VKB_INFO("probe: no cache directory to keep a device profile in; not probing (VKBRIDGE_PROBE=force: probe anyway)");
        __atomic_store_n(&bcv_state, -1, __ATOMIC_RELEASE);
        return;
    }
    if (nprofiles >= VKB_MAX_PDS || !idt->vkGetPhysicalDeviceProperties2) return;
    double t0 = mono_ms();
    pprofile *pf = &profiles[nprofiles];
    memset(pf, 0, sizeof(*pf));
    pf->beat_fd = -1;
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    idt->vkGetPhysicalDeviceProperties2(pd, &p2);
    if (p2.properties.apiVersion >= VK_API_VERSION_1_2) {
        id.pNext = &dp;
        idt->vkGetPhysicalDeviceProperties2(pd, &p2);
    }
    snprintf(pf->gpu, sizeof(pf->gpu), "%s", p2.properties.deviceName);
    snprintf(pf->driver, sizeof(pf->driver), "%.60s %.90s", dp.driverName, dp.driverInfo);
    pf->vendor = p2.properties.vendorID;
    pf->device = p2.properties.deviceID;
    pf->driver_version = p2.properties.driverVersion;
    memcpy(pf->uuid, id.deviceUUID, VK_UUID_SIZE);
    pf->props = p2.properties;
    snprintf(pf->driver_info, sizeof(pf->driver_info), "%s", dp.driverInfo);
    vkb_errata_set_probe_source(errata_source);
    profile_key(pf, &p2.properties, id.driverUUID);
    pf->emu_before = k->info.emu;
    pf->missing_before = k->info.missing;
    __atomic_store_n(&nprofiles, nprofiles + 1, __ATOMIC_RELEASE);

    int force = mode && !strcmp(mode, "force");
    int complete = !force && pf->path[0] && read_profile(pf);
    pf->cached = pf->n > 0;
    uint32_t emu = k->info.emu;
    pctx c;
    ctx_init(&c, idt, inst, pd, emu);
    pf->possible = c.possible;
    if (complete || pf->n) {
        /* the decisions again, from the results as recorded (the switches may have changed) */
        for (int i = 0; i < pf->n; i++)
            for (size_t j = 0; j < NPROBES; j++)
                if (!strcmp(probes[j].name, pf->res[i].name))
                    decide(&probes[j], &pf->res[i], &emu, c.possible, pf->missing_before, vkb_mode(),
                           pf->res[i].status != PS_PASS && pf->res[i].status != PS_SKIP);
    }
    if (complete) {
        int bad = 0;
        for (int i = 0; i < pf->n; i++) bad += pf->res[i].status != PS_PASS && pf->res[i].status != PS_SKIP;
        VKB_INFO("probe: device profile %s (%d probes, %d not passed; VKBRIDGE_PROBE=force re-runs it)", pf->path, pf->n, bad);
    } else {
        char dv[40];
        if (pf->vendor == 0x1010) snprintf(dv, sizeof(dv), "PowerVR driver CL %u", pf->driver_version);
        else snprintf(dv, sizeof(dv), "driver 0x%08x", pf->driver_version);
        VKB_INFO("probe: probing %s (%s) once for this driver and bridge build: %s", pf->gpu, dv,
                 pf->n ? "continuing an earlier run" : "a fresh run");
        pf->deferred = run_probes(&c, pf, &emu, budget_s() * 1000.0, 1);
        pf->complete = !pf->deferred;
        pf->emu_after = emu;
        write_profile(pf);
    }
    ctx_free(&c);
    int cur = vkb_mode();
    decide_unrun(pf, &emu, cur, 1);
    pf->emu_after = emu;
    /* both modes' masks, the one in force picked at each vkbQueryServer and vkCreateDevice
     * (vkb_pds_select_mode): a switch applies from the next client */
    vkb_server_pd_info base = k->info;
    for (int m = 0; m < 2; m++) {
        uint32_t e = 0;
        decide_all(pf, m, &e, NULL, NULL);
        uint32_t add = e & ~base.emu;
        k->info_mode[m] = base;
        k->info_mode[m].emu |= add;
        k->info_mode[m].emu_available |= add;
        k->info_mode[m].missing |= add;
    }
    k->per_mode = 1;
    uint32_t added = k->info_mode[cur].emu & ~base.emu;
    k->info = k->info_mode[cur];
    if (added) VKB_WARN("probe: emulations switched on by the probe: 0x%x (emulating 0x%x now, %s mode)", added, k->info.emu, vkb_mode_name(cur));
    if (k->info_mode[!cur].emu != k->info.emu)
        VKB_INFO("probe: %s mode would emulate 0x%x (this mode 0x%x)", vkb_mode_name(!cur), k->info_mode[!cur].emu, k->info.emu);
    uint64_t fl = apply_flags(pf);
    if (fl) VKB_INFO("probe: runtime flags 0x%" PRIx64 " from the probe", fl);
    pf->start_ms = mono_ms() - t0;
    if (complete) VKB_INFO("probe: cached profile, %.1f ms at start", pf->start_ms);
    else
        VKB_INFO("probe: %.0f ms at start%s", pf->start_ms,
                 pf->deferred ? (pf->n ? "; the rest continues after the server listens" : "; all of it continues after the server listens") : "");
    if (pf->deferred) {
        /* started once the self-test's instance is gone (vkb_probe_start_background) */
        bg_job *j = calloc(1, sizeof(*j));
        if (j) {
            j->pf_index = (int)(pf - profiles);
            memcpy(j->uuid, id.deviceUUID, VK_UUID_SIZE);
            /* what the probes see as emulated: not safe mode's stand-ins for them, or they would
             * skip instead of verifying the native path */
            j->emu = k->info_mode[VKB_MODE_PERFORMANCE].emu;
            pthread_mutex_lock(&plock);
            pending[j->pf_index] = j;
            pthread_mutex_unlock(&plock);
        }
    }
}

void vkb_probe_start_background(void)
{
    for (int i = 0; i < VKB_MAX_PDS; i++) {
        pthread_mutex_lock(&plock);
        bg_job *j = pending[i];
        pending[i] = NULL;
        pthread_mutex_unlock(&plock);
        if (!j) continue;
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, background, j) != 0) {
            VKB_WARN("probe: no background thread; the rest runs at the next start");
            free(j);
        }
        pthread_attr_destroy(&a);
    }
}

/* ------------------------------------------------------------------ for the session report */

/* The sub-cases in bits as their names ("depth_write, cull") */
static void case_names(const probe_def *p, uint32_t bits, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    const char *c = p && p->cases ? p->cases : "";
    for (int i = 0; *c && n + 1 < cap; i++) {
        size_t len = strcspn(c, ",");
        if (bits >> i & 1) n += (size_t)snprintf(out + n, cap - n, "%s%.*s", n ? ", " : "", (int)len, c);
        c += len;
        if (*c == ',') c++;
    }
}

/* An MK probe's result as the errata table shows it: "pass", "fail (cull)", "not run" */
static void mk_result(const pprofile *pf, const char *probe, char *out, size_t cap, uint32_t *failed, int *status)
{
    const presult *r = NULL;
    if (!strcmp(probe, "zink_profile")) { /* pv_zink_name's own probe (server_errata_zink.c): not in the profile */
        int z = vkb_zink_profile_result();
        *failed = 0;
        *status = z == VKB_ERRATA_PROBE_FAIL ? PS_FAIL : z == VKB_ERRATA_PROBE_PASS ? PS_PASS : -1;
        snprintf(out, cap, "%s", z == VKB_ERRATA_PROBE_FAIL ? "fail (the driver's profile for \"mesa zink\" hides features)"
                                 : z == VKB_ERRATA_PROBE_PASS ? "pass" : "not run (not a PowerVR driver, or it could not run)");
        return;
    }
    for (int i = 0; i < pf->n; i++)
        if (!strcmp(pf->res[i].name, probe)) r = &pf->res[i];
    *failed = r ? r->failed : 0;
    *status = r ? r->status : -1;
    if (!r) {
        snprintf(out, cap, "%s", pf->running[0] && !strcmp(pf->running, probe) ? "running" : "not run yet");
        return;
    }
    char cs[160];
    case_names(def_of(probe), r->failed, cs, sizeof(cs));
    snprintf(out, cap, "%s%s%s%s", ps_name[r->status], cs[0] ? " (" : "", cs, cs[0] ? ")" : "");
}

/* With plock not held (the decisions ask the probe's results through errata_source) */
static void errata_md(FILE *f, int i, const VkPhysicalDeviceProperties *props, const char *info)
{
    vkb_errata_row rows[VKB_ERRATA_COUNT];
    int n = vkb_errata_explain(props, info, rows);
    int mode = vkb_mode();
    fprintf(f, "Driver errata, GPU %d (decided at each vkCreateDevice; mode now: %s):\n\n", i, mode == VKB_MODE_PERFORMANCE ? "performance" : "safe");
    fprintf(f, "| erratum | item | probe | probe result | safe mode | performance mode |\n|---|---|---|---|---|---|\n");
    for (int k = 0; k < n; k++) {
        const vkb_errata_row *x = &rows[k];
        char res[200] = "-";
        uint32_t failed = 0;
        int st = -1;
        if (x->probe) {
            pthread_mutex_lock(&plock);
            if (i < nprofiles) mk_result(&profiles[i], x->probe, res, sizeof(res), &failed, &st);
            pthread_mutex_unlock(&plock);
        }
        int bold = st >= 0 && st != PS_PASS && st != PS_SKIP;
        fprintf(f, "| %s | %s | %s%s | %s%s%s | ", x->name, x->mk, x->probe ? x->probe : "none (by version)",
                x->probe && !x->probe_gates ? " (records)" : "", bold ? "**" : "", res, bold ? "**" : "");
        for (int m = 0; m < 2; m++)
            fprintf(f, "%s%s (%s)%s | ", x->on[m] ? "**ON**" : "off", x->on[m] && !x->has_fix ? ", no fix in this build" : "", x->why[m],
                    m == mode ? " - in force" : "");
        fprintf(f, "\n");
    }
    fprintf(f, "\n");
}

void vkb_probe_write_md(FILE *f)
{
    VkPhysicalDeviceProperties props[VKB_MAX_PDS];
    char info[VKB_MAX_PDS][VK_MAX_DRIVER_INFO_SIZE];
    pthread_mutex_lock(&plock);
    int np = nprofiles;
    for (int i = 0; i < np; i++) props[i] = profiles[i].props, memcpy(info[i], profiles[i].driver_info, sizeof(info[i]));
    if (!nprofiles) fprintf(f, "No probe ran (no cache directory, or VKBRIDGE_PROBE=0); no probe decisions in either mode.\n\n");
    int mode = vkb_mode();
    for (int i = 0; i < nprofiles; i++) {
        pprofile *pf = &profiles[i];
        presult decd[MAX_RESULTS];
        uint32_t emu_m[2];
        decide_all(pf, !mode, &emu_m[!mode], NULL, NULL);
        decide_all(pf, mode, &emu_m[mode], NULL, decd); /* the decisions of the mode in force */
        fprintf(f, "Profile `%s` - %s, %s; %s, %.0f ms at start", pf->path[0] ? pf->path : "(not saved)", pf->gpu, pf->driver,
                !pf->cached ? "probed now" : pf->cost_ms > 0 ? "continued from the cache" : "from the cache",
                pf->start_ms);
        if (pf->bg_ms) fprintf(f, ", %.0f ms in the background", pf->bg_ms);
        fprintf(f, "%s.\n\n", pf->complete ? "" : pf->running[0] ? " (running)" : " (background part pending)");
        fprintf(f, "Emulation mask: 0x%x before the probe; after it 0x%x in safe mode, 0x%x in performance mode (%s mode in force%s).\n\n",
                pf->emu_before, emu_m[VKB_MODE_SAFE], emu_m[VKB_MODE_PERFORMANCE], vkb_mode_name(mode),
                mode == VKB_MODE_SAFE ? ": a probe that has not verified the native path keeps its emulation" : "");
        if (mode == VKB_MODE_SAFE) {
            char unrun[400] = "";
            size_t ul = 0;
            for (size_t j = 0; j < NPROBES; j++) {
                const probe_def *p = &probes[j];
                if (!safe_gated(p, pf->missing_before, mode) || (pf->emu_before & p->emu) || !(emu_m[mode] & p->emu) || find_result(pf, p->name)) continue;
                ul += (size_t)snprintf(unrun + ul, sizeof(unrun) - ul, "%s%s (%s)", ul ? ", " : "", emu_name(p->emu, NULL), p->name);
                if (ul >= sizeof(unrun)) break;
            }
            if (unrun[0]) fprintf(f, "Emulated until their probe passes (not run yet, safe mode): %s.\n\n", unrun);
        }
        fprintf(f, "| probe | result | ms | decision (%s mode) | detail |\n|---|---|---:|---|---|\n", vkb_mode_name(mode));
        for (int r = 0; r < pf->n; r++) {
            const presult *x = &decd[r];
            char dec[160] = "";
            if (x->emu)
                snprintf(dec, sizeof(dec), "emulate %s (0x%x)%s", emu_name(x->emu, NULL), x->emu, x->status == PS_ERROR ? ": not verified, safe mode" : "");
            if (x->flags) snprintf(dec + strlen(dec), sizeof(dec) - strlen(dec), "%s%s", dec[0] ? "; " : "", flag_text(x->flags));
            fprintf(f, "| %s | %s%s%s | %.1f | %s | ", x->name, x->status == PS_PASS || x->status == PS_SKIP ? "" : "**", ps_name[x->status],
                    x->status == PS_PASS || x->status == PS_SKIP ? "" : "**", x->ms, dec);
            for (const char *s = x->detail; *s; s++) fputc(*s == '|' ? '/' : *s, f);
            fprintf(f, " |\n");
        }
        fprintf(f, "\n");
    }
    pthread_mutex_unlock(&plock);
    for (int i = 0; i < np; i++) errata_md(f, i, &props[i], info[i]);
}

static void json_errata(FILE *f, int i, const VkPhysicalDeviceProperties *props, const char *info)
{
    vkb_errata_row rows[VKB_ERRATA_COUNT];
    int n = vkb_errata_explain(props, info, rows);
    fprintf(f, ", \"mode\": \"%s\", \"errata\": [", vkb_mode() == VKB_MODE_PERFORMANCE ? "performance" : "safe");
    for (int k = 0; k < n; k++) {
        const vkb_errata_row *x = &rows[k];
        char res[200] = "", cs[160] = "";
        uint32_t failed = 0;
        int st = -1;
        if (x->probe) {
            pthread_mutex_lock(&plock);
            if (i < nprofiles) mk_result(&profiles[i], x->probe, res, sizeof(res), &failed, &st);
            pthread_mutex_unlock(&plock);
            case_names(def_of(x->probe), failed, cs, sizeof(cs));
        }
        fprintf(f, "%s\n      {\"name\": \"%s\", \"item\": \"%s\", \"probe\": ", k ? "," : "", x->name, x->mk);
        json_str(f, x->probe ? x->probe : "");
        fprintf(f, ", \"probe_gates\": %s, \"probe_status\": \"%s\", \"failed_cases\": ", x->probe_gates ? "true" : "false",
                st >= 0 ? ps_name[st] : "not run");
        json_str(f, cs);
        fprintf(f, ", \"decision\": \"%s\"", x->probe_result == VKB_ERRATA_PROBE_FAIL ? "fail" : x->probe_result == VKB_ERRATA_PROBE_PASS ? "pass" : "none");
        for (int m = 0; m < 2; m++) {
            fprintf(f, ", \"%s\": {\"on\": %s, \"why\": ", m ? "performance" : "safe", x->on[m] ? "true" : "false");
            json_str(f, x->why[m]);
            fprintf(f, "}");
        }
        fprintf(f, "}");
    }
    fprintf(f, "]");
}

void vkb_probe_write_json(FILE *f)
{
    pthread_mutex_lock(&plock);
    fprintf(f, "[");
    for (int i = 0; i < nprofiles; i++) {
        const pprofile *pf = &profiles[i];
        fprintf(f, "%s\n    {\"path\": ", i ? "," : "");
        json_str(f, pf->path);
        fprintf(f, ", \"gpu\": ");
        json_str(f, pf->gpu);
        fprintf(f, ", \"driver\": ");
        json_str(f, pf->driver);
        fprintf(f, ", \"key\": ");
        json_str(f, pf->key);
        uint32_t es = 0, ep = 0;
        decide_all(&profiles[i], VKB_MODE_SAFE, &es, NULL, NULL);
        decide_all(&profiles[i], VKB_MODE_PERFORMANCE, &ep, NULL, NULL);
        fprintf(f, ", \"emu_safe\": \"0x%x\", \"emu_performance\": \"0x%x\"", es, ep);
        fprintf(f, ", \"cached\": %s, \"complete\": %s, \"cost_ms\": %.1f, \"start_ms\": %.1f, \"background_ms\": %.1f, \"emu_before\": \"0x%x\", \"emu_after\": \"0x%x\", \"results\": [",
                pf->cached ? "true" : "false", pf->complete ? "true" : "false", pf->cost_ms, pf->start_ms, pf->bg_ms, pf->emu_before, pf->emu_after);
        for (int r = 0; r < pf->n; r++) {
            const presult *x = &pf->res[r];
            fprintf(f, "%s\n      {\"name\": ", r ? "," : "");
            json_str(f, x->name);
            fprintf(f, ", \"status\": \"%s\", \"ms\": %.1f, \"emu\": \"0x%x\", \"flags\": \"0x%" PRIx64 "\", \"cases\": \"0x%x\", \"failed\": \"0x%x\", \"detail\": ",
                    ps_name[x->status], x->ms, x->emu, x->flags, x->cases, x->failed);
            json_str(f, x->detail);
            fprintf(f, "}");
        }
        fprintf(f, "]");
        VkPhysicalDeviceProperties props = pf->props;
        char info[VK_MAX_DRIVER_INFO_SIZE];
        memcpy(info, pf->driver_info, sizeof(info));
        pthread_mutex_unlock(&plock);
        json_errata(f, i, &props, info);
        pthread_mutex_lock(&plock);
        fprintf(f, "}");
    }
    fprintf(f, "%s]", nprofiles ? "\n  " : "");
    pthread_mutex_unlock(&plock);
}

/* One line per probe that did not pass (the report's summary). */
int vkb_probe_issues(void (*cb)(void *arg, const char *line), void *arg)
{
    int n = 0;
    int mode = vkb_mode();
    pthread_mutex_lock(&plock);
    for (int i = 0; i < nprofiles; i++) {
        presult decd[MAX_RESULTS];
        decide_all(&profiles[i], mode, NULL, NULL, decd); /* the decisions of the mode in force */
        for (int r = 0; r < profiles[i].n; r++) {
            const presult *x = &decd[r];
            if (x->status == PS_PASS || x->status == PS_SKIP) continue;
            char line[700], gpu[16] = "";
            if (nprofiles > 1) snprintf(gpu, sizeof(gpu), " (GPU %d)", i);
            int len = snprintf(line, sizeof(line), "probe %s%s: %s", x->name, gpu, ps_name[x->status]);
            if (x->emu) len += snprintf(line + len, sizeof(line) - (size_t)len, " -> %s emulated", emu_name(x->emu, NULL));
            if (x->flags && len < (int)sizeof(line)) len += snprintf(line + len, sizeof(line) - (size_t)len, " -> %s", flag_text(x->flags));
            if (len < (int)sizeof(line)) snprintf(line + len, sizeof(line) - (size_t)len, " (%.200s)", x->detail);
            cb(arg, line);
            n++;
        }
    }
    pthread_mutex_unlock(&plock);
    return n;
}
