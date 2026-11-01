/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * msaa2_as_4 (MK-46): the Mali-G77's r32p1 (Retroid Pocket 4 Pro, 2026-10-29) multisamples at 1x
 * and 4x only. Vulkan allows that, but DXVK 1.10's D3D9 tells a game 2x is there whatever the
 * driver says, then finds no 2x in the image format query and fails the texture ("D3D9: Cannot
 * create texture ... Samples: 2"); Tales of Symphonia asks for a 2x render target at start and
 * quits when it does not get one (its window closed at once, back to Steam).
 *
 * The clients are shown 2x wherever the driver has 4x (the sample-count limits and the image
 * format queries: vkb_errata_msaa2_instance_install), and everything made with 2 samples is made
 * with 4: images, render pass attachments (vkCreateRenderPass and 2) and graphics pipelines (their
 * rasterization samples; the sample mask's two bits become four, each covering two samples).
 * Images, passes and pipelines all agree, so a 2x pass runs as a 4x one; a resolve to 1x is the
 * same command. What a shader can tell (gl_SampleID up to 3, textureSamples 4) D3D9 cannot ask.
 * Installed driver-near like every erratum, so the emulations' own passes and pipelines (dynamic
 * rendering's) get the same treatment. A driver with 2x of its own is left alone; no imitation.
 *
 * Logged: the first promotion of each kind, and a device total at vkDestroyDevice.
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <stdlib.h>
#include <string.h>

typedef struct vkb_errata_msaa2 {
    PFN_vkCreateImage fix_img;
    PFN_vkCreateRenderPass fix_rp;
    PFN_vkCreateRenderPass2 fix_rp2;
    PFN_vkCreateGraphicsPipelines fix_gfx;
    unsigned long images, passes, pipelines;
} vkb_errata_msaa2;

static vkb_errata_msaa2 *last_state;

static vkb_errata_msaa2 *state(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->msaa2 ? e->msaa2 : __atomic_load_n(&last_state, __ATOMIC_ACQUIRE);
}

#define TWO VK_SAMPLE_COUNT_2_BIT
#define FOUR VK_SAMPLE_COUNT_4_BIT

static void counted(unsigned long *n, const char *what)
{
    if (__atomic_fetch_add(n, 1, __ATOMIC_RELAXED) == 0)
        VKB_INFO("errata: msaa2_as_4: first 2x %s made with 4 samples - MK-46", what);
}

/* ------------------------------------------------------------------ what the clients are shown */

static void counts_up(VkSampleCountFlags *c)
{
    if (*c & FOUR) *c |= TWO;
}

static void limits_up(VkPhysicalDeviceLimits *l)
{
    counts_up(&l->framebufferColorSampleCounts);
    counts_up(&l->framebufferDepthSampleCounts);
    counts_up(&l->framebufferStencilSampleCounts);
    counts_up(&l->framebufferNoAttachmentsSampleCounts);
    counts_up(&l->sampledImageColorSampleCounts);
    counts_up(&l->sampledImageIntegerSampleCounts);
    counts_up(&l->sampledImageDepthSampleCounts);
    counts_up(&l->sampledImageStencilSampleCounts);
    counts_up(&l->storageImageSampleCounts);
}

static const vkb_srv_table *query_inst(void)
{
    const vkb_srv_table *t = vkb_srv_current_table();
    return t && t->is_device ? t->parent : t;
}

static int shown(const vkb_srv_table *t, VkPhysicalDevice pd)
{
    char why[320];
    if (!vkb_errata_msaa2_shown(t, pd, why, sizeof(why))) return 0;
    vkb_errata_info_once("errata: msaa2_as_4: 2x multisampling offered to clients and made with 4 samples (%s) - MK-46", why);
    return 1;
}

static VKAPI_ATTR void VKAPI_CALL ms_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->ms_p1) return;
    t->ms_p1(pd, p);
    if (shown(t, pd)) limits_up(&p->limits);
}

static VKAPI_ATTR void VKAPI_CALL ms_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->ms_p2) return;
    t->ms_p2(pd, p);
    if (shown(t, pd)) limits_up(&p->properties.limits);
}

static VKAPI_ATTR VkResult VKAPI_CALL ms_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat format, VkImageType type,
                                                                               VkImageTiling tiling, VkImageUsageFlags usage,
                                                                               VkImageCreateFlags flags, VkImageFormatProperties *props)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->ms_if1) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = t->ms_if1(pd, format, type, tiling, usage, flags, props);
    if (r == VK_SUCCESS && shown(t, pd)) counts_up(&props->sampleCounts);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL ms_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                                const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                                VkImageFormatProperties2 *props)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->ms_if2) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = t->ms_if2(pd, info, props);
    if (r == VK_SUCCESS && shown(t, pd)) counts_up(&props->imageFormatProperties.sampleCounts);
    return r;
}

void vkb_errata_msaa2_instance_install(vkb_srv_table *t)
{
    /* the decision is asked per query (per physical device): a driver with 2x of its own costs a
     * cached lookup per query, nothing else */
    if (t->dt.vkGetPhysicalDeviceProperties)
        t->ms_p1 = t->dt.vkGetPhysicalDeviceProperties, t->dt.vkGetPhysicalDeviceProperties = ms_GetPhysicalDeviceProperties;
    if (t->dt.vkGetPhysicalDeviceProperties2)
        t->ms_p2 = t->dt.vkGetPhysicalDeviceProperties2, t->dt.vkGetPhysicalDeviceProperties2 = ms_GetPhysicalDeviceProperties2;
    if (t->dt.vkGetPhysicalDeviceImageFormatProperties)
        t->ms_if1 = t->dt.vkGetPhysicalDeviceImageFormatProperties,
        t->dt.vkGetPhysicalDeviceImageFormatProperties = ms_GetPhysicalDeviceImageFormatProperties;
    if (t->dt.vkGetPhysicalDeviceImageFormatProperties2)
        t->ms_if2 = t->dt.vkGetPhysicalDeviceImageFormatProperties2,
        t->dt.vkGetPhysicalDeviceImageFormatProperties2 = ms_GetPhysicalDeviceImageFormatProperties2;
}

/* ------------------------------------------------------------------ what the driver is given */

static VKAPI_ATTR VkResult VKAPI_CALL ms_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    vkb_errata_msaa2 *s = state();
    if (ci->samples != TWO) return s->fix_img(d, ci, a, out);
    VkImageCreateInfo c = *ci;
    c.samples = FOUR;
    counted(&s->images, "image");
    return s->fix_img(d, &c, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL ms_CreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a,
                                                          VkRenderPass *out)
{
    vkb_errata_msaa2 *s = state();
    uint32_t k = 0;
    while (k < ci->attachmentCount && ci->pAttachments[k].samples != TWO) k++;
    if (k == ci->attachmentCount) return s->fix_rp(d, ci, a, out);
    VkAttachmentDescription *at = malloc(ci->attachmentCount * sizeof(*at));
    if (!at) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(at, ci->pAttachments, ci->attachmentCount * sizeof(*at));
    for (; k < ci->attachmentCount; k++)
        if (at[k].samples == TWO) at[k].samples = FOUR;
    VkRenderPassCreateInfo c = *ci;
    c.pAttachments = at;
    counted(&s->passes, "render pass");
    VkResult r = s->fix_rp(d, &c, a, out);
    free(at);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL ms_CreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a,
                                                           VkRenderPass *out)
{
    vkb_errata_msaa2 *s = state();
    uint32_t k = 0;
    while (k < ci->attachmentCount && ci->pAttachments[k].samples != TWO) k++;
    if (k == ci->attachmentCount) return s->fix_rp2(d, ci, a, out);
    VkAttachmentDescription2 *at = malloc(ci->attachmentCount * sizeof(*at));
    if (!at) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(at, ci->pAttachments, ci->attachmentCount * sizeof(*at));
    for (; k < ci->attachmentCount; k++)
        if (at[k].samples == TWO) at[k].samples = FOUR;
    VkRenderPassCreateInfo2 c = *ci;
    c.pAttachments = at;
    counted(&s->passes, "render pass");
    VkResult r = s->fix_rp2(d, &c, a, out);
    free(at);
    return r;
}

/* Sample i of 2 covers samples 2i and 2i+1 of 4 */
static VkSampleMask mask_up(VkSampleMask m)
{
    return (m & 1 ? 0x3u : 0) | (m & 2 ? 0xcu : 0);
}

static VKAPI_ATTR VkResult VKAPI_CALL ms_CreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                                 const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                                 VkPipeline *out)
{
    vkb_errata_msaa2 *s = state();
    uint32_t k = 0;
    while (k < n && !(ci[k].pMultisampleState && ci[k].pMultisampleState->rasterizationSamples == TWO)) k++;
    if (k == n) return s->fix_gfx(d, cache, n, ci, a, out);
    VkGraphicsPipelineCreateInfo *c = malloc(n * sizeof(*c));
    VkPipelineMultisampleStateCreateInfo *ms = malloc(n * sizeof(*ms));
    VkSampleMask *mask = malloc(n * sizeof(*mask));
    if (!c || !ms || !mask) {
        free(c), free(ms), free(mask);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(c, ci, n * sizeof(*c));
    for (; k < n; k++) {
        if (!ci[k].pMultisampleState || ci[k].pMultisampleState->rasterizationSamples != TWO) continue;
        ms[k] = *ci[k].pMultisampleState;
        ms[k].rasterizationSamples = FOUR;
        if (ms[k].pSampleMask) {
            mask[k] = mask_up(ms[k].pSampleMask[0]);
            ms[k].pSampleMask = &mask[k];
        }
        c[k].pMultisampleState = &ms[k];
        counted(&s->pipelines, "pipeline");
    }
    VkResult r = s->fix_gfx(d, cache, n, c, a, out);
    free(c), free(ms), free(mask);
    return r;
}

void vkb_errata_msaa2_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (!vkb_errata_msaa2_driver_lacks(t, t->physical)) {
        if (!e->probe) vkb_errata_info_once("errata: msaa2_as_4: the driver has 2x multisampling of its own; nothing to do - MK-46");
        return;
    }
    if (!e->msaa2) e->msaa2 = calloc(1, sizeof(*e->msaa2));
    vkb_errata_msaa2 *s = e->msaa2;
    if (!s || !t->real.vkCreateImage || !t->real.vkCreateRenderPass || !t->real.vkCreateGraphicsPipelines) return;
    if (!e->probe)
        vkb_errata_info_once("errata: msaa2_as_4: images, render passes and pipelines asking for 2 samples made with 4 (the driver "
                             "has 1x and 4x only) - MK-46");
    s->fix_img = t->real.vkCreateImage;
    s->fix_rp = t->real.vkCreateRenderPass;
    s->fix_rp2 = t->real.vkCreateRenderPass2;
    s->fix_gfx = t->real.vkCreateGraphicsPipelines;
    ERRATA_HOOK(t, vkCreateImage, ms_CreateImage);
    ERRATA_HOOK(t, vkCreateRenderPass, ms_CreateRenderPass);
    if (s->fix_rp2) ERRATA_HOOK(t, vkCreateRenderPass2, ms_CreateRenderPass2);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, ms_CreateGraphicsPipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_msaa2_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_msaa2 *s = e->msaa2;
    if (!s) return;
    if (s->images || s->passes || s->pipelines)
        VKB_INFO("errata: msaa2_as_4: device %u: %lu image(s), %lu render pass(es), %lu pipeline(s) made with 4 samples for 2", t->id,
                 s->images, s->passes, s->pipelines);
    e->msaa2 = NULL;
    /* the last one made stays: a server thread that finds no table falls back to it */
    if (e->probe || __atomic_load_n(&last_state, __ATOMIC_ACQUIRE) != s) free(s);
}
