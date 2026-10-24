/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * VKBRIDGE_TEX_MAXDIM=<n>: textures whose largest side is over n texels are created without their
 * top mip levels, so the largest level kept is at most n a side (a 4096x4096 BC7 texture capped
 * at 1024: 1/16 of the memory). Off by default; per game through presets. What a game at 720p or
 * 1080p shows of a 4096 level is little, and on an 8-12 GB tablet the textures are what runs out
 * (Subnautica's 2.5 GB of transcoded textures, 2026-10-12). Idea from GameNative's BCn mip cap
 * (WRAPPER_* knobs; code not used, written anew here).
 *
 * Which images: 2D, single-sample, optimal tiling, with a mip chain (a level to keep below the cut)
 * and only SAMPLED / TRANSFER_SRC / TRANSFER_DST usage - no render target or storage image, which
 * the GPU writes at the app's size - not sparse, aliased, disjoint or shared with other processes;
 * and by default only BC images (emulated, so their data only ever comes in through uploads and
 * copies the bridge rewrites anyway). `<n>,all` takes any such image (DXVK's immutable textures
 * qualify), `<n>,strict` stops capping new images once a shader that reads the size of a sampled
 * 2D image or fetches its texels by index has been seen (below).
 *
 * The app keeps its own mip numbering: level m of the app's image is level m - drop of the
 * driver's, and the top `drop` levels do not exist. Every command naming levels is rewritten:
 * uploads, copies and resolves into a dropped level are left out, copies out of one too (logged
 * once), but blits from one read the largest kept level instead, scaled; barriers and clears over dropped
 * levels only are left out and the rest cut to the kept levels; views of dropped levels only see
 * the largest kept level. The app's view of the image itself does not change except through the
 * shaders: textureSize / GetDimensions report the smaller size and level count, and texelFetch /
 * Load with the app's coordinates at the app's level reads other texels. Reads with normalized
 * coordinates (nearly all texture reads) only lose the detail of the dropped levels, which shows up
 * close. The scan below is for the others: with `strict`, capping
 * stops once such a shader appears; without, it is counted and logged once, which says where to
 * look if a game draws wrong with the cap. The scan sees modules made with vkCreateShaderModule,
 * not SPIR-V given inline to a pipeline.
 */
#define _GNU_SOURCE
#include "server_emu.h"
#include "spirv_opclass.inc"
#include "vkb_emu_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct texcap {
    uint32_t maxdim;
    int all, strict;
    int stopped;                     /* strict: a shader reading sizes was seen; no new image is capped */
    PFN_vkCmdPipelineBarrier barrier;
    PFN_vkCmdPipelineBarrier2 barrier2;
    PFN_vkCmdWaitEvents wait;
    PFN_vkCmdWaitEvents2 wait2;
    PFN_vkCmdClearColorImage clear;
    PFN_vkCreateImageView view;
    PFN_vkCreateShaderModule module;
} texcap;

static int g_live;                   /* capped images alive, every device (atomic): commands look only when some are */
static uint64_t g_capped, g_sized;   /* images capped, shader modules reading sizes / fetching (atomic) */

static texcap *state(void)
{
    vkb_emu_device *e = vkb_emu_cur();
    return e ? e->texcap : NULL;
}

/* VKBRIDGE_TEX_MAXDIM=<n>[,all][,strict]; 0 / unset: off. Values under 64 are taken as 64. */
static int parse(texcap *t)
{
    const char *v = vkb_srv_env("VKBRIDGE_TEX_MAXDIM"); /* <cache dir>/env and presets too */
    if (!v || !*v) return 0;
    char *end;
    unsigned long n = strtoul(v, &end, 10);
    t->all = t->strict = 0;
    while (*end == ',') {
        if (!strncmp(end, ",all", 4)) t->all = 1, end += 4;
        else if (!strncmp(end, ",strict", 7)) t->strict = 1, end += 7;
        else break;
    }
    if (*end || end == v) {
        VKB_WARN("texture cap: VKBRIDGE_TEX_MAXDIM=%s unknown (<texels>[,all][,strict]): off", v);
        return 0;
    }
    if (!n) return 0;
    t->maxdim = n < 64 ? 64 : n > 65536 ? 65536 : (uint32_t)n;
    return 1;
}

/* ------------------------------------------------------------------ images */

uint32_t vkb_texcap_create_info(vkb_emu_device *e, VkImageCreateInfo *ci)
{
    texcap *t = e ? e->texcap : NULL;
    if (!t || __atomic_load_n(&t->stopped, __ATOMIC_RELAXED)) return 0;
    const VkImageUsageFlags texture = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageCreateFlags refused = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                                       VK_IMAGE_CREATE_SPARSE_ALIASED_BIT | VK_IMAGE_CREATE_ALIAS_BIT | VK_IMAGE_CREATE_DISJOINT_BIT |
                                       VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT | VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT;
    if (ci->imageType != VK_IMAGE_TYPE_2D || ci->mipLevels < 2 || ci->samples != VK_SAMPLE_COUNT_1_BIT ||
        ci->tiling != VK_IMAGE_TILING_OPTIMAL || !(ci->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || (ci->usage & ~texture) ||
        (ci->flags & refused))
        return 0;
    if (!(vkb_is_bc(ci->format) && (e->flags & VKB_EMU_BCN)) && !t->all) return 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) return 0; /* another process's view of it */
    uint32_t w = ci->extent.width, h = ci->extent.height, drop = 0;
    while (drop + 1 < ci->mipLevels && ((w >> drop) > t->maxdim || (h >> drop) > t->maxdim)) drop++;
    if (!drop) return 0;
    ci->extent.width = w >> drop ? w >> drop : 1;
    ci->extent.height = h >> drop ? h >> drop : 1;
    ci->mipLevels -= drop;
    return drop;
}

void vkb_texcap_created(vkb_emu_device *e, vkb_emu_image *r, uint32_t drop, const VkExtent3D *app, uint32_t app_mips)
{
    if (!r || !drop) return;
    r->drop = drop;
    __atomic_add_fetch(&g_live, 1, __ATOMIC_RELAXED);
    uint64_t n = __atomic_add_fetch(&g_capped, 1, __ATOMIC_RELAXED);
    if (n <= 8)
        VKB_INFO("texture cap: image %ux%u (%u mips, format %d) created as %ux%u (%u mips)%s", app->width, app->height, app_mips,
                 r->client_format, r->extent.width, r->extent.height, r->mips, n == 8 ? "; further ones counted" : "");
    else if (!(n & (n - 1)))
        VKB_INFO("texture cap: %llu images capped so far", (unsigned long long)n);
}

void vkb_texcap_destroyed(vkb_emu_image *r)
{
    if (r && r->drop) __atomic_sub_fetch(&g_live, 1, __ATOMIC_RELAXED);
}

/* The capped image's record, or NULL (not capped, or nothing capped anywhere). */
static vkb_emu_image *capped(vkb_emu_device *e, VkImage img)
{
    if (!img || !__atomic_load_n(&g_live, __ATOMIC_RELAXED) || !e) return NULL;
    vkb_emu_image *r = vkb_emu_find_image(e, img);
    return r && r->drop ? r : NULL;
}

/* An app level in place: 1 kept (now the driver's level), 0 dropped. */
static int level(const vkb_emu_image *r, uint32_t *mip)
{
    if (*mip < r->drop) return 0;
    *mip -= r->drop;
    return 1;
}

/* A range of app levels in place: 1 some kept, 0 all dropped. view: a range of dropped levels only
 * becomes the largest kept level (1). */
static int range(const vkb_emu_image *r, VkImageSubresourceRange *s, int view)
{
    uint32_t d = r->drop;
    if (s->levelCount == VK_REMAINING_MIP_LEVELS) {
        s->baseMipLevel = s->baseMipLevel > d ? s->baseMipLevel - d : 0;
        return 1;
    }
    uint32_t end = s->baseMipLevel + s->levelCount;
    if (end <= d) {
        if (!view) return 0;
        s->baseMipLevel = 0, s->levelCount = 1;
        return 1;
    }
    uint32_t base = s->baseMipLevel > d ? s->baseMipLevel : d;
    s->baseMipLevel = base - d, s->levelCount = end - base;
    return 1;
}

static void dropped_read(void)
{
    VKB_ONCE(VKB_LOG_WARN, "texture cap: a copy out of a level the cap dropped is left out (logged once)");
}

/* ------------------------------------------------------------------ copies (from the emulation layer's wrappers) */

uint32_t vkb_texcap_buffer_image(vkb_emu_device *e, VkImage img, int out, VkBufferImageCopy *r, uint32_t n)
{
    vkb_emu_image *c = capped(e, img);
    if (!c) return n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (level(c, &r[i].imageSubresource.mipLevel)) r[k++] = r[i];
        else if (out) dropped_read();
    return k;
}

uint32_t vkb_texcap_buffer_image2(vkb_emu_device *e, VkImage img, int out, VkBufferImageCopy2 *r, uint32_t n)
{
    vkb_emu_image *c = capped(e, img);
    if (!c) return n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (level(c, &r[i].imageSubresource.mipLevel)) r[k++] = r[i];
        else if (out) dropped_read();
    return k;
}

uint32_t vkb_texcap_image_copy(vkb_emu_device *e, VkImage src, VkImage dst, VkImageCopy *r, uint32_t n)
{
    vkb_emu_image *s = capped(e, src), *d = capped(e, dst);
    if (!s && !d) return n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        int ks = !s || level(s, &r[i].srcSubresource.mipLevel), kd = !d || level(d, &r[i].dstSubresource.mipLevel);
        if (ks && kd) r[k++] = r[i];
        else if (!ks && kd) dropped_read();
    }
    return k;
}

uint32_t vkb_texcap_image_copy2(vkb_emu_device *e, VkImage src, VkImage dst, VkImageCopy2 *r, uint32_t n)
{
    vkb_emu_image *s = capped(e, src), *d = capped(e, dst);
    if (!s && !d) return n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        int ks = !s || level(s, &r[i].srcSubresource.mipLevel), kd = !d || level(d, &r[i].dstSubresource.mipLevel);
        if (ks && kd) r[k++] = r[i];
        else if (!ks && kd) dropped_read();
    }
    return k;
}

/* A blit from a dropped level reads the largest kept one, its box scaled down to that level. */
static int blit_src(const vkb_emu_image *s, VkImageSubresourceLayers *sub, VkOffset3D o[2])
{
    if (level(s, &sub->mipLevel)) return 1;
    uint32_t sh = s->drop - sub->mipLevel;
    for (int j = 0; j < 2; j++) o[j].x >>= sh, o[j].y >>= sh;
    sub->mipLevel = 0;
    return o[0].x != o[1].x && o[0].y != o[1].y;
}

uint32_t vkb_texcap_blit(vkb_emu_device *e, VkImage src, VkImage dst, VkImageBlit *r, uint32_t n)
{
    vkb_emu_image *s = capped(e, src), *d = capped(e, dst);
    if (!s && !d) return n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        int kd = !d || level(d, &r[i].dstSubresource.mipLevel);
        if (kd && (!s || blit_src(s, &r[i].srcSubresource, r[i].srcOffsets))) r[k++] = r[i];
    }
    return k;
}

uint32_t vkb_texcap_blit2(vkb_emu_device *e, VkImage src, VkImage dst, VkImageBlit2 *r, uint32_t n)
{
    vkb_emu_image *s = capped(e, src), *d = capped(e, dst);
    if (!s && !d) return n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        int kd = !d || level(d, &r[i].dstSubresource.mipLevel);
        if (kd && (!s || blit_src(s, &r[i].srcSubresource, r[i].srcOffsets))) r[k++] = r[i];
    }
    return k;
}

/* A resolve into a capped texture (its source is multisampled, so never capped): regions into
 * dropped levels left out. */
uint32_t vkb_texcap_resolve(vkb_emu_device *e, VkImage dst, VkImageResolve *r, uint32_t n)
{
    vkb_emu_image *d = capped(e, dst);
    if (!d) return n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (level(d, &r[i].dstSubresource.mipLevel)) r[k++] = r[i];
    return k;
}

uint32_t vkb_texcap_resolve2(vkb_emu_device *e, VkImage dst, VkImageResolve2 *r, uint32_t n)
{
    vkb_emu_image *d = capped(e, dst);
    if (!d) return n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (level(d, &r[i].dstSubresource.mipLevel)) r[k++] = r[i];
    return k;
}

/* ------------------------------------------------------------------ barriers, clears, views */

static uint32_t image_barriers(vkb_emu_device *e, VkImageMemoryBarrier *b, uint32_t n)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        vkb_emu_image *c = capped(e, b[i].image);
        if (!c || range(c, &b[i].subresourceRange, 0)) b[k++] = b[i];
    }
    return k;
}

static uint32_t image_barriers2(vkb_emu_device *e, VkImageMemoryBarrier2 *b, uint32_t n)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        vkb_emu_image *c = capped(e, b[i].image);
        if (!c || range(c, &b[i].subresourceRange, 0)) b[k++] = b[i];
    }
    return k;
}

static VKAPI_ATTR void VKAPI_CALL tc_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds,
                                                       VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                                       const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im)
{
    texcap *t = state();
    if (ni && __atomic_load_n(&g_live, __ATOMIC_RELAXED)) ni = image_barriers(vkb_emu_cur(), (VkImageMemoryBarrier *)im, ni);
    t->barrier(cb, ss, ds, f, nm, m, nb, b, ni, ni ? im : NULL);
}

static void dependency(vkb_emu_device *e, const VkDependencyInfo *d)
{
    VkDependencyInfo *w = (VkDependencyInfo *)d;
    if (w->imageMemoryBarrierCount && __atomic_load_n(&g_live, __ATOMIC_RELAXED))
        w->imageMemoryBarrierCount = image_barriers2(e, (VkImageMemoryBarrier2 *)w->pImageMemoryBarriers, w->imageMemoryBarrierCount);
}

static VKAPI_ATTR void VKAPI_CALL tc_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *d)
{
    dependency(vkb_emu_cur(), d);
    state()->barrier2(cb, d);
}

static VKAPI_ATTR void VKAPI_CALL tc_CmdWaitEvents(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, VkPipelineStageFlags ss,
                                                  VkPipelineStageFlags ds, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                                  const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im)
{
    texcap *t = state();
    if (ni && __atomic_load_n(&g_live, __ATOMIC_RELAXED)) ni = image_barriers(vkb_emu_cur(), (VkImageMemoryBarrier *)im, ni);
    t->wait(cb, ne, ev, ss, ds, nm, m, nb, b, ni, ni ? im : NULL);
}

static VKAPI_ATTR void VKAPI_CALL tc_CmdWaitEvents2(VkCommandBuffer cb, uint32_t ne, const VkEvent *ev, const VkDependencyInfo *d)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < ne; i++) dependency(e, &d[i]);
    state()->wait2(cb, ne, ev, d);
}

static VKAPI_ATTR void VKAPI_CALL tc_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue *c,
                                                       uint32_t n, const VkImageSubresourceRange *r)
{
    texcap *t = state();
    vkb_emu_image *im = capped(vkb_emu_cur(), img);
    if (im) {
        VkImageSubresourceRange *w = (VkImageSubresourceRange *)r;
        uint32_t k = 0;
        for (uint32_t i = 0; i < n; i++)
            if (range(im, &w[i], 0)) w[k++] = w[i];
        if (!(n = k)) return;
    }
    t->clear(cb, img, l, c, n, r);
}

static VKAPI_ATTR VkResult VKAPI_CALL tc_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                        VkImageView *out)
{
    texcap *t = state();
    vkb_emu_image *im = capped(vkb_emu_cur(), ci->image);
    if (im) range(im, &((VkImageViewCreateInfo *)ci)->subresourceRange, 1);
    return t->view(device, ci, a, out);
}

/* ------------------------------------------------------------------ the shader scan */

/* 1 when the module reads the size or level count of a sampled 2D image (OpImageQuerySizeLod,
 * OpImageQuerySize, OpImageQueryLevels) or fetches its texels by index (OpImageFetch): what a capped
 * texture answers differently. Storage images (Sampled = 2), buffers and other shapes are not
 * capped and so not counted. */
int vkb_texcap_spirv_sized(const uint32_t *w, size_t words)
{
    if (words < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3];
    if (bound > (1u << 22)) return 0;
    uint8_t *img2d = calloc(bound, 1); /* id: 1 a sampled 2D image type, 2 a value of such a type */
    uint32_t *type = calloc(bound, sizeof(uint32_t));
    int found = 0;
    for (size_t i = 5; i < words && img2d && type && !found;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > words) break;
        if (op == 25 && n >= 9 && w[i + 1] < bound) { /* OpTypeImage: result, sampled type, Dim, depth, arrayed, MS, sampled */
            if (w[i + 3] == 1 && w[i + 6] == 0 && w[i + 7] == 1) img2d[w[i + 1]] = 1;
        } else if (spv_op_class(op) == 2 && n >= 3 && w[i + 2] < bound) {
            type[w[i + 2]] = w[i + 1];
            if ((op == 95 || op == 103 || op == 104 || op == 106) && n >= 4) { /* fetch, query size (lod), levels */
                uint32_t im = w[i + 3];
                if (im < bound && type[im] < bound && img2d[type[im]] == 1) found = 1;
            }
        }
        i += n;
    }
    free(img2d);
    free(type);
    return found;
}

static VKAPI_ATTR VkResult VKAPI_CALL tc_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci,
                                                           const VkAllocationCallbacks *a, VkShaderModule *out)
{
    texcap *t = state();
    if (ci->pCode && vkb_texcap_spirv_sized(ci->pCode, ci->codeSize / 4)) {
        uint64_t n = __atomic_add_fetch(&g_sized, 1, __ATOMIC_RELAXED);
        if (t->strict && !__atomic_exchange_n(&t->stopped, 1, __ATOMIC_RELAXED))
            VKB_WARN("texture cap: a shader reads the size of 2D textures or fetches their texels (strict): no more textures are "
                     "capped (%llu were)", (unsigned long long)__atomic_load_n(&g_capped, __ATOMIC_RELAXED));
        else if (n == 1)
            VKB_INFO("texture cap: a shader reads the size of 2D textures or fetches their texels; a capped texture bound there "
                     "answers with its smaller size (if the game draws wrong with VKBRIDGE_TEX_MAXDIM, try ',strict' or no cap)");
    }
    return t->module(device, ci, a, out);
}

/* ------------------------------------------------------------------ install */

void vkb_texcap_install(vkb_srv_table *dev)
{
    texcap tmp = {0};
    if (!parse(&tmp)) return;
    texcap *t = calloc(1, sizeof(*t));
    *t = tmp;
    vkb_dispatch *dt = &dev->dt;
#define HOOK(f, mine, slot) do { if (dt->f) { t->slot = dt->f; dt->f = mine; } } while (0)
    HOOK(vkCmdPipelineBarrier, tc_CmdPipelineBarrier, barrier);
    HOOK(vkCmdPipelineBarrier2, tc_CmdPipelineBarrier2, barrier2);
    HOOK(vkCmdWaitEvents, tc_CmdWaitEvents, wait);
    HOOK(vkCmdWaitEvents2, tc_CmdWaitEvents2, wait2);
    HOOK(vkCmdClearColorImage, tc_CmdClearColorImage, clear);
    HOOK(vkCreateImageView, tc_CreateImageView, view);
    HOOK(vkCreateShaderModule, tc_CreateShaderModule, module);
#undef HOOK
    dev->emu->texcap = t;
    VKB_INFO("texture cap: %s 2D textures with mips capped at %u texels a side (VKBRIDGE_TEX_MAXDIM)%s",
             t->all ? "sampled-only" : "BC", t->maxdim, t->strict ? ", until a shader reads texture sizes (strict)" : "");
}

void vkb_texcap_uninstall(vkb_srv_table *dev)
{
    texcap *t = dev->emu ? dev->emu->texcap : NULL;
    if (!t) return;
    vkb_dispatch *dt = &dev->dt;
    if (dt->vkCmdPipelineBarrier == tc_CmdPipelineBarrier) dt->vkCmdPipelineBarrier = t->barrier;
    if (dt->vkCmdPipelineBarrier2 == tc_CmdPipelineBarrier2) dt->vkCmdPipelineBarrier2 = t->barrier2;
    if (dt->vkCmdWaitEvents == tc_CmdWaitEvents) dt->vkCmdWaitEvents = t->wait;
    if (dt->vkCmdWaitEvents2 == tc_CmdWaitEvents2) dt->vkCmdWaitEvents2 = t->wait2;
    if (dt->vkCmdClearColorImage == tc_CmdClearColorImage) dt->vkCmdClearColorImage = t->clear;
    if (dt->vkCreateImageView == tc_CreateImageView) dt->vkCreateImageView = t->view;
    if (dt->vkCreateShaderModule == tc_CreateShaderModule) dt->vkCreateShaderModule = t->module;
    dev->emu->texcap = NULL;
    free(t);
}
