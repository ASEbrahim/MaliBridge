/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * What a process leaves alive when it ends without destroying its device. Wine/DXVK games end that
 * way as a rule; the server's cleanup (server_conn.c vkb_proc_cleanup) frees their memory and then
 * destroys the device with the rest still alive in it. Destroying a device with live objects is
 * not allowed, and drivers do not free those objects for it: their host memory stays in the
 * server, which lives for the whole session. a player log (Tab S11, G925, test12): the server's heap
 * 195 -> 617 -> 1037 -> 1446 MB after three Skyrim SE runs in one session, never back; the session
 * ended with Android's low-memory kill. Lavapipe keeps the same (host/exitheap_test.sh).
 *
 * Here the objects whose driver memory is large - descriptor pools (their sets go with them),
 * command pools (their command buffers too), pipelines, shader modules, pipeline layouts,
 * descriptor set layouts, pipeline caches, query pools - are noted as the driver makes them, at
 * the bottom of the device's dispatch (under every layer: the handles are the driver's, the
 * layers' own objects included), and forgotten as the driver destroys them. When a process's
 * cleanup destroys its device, whatever is still noted is destroyed first, through the driver's
 * own entry points, after the device is idle and every layer has let go of its own objects.
 * A device the app destroys itself is left as before (the notes are dropped, nothing destroyed).
 * VKBRIDGE_EXIT_CHILDREN=0: not installed (the old cleanup).
 *
 * PowerVR (vendor 0x1010; a player log, Pixel 10 / 10 Pro XL DXT and Pixel 11 CXTP, drivers 24.3, 25.1,
 * 25.3): the driver's vkDestroyDevice never returns while the device still has live images, image
 * views, render passes or framebuffers, and every vkCreateDevice after it in the server waits on a
 * driver lock behind it (the bridge's self-test leaves them; Steam never started). Mali drivers take
 * such a destroy. On PowerVR every other kind the server can make on a device is noted too (images,
 * image views, buffers, buffer views, samplers, YCbCr conversions, render passes, framebuffers,
 * semaphores, fences, events, update templates), and whatever is still noted is destroyed before the
 * driver's vkDestroyDevice on BOTH paths: the process's cleanup and the app's own destroy (after
 * the device is idle). Elsewhere those kinds are not wrapped at all: the Mali path is the calls it
 * was. VKBRIDGE_DESTROY_CHILDREN=0: not on PowerVR either; =1: on any GPU.
 * A handle the driver hands out twice (allowed for non-dispatchable handles) is noted once: one
 * destroy of it forgets it, so at most it is left to the driver as before, never destroyed twice.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* In the order they are destroyed: what uses an object before it (command buffers, framebuffers,
 * pipelines, sets before views, render passes, images, layouts; samplers after the set layouts that
 * may hold them). The first eight kinds keep their old order among themselves. */
enum {
    K_CMDPOOL, K_FRAMEBUFFER, K_PIPELINE, K_DESCPOOL, K_UPDTEMPLATE, K_IMAGEVIEW, K_BUFFERVIEW, K_RENDERPASS, K_IMAGE, K_BUFFER,
    K_PLAYOUT, K_DSLAYOUT, K_SAMPLER, K_YCBCR, K_MODULE, K_PCACHE, K_QUERYPOOL, K_SEMAPHORE, K_FENCE, K_EVENT, K_N
};
static const char *const k_name[K_N] = {"command pool", "framebuffer", "pipeline", "descriptor pool", "update template", "image view",
                                        "buffer view", "render pass", "image", "buffer", "pipeline layout", "set layout", "sampler",
                                        "YCbCr conversion", "shader module", "pipeline cache", "query pool", "semaphore", "fence", "event"};

typedef struct { /* open addressing, linear probing, backward-shift deletion; 0 = empty slot */
    uint64_t *slot;
    size_t cap, n;
} hset;

typedef struct vkb_children {
    VkDevice dev;
    hset set[K_N];
    /* the driver's entry points (what the device's dispatch had before) */
    PFN_vkCreateCommandPool create_cmdpool;
    PFN_vkDestroyCommandPool destroy_cmdpool;
    PFN_vkCreateGraphicsPipelines create_gfx;
    PFN_vkCreateComputePipelines create_comp;
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCreateDescriptorPool create_descpool;
    PFN_vkDestroyDescriptorPool destroy_descpool;
    PFN_vkCreatePipelineLayout create_playout;
    PFN_vkDestroyPipelineLayout destroy_playout;
    PFN_vkCreateDescriptorSetLayout create_dslayout;
    PFN_vkDestroyDescriptorSetLayout destroy_dslayout;
    PFN_vkCreateShaderModule create_module;
    PFN_vkDestroyShaderModule destroy_module;
    PFN_vkCreatePipelineCache create_pcache;
    PFN_vkDestroyPipelineCache destroy_pcache;
    PFN_vkCreateQueryPool create_querypool;
    PFN_vkDestroyQueryPool destroy_querypool;
    /* PowerVR only (pvr): the other kinds, and the driver's idle */
    int pvr;
    PFN_vkDeviceWaitIdle wait_idle;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkCreateImageView create_imageview;
    PFN_vkDestroyImageView destroy_imageview;
    PFN_vkCreateBuffer create_buffer;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkCreateBufferView create_bufferview;
    PFN_vkDestroyBufferView destroy_bufferview;
    PFN_vkCreateSampler create_sampler;
    PFN_vkDestroySampler destroy_sampler;
    PFN_vkCreateSamplerYcbcrConversion create_ycbcr;
    PFN_vkDestroySamplerYcbcrConversion destroy_ycbcr;
    PFN_vkCreateRenderPass create_renderpass;
    PFN_vkCreateRenderPass2 create_renderpass2;
    PFN_vkDestroyRenderPass destroy_renderpass;
    PFN_vkCreateFramebuffer create_framebuffer;
    PFN_vkDestroyFramebuffer destroy_framebuffer;
    PFN_vkCreateSemaphore create_semaphore;
    PFN_vkDestroySemaphore destroy_semaphore;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkCreateEvent create_event;
    PFN_vkDestroyEvent destroy_event;
    PFN_vkCreateDescriptorUpdateTemplate create_updtemplate;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_updtemplate;
    struct vkb_children *next;
} vkb_children;

/* One lock for all devices: creates and destroys of these objects are not per-draw calls. */
static pthread_mutex_t ch_lock = PTHREAD_MUTEX_INITIALIZER;
static vkb_children *ch_list;

static size_t h_pos(const hset *h, uint64_t v)
{
    uint64_t x = v * 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 17) & (h->cap - 1);
}

static int h_add(hset *h, uint64_t v);

static int h_grow(hset *h)
{
    hset g = {calloc(h->cap ? h->cap * 2 : 256, sizeof(uint64_t)), h->cap ? h->cap * 2 : 256, 0};
    if (!g.slot) return -1;
    for (size_t i = 0; i < h->cap; i++)
        if (h->slot[i]) h_add(&g, h->slot[i]);
    free(h->slot);
    *h = g;
    return 0;
}

static int h_add(hset *h, uint64_t v)
{
    if (!v) return 0;
    if ((h->n + 1) * 4 > h->cap * 3 && h_grow(h)) return -1;
    size_t i = h_pos(h, v);
    while (h->slot[i]) {
        if (h->slot[i] == v) return 0;
        i = (i + 1) & (h->cap - 1);
    }
    h->slot[i] = v;
    h->n++;
    return 0;
}

static void h_del(hset *h, uint64_t v)
{
    if (!v || !h->cap) return;
    size_t i = h_pos(h, v);
    while (h->slot[i] && h->slot[i] != v) i = (i + 1) & (h->cap - 1);
    if (!h->slot[i]) return;
    h->slot[i] = 0;
    h->n--;
    for (size_t j = (i + 1) & (h->cap - 1); h->slot[j]; j = (j + 1) & (h->cap - 1)) {
        size_t home = h_pos(h, h->slot[j]);
        /* move slot j back to i when its home is not in (i, j] (cyclically) */
        if ((j > i && (home <= i || home > j)) || (j < i && home <= i && home > j)) {
            h->slot[i] = h->slot[j];
            h->slot[j] = 0;
            i = j;
        }
    }
}

static vkb_children *find(VkDevice dev) /* under ch_lock */
{
    for (vkb_children *c = ch_list; c; c = c->next)
        if (c->dev == dev) return c;
    return NULL;
}

static void note(VkDevice dev, int k, uint64_t h, int add)
{
    pthread_mutex_lock(&ch_lock);
    vkb_children *c = find(dev);
    if (c) {
        if (add)
            h_add(&c->set[k], h);
        else
            h_del(&c->set[k], h);
    }
    pthread_mutex_unlock(&ch_lock);
}

static vkb_children *get(VkDevice dev)
{
    pthread_mutex_lock(&ch_lock);
    vkb_children *c = find(dev);
    pthread_mutex_unlock(&ch_lock);
    return c; /* lives until its device's table is destroyed, after the device's last call */
}

#define H(x) ((uint64_t)(uintptr_t)(x))

static VKAPI_ATTR VkResult VKAPI_CALL ch_CreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *p)
{
    VkResult r = get(d)->create_cmdpool(d, ci, a, p);
    if (r == VK_SUCCESS) note(d, K_CMDPOOL, H(*p), 1);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL ch_DestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a)
{
    note(d, K_CMDPOOL, H(p), 0);
    get(d)->destroy_cmdpool(d, p, a);
}

static void note_pipelines(VkDevice d, uint32_t n, const VkPipeline *p)
{
    if (!p) return;
    pthread_mutex_lock(&ch_lock);
    vkb_children *c = find(d);
    for (uint32_t i = 0; c && i < n; i++) h_add(&c->set[K_PIPELINE], H(p[i])); /* failed ones are VK_NULL_HANDLE */
    pthread_mutex_unlock(&ch_lock);
}
static VKAPI_ATTR VkResult VKAPI_CALL ch_CreateGraphicsPipelines(VkDevice d, VkPipelineCache pc, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkPipeline *p)
{
    VkResult r = get(d)->create_gfx(d, pc, n, ci, a, p);
    note_pipelines(d, n, p);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL ch_CreateComputePipelines(VkDevice d, VkPipelineCache pc, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *p)
{
    VkResult r = get(d)->create_comp(d, pc, n, ci, a, p);
    note_pipelines(d, n, p);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL ch_DestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a)
{
    note(d, K_PIPELINE, H(p), 0);
    get(d)->destroy_pipeline(d, p, a);
}

#define PAIR(K, Obj, CI, cf, df)                                                                                               \
    static VKAPI_ATTR VkResult VKAPI_CALL ch_Create##Obj(VkDevice d, const CI *ci, const VkAllocationCallbacks *a, Vk##Obj *o) \
    {                                                                                                                          \
        VkResult r = get(d)->cf(d, ci, a, o);                                                                                  \
        if (r == VK_SUCCESS) note(d, K, H(*o), 1);                                                                             \
        return r;                                                                                                              \
    }                                                                                                                          \
    static VKAPI_ATTR void VKAPI_CALL ch_Destroy##Obj(VkDevice d, Vk##Obj o, const VkAllocationCallbacks *a)                   \
    {                                                                                                                          \
        note(d, K, H(o), 0);                                                                                                   \
        get(d)->df(d, o, a);                                                                                                   \
    }
PAIR(K_DESCPOOL, DescriptorPool, VkDescriptorPoolCreateInfo, create_descpool, destroy_descpool)
PAIR(K_PLAYOUT, PipelineLayout, VkPipelineLayoutCreateInfo, create_playout, destroy_playout)
PAIR(K_DSLAYOUT, DescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, create_dslayout, destroy_dslayout)
PAIR(K_MODULE, ShaderModule, VkShaderModuleCreateInfo, create_module, destroy_module)
PAIR(K_PCACHE, PipelineCache, VkPipelineCacheCreateInfo, create_pcache, destroy_pcache)
PAIR(K_QUERYPOOL, QueryPool, VkQueryPoolCreateInfo, create_querypool, destroy_querypool)
/* PowerVR only (installed by vkb_children_device_init when c->pvr) */
PAIR(K_IMAGE, Image, VkImageCreateInfo, create_image, destroy_image)
PAIR(K_IMAGEVIEW, ImageView, VkImageViewCreateInfo, create_imageview, destroy_imageview)
PAIR(K_BUFFER, Buffer, VkBufferCreateInfo, create_buffer, destroy_buffer)
PAIR(K_BUFFERVIEW, BufferView, VkBufferViewCreateInfo, create_bufferview, destroy_bufferview)
PAIR(K_SAMPLER, Sampler, VkSamplerCreateInfo, create_sampler, destroy_sampler)
PAIR(K_YCBCR, SamplerYcbcrConversion, VkSamplerYcbcrConversionCreateInfo, create_ycbcr, destroy_ycbcr)
PAIR(K_RENDERPASS, RenderPass, VkRenderPassCreateInfo, create_renderpass, destroy_renderpass)
PAIR(K_FRAMEBUFFER, Framebuffer, VkFramebufferCreateInfo, create_framebuffer, destroy_framebuffer)
PAIR(K_SEMAPHORE, Semaphore, VkSemaphoreCreateInfo, create_semaphore, destroy_semaphore)
PAIR(K_FENCE, Fence, VkFenceCreateInfo, create_fence, destroy_fence)
PAIR(K_EVENT, Event, VkEventCreateInfo, create_event, destroy_event)
PAIR(K_UPDTEMPLATE, DescriptorUpdateTemplate, VkDescriptorUpdateTemplateCreateInfo, create_updtemplate, destroy_updtemplate)
#undef PAIR
static VKAPI_ATTR VkResult VKAPI_CALL ch_CreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *o)
{
    VkResult r = get(d)->create_renderpass2(d, ci, a, o);
    if (r == VK_SUCCESS) note(d, K_RENDERPASS, H(*o), 1);
    return r;
}

/* PowerVR's driver (Imagination's vendor ID), or asked for: VKBRIDGE_DESTROY_CHILDREN=0/1; host tests
 * imitate that driver (VKBRIDGE_FAKE_MISSING=pvrdestroy) and take it too */
static int destroy_children_wanted(vkb_srv_table *t)
{
    const char *e = vkb_srv_env("VKBRIDGE_DESTROY_CHILDREN");
    if (e && *e) return strcmp(e, "0") != 0;
    if (vkb_emu_faked("pvrdestroy")) return 1;
    if (!t->real.vkGetPhysicalDeviceProperties) return 0;
    VkPhysicalDeviceProperties p;
    t->real.vkGetPhysicalDeviceProperties(t->physical, &p);
    return p.vendorID == 0x1010;
}

/* Both tables: the layers above take their next from t->dt, and some call t->real directly. */
#define SWAP(field, saved, wrapper)                                         \
    do {                                                                    \
        c->saved = t->real.field;                                           \
        t->real.field = wrapper;                                            \
    } while (0)

void vkb_children_device_init(vkb_srv_table *t)
{
    const char *e = vkb_srv_env("VKBRIDGE_EXIT_CHILDREN");
    if (e && *e == '0') return;
    vkb_dispatch *r = &t->real;
    if (!r->vkCreateCommandPool || !r->vkDestroyCommandPool || !r->vkCreateGraphicsPipelines || !r->vkCreateComputePipelines ||
        !r->vkDestroyPipeline || !r->vkCreateDescriptorPool || !r->vkDestroyDescriptorPool || !r->vkCreatePipelineLayout ||
        !r->vkDestroyPipelineLayout || !r->vkCreateDescriptorSetLayout || !r->vkDestroyDescriptorSetLayout || !r->vkCreateShaderModule ||
        !r->vkDestroyShaderModule || !r->vkCreatePipelineCache || !r->vkDestroyPipelineCache || !r->vkCreateQueryPool ||
        !r->vkDestroyQueryPool)
        return;
    vkb_children *c = calloc(1, sizeof(*c));
    if (!c) return;
    c->dev = t->device;
    SWAP(vkCreateCommandPool, create_cmdpool, ch_CreateCommandPool);
    SWAP(vkDestroyCommandPool, destroy_cmdpool, ch_DestroyCommandPool);
    SWAP(vkCreateGraphicsPipelines, create_gfx, ch_CreateGraphicsPipelines);
    SWAP(vkCreateComputePipelines, create_comp, ch_CreateComputePipelines);
    SWAP(vkDestroyPipeline, destroy_pipeline, ch_DestroyPipeline);
    SWAP(vkCreateDescriptorPool, create_descpool, ch_CreateDescriptorPool);
    SWAP(vkDestroyDescriptorPool, destroy_descpool, ch_DestroyDescriptorPool);
    SWAP(vkCreatePipelineLayout, create_playout, ch_CreatePipelineLayout);
    SWAP(vkDestroyPipelineLayout, destroy_playout, ch_DestroyPipelineLayout);
    SWAP(vkCreateDescriptorSetLayout, create_dslayout, ch_CreateDescriptorSetLayout);
    SWAP(vkDestroyDescriptorSetLayout, destroy_dslayout, ch_DestroyDescriptorSetLayout);
    SWAP(vkCreateShaderModule, create_module, ch_CreateShaderModule);
    SWAP(vkDestroyShaderModule, destroy_module, ch_DestroyShaderModule);
    SWAP(vkCreatePipelineCache, create_pcache, ch_CreatePipelineCache);
    SWAP(vkDestroyPipelineCache, destroy_pcache, ch_DestroyPipelineCache);
    SWAP(vkCreateQueryPool, create_querypool, ch_CreateQueryPool);
    SWAP(vkDestroyQueryPool, destroy_querypool, ch_DestroyQueryPool);
    if (destroy_children_wanted(t)) {
        /* every kind in pairs: one whose create or destroy the driver lacks is not noted (never destroyed for it) */
        c->pvr = 1;
        c->wait_idle = r->vkDeviceWaitIdle;
#define PSWAP(cf, df, Obj)                                    \
    if (r->vkCreate##Obj && r->vkDestroy##Obj) {              \
        SWAP(vkCreate##Obj, cf, ch_Create##Obj);              \
        SWAP(vkDestroy##Obj, df, ch_Destroy##Obj);            \
    }
        PSWAP(create_image, destroy_image, Image)
        PSWAP(create_imageview, destroy_imageview, ImageView)
        PSWAP(create_buffer, destroy_buffer, Buffer)
        PSWAP(create_bufferview, destroy_bufferview, BufferView)
        PSWAP(create_sampler, destroy_sampler, Sampler)
        PSWAP(create_ycbcr, destroy_ycbcr, SamplerYcbcrConversion)
        PSWAP(create_renderpass, destroy_renderpass, RenderPass)
        PSWAP(create_framebuffer, destroy_framebuffer, Framebuffer)
        PSWAP(create_semaphore, destroy_semaphore, Semaphore)
        PSWAP(create_fence, destroy_fence, Fence)
        PSWAP(create_event, destroy_event, Event)
        PSWAP(create_updtemplate, destroy_updtemplate, DescriptorUpdateTemplate)
#undef PSWAP
        if (c->destroy_renderpass && r->vkCreateRenderPass2) SWAP(vkCreateRenderPass2, create_renderpass2, ch_CreateRenderPass2);
        VKB_INFO("device of pid %d: PowerVR: what its app leaves alive in it is destroyed before the device (server_children.c)",
                 t->proc ? t->proc->pid : 0);
    }
    pthread_mutex_lock(&ch_lock);
    c->next = ch_list;
    ch_list = c;
    pthread_mutex_unlock(&ch_lock);
}
#undef SWAP

/* destroy_left: the process ended without destroying the device (the device is idle, the layers
 * have let go of theirs): what is still noted is destroyed, children before what they were made
 * from. 0: the app destroys the device itself; the notes go, as before nothing is destroyed,
 * except on PowerVR (c->pvr), where it is destroyed too, after the device is idle. */
void vkb_children_device_destroy(vkb_srv_table *t, int destroy_left)
{
    pthread_mutex_lock(&ch_lock);
    vkb_children **pp = &ch_list;
    while (*pp && (*pp)->dev != t->device) pp = &(*pp)->next;
    vkb_children *c = *pp;
    if (c) *pp = c->next;
    pthread_mutex_unlock(&ch_lock);
    if (!c) return;
    unsigned long left[K_N] = {0}, total = 0;
    int destroy = destroy_left || c->pvr;
    if (c->pvr && !destroy_left && c->wait_idle) {
        int any = 0;
        for (int k = 0; k < K_N && !any; k++) any = c->set[k].n != 0;
        if (any) c->wait_idle(c->dev); /* the app's own destroy: its work may still run on what goes now */
    }
    for (int k = 0; k < K_N; k++) {
        hset *h = &c->set[k];
        for (size_t i = 0; i < h->cap; i++) {
            uint64_t v = h->slot[i];
            if (!v) continue;
            left[k]++;
            if (!destroy) continue;
            VkDevice d = c->dev;
            switch (k) {
            case K_CMDPOOL: c->destroy_cmdpool(d, (VkCommandPool)(uintptr_t)v, NULL); break;
            case K_PIPELINE: c->destroy_pipeline(d, (VkPipeline)(uintptr_t)v, NULL); break;
            case K_DESCPOOL: c->destroy_descpool(d, (VkDescriptorPool)(uintptr_t)v, NULL); break;
            case K_PLAYOUT: c->destroy_playout(d, (VkPipelineLayout)(uintptr_t)v, NULL); break;
            case K_DSLAYOUT: c->destroy_dslayout(d, (VkDescriptorSetLayout)(uintptr_t)v, NULL); break;
            case K_MODULE: c->destroy_module(d, (VkShaderModule)(uintptr_t)v, NULL); break;
            case K_PCACHE: c->destroy_pcache(d, (VkPipelineCache)(uintptr_t)v, NULL); break;
            case K_QUERYPOOL: c->destroy_querypool(d, (VkQueryPool)(uintptr_t)v, NULL); break;
            case K_IMAGE: c->destroy_image(d, (VkImage)(uintptr_t)v, NULL); break;
            case K_IMAGEVIEW: c->destroy_imageview(d, (VkImageView)(uintptr_t)v, NULL); break;
            case K_BUFFER: c->destroy_buffer(d, (VkBuffer)(uintptr_t)v, NULL); break;
            case K_BUFFERVIEW: c->destroy_bufferview(d, (VkBufferView)(uintptr_t)v, NULL); break;
            case K_SAMPLER: c->destroy_sampler(d, (VkSampler)(uintptr_t)v, NULL); break;
            case K_YCBCR: c->destroy_ycbcr(d, (VkSamplerYcbcrConversion)(uintptr_t)v, NULL); break;
            case K_RENDERPASS: c->destroy_renderpass(d, (VkRenderPass)(uintptr_t)v, NULL); break;
            case K_FRAMEBUFFER: c->destroy_framebuffer(d, (VkFramebuffer)(uintptr_t)v, NULL); break;
            case K_SEMAPHORE: c->destroy_semaphore(d, (VkSemaphore)(uintptr_t)v, NULL); break;
            case K_FENCE: c->destroy_fence(d, (VkFence)(uintptr_t)v, NULL); break;
            case K_EVENT: c->destroy_event(d, (VkEvent)(uintptr_t)v, NULL); break;
            case K_UPDTEMPLATE: c->destroy_updtemplate(d, (VkDescriptorUpdateTemplate)(uintptr_t)v, NULL); break;
            }
        }
        total += left[k];
        free(h->slot);
    }
    /* the driver's entry points back where they were (the table may still be read after this) */
    t->real.vkCreateCommandPool = c->create_cmdpool;
    t->real.vkDestroyCommandPool = c->destroy_cmdpool;
    t->real.vkCreateGraphicsPipelines = c->create_gfx;
    t->real.vkCreateComputePipelines = c->create_comp;
    t->real.vkDestroyPipeline = c->destroy_pipeline;
    t->real.vkCreateDescriptorPool = c->create_descpool;
    t->real.vkDestroyDescriptorPool = c->destroy_descpool;
    t->real.vkCreatePipelineLayout = c->create_playout;
    t->real.vkDestroyPipelineLayout = c->destroy_playout;
    t->real.vkCreateDescriptorSetLayout = c->create_dslayout;
    t->real.vkDestroyDescriptorSetLayout = c->destroy_dslayout;
    t->real.vkCreateShaderModule = c->create_module;
    t->real.vkDestroyShaderModule = c->destroy_module;
    t->real.vkCreatePipelineCache = c->create_pcache;
    t->real.vkDestroyPipelineCache = c->destroy_pcache;
    t->real.vkCreateQueryPool = c->create_querypool;
    t->real.vkDestroyQueryPool = c->destroy_querypool;
    if (c->pvr) {
#define PBACK(cf, df, Obj)                  \
    if (c->cf) {                            \
        t->real.vkCreate##Obj = c->cf;      \
        t->real.vkDestroy##Obj = c->df;     \
    }
        PBACK(create_image, destroy_image, Image)
        PBACK(create_imageview, destroy_imageview, ImageView)
        PBACK(create_buffer, destroy_buffer, Buffer)
        PBACK(create_bufferview, destroy_bufferview, BufferView)
        PBACK(create_sampler, destroy_sampler, Sampler)
        PBACK(create_ycbcr, destroy_ycbcr, SamplerYcbcrConversion)
        PBACK(create_renderpass, destroy_renderpass, RenderPass)
        PBACK(create_framebuffer, destroy_framebuffer, Framebuffer)
        PBACK(create_semaphore, destroy_semaphore, Semaphore)
        PBACK(create_fence, destroy_fence, Fence)
        PBACK(create_event, destroy_event, Event)
        PBACK(create_updtemplate, destroy_updtemplate, DescriptorUpdateTemplate)
#undef PBACK
        if (c->create_renderpass2) t->real.vkCreateRenderPass2 = c->create_renderpass2;
    }
    if (destroy && total) {
        char line[768];
        int n = 0;
        for (int k = 0; k < K_N; k++)
            if (left[k]) n += snprintf(line + n, n < (int)sizeof(line) ? sizeof(line) - (size_t)n : 0, "%s %lu %s(s)", n ? "," : "", left[k], k_name[k]);
        if (destroy_left)
            VKB_INFO("device %u: left alive by its process, destroyed before the device:%s", t->id, line);
        else
            VKB_INFO("device %u: destroyed by its app with objects still alive in it (PowerVR cannot take that), destroyed first:%s", t->id,
                     line);
    }
    free(c);
}

/* ------------------------------------------------------------------ host imitation
 * VKBRIDGE_FAKE_MISSING=pvrdestroy (host tests, host/pvrdestroy_test.sh): the PowerVR DDK driver of the
 * Pixel 10 / 10 Pro XL (DXT) and Pixel 11 (CXTP), drivers 24.3, 25.1, 25.3 (2026-11-10):
 * its vkDestroyDevice never returns while the device still
 * has live images, image views, render passes or framebuffers (it waits in the services kernel from
 * its parameter-buffer teardown), and the next vkCreateDevice in the process then waits on a driver
 * lock behind it (a player log: Steam never starts). Imitated beneath everything, under the driver's own
 * entry points: the four kinds are counted per device; a vkDestroyDevice with any alive blocks for
 * good, and so does every device made after it. */
typedef struct {
    VkDevice dev;
    int live;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkCreateImageView create_view;
    PFN_vkDestroyImageView destroy_view;
    PFN_vkCreateRenderPass create_rp;
    PFN_vkCreateRenderPass2 create_rp2;
    PFN_vkDestroyRenderPass destroy_rp;
    PFN_vkCreateFramebuffer create_fb;
    PFN_vkDestroyFramebuffer destroy_fb;
    PFN_vkDestroyDevice destroy_device;
} fake_dev;
static fake_dev fake_d[16];
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static int fake_stuck;

static fake_dev *fake_find(VkDevice d)
{
    for (int i = 0; i < 16; i++)
        if (fake_d[i].dev == d) return &fake_d[i];
    return NULL;
}
static void fake_count(VkDevice d, int by)
{
    pthread_mutex_lock(&fake_lock);
    fake_dev *f = fake_find(d);
    if (f) f->live += by;
    pthread_mutex_unlock(&fake_lock);
}
static fake_dev *fake_get(VkDevice d)
{
    pthread_mutex_lock(&fake_lock);
    fake_dev *f = fake_find(d);
    pthread_mutex_unlock(&fake_lock);
    return f;
}
static void fake_block(const char *what)
{
    VKB_WARN("imitated PowerVR driver (VKBRIDGE_FAKE_MISSING=pvrdestroy): %s never returns", what);
    for (;;) sleep(1000);
}
static VKAPI_ATTR VkResult VKAPI_CALL fake_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *o)
{
    VkResult r = fake_get(d)->create_image(d, ci, a, o);
    if (r == VK_SUCCESS) fake_count(d, 1);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL fake_DestroyImage(VkDevice d, VkImage o, const VkAllocationCallbacks *a)
{
    if (o) fake_count(d, -1);
    fake_get(d)->destroy_image(d, o, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL fake_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *o)
{
    VkResult r = fake_get(d)->create_view(d, ci, a, o);
    if (r == VK_SUCCESS) fake_count(d, 1);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL fake_DestroyImageView(VkDevice d, VkImageView o, const VkAllocationCallbacks *a)
{
    if (o) fake_count(d, -1);
    fake_get(d)->destroy_view(d, o, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL fake_CreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *o)
{
    VkResult r = fake_get(d)->create_rp(d, ci, a, o);
    if (r == VK_SUCCESS) fake_count(d, 1);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL fake_CreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *o)
{
    VkResult r = fake_get(d)->create_rp2(d, ci, a, o);
    if (r == VK_SUCCESS) fake_count(d, 1);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL fake_DestroyRenderPass(VkDevice d, VkRenderPass o, const VkAllocationCallbacks *a)
{
    if (o) fake_count(d, -1);
    fake_get(d)->destroy_rp(d, o, a);
}
static VKAPI_ATTR VkResult VKAPI_CALL fake_CreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *o)
{
    VkResult r = fake_get(d)->create_fb(d, ci, a, o);
    if (r == VK_SUCCESS) fake_count(d, 1);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL fake_DestroyFramebuffer(VkDevice d, VkFramebuffer o, const VkAllocationCallbacks *a)
{
    if (o) fake_count(d, -1);
    fake_get(d)->destroy_fb(d, o, a);
}
static VKAPI_ATTR void VKAPI_CALL fake_DestroyDevice(VkDevice d, const VkAllocationCallbacks *a)
{
    pthread_mutex_lock(&fake_lock);
    fake_dev *f = fake_find(d);
    int live = f ? f->live : 0;
    PFN_vkDestroyDevice next = f ? f->destroy_device : NULL;
    if (f && live > 0) fake_stuck = 1;
    if (f && live <= 0) memset(f, 0, sizeof(*f));
    pthread_mutex_unlock(&fake_lock);
    if (live > 0) {
        char what[160];
        snprintf(what, sizeof(what), "vkDestroyDevice with %d live image(s)/view(s)/render pass(es)/framebuffer(s)", live);
        fake_block(what);
    }
    if (next) next(d, a);
}

void vkb_children_fake_init(vkb_srv_table *t)
{
    if (!vkb_emu_faked("pvrdestroy")) return;
    vkb_dispatch *r = &t->real;
    pthread_mutex_lock(&fake_lock);
    int stuck = fake_stuck;
    pthread_mutex_unlock(&fake_lock);
    if (stuck) fake_block("vkCreateDevice (it waits on the driver lock the stuck vkDestroyDevice holds)");
    if (!r->vkCreateImage || !r->vkDestroyImage || !r->vkCreateImageView || !r->vkDestroyImageView || !r->vkCreateRenderPass ||
        !r->vkDestroyRenderPass || !r->vkCreateFramebuffer || !r->vkDestroyFramebuffer || !r->vkDestroyDevice)
        return;
    pthread_mutex_lock(&fake_lock);
    fake_dev *f = fake_find(VK_NULL_HANDLE);
    if (f) {
        f->dev = t->device;
        f->live = 0;
        f->create_image = r->vkCreateImage, r->vkCreateImage = fake_CreateImage;
        f->destroy_image = r->vkDestroyImage, r->vkDestroyImage = fake_DestroyImage;
        f->create_view = r->vkCreateImageView, r->vkCreateImageView = fake_CreateImageView;
        f->destroy_view = r->vkDestroyImageView, r->vkDestroyImageView = fake_DestroyImageView;
        f->create_rp = r->vkCreateRenderPass, r->vkCreateRenderPass = fake_CreateRenderPass;
        f->create_rp2 = r->vkCreateRenderPass2;
        if (r->vkCreateRenderPass2) r->vkCreateRenderPass2 = fake_CreateRenderPass2;
        f->destroy_rp = r->vkDestroyRenderPass, r->vkDestroyRenderPass = fake_DestroyRenderPass;
        f->create_fb = r->vkCreateFramebuffer, r->vkCreateFramebuffer = fake_CreateFramebuffer;
        f->destroy_fb = r->vkDestroyFramebuffer, r->vkDestroyFramebuffer = fake_DestroyFramebuffer;
        f->destroy_device = r->vkDestroyDevice, r->vkDestroyDevice = fake_DestroyDevice;
    }
    pthread_mutex_unlock(&fake_lock);
    if (f) VKB_INFO("device of pid %d: imitating a PowerVR driver whose vkDestroyDevice never returns with live render objects (VKBRIDGE_FAKE_MISSING=pvrdestroy)", t->proc ? t->proc->pid : 0);
}
