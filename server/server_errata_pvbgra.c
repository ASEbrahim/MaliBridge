/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_bgra_store (PV-07, PowerVR): a B8G8R8A8 image written by storage writes holds its texels in
 * R8G8B8A8 byte order. PowerVR has no STORAGE feature on B8G8R8A8; gamescope makes its output images
 * B8G8R8A8 (DRM XRGB8888, our compositor lists it first) with STORAGE anyway, its composite shader
 * writes straight into them (rendervulkan.cpp vulkan_composite: bindTarget(compositeImage)), and the
 * same image's dma-buf goes to our compositor as an XR24 wl_buffer. Every composited pixel reaches the
 * screen with red and blue exchanged (Pixel 10 Pro XL, DXT 25.1@6794074: Steam's #1A9FFF toggle read
 * back as (255,159,26)). Reads (storage reads, sampling) of such an image are right
 * (PV-03): only the writes are swapped.
 *
 * Imitation (host, VKBRIDGE_ERRATA_IMITATE=pv_bgra_store), PowerVR's form for write-only targets: a
 * B8G8R8A8 image with STORAGE usage is made MUTABLE_FORMAT (R8G8B8A8_UNORM added to its view list),
 * each B8G8R8A8 view of it gets an R8G8B8A8_UNORM twin, and a STORAGE_IMAGE descriptor written with
 * the view gets the twin: the shader's (r,g,b,a) lands as bytes r,g,b,a in a B8G8R8A8 image. Sampled
 * descriptors keep the view (reads right). Unlike PowerVR, a storage READ through the twin is swapped
 * too; gamescope's targets and the probe's image are write-only, so that difference never shows.
 * Descriptor update templates are not followed (nothing under test uses them on such an image).
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define NB 64

typedef struct pvb_imi_img {
    VkImage image;
    struct pvb_imi_img *next;
} pvb_imi_img;

typedef struct pvb_imi_view {
    VkImageView view, twin;
    struct pvb_imi_view *next;
} pvb_imi_view;

struct vkb_errata_pvbgra_imi {
    pthread_mutex_t lock;
    pvb_imi_img *imgs[NB];
    pvb_imi_view *views[NB];
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkCreateImageView create_view;
    PFN_vkDestroyImageView destroy_view;
    PFN_vkUpdateDescriptorSets update_sets;
    PFN_vkCmdPushDescriptorSet push_set;
};

static unsigned hb(uint64_t h)
{
    return (unsigned)((h >> 4) ^ (h >> 13)) % NB;
}

static int is_bgra8(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

static struct vkb_errata_pvbgra_imi *imi_of(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e ? e->pvbgra_imi : NULL;
}

static int imi_has_image(struct vkb_errata_pvbgra_imi *m, VkImage image)
{
    int found = 0;
    pthread_mutex_lock(&m->lock);
    for (pvb_imi_img *i = m->imgs[hb((uint64_t)(uintptr_t)image)]; i && !found; i = i->next) found = i->image == image;
    pthread_mutex_unlock(&m->lock);
    return found;
}

static VkImageView imi_twin(struct vkb_errata_pvbgra_imi *m, VkImageView v)
{
    VkImageView t = VK_NULL_HANDLE;
    pthread_mutex_lock(&m->lock);
    for (pvb_imi_view *i = m->views[hb((uint64_t)(uintptr_t)v)]; i; i = i->next)
        if (i->view == v) {
            t = i->twin;
            break;
        }
    pthread_mutex_unlock(&m->lock);
    return t;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    struct vkb_errata_pvbgra_imi *m = imi_of();
    if (!ci || !is_bgra8(ci->format) || !(ci->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return m->create_image(d, ci, a, out);
    /* MUTABLE_FORMAT with R8G8B8A8_UNORM in the view list, so the twin view can be made */
    VkImageCreateInfo c = *ci;
    c.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImageFormatListCreateInfo *fl = NULL;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b && !fl; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) fl = (VkImageFormatListCreateInfo *)b;
    VkFormat list[17];
    const VkFormat *keep = NULL;
    uint32_t keep_n = 0;
    if (fl && fl->viewFormatCount && fl->viewFormatCount < 16) {
        keep = fl->pViewFormats, keep_n = fl->viewFormatCount;
        memcpy(list, keep, keep_n * sizeof(VkFormat));
        list[keep_n] = VK_FORMAT_R8G8B8A8_UNORM;
        fl->pViewFormats = list, fl->viewFormatCount = keep_n + 1;
    }
    VkResult r = m->create_image(d, &c, a, out);
    if (keep) fl->pViewFormats = keep, fl->viewFormatCount = keep_n;
    if (r != VK_SUCCESS) return r;
    pvb_imi_img *i = calloc(1, sizeof(*i));
    if (i) {
        i->image = *out;
        pthread_mutex_lock(&m->lock);
        unsigned h = hb((uint64_t)(uintptr_t)*out);
        i->next = m->imgs[h];
        m->imgs[h] = i;
        pthread_mutex_unlock(&m->lock);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL imi_DestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks *a)
{
    struct vkb_errata_pvbgra_imi *m = imi_of();
    if (m && image) {
        pthread_mutex_lock(&m->lock);
        for (pvb_imi_img **pp = &m->imgs[hb((uint64_t)(uintptr_t)image)]; *pp; pp = &(*pp)->next)
            if ((*pp)->image == image) {
                pvb_imi_img *i = *pp;
                *pp = i->next;
                free(i);
                break;
            }
        pthread_mutex_unlock(&m->lock);
    }
    m->destroy_image(d, image, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                          VkImageView *out)
{
    struct vkb_errata_pvbgra_imi *m = imi_of();
    VkResult r = m->create_view(d, ci, a, out);
    if (r != VK_SUCCESS || !is_bgra8(ci->format) || !imi_has_image(m, ci->image)) return r;
    VkImageViewCreateInfo c = *ci;
    c.format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageView twin = VK_NULL_HANDLE;
    if (m->create_view(d, &c, a, &twin) != VK_SUCCESS) return r;
    pvb_imi_view *v = calloc(1, sizeof(*v));
    if (!v) {
        m->destroy_view(d, twin, a);
        return r;
    }
    v->view = *out, v->twin = twin;
    pthread_mutex_lock(&m->lock);
    unsigned h = hb((uint64_t)(uintptr_t)*out);
    v->next = m->views[h];
    m->views[h] = v;
    pthread_mutex_unlock(&m->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL imi_DestroyImageView(VkDevice d, VkImageView view, const VkAllocationCallbacks *a)
{
    struct vkb_errata_pvbgra_imi *m = imi_of();
    VkImageView twin = VK_NULL_HANDLE;
    if (m && view) {
        pthread_mutex_lock(&m->lock);
        for (pvb_imi_view **pp = &m->views[hb((uint64_t)(uintptr_t)view)]; *pp; pp = &(*pp)->next)
            if ((*pp)->view == view) {
                pvb_imi_view *v = *pp;
                *pp = v->next;
                twin = v->twin;
                free(v);
                break;
            }
        pthread_mutex_unlock(&m->lock);
    }
    if (twin) m->destroy_view(d, twin, a);
    m->destroy_view(d, view, a);
}

/* The writes with a storage-image twin swapped in: a copy of the writes and their image infos, NULL when none */
static VkWriteDescriptorSet *imi_writes(struct vkb_errata_pvbgra_imi *m, uint32_t n, const VkWriteDescriptorSet *w, VkDescriptorImageInfo **infos)
{
    size_t total = 0;
    int any = 0;
    for (uint32_t i = 0; i < n; i++)
        if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && w[i].pImageInfo) {
            total += w[i].descriptorCount;
            for (uint32_t k = 0; k < w[i].descriptorCount && !any; k++) any = imi_twin(m, w[i].pImageInfo[k].imageView) != VK_NULL_HANDLE;
        }
    if (!any) return NULL;
    VkWriteDescriptorSet *c = malloc(n * sizeof(*c));
    VkDescriptorImageInfo *ii = malloc((total ? total : 1) * sizeof(*ii));
    if (!c || !ii) {
        free(c);
        free(ii);
        return NULL;
    }
    memcpy(c, w, n * sizeof(*c));
    size_t at = 0;
    for (uint32_t i = 0; i < n; i++)
        if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && w[i].pImageInfo) {
            for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
                ii[at + k] = w[i].pImageInfo[k];
                VkImageView t = imi_twin(m, ii[at + k].imageView);
                if (t) ii[at + k].imageView = t;
            }
            c[i].pImageInfo = ii + at;
            at += w[i].descriptorCount;
        }
    *infos = ii;
    return c;
}

static VKAPI_ATTR void VKAPI_CALL imi_UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                                           const VkCopyDescriptorSet *cp)
{
    struct vkb_errata_pvbgra_imi *m = imi_of();
    VkDescriptorImageInfo *ii = NULL;
    VkWriteDescriptorSet *c = nw ? imi_writes(m, nw, w, &ii) : NULL;
    m->update_sets(d, nw, c ? c : w, nc, cp);
    free(c);
    free(ii);
}

static VKAPI_ATTR void VKAPI_CALL imi_CmdPushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout pl, uint32_t set,
                                                           uint32_t nw, const VkWriteDescriptorSet *w)
{
    struct vkb_errata_pvbgra_imi *m = imi_of();
    VkDescriptorImageInfo *ii = NULL;
    VkWriteDescriptorSet *c = nw ? imi_writes(m, nw, w, &ii) : NULL;
    m->push_set(cb, bp, pl, set, nw, c ? c : w);
    free(c);
    free(ii);
}

void vkb_errata_pvbgra_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    struct vkb_errata_pvbgra_imi *m = calloc(1, sizeof(*m));
    if (!m) return;
    pthread_mutex_init(&m->lock, NULL);
    m->create_image = t->real.vkCreateImage;
    m->destroy_image = t->real.vkDestroyImage;
    m->create_view = t->real.vkCreateImageView;
    m->destroy_view = t->real.vkDestroyImageView;
    m->update_sets = t->real.vkUpdateDescriptorSets;
    m->push_set = t->real.vkCmdPushDescriptorSet;
    e->pvbgra_imi = m;
    ERRATA_HOOK(t, vkCreateImage, imi_CreateImage);
    ERRATA_HOOK(t, vkDestroyImage, imi_DestroyImage);
    ERRATA_HOOK(t, vkCreateImageView, imi_CreateImageView);
    ERRATA_HOOK(t, vkDestroyImageView, imi_DestroyImageView);
    ERRATA_HOOK(t, vkUpdateDescriptorSets, imi_UpdateDescriptorSets);
    if (m->push_set) ERRATA_HOOK(t, vkCmdPushDescriptorSet, imi_CmdPushDescriptorSet);
}

static void fix_destroy(vkb_srv_table *t, vkb_errata_dev *e);

void vkb_errata_pvbgra_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    fix_destroy(t, e);
    struct vkb_errata_pvbgra_imi *m = e->pvbgra_imi;
    if (m) {
        for (int b = 0; b < NB; b++) {
            for (pvb_imi_img *i = m->imgs[b], *n; i; i = n) n = i->next, free(i);
            for (pvb_imi_view *v = m->views[b], *n; v; v = n) n = v->next, free(v); /* twins go with the device */
        }
        pthread_mutex_destroy(&m->lock);
        free(m);
        e->pvbgra_imi = NULL;
    }
}

/* ------------------------------------------------------------------ the fix
 *
 * Where the startup probe pv_bgra_store saw the swap (a B8G8R8A8 storage write read back with R and B exchanged, the
 * same write through an R8G8B8A8 image right): an image made B8G8R8A8 (UNORM or sRGB) with STORAGE usage and none of
 * COLOR_ATTACHMENT, INPUT_ATTACHMENT, DEPTH_STENCIL_ATTACHMENT or TRANSFER_DST, neither aliased, sparse nor imported with
 * an explicit DRM layout, is made R8G8B8A8 for the driver: its texels can only come from storage writes, which this
 * driver puts in R8G8B8A8 order anyway, so the image holds what its R8G8B8A8 views read and sample. Its view list and its
 * B8G8R8A8 views become R8G8B8A8. A vkCmdCopyImage(2) between it and a B8G8R8A8 image (same encoding) becomes a NEAREST
 * blit of the same regions, which converts; between two such images or with any other format, and to buffers, the copy
 * stays raw and is said once (the driver's R8G8B8A8 order, the same bytes the bug left there).
 * Its memory's dma-buf, once exported (vkGetMemoryFdKHR at the top of the device's table), is registered for DroidDeck's
 * compositor (server_pvbgra_reg.c): it reads that XR24 wl_buffer as RGBA. Nothing is installed where the probe passes.
 */
#include "server_pvbgra_reg.h"

typedef struct pvb_img {
    VkImage image;
    VkFormat client;     /* the client's format (B8G8R8A8_*) */
    VkImageTiling tiling; /* as made */
    int converted;       /* 1: made R8G8B8A8; 0: a plain B8G8R8A8 image (a copy partner) */
    struct pvb_img *next;
} pvb_img;

typedef struct pvb_mem {
    VkDeviceMemory mem;
    struct pvb_mem *next;
} pvb_mem;

struct vkb_errata_pvbgra {
    pthread_mutex_t lock;
    pvb_img *imgs[NB];
    pvb_mem *mems[NB];
    uint32_t table;
    unsigned converted, registered, blits;
    VkFormatFeatureFlags rgba_lin, rgba_opt, bgra_lin, bgra_opt; /* the formats' features, for the blits */
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkCreateImageView create_view;
    PFN_vkCmdCopyImage copy;
    PFN_vkCmdCopyImage2 copy2;
    PFN_vkCmdBlitImage blit;
    PFN_vkCmdBlitImage2 blit2;
    PFN_vkCmdCopyImageToBuffer copy_to_buf;
    /* the top of the table (vkb_errata_pvbgra_export_install) */
    PFN_vkBindImageMemory bind;
    PFN_vkBindImageMemory2 bind2;
    PFN_vkFreeMemory free_mem;
    PFN_vkGetMemoryFdKHR get_fd;
};

/* What the wrappers call next when the call has no device of its own with the fix (a server thread's call: the
 * errata's fallback is the last device made, which may have none): the first install's functions, the same driver /
 * layer functions for every device of this GPU. Bookkeeping is skipped then. Never freed. */
static struct vkb_errata_pvbgra g_next;
static int g_next_set, g_top_set;

static VkFormat rgba_of(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_R8G8B8A8_SRGB : f == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : f;
}

static struct vkb_errata_pvbgra *fix_of(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e ? e->pvbgra : NULL;
}

/* the next functions for f (the fallback's when NULL) */
#define NX(f) ((f) ? (f) : &g_next)

/* An image the fix makes R8G8B8A8 */
static int fix_wants(const VkImageCreateInfo *ci)
{
    const VkImageUsageFlags bad = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!ci || !is_bgra8(ci->format) || !(ci->usage & VK_IMAGE_USAGE_STORAGE_BIT) || (ci->usage & bad) || ci->samples != VK_SAMPLE_COUNT_1_BIT)
        return 0;
    if (ci->flags & (VK_IMAGE_CREATE_ALIAS_BIT | VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                     VK_IMAGE_CREATE_SPARSE_ALIASED_BIT))
        return 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT) return 0; /* an import: its bytes come from elsewhere */
    return 1;
}

static pvb_img *img_find_locked(struct vkb_errata_pvbgra *f, VkImage image)
{
    for (pvb_img *i = f->imgs[hb((uint64_t)(uintptr_t)image)]; i; i = i->next)
        if (i->image == image) return i;
    return NULL;
}

/* A copy of the image's record (0 when it has none) */
static int img_get(struct vkb_errata_pvbgra *f, VkImage image, pvb_img *out)
{
    if (!f) return 0;
    pthread_mutex_lock(&f->lock);
    pvb_img *i = img_find_locked(f, image);
    if (i) *out = *i;
    pthread_mutex_unlock(&f->lock);
    return i != NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL fix_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    struct vkb_errata_pvbgra *f = fix_of();
    if (!f || !ci || !is_bgra8(ci->format)) return NX(f)->create_image(d, ci, a, out);
    int conv = fix_wants(ci);
    VkImageCreateInfo c = *ci;
    VkImageFormatListCreateInfo *fl = NULL;
    VkFormat list[16];
    const VkFormat *keep = NULL;
    if (conv) {
        c.format = rgba_of(ci->format);
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b && !fl; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) fl = (VkImageFormatListCreateInfo *)b;
        if (fl && fl->viewFormatCount && fl->viewFormatCount <= 16 && fl->pViewFormats) {
            keep = fl->pViewFormats;
            for (uint32_t k = 0; k < fl->viewFormatCount; k++) list[k] = rgba_of(keep[k]);
            fl->pViewFormats = list;
        }
    }
    VkResult r = f->create_image(d, &c, a, out);
    if (keep) fl->pViewFormats = keep;
    if (r != VK_SUCCESS) return r;
    pvb_img *i = calloc(1, sizeof(*i));
    if (i) {
        i->image = *out, i->client = ci->format, i->tiling = ci->tiling, i->converted = conv;
        pthread_mutex_lock(&f->lock);
        unsigned h = hb((uint64_t)(uintptr_t)*out);
        i->next = f->imgs[h];
        f->imgs[h] = i;
        unsigned n = conv ? ++f->converted : 0;
        pthread_mutex_unlock(&f->lock);
        if (conv && n <= 6) {
            vkb_srv_table *t = vkb_srv_current_table();
            VKB_INFO("pv_bgra_store: image %llx (pid %d) %ux%u B8G8R8A8 with storage made R8G8B8A8 (usage 0x%x, tiling %d, flags 0x%x) - PV-07",
                     (unsigned long long)(uintptr_t)*out, t && t->proc ? t->proc->pid : 0, ci->extent.width, ci->extent.height, ci->usage,
                     ci->tiling, ci->flags);
        }
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL fix_DestroyImage(VkDevice d, VkImage image, const VkAllocationCallbacks *a)
{
    struct vkb_errata_pvbgra *f = fix_of();
    if (f && image) {
        pthread_mutex_lock(&f->lock);
        for (pvb_img **pp = &f->imgs[hb((uint64_t)(uintptr_t)image)]; *pp; pp = &(*pp)->next)
            if ((*pp)->image == image) {
                pvb_img *i = *pp;
                *pp = i->next;
                free(i);
                break;
            }
        pthread_mutex_unlock(&f->lock);
    }
    NX(f)->destroy_image(d, image, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL fix_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                          VkImageView *out)
{
    struct vkb_errata_pvbgra *f = fix_of();
    pvb_img im;
    if (!ci || !is_bgra8(ci->format) || !img_get(f, ci->image, &im) || !im.converted) return NX(f)->create_view(d, ci, a, out);
    VkImageViewCreateInfo c = *ci;
    c.format = rgba_of(ci->format);
    return f->create_view(d, &c, a, out);
}

static VkFormatFeatureFlags feats(const struct vkb_errata_pvbgra *f, int rgba, VkImageTiling tiling)
{
    int opt = tiling == VK_IMAGE_TILING_OPTIMAL; /* LINEAR and DRM-modifier tilings: the linear features */
    return rgba ? (opt ? f->rgba_opt : f->rgba_lin) : (opt ? f->bgra_opt : f->bgra_lin);
}

/* 1: a copy between src and dst is made a blit (one converted, the other a plain B8G8R8A8 image of the same encoding,
 * both formats blittable); 0: raw */
static int copy_blits(struct vkb_errata_pvbgra *f, VkImage src, VkImage dst)
{
    if (!f) return 0;
    pvb_img s = {0}, t = {0};
    int hs = img_get(f, src, &s), ht = img_get(f, dst, &t);
    int cs = hs && s.converted, ct = ht && t.converted;
    if (cs == ct) return 0;
    if (!hs || !ht || rgba_of(s.client) != rgba_of(t.client)) {
        VKB_ONCE(VKB_LOG_INFO, "pv_bgra_store: a copy between a converted image and an image of another format keeps the driver's R8G8B8A8 "
                               "byte order (as before the fix)");
        return 0;
    }
    if (!(feats(f, cs, s.tiling) & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(feats(f, ct, t.tiling) & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VKB_ONCE(VKB_LOG_WARN, "pv_bgra_store: a copy between a converted image and a B8G8R8A8 one stays raw: the driver cannot blit them "
                               "(R and B exchanged in the copy)");
        return 0;
    }
    if (__atomic_fetch_add(&f->blits, 1, __ATOMIC_RELAXED) == 0)
        VKB_INFO("pv_bgra_store: a copy between a converted image and a B8G8R8A8 one made a blit (it converts) - PV-07");
    return 1;
}

static VkOffset3D plus(VkOffset3D o, VkExtent3D e)
{
    return (VkOffset3D){o.x + (int32_t)e.width, o.y + (int32_t)e.height, o.z + (int32_t)e.depth};
}

static VKAPI_ATTR void VKAPI_CALL fix_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                                   const VkImageCopy *r)
{
    struct vkb_errata_pvbgra *f = fix_of();
    if (!n || !copy_blits(f, src, dst)) {
        NX(f)->copy(cb, src, sl, dst, dl, n, r);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        VkImageBlit b = {r[i].srcSubresource, {r[i].srcOffset, plus(r[i].srcOffset, r[i].extent)}, r[i].dstSubresource,
                         {r[i].dstOffset, plus(r[i].dstOffset, r[i].extent)}};
        f->blit(cb, src, sl, dst, dl, 1, &b, VK_FILTER_NEAREST);
    }
}

static VKAPI_ATTR void VKAPI_CALL fix_CmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2 *ci)
{
    struct vkb_errata_pvbgra *f = fix_of();
    if (!ci || !ci->regionCount || !copy_blits(f, ci->srcImage, ci->dstImage)) {
        NX(f)->copy2(cb, ci);
        return;
    }
    for (uint32_t i = 0; i < ci->regionCount; i++) {
        const VkImageCopy2 *r = &ci->pRegions[i];
        if (f->blit2) {
            VkImageBlit2 b = {VK_STRUCTURE_TYPE_IMAGE_BLIT_2, NULL, r->srcSubresource, {r->srcOffset, plus(r->srcOffset, r->extent)},
                              r->dstSubresource, {r->dstOffset, plus(r->dstOffset, r->extent)}};
            VkBlitImageInfo2 bi = {VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, NULL, ci->srcImage, ci->srcImageLayout, ci->dstImage, ci->dstImageLayout,
                                   1, &b, VK_FILTER_NEAREST};
            f->blit2(cb, &bi);
        } else {
            VkImageBlit b = {r->srcSubresource, {r->srcOffset, plus(r->srcOffset, r->extent)}, r->dstSubresource,
                             {r->dstOffset, plus(r->dstOffset, r->extent)}};
            f->blit(cb, ci->srcImage, ci->srcImageLayout, ci->dstImage, ci->dstImageLayout, 1, &b, VK_FILTER_NEAREST);
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL fix_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n,
                                                           const VkBufferImageCopy *r)
{
    struct vkb_errata_pvbgra *f = fix_of();
    pvb_img s;
    if (img_get(f, src, &s) && s.converted)
        VKB_ONCE(VKB_LOG_INFO, "pv_bgra_store: a copy from a converted image to a buffer keeps the driver's R8G8B8A8 byte order (as before the fix)");
    NX(f)->copy_to_buf(cb, src, sl, dst, n, r);
}

void vkb_errata_pvbgra_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (e->pvbgra) return;
    struct vkb_errata_pvbgra *f = calloc(1, sizeof(*f));
    if (!f) return;
    pthread_mutex_init(&f->lock, NULL);
    f->table = t->id;
    const vkb_srv_table *inst = t->parent;
    if (inst && inst->real.vkGetPhysicalDeviceFormatProperties) {
        VkFormatProperties p;
        inst->real.vkGetPhysicalDeviceFormatProperties(t->physical, VK_FORMAT_R8G8B8A8_UNORM, &p);
        f->rgba_lin = p.linearTilingFeatures, f->rgba_opt = p.optimalTilingFeatures;
        inst->real.vkGetPhysicalDeviceFormatProperties(t->physical, VK_FORMAT_B8G8R8A8_UNORM, &p);
        f->bgra_lin = p.linearTilingFeatures, f->bgra_opt = p.optimalTilingFeatures;
    } else
        f->rgba_lin = f->rgba_opt = f->bgra_lin = f->bgra_opt = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    f->create_image = t->real.vkCreateImage;
    f->destroy_image = t->real.vkDestroyImage;
    f->create_view = t->real.vkCreateImageView;
    f->copy = t->real.vkCmdCopyImage;
    f->copy2 = t->real.vkCmdCopyImage2;
    f->blit = t->real.vkCmdBlitImage;
    f->blit2 = t->real.vkCmdBlitImage2;
    f->copy_to_buf = t->real.vkCmdCopyImageToBuffer;
    e->pvbgra = f;
    if (!__atomic_exchange_n(&g_next_set, 1, __ATOMIC_ACQ_REL)) {
        g_next.create_image = f->create_image, g_next.destroy_image = f->destroy_image, g_next.create_view = f->create_view;
        g_next.copy = f->copy, g_next.copy2 = f->copy2, g_next.blit = f->blit, g_next.blit2 = f->blit2, g_next.copy_to_buf = f->copy_to_buf;
    }
    ERRATA_HOOK(t, vkCreateImage, fix_CreateImage);
    ERRATA_HOOK(t, vkDestroyImage, fix_DestroyImage);
    ERRATA_HOOK(t, vkCreateImageView, fix_CreateImageView);
    if (f->blit) {
        ERRATA_HOOK(t, vkCmdCopyImage, fix_CmdCopyImage);
        if (f->copy2) ERRATA_HOOK(t, vkCmdCopyImage2, fix_CmdCopyImage2);
    }
    ERRATA_HOOK(t, vkCmdCopyImageToBuffer, fix_CmdCopyImageToBuffer);
    vkb_errata_info_once("errata: pv_bgra_store: B8G8R8A8 storage images made R8G8B8A8 (the order this driver writes them in), their "
                         "dma-bufs read as RGBA by the compositor%s - PV-07",
                         vkb_pvbgra_reg_usable() ? "" : " (not on this kernel: before Linux 5.3 dma-bufs share one inode)");
}

/* The image's format as the driver holds it (the dma-buf-over-AHB record, server_dmabuf_ahb.c) */
VkFormat vkb_errata_pvbgra_format(vkb_srv_table *t, VkImage image, VkFormat f)
{
    struct vkb_errata_pvbgra *x = t && t->errata ? t->errata->pvbgra : NULL;
    pvb_img im;
    return x && is_bgra8(f) && img_get(x, image, &im) && im.converted ? rgba_of(f) : f;
}

/* ---- the top of the device's table: memory bound to a converted image, its dma-buf registered once exported */

static struct vkb_errata_pvbgra *top_of(vkb_srv_table **tp)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (tp) *tp = t;
    return t && t->errata ? t->errata->pvbgra : NULL;
}

static void mem_note(struct vkb_errata_pvbgra *f, VkImage image, VkDeviceMemory mem)
{
    pvb_img im;
    if (!f || !mem || !img_get(f, image, &im) || !im.converted) return;
    pvb_mem *m = calloc(1, sizeof(*m));
    if (!m) return;
    m->mem = mem;
    pthread_mutex_lock(&f->lock);
    unsigned h = hb((uint64_t)(uintptr_t)mem);
    m->next = f->mems[h];
    f->mems[h] = m;
    pthread_mutex_unlock(&f->lock);
}

static int mem_has(struct vkb_errata_pvbgra *f, VkDeviceMemory mem)
{
    int found = 0;
    if (!f) return 0;
    pthread_mutex_lock(&f->lock);
    for (pvb_mem *m = f->mems[hb((uint64_t)(uintptr_t)mem)]; m && !found; m = m->next) found = m->mem == mem;
    pthread_mutex_unlock(&f->lock);
    return found;
}

static VKAPI_ATTR VkResult VKAPI_CALL top_BindImageMemory(VkDevice d, VkImage image, VkDeviceMemory mem, VkDeviceSize off)
{
    struct vkb_errata_pvbgra *f = top_of(NULL);
    VkResult r = NX(f)->bind(d, image, mem, off);
    if (r == VK_SUCCESS) mem_note(f, image, mem);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL top_BindImageMemory2(VkDevice d, uint32_t n, const VkBindImageMemoryInfo *bi)
{
    struct vkb_errata_pvbgra *f = top_of(NULL);
    VkResult r = NX(f)->bind2(d, n, bi);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) mem_note(f, bi[i].image, bi[i].memory);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL top_FreeMemory(VkDevice d, VkDeviceMemory mem, const VkAllocationCallbacks *a)
{
    struct vkb_errata_pvbgra *f = top_of(NULL);
    if (f && mem) {
        pthread_mutex_lock(&f->lock);
        for (pvb_mem **pp = &f->mems[hb((uint64_t)(uintptr_t)mem)]; *pp;)
            if ((*pp)->mem == mem) {
                pvb_mem *m = *pp;
                *pp = m->next;
                free(m);
            } else
                pp = &(*pp)->next;
        pthread_mutex_unlock(&f->lock);
        vkb_pvbgra_reg_drop_mem((uint64_t)(uintptr_t)mem, f->table);
    }
    NX(f)->free_mem(d, mem, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL top_GetMemoryFdKHR(VkDevice d, const VkMemoryGetFdInfoKHR *gi, int *fd)
{
    vkb_srv_table *t;
    struct vkb_errata_pvbgra *f = top_of(&t);
    VkResult r = NX(f)->get_fd(d, gi, fd);
    if (!f || r != VK_SUCCESS || !gi || !fd || *fd < 0 || gi->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT || !mem_has(f, gi->memory))
        return r;
    char key[48];
    /* the server is its own process on most builds: the name tells the compositor; the register answers in-process */
    int nerr = vkb_pvbgra_name(*fd);
    if (vkb_pvbgra_reg_add(*fd, (uint64_t)(uintptr_t)gi->memory, f->table, key, sizeof(key)) || !nerr) {
        if (__atomic_add_fetch(&f->registered, 1, __ATOMIC_RELAXED) <= 6)
            VKB_INFO("pv_bgra_store: dma-buf %s of a converted image registered and %s (pid %d): the compositor reads it as RGBA - PV-07",
                     *key ? key : "?", nerr ? "NOT named" : "named " VKB_DMABUF_RGBA8_NAME, t && t->proc ? t->proc->pid : 0);
        if (nerr) VKB_ONCE(VKB_LOG_WARN, "pv_bgra_store: DMA_BUF_SET_NAME refused (%s): a compositor in another process keeps the client's format", strerror(nerr));
    } else
        VKB_ONCE(VKB_LOG_INFO, "pv_bgra_store: a converted image's dma-buf not registered (%s): the compositor reads it as the client's format",
                 vkb_pvbgra_reg_usable() ? "no fd" : "before Linux 5.3 dma-bufs share one inode");
    return r;
}

void vkb_errata_pvbgra_export_install(vkb_srv_table *t)
{
    struct vkb_errata_pvbgra *f = t && t->errata ? t->errata->pvbgra : NULL;
    if (!f || !t->dt.vkBindImageMemory || !t->dt.vkFreeMemory) return;
    f->bind = t->dt.vkBindImageMemory;
    f->bind2 = t->dt.vkBindImageMemory2;
    f->free_mem = t->dt.vkFreeMemory;
    f->get_fd = t->dt.vkGetMemoryFdKHR;
    if (!__atomic_exchange_n(&g_top_set, 1, __ATOMIC_ACQ_REL))
        g_next.bind = f->bind, g_next.bind2 = f->bind2, g_next.free_mem = f->free_mem, g_next.get_fd = f->get_fd;
    t->dt.vkBindImageMemory = top_BindImageMemory;
    if (f->bind2) t->dt.vkBindImageMemory2 = top_BindImageMemory2;
    t->dt.vkFreeMemory = top_FreeMemory;
    if (f->get_fd) t->dt.vkGetMemoryFdKHR = top_GetMemoryFdKHR;
}

static void fix_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    struct vkb_errata_pvbgra *f = e->pvbgra;
    if (!f) return;
    /* the top of the table back as it was: calls after the errata's end (the device's last frees) skip the fix */
    if (t && t->dt.vkBindImageMemory == top_BindImageMemory) t->dt.vkBindImageMemory = f->bind;
    if (t && t->dt.vkBindImageMemory2 == top_BindImageMemory2) t->dt.vkBindImageMemory2 = f->bind2;
    if (t && t->dt.vkFreeMemory == top_FreeMemory) t->dt.vkFreeMemory = f->free_mem;
    if (t && t->dt.vkGetMemoryFdKHR == top_GetMemoryFdKHR) t->dt.vkGetMemoryFdKHR = f->get_fd;
    vkb_pvbgra_reg_drop_table(f->table);
    if (f->converted || f->registered)
        VKB_INFO("pv_bgra_store: device %u: %u image(s) made R8G8B8A8, %u dma-buf(s) registered, %u copy(ies) made blits", f->table, f->converted,
                 f->registered, f->blits);
    for (int b = 0; b < NB; b++) {
        for (pvb_img *i = f->imgs[b], *n; i; i = n) n = i->next, free(i);
        for (pvb_mem *m = f->mems[b], *n; m; m = n) n = m->next, free(m);
    }
    pthread_mutex_destroy(&f->lock);
    free(f);
    e->pvbgra = NULL;
}
