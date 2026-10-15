/* SPDX-License-Identifier: GPL-3.0-or-later
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
 *   stop mark (SIGTERM/SIGINT/SIGHUP, the app gone)      interrupted: run again, nothing decided
 *   crash mark (a fatal signal outside the probe's guard, died (on another thread: interrupted)
 *     on the probe's thread)
 *   no mark, still inside past its limit + 2 s           died (the watchdog could not interrupt it)
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
 * does not build leaves its formats decoded. VKBRIDGE_PROBE_APPLY=0 records without applying.
 *
 * Cost: bounded at start (VKBRIDGE_PROBE_BUDGET_S, 6 s; the app waits 15 s for a server's socket):
 * what does not fit runs in a background thread after the server listens, and its emulation
 * decisions apply from the next start (its runtime flags at once). With a profile cached the start
 * only reads it. VKBRIDGE_PROBE=0 turns the probe off, =force re-runs it.
 *
 * Imitation for host tests: VKBRIDGE_PROBE_FAKE=<probe>:<fail|crash|hang|die|abort|stall|slow>,...
 * makes a probe that applies fail, fault, spin until its limit, end the process without a word
 * (_exit), abort, spin with the watchdog's signal blocked (only a kill ends it), or take 3 s
 * longer (to stop a session inside it); VKBRIDGE_FAKE_MISSING applies as everywhere (a feature taken as missing is
 * already emulated, so its probe is skipped).
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "vkb_ext_alias.h"
#include "probe_spv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef VKB_GIT
#define VKB_GIT "unknown"
#endif

/* Bump when a probe is added, removed or its pass criterion changes: cached profiles are re-run. */
#define PROBE_VERSION 1
#define MAX_RESULTS 48
#define DETAIL 480

enum { PS_PASS, PS_FAIL, PS_CRASH, PS_HANG, PS_DIED, PS_SKIP, PS_ERROR, PS_COUNT };
static const char *const ps_name[PS_COUNT] = {"pass", "fail", "crash", "hang", "died", "skip", "error"};

typedef struct presult {
    char name[32];
    int status;
    double ms;
    uint32_t emu;   /* emulation switched on because of it */
    uint32_t flags; /* VKB_PROBE_FLAG_* set because of it */
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
} pprofile;

static pthread_mutex_t plock = PTHREAD_MUTEX_INITIALIZER;
static pprofile profiles[VKB_MAX_PDS];
static int nprofiles;
static uint32_t pflags; /* runtime flags in force */

int vkb_probe_flag(uint32_t f)
{
    return (__atomic_load_n(&pflags, __ATOMIC_ACQUIRE) & f) != 0;
}

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
    const char *divisor_name;
    /* the running probe */
    int st;
    int gpu_stuck; /* a submission never finished, or the device was lost: the device is abandoned */
    char detail[DETAIL];
    size_t dlen;
    pobj objs[96];
    int nobjs;
    /* flags a probe asks for when it does not pass (bc transcoders: per target) */
    uint32_t want_flags;
    int layer_vs_passed;
} pctx;

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
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    const char *exts[16];
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
    f2.pNext = chain;
    if (i->vkGetPhysicalDeviceFeatures2) i->vkGetPhysicalDeviceFeatures2(c->pd, &f2);
    else i->vkGetPhysicalDeviceFeatures(c->pd, &f2.features);
    if (has_ext(c, "VK_KHR_push_descriptor") && !(c->emu & VKB_EMU_PUSH_DESC)) exts[ne++] = "VK_KHR_push_descriptor";
    if (has_ext(c, "VK_KHR_external_memory_fd")) exts[ne++] = "VK_KHR_external_memory_fd";
    if (has_ext(c, "VK_EXT_external_memory_dma_buf") && has_ext(c, "VK_KHR_external_memory_fd")) exts[ne++] = "VK_EXT_external_memory_dma_buf";
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
    c->compression = cc.imageCompressionControl && c->d.vkGetImageSubresourceLayout2;
    c->bindless = bindless;
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
    say(c, "%s, Vulkan %u.%u.%u, driver 0x%08x", c->props.deviceName, VK_API_VERSION_MAJOR(c->props.apiVersion),
        VK_API_VERSION_MINOR(c->props.apiVersion), VK_API_VERSION_PATCH(c->props.apiVersion), c->props.driverVersion);
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
        say(c, "; not claimed exportable%s", (c->emu & VKB_EMU_DMABUF_EXPORT) ? " (emulated)" : "");
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
} tx_build;

static void tx_one(void *arg)
{
    tx_build *b = arg;
    b->r = vkb_emu_bcn_build_transcoder(&b->c->d, b->c->dev, b->cache, b->index, &b->ms);
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
    int n = vkb_emu_bcn_transcoders(), stuck = 0;
    for (int k = 0; k < n; k++) {
        uint32_t flag = 0;
        const char *name = vkb_emu_bcn_transcoder_name(k, &flag);
        tx_build b = {c, cache, k, VK_ERROR_UNKNOWN, 0};
        int sig = 0;
        int g = vkb_guard_run("probe: BC transcoder", sub_limit_s(), tx_one, &b, &sig);
        if (g == VKB_GUARD_DONE && b.r == VK_SUCCESS) {
            say(c, "%s%s %.0f ms", k ? ", " : "", name, b.ms);
            continue;
        }
        say(c, "%s%s %s", k ? ", " : "", name, g == VKB_GUARD_TIMED_OUT ? "HANG" : g == VKB_GUARD_CRASHED ? "CRASH" : "FAILED");
        c->want_flags |= flag;
        outcome(c, g == VKB_GUARD_TIMED_OUT ? PS_HANG : g == VKB_GUARD_CRASHED ? PS_CRASH : PS_FAIL);
        if (g != VKB_GUARD_DONE) { /* the driver may hold a lock now: no more compiles here */
            stuck = 1;
            break;
        }
    }
    /* after a hang or a crash the cache is left as it is (the build may still be in it) */
    if (!stuck) vkb_pcache_own_close(&c->d, c->dev, cache, path);
    if (c->want_flags) say(c, " - formats of a failed transcoder are decoded instead");
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
        say(c, " - the SPIR-V fix (server_spvfix.c) %s", c->props.vendorID == 0x13B5 ? "is on for ARM" : "is turned on");
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
    /* signalled from the queue (wait-before-signal on the host side) */
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &v};
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 0, NULL, NULL, 0, NULL, 1, &s};
    VkResult q = c->d.vkQueueSubmit(c->q, 1, &sub, VK_NULL_HANDLE);
    VkResult w = c->d.vkWaitSemaphores(c->dev, &wi, 1000000000ull);
    uint64_t now = 0;
    if (c->d.vkGetSemaphoreCounterValue) c->d.vkGetSemaphoreCounterValue(c->dev, s, &now);
    say(c, "; signalled by a submission (%d): wait -> %d, value %llu", q, w, (unsigned long long)now);
    bad |= q != VK_SUCCESS || w != VK_SUCCESS || now != 1;
    if (bad) say(c, " - illegal results are retried at run time (server_wait.c)");
    c->d.vkQueueWaitIdle(c->q);
    outcome(c, bad ? PS_FAIL : PS_PASS);
}

/* ------------------------------------------------------------------ the list */

typedef struct probe_def {
    const char *name;
    void (*fn)(pctx *c);
    uint32_t emu;      /* emulation that stands in when it does not pass */
    uint32_t flags;    /* runtime flags it can set (the probe says which in want_flags) */
    int heavy;         /* compiles big shaders: a longer limit, last in line, first to be deferred */
    const char *about; /* the failure class (docs: failure-catalogue) */
} probe_def;

static const probe_def probes[] = {
    {"identity", p_identity, 0, 0, 0, "F18/F46"},
    {"memory_types", p_memory_types, 0, 0, 0, "F01"},
    {"dma_heaps", p_dma_heaps, 0, 0, 0, "F02"},
    {"dmabuf_export", p_dmabuf_export, VKB_EMU_DMABUF_EXPORT, 0, 0, "F03"},
    {"dmabuf_linear_image", p_dmabuf_linear, 0, 0, 0, "F05"},
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
    {"waits", p_waits, 0, 0, 0, "F30"},
    {"layer_vs", p_layer_vs, 0, 0, 0, "F22"},
    {"layer_gs", p_layer_gs, 0, VKB_PROBE_FLAG_GS_FOLD, 0, "F22"},
    {"pipeline_cache", p_pipeline_cache, 0, 0, 0, "F37"},
    {"compile_lut_standin", p_compile_lut_standin, 0, 0, 1, "F26"},
    {"compile_aces", p_compile_aces, 0, VKB_PROBE_FLAG_LUT_NOW, 1, "F26"},
    {"bc_transcoders", p_bc_transcoders, 0, VKB_PROBE_FLAG_TX_ALL, 1, "F09"},
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

static const char *flag_text(uint32_t f)
{
    if (f & VKB_PROBE_FLAG_LUT_NOW) return "UE4 LUT pipelines get their stand-in at once";
    if (f & VKB_PROBE_FLAG_GS_FOLD) return "layer-only geometry stages folded into the vertex shader";
    if (f & VKB_PROBE_FLAG_SPIRV_FIX) return "SPIR-V spec-constant fix on";
    if (f & VKB_PROBE_FLAG_TX_ALL) return "formats of the failed BC transcoder decoded";
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
        fprintf(f, ", \"status\": \"%s\", \"ms\": %.1f, \"emu\": \"0x%x\", \"flags\": \"0x%x\", \"detail\": ", ps_name[r->status], r->ms, r->emu,
                r->flags);
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
    if (p->fn == p_bc_transcoders) return -1;
    return p->heavy ? 2 * sub_limit_s() : sub_limit_s();
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
    pthread_mutex_lock(&plock);
    pf->beat_last = mono_ms();
    pf->beat_active = pf->beat_gap = 0;
    pf->beat_tid = (int)gettid();
    if (fd >= 0) {
        __atomic_store_n(&pf->beat_fd, fd, __ATOMIC_RELEASE);
        beat_write(pf);
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
    char path[700], buf[160] = "";
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
            if (jget(line, "flags", v, sizeof(v))) r->flags = (uint32_t)strtoul(v, NULL, 16);
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
    if (p->fn == p_dma_heaps) return 1;
    if (p->fn == p_dmabuf_export) return c->dmabuf;
    if (p->fn == p_dmabuf_linear) return c->dmabuf;
    if (p->fn == p_compression) return c->compression;
    if (p->fn == p_bindless) return c->bindless;
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
    c->gpu_stuck = 0;
    *dev_lost = 0;
    if (!c->dev && !dev_create(c)) {
        r->status = PS_ERROR;
        snprintf(r->detail, sizeof(r->detail), "no probe device");
        return 0;
    }
    run_arg a = {c, p, applies(c, p) ? fake_mode(p->name) : 0};
    /* the transcoders arm each build themselves (a hang there must not end the whole probe) */
    double limit = probe_limit_s(p);
    int sig = 0;
    double t0 = mono_ms();
    int g = vkb_guard_run(p->name, limit, run_one, &a, &sig);
    r->ms = mono_ms() - t0;
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
    r->flags = c->want_flags; /* kept only if they apply (decide) */
    return 1;
}

/* What a result switches on: emulation bits and runtime flags (only for outcomes other than pass/skip/error). */
static void decide(pctx *c, const probe_def *p, presult *r, uint32_t *emu, int log)
{
    int bad = r->status == PS_FAIL || r->status == PS_CRASH || r->status == PS_HANG || r->status == PS_DIED;
    uint32_t flags = r->flags & p->flags;
    if (p->fn == p_compile_aces) flags = (r->status == PS_HANG || r->status == PS_DIED) ? VKB_PROBE_FLAG_LUT_NOW : 0;
    else if (!bad) flags = 0;
    if (p->fn == p_bc_transcoders && r->status == PS_DIED) flags = VKB_PROBE_FLAG_TX_ALL;
    r->flags = flags;
    r->emu = 0;
    if (!bad) return;
    int apply = env_on("VKBRIDGE_PROBE_APPLY", 1) && env_on("VKBRIDGE_EMULATE", 1);
    if (p->emu && !(*emu & p->emu)) {
        const char *env = NULL;
        const char *what = emu_name(p->emu, &env);
        int ok = (VKB_EMU_IMPLEMENTED & p->emu) && env_on(env, 1);
        if (p->emu == VKB_EMU_DEPTH_CLIP) ok &= c->feat.depthClamp;
        if (p->emu == VKB_EMU_BCN) ok &= vkb_emu_bcn_supported(c->idt, c->pd);
        if (p->emu == VKB_EMU_DMABUF_EXPORT) ok &= vkb_mem_dmaheap_available();
        if (ok && apply) {
            r->emu = p->emu;
            *emu |= p->emu;
        }
        if (log)
            VKB_WARN("probe: %s %s (%s): %s %s", p->name, ps_name[r->status], r->detail, what,
                     !ok ? "cannot be emulated here" : apply ? "EMULATED instead of the driver's" : "would be emulated (VKBRIDGE_PROBE_APPLY=0)");
    }
    if (flags) {
        if (log)
            VKB_WARN("probe: %s %s (%s): %s%s", p->name, ps_name[r->status], r->detail, flag_text(flags), apply ? "" : " - not applied (VKBRIDGE_PROBE_APPLY=0)");
        if (!apply) r->flags = 0;
    }
    if (log && !p->emu && !flags) VKB_WARN("probe: %s %s: %s", p->name, ps_name[r->status], r->detail);
}

static uint32_t apply_flags(pprofile *pf)
{
    uint32_t f = 0;
    for (int i = 0; i < pf->n; i++) f |= pf->res[i].flags;
    __atomic_or_fetch(&pflags, f, __ATOMIC_ACQ_REL);
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
                                       "VKBRIDGE_PROBE_APPLY", "VKBRIDGE_BC_TRANSCODE"};
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
    int left = 0;
    for (size_t i = 0; i < NPROBES; i++) {
        const probe_def *p = &probes[i];
        if (find_result(pf, p->name)) continue;
        double spent = mono_ms() - start;
        double need = (p->heavy ? 2 : 1) * sub_limit_s() * 1000.0;
        if (budget_ms > 0 && spent + (p->heavy ? need : 0) > budget_ms) {
            left++;
            continue;
        }
        if (pf->n >= MAX_RESULTS) break;
        c->emu = *emu;
        c->layer_vs_passed = 0;
        presult *vs = find_result(pf, "layer_vs");
        c->layer_vs_passed = vs && vs->status == PS_PASS;
        snprintf(pf->running, sizeof(pf->running), "%s", p->name);
        write_profile(pf);
        probe_begin(pf);
        presult r;
        int lost = 0;
        probe_one(c, p, &r, &lost);
        double gap = probe_end(pf);
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
        decide(c, p, &r, emu, log);
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
}

static void ctx_free(pctx *c)
{
    dev_destroy(c, 0);
    free(c->ext);
    c->ext = NULL;
}

static void *background(void *arg)
{
    bg_job *j = arg;
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
        VKB_INFO("probe: %d probe(s) continue in the background (their emulation decisions apply from the next start)", pf->deferred);
        int left = run_probes(&c, pf, &emu, 0, 1);
        pthread_mutex_lock(&plock);
        pf->complete = !left;
        pf->deferred = 0;
        pf->emu_after = emu; /* for the next start; this one keeps what clients were told */
        pf->bg_ms = mono_ms() - t0;
        pthread_mutex_unlock(&plock);
        uint32_t fl = apply_flags(pf);
        write_profile(pf);
        VKB_INFO("probe: background probes done (%.0f ms background, %.0f ms in all)%s; runtime flags 0x%x", pf->bg_ms, pf->start_ms + pf->bg_ms,
                 left ? "; some left for the next start" : "", fl);
        ctx_free(&c);
        break;
    }
    dt.vkDestroyInstance(inst, NULL);
    free(j);
    return NULL;
}

void vkb_probe_device(vkb_pd_knowledge *k, VkInstance inst, const vkb_dispatch *idt, VkPhysicalDevice pd)
{
    const char *mode = vkb_srv_env("VKBRIDGE_PROBE");
    if (mode && !strcmp(mode, "0")) {
        VKB_INFO("probe: off (VKBRIDGE_PROBE=0)");
        return;
    }
    if (!vkb_pcache_dir() && !(mode && !strcmp(mode, "force"))) {
        VKB_INFO("probe: no cache directory to keep a device profile in; not probing (VKBRIDGE_PROBE=force: probe anyway)");
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
    profile_key(pf, &p2.properties, id.driverUUID);
    pf->emu_before = k->info.emu;
    __atomic_store_n(&nprofiles, nprofiles + 1, __ATOMIC_RELEASE);

    int force = mode && !strcmp(mode, "force");
    int complete = !force && pf->path[0] && read_profile(pf);
    pf->cached = pf->n > 0;
    uint32_t emu = k->info.emu;
    pctx c;
    ctx_init(&c, idt, inst, pd, emu);
    if (complete || pf->n) {
        /* the decisions again, from the results as recorded (the switches may have changed) */
        for (int i = 0; i < pf->n; i++)
            for (size_t j = 0; j < NPROBES; j++)
                if (!strcmp(probes[j].name, pf->res[i].name)) decide(&c, &probes[j], &pf->res[i], &emu, pf->res[i].status != PS_PASS && pf->res[i].status != PS_SKIP);
    }
    if (complete) {
        int bad = 0;
        for (int i = 0; i < pf->n; i++) bad += pf->res[i].status != PS_PASS && pf->res[i].status != PS_SKIP;
        VKB_INFO("probe: device profile %s (%d probes, %d not passed; VKBRIDGE_PROBE=force re-runs it)", pf->path, pf->n, bad);
    } else {
        VKB_INFO("probe: probing %s (driver 0x%08x) once for this driver and bridge build: %s", pf->gpu, pf->driver_version,
                 pf->n ? "continuing an earlier run" : "a fresh run");
        pf->deferred = run_probes(&c, pf, &emu, budget_s() * 1000.0, 1);
        pf->complete = !pf->deferred;
        pf->emu_after = emu;
        write_profile(pf);
    }
    ctx_free(&c);
    pf->emu_after = emu;
    uint32_t added = emu & ~k->info.emu;
    if (added) {
        k->info.emu |= added;
        k->info.emu_available |= added;
        k->info.missing |= added;
        VKB_WARN("probe: emulations switched on by the probe: 0x%x (emulating 0x%x now)", added, k->info.emu);
    }
    uint32_t fl = apply_flags(pf);
    if (fl) VKB_INFO("probe: runtime flags 0x%x from the probe", fl);
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
            j->emu = k->info.emu;
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

void vkb_probe_write_md(FILE *f)
{
    pthread_mutex_lock(&plock);
    if (!nprofiles) fprintf(f, "No probe ran (no cache directory, or VKBRIDGE_PROBE=0).\n\n");
    for (int i = 0; i < nprofiles; i++) {
        const pprofile *pf = &profiles[i];
        fprintf(f, "Profile `%s` - %s, %s; %s, %.0f ms at start", pf->path[0] ? pf->path : "(not saved)", pf->gpu, pf->driver,
                !pf->cached ? "probed now" : pf->cost_ms > 0 ? "continued from the cache" : "from the cache",
                pf->start_ms);
        if (pf->bg_ms) fprintf(f, ", %.0f ms in the background", pf->bg_ms);
        fprintf(f, "%s.\n\n", pf->complete ? "" : pf->running[0] ? " (running)" : " (background part pending)");
        fprintf(f, "Emulation mask: 0x%x before the probe, 0x%x after.\n\n", pf->emu_before, pf->emu_after);
        fprintf(f, "| probe | result | ms | decision | detail |\n|---|---|---:|---|---|\n");
        for (int r = 0; r < pf->n; r++) {
            const presult *x = &pf->res[r];
            char dec[160] = "";
            if (x->emu) snprintf(dec, sizeof(dec), "emulate %s (0x%x)", emu_name(x->emu, NULL), x->emu);
            if (x->flags) snprintf(dec + strlen(dec), sizeof(dec) - strlen(dec), "%s%s", dec[0] ? "; " : "", flag_text(x->flags));
            fprintf(f, "| %s | %s%s%s | %.1f | %s | ", x->name, x->status == PS_PASS || x->status == PS_SKIP ? "" : "**", ps_name[x->status],
                    x->status == PS_PASS || x->status == PS_SKIP ? "" : "**", x->ms, dec);
            for (const char *s = x->detail; *s; s++) fputc(*s == '|' ? '/' : *s, f);
            fprintf(f, " |\n");
        }
        fprintf(f, "\n");
    }
    pthread_mutex_unlock(&plock);
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
        fprintf(f, ", \"cached\": %s, \"complete\": %s, \"cost_ms\": %.1f, \"start_ms\": %.1f, \"background_ms\": %.1f, \"emu_before\": \"0x%x\", \"emu_after\": \"0x%x\", \"results\": [",
                pf->cached ? "true" : "false", pf->complete ? "true" : "false", pf->cost_ms, pf->start_ms, pf->bg_ms, pf->emu_before, pf->emu_after);
        for (int r = 0; r < pf->n; r++) {
            const presult *x = &pf->res[r];
            fprintf(f, "%s\n      {\"name\": ", r ? "," : "");
            json_str(f, x->name);
            fprintf(f, ", \"status\": \"%s\", \"ms\": %.1f, \"emu\": \"0x%x\", \"flags\": \"0x%x\", \"detail\": ", ps_name[x->status], x->ms, x->emu, x->flags);
            json_str(f, x->detail);
            fprintf(f, "}");
        }
        fprintf(f, "]}");
    }
    fprintf(f, "%s]", nprofiles ? "\n  " : "");
    pthread_mutex_unlock(&plock);
}

/* One line per probe that did not pass (the report's summary). */
int vkb_probe_issues(void (*cb)(void *arg, const char *line), void *arg)
{
    int n = 0;
    pthread_mutex_lock(&plock);
    for (int i = 0; i < nprofiles; i++)
        for (int r = 0; r < profiles[i].n; r++) {
            const presult *x = &profiles[i].res[r];
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
    pthread_mutex_unlock(&plock);
    return n;
}
