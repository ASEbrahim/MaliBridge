/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Driver errata on images and image views (server_errata.c decides which are on):
 *
 * view_counts (MK-07, Arm erratum 4222073, r19p0-r54p0): a 2D-array view of a 3D image with
 *   baseArrayLayer >= 1 and VK_REMAINING_ARRAY_LAYERS gets its slices wrong or loses the device.
 *   The fix gives every view explicit counts: VK_REMAINING_ARRAY_LAYERS and VK_REMAINING_MIP_LEVELS
 *   are resolved against the image's own create info, as the spec defines them - for a 3D image
 *   viewed as 2D or 2D array, the slices of the view's mip level (depth >> baseMipLevel) from
 *   baseArrayLayer on. Always on: it is exact, and costs a lookup per view that uses them.
 *   Copies, blits and barriers are not covered: the erratum is the view's layer count alone, and
 *   VK_REMAINING_ARRAY_LAYERS in a copy region is already resolved by the emulation layer above
 *   (server_emu_layer.c fix_layers, before the BC decoder sees the region).
 *   Imitation (host): a 3D image's 2D/2D-array view with VK_REMAINING_ARRAY_LAYERS starts at
 *   slice 0 - the wrong slices of the erratum, not its device loss.
 *
 * view_quarantine (MK-06, Arm erratum 3786926, r49p1-r49p2, r50p0-r51p0): vkCmdBeginRendering
 *   caches state by the VkRenderingInfo's contents, image-view handles included, and a handle
 *   value the driver hands out again after vkDestroyImageView hits the stale entry (artifacts, or
 *   a device loss inside dynamic rendering). The fix keeps handle values from coming back soon:
 *   a view that vkCmdBeginRendering has seen is not destroyed when the app destroys it but goes
 *   into a FIFO (4096 views, VKBRIDGE_VIEW_QUARANTINE=n); the oldest is destroyed when it is
 *   full. The app's destroy already promised the GPU is done with the view, so no fence is
 *   tracked: what the fix holds back is the handle value, not GPU work. vkDestroyImage of an
 *   image with views in the quarantine waits for them (at most a quarter of the FIFO's size in
 *   images, 1024 by default: past it the oldest views go first); its memory may be freed before,
 *   which Vulkan allows. Images shared with another process (external memory, DRM format
 *   modifiers) are left alone. vkDestroyDevice destroys what is held. Bounded: at most the FIFO's
 *   views and their images, a few MB.
 *   Imitation (host): a 256-entry cache of image-view handle values seen by vkCmdBeginRendering,
 *   which destroying a view does not clear: a colour attachment whose handle value comes back on
 *   another image hits the stale entry, and the pass leaves that attachment unwritten.
 *
 * Both share one set of wrappers (vkCreateImage, vkDestroyImage, vkCreateImageView,
 * vkDestroyImageView, vkCmdBeginRendering), installed once beneath the fixes and imitations of
 * later errata, with each fix above its imitation inside it; and one image record map
 * ({type, extent, mips, layers, flags}, filled at vkCreateImage - the emulation layer's records
 * are above and keyed by its own needs).
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define IMG_BUCKETS 4096
#define VIEW_BUCKETS 4096
#define QUARANTINE_DEFAULT 4096
#define STALE_ENTRIES 256 /* the imitated driver cache */
#define MAX_DEVICES 64

typedef struct img_rec {
    VkImage h;
    VkImageType type;
    VkExtent3D extent;
    uint32_t mips, layers;
    VkImageCreateFlags flags;
    uint8_t shared;   /* external memory or a DRM format modifier: never held back */
    uint8_t deferred; /* the app destroyed it; destroyed when its last held view goes */
    uint32_t held;    /* its views in the quarantine */
    struct img_rec *next;
} img_rec;

typedef struct view_rec {
    VkImageView h;
    VkImage image;
    uint8_t rendered; /* seen by vkCmdBeginRendering */
    struct view_rec *next;
} view_rec;

typedef struct {
    VkImageView view;
    VkImage image;
} held_view;

typedef struct vkb_errata_views {
    pthread_mutex_t lock;
    VkDevice device;
    vkb_errata_dev *owner;
    vkb_dispatch below; /* t->real as it was beneath these wrappers */
    img_rec *imgs[IMG_BUCKETS];
    view_rec *views[VIEW_BUCKETS];
    uint32_t nimgs, nviews;
    held_view *q;            /* the quarantine: a ring of qcap */
    uint32_t qcap, qhead, qn, img_cap, deferred;
    struct {
        VkImageView view;
        VkImage image;
    } stale[STALE_ENTRIES];
    uint32_t stale_next;
    /* counts, logged at vkDestroyDevice */
    uint64_t resolved, resolved_3d, unknown_image, imi_counts;
    uint64_t held_total, held_max, released, deferred_total, deferred_max, imi_stale;
    int logged_resolve, logged_hold, logged_imi;
} vkb_errata_views;

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static vkb_errata_views *reg[MAX_DEVICES];

static unsigned hash(uint64_t k, unsigned buckets)
{
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    k ^= k >> 29;
    return (unsigned)k & (buckets - 1);
}

#define ON(e, id) ((e)->on >> (id) & 1)
#define IMI(e, id) ((e)->imitated >> (id) & 1)

/* The views state for a call on device d and, in *out, d's errata: the call's table, else the
 * device that owns d (a call on a server thread of its own). NULL: none known, *out the call's. */
static vkb_errata_views *for_device(VkDevice d, vkb_errata_dev **out)
{
    vkb_errata_dev *e = vkb_errata_cur();
    *out = e;
    if (e && e->views && e->views->device == d) return e->views;
    vkb_errata_views *v = NULL;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < MAX_DEVICES && !v; i++)
        if (reg[i] && reg[i]->device == d) v = reg[i];
    pthread_mutex_unlock(&reg_lock);
    if (v) *out = v->owner;
    return v;
}

/* ------------------------------------------------------------------ records (under v->lock) */

static img_rec *find_img(vkb_errata_views *v, VkImage h)
{
    for (img_rec *r = v->imgs[hash((uint64_t)h, IMG_BUCKETS)]; r; r = r->next)
        if (r->h == h) return r;
    return NULL;
}

static void drop_img(vkb_errata_views *v, VkImage h)
{
    for (img_rec **p = &v->imgs[hash((uint64_t)h, IMG_BUCKETS)]; *p; p = &(*p)->next)
        if ((*p)->h == h) {
            img_rec *r = *p;
            *p = r->next;
            free(r);
            v->nimgs--;
            return;
        }
}

static view_rec *find_view(vkb_errata_views *v, VkImageView h)
{
    for (view_rec *r = v->views[hash((uint64_t)h, VIEW_BUCKETS)]; r; r = r->next)
        if (r->h == h) return r;
    return NULL;
}

/* takes the record out of the map; *image: its image */
static int take_view(vkb_errata_views *v, VkImageView h, view_rec *out)
{
    for (view_rec **p = &v->views[hash((uint64_t)h, VIEW_BUCKETS)]; *p; p = &(*p)->next)
        if ((*p)->h == h) {
            view_rec *r = *p;
            *p = r->next;
            *out = *r;
            free(r);
            v->nviews--;
            return 1;
        }
    return 0;
}

/* The oldest held view destroyed (its image too, when that was waiting for it). Under v->lock:
 * the driver's destroys are quick, and the lock keeps the next create from racing the FIFO. */
static void release_oldest(vkb_errata_views *v)
{
    held_view h = v->q[v->qhead];
    v->qhead = (v->qhead + 1) % v->qcap;
    v->qn--;
    v->released++;
    v->below.vkDestroyImageView(v->device, h.view, NULL);
    img_rec *r = find_img(v, h.image);
    if (r && r->held && !--r->held && r->deferred) {
        v->deferred--;
        drop_img(v, h.image);
        v->below.vkDestroyImage(v->device, h.image, NULL);
    }
}

/* ------------------------------------------------------------------ the wrappers */

static int shared_image(const VkImageCreateInfo *ci)
{
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
        if ((s->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO &&
             ((const VkExternalMemoryImageCreateInfo *)s)->handleTypes) ||
            s->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT ||
            s->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT)
            return 1;
    return ci->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
}

static VKAPI_ATTR VkResult VKAPI_CALL v_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    vkb_errata_dev *e;
    vkb_errata_views *v = for_device(d, &e);
    if (!v) return e->next_imi.vkCreateImage(d, ci, a, out);
    VkResult r = v->below.vkCreateImage(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    img_rec *n = calloc(1, sizeof(*n));
    if (!n) return r;
    n->h = *out;
    n->type = ci->imageType;
    n->extent = ci->extent;
    n->mips = ci->mipLevels;
    n->layers = ci->arrayLayers;
    n->flags = ci->flags;
    n->shared = shared_image(ci);
    pthread_mutex_lock(&v->lock);
    drop_img(v, *out); /* a stale record of a handle the driver gave out again (none expected) */
    unsigned b = hash((uint64_t)*out, IMG_BUCKETS);
    n->next = v->imgs[b];
    v->imgs[b] = n;
    v->nimgs++;
    pthread_mutex_unlock(&v->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL v_DestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks *a)
{
    vkb_errata_dev *e;
    vkb_errata_views *v = for_device(d, &e);
    if (!v) {
        e->next_imi.vkDestroyImage(d, img, a);
        return;
    }
    if (img) {
        pthread_mutex_lock(&v->lock);
        img_rec *r = find_img(v, img);
        if (r && r->held && !r->deferred) {
            /* views of it are held back: it goes with the last of them */
            r->deferred = 1;
            v->deferred++;
            v->deferred_total++;
            while (v->deferred > v->img_cap && v->qn) release_oldest(v);
            if (v->deferred > v->deferred_max) v->deferred_max = v->deferred;
            pthread_mutex_unlock(&v->lock);
            return;
        }
        drop_img(v, img);
        pthread_mutex_unlock(&v->lock);
    }
    v->below.vkDestroyImage(d, img, a);
}

static const char *view_type_name(VkImageViewType t)
{
    static const char *n[] = {"1D", "2D", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY", "CUBE_ARRAY"};
    return (unsigned)t < 7 ? n[t] : "?";
}

static int array_view(VkImageViewType t)
{
    return t == VK_IMAGE_VIEW_TYPE_1D_ARRAY || t == VK_IMAGE_VIEW_TYPE_2D_ARRAY || t == VK_IMAGE_VIEW_TYPE_CUBE ||
           t == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
}

/* A 3D image seen as 2D or 2D array: its array layers are the slices of the view's mip level. */
static int slices_view(const img_rec *r, const VkImageViewCreateInfo *ci)
{
    return r->type == VK_IMAGE_TYPE_3D && (ci->viewType == VK_IMAGE_VIEW_TYPE_2D || ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
}

/* view_counts: VK_REMAINING_* in ci resolved into *out (1: changed) */
static int resolve_counts(vkb_errata_views *v, const VkImageViewCreateInfo *ci, VkImageViewCreateInfo *out)
{
    const VkImageSubresourceRange *s = &ci->subresourceRange;
    if (s->layerCount != VK_REMAINING_ARRAY_LAYERS && s->levelCount != VK_REMAINING_MIP_LEVELS) return 0;
    pthread_mutex_lock(&v->lock);
    img_rec *r = find_img(v, ci->image);
    img_rec c = r ? *r : (img_rec){0};
    pthread_mutex_unlock(&v->lock);
    if (!r) {
        __atomic_add_fetch(&v->unknown_image, 1, __ATOMIC_RELAXED);
        return 0;
    }
    *out = *ci;
    VkImageSubresourceRange *o = &out->subresourceRange;
    int sl = slices_view(&c, ci);
    if (s->levelCount == VK_REMAINING_MIP_LEVELS)
        /* a 3D image's 2D (array) view has one level, the only valid count */
        o->levelCount = sl ? 1 : c.mips > s->baseMipLevel ? c.mips - s->baseMipLevel : 1;
    if (s->layerCount == VK_REMAINING_ARRAY_LAYERS) {
        uint32_t layers = c.layers;
        if (sl) {
            layers = c.extent.depth >> (s->baseMipLevel < 31 ? s->baseMipLevel : 31);
            if (!layers) layers = 1;
        }
        /* a view type without layers has one, the only valid count */
        o->layerCount = !array_view(ci->viewType) && !(sl && ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY) ? 1
                        : layers > s->baseArrayLayer ? layers - s->baseArrayLayer : 1;
    }
    __atomic_add_fetch(&v->resolved, 1, __ATOMIC_RELAXED);
    if (sl) __atomic_add_fetch(&v->resolved_3d, 1, __ATOMIC_RELAXED);
    if (sl && !__atomic_exchange_n(&v->logged_resolve, 1, __ATOMIC_RELAXED))
        VKB_INFO("errata: view_counts: a %s view of a 3D image (%ux%ux%u, %u mips) with base layer %u, VK_REMAINING_ARRAY_LAYERS: "
                 "%u layer(s) given explicitly (the first such view)",
                 view_type_name(ci->viewType), c.extent.width, c.extent.height, c.extent.depth, c.mips, s->baseArrayLayer, o->layerCount);
    return 1;
}

/* the imitated MK-07: a 3D image's slices view with VK_REMAINING_ARRAY_LAYERS starts at slice 0 */
static int imitate_counts(vkb_errata_views *v, const VkImageViewCreateInfo *ci, VkImageViewCreateInfo *out)
{
    const VkImageSubresourceRange *s = &ci->subresourceRange;
    if (s->layerCount != VK_REMAINING_ARRAY_LAYERS || !s->baseArrayLayer) return 0;
    pthread_mutex_lock(&v->lock);
    img_rec *r = find_img(v, ci->image);
    int sl = r && slices_view(r, ci);
    pthread_mutex_unlock(&v->lock);
    if (!sl) return 0;
    *out = *ci;
    out->subresourceRange.baseArrayLayer = 0;
    __atomic_add_fetch(&v->imi_counts, 1, __ATOMIC_RELAXED);
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL v_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                        VkImageView *out)
{
    vkb_errata_dev *e;
    vkb_errata_views *v = for_device(d, &e);
    if (!v) return e->next_imi.vkCreateImageView(d, ci, a, out);
    VkImageViewCreateInfo fixed, bug;
    const VkImageViewCreateInfo *use = ci;
    if (ON(e, VKB_ERRATUM_VIEW_COUNTS) && resolve_counts(v, use, &fixed)) use = &fixed;
    if (IMI(e, VKB_ERRATUM_VIEW_COUNTS) && imitate_counts(v, use, &bug)) use = &bug;
    VkResult r = v->below.vkCreateImageView(d, use, a, out);
    if (r != VK_SUCCESS || !(ON(e, VKB_ERRATUM_VIEW_QUARANTINE) || IMI(e, VKB_ERRATUM_VIEW_QUARANTINE))) return r;
    view_rec *n = calloc(1, sizeof(*n));
    if (!n) return r;
    n->h = *out;
    n->image = ci->image;
    pthread_mutex_lock(&v->lock);
    view_rec old;
    take_view(v, *out, &old); /* a stale record of a handle given out again (none expected) */
    unsigned b = hash((uint64_t)*out, VIEW_BUCKETS);
    n->next = v->views[b];
    v->views[b] = n;
    v->nviews++;
    pthread_mutex_unlock(&v->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL v_DestroyImageView(VkDevice d, VkImageView view, const VkAllocationCallbacks *a)
{
    vkb_errata_dev *e;
    vkb_errata_views *v = for_device(d, &e);
    if (!v) {
        e->next_imi.vkDestroyImageView(d, view, a);
        return;
    }
    if (view) {
        pthread_mutex_lock(&v->lock);
        view_rec vr;
        if (take_view(v, view, &vr) && vr.rendered && ON(e, VKB_ERRATUM_VIEW_QUARANTINE)) {
            img_rec *r = find_img(v, vr.image);
            if (r && !r->shared && !r->deferred && v->qcap) {
                if (v->qn == v->qcap) release_oldest(v);
                v->q[(v->qhead + v->qn) % v->qcap] = (held_view){view, vr.image};
                v->qn++;
                r->held++;
                v->held_total++;
                if (v->qn > v->held_max) v->held_max = v->qn;
                int first = !v->logged_hold;
                v->logged_hold = 1;
                pthread_mutex_unlock(&v->lock);
                if (first)
                    VKB_INFO("errata: view_quarantine: views used by vkCmdBeginRendering are destroyed late (%u held at most, "
                             "and their images up to %u)", v->qcap, v->img_cap);
                return;
            }
        }
        pthread_mutex_unlock(&v->lock);
    }
    v->below.vkDestroyImageView(d, view, a);
}

/* the imitated MK-06: a colour attachment whose view handle value the cache has seen on another
 * image is not written (1: *att changed) */
static int imitate_stale(vkb_errata_views *v, VkRenderingAttachmentInfo *att)
{
    if (!att->imageView) return 0;
    view_rec *vr = find_view(v, att->imageView);
    if (!vr) return 0;
    for (uint32_t i = 0; i < STALE_ENTRIES; i++)
        if (v->stale[i].view == att->imageView) {
            if (v->stale[i].image == vr->image) return 0;
            v->stale[i].image = vr->image;
            v->imi_stale++;
            att->imageView = VK_NULL_HANDLE;
            att->resolveImageView = VK_NULL_HANDLE;
            att->resolveMode = VK_RESOLVE_MODE_NONE;
            return 1;
        }
    v->stale[v->stale_next].view = att->imageView;
    v->stale[v->stale_next].image = vr->image;
    v->stale_next = (v->stale_next + 1) % STALE_ENTRIES;
    return 0;
}

static void mark(vkb_errata_views *v, VkImageView h)
{
    view_rec *r = h ? find_view(v, h) : NULL;
    if (r) r->rendered = 1;
}

static VKAPI_ATTR void VKAPI_CALL v_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *info)
{
    vkb_errata_dev *e = vkb_errata_cur();
    vkb_errata_views *v = e ? e->views : NULL;
    if (!v) {
        e->next_imi.vkCmdBeginRendering(cb, info);
        return;
    }
    int fix = ON(e, VKB_ERRATUM_VIEW_QUARANTINE), imi = IMI(e, VKB_ERRATUM_VIEW_QUARANTINE);
    VkRenderingInfo ri;
    VkRenderingAttachmentInfo local[16], *atts = NULL;
    const VkRenderingInfo *use = info;
    if (fix || imi) {
        pthread_mutex_lock(&v->lock);
        if (fix) {
            for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
                mark(v, info->pColorAttachments[i].imageView);
                mark(v, info->pColorAttachments[i].resolveImageView);
            }
            if (info->pDepthAttachment) mark(v, info->pDepthAttachment->imageView), mark(v, info->pDepthAttachment->resolveImageView);
            if (info->pStencilAttachment)
                mark(v, info->pStencilAttachment->imageView), mark(v, info->pStencilAttachment->resolveImageView);
            for (const VkBaseInStructure *s = info->pNext; s; s = s->pNext)
                if (s->sType == VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR)
                    mark(v, ((const VkRenderingFragmentShadingRateAttachmentInfoKHR *)s)->imageView);
                else if (s->sType == VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT)
                    mark(v, ((const VkRenderingFragmentDensityMapAttachmentInfoEXT *)s)->imageView);
        }
        if (imi && info->colorAttachmentCount) {
            uint32_t n = info->colorAttachmentCount;
            atts = n <= 16 ? local : malloc(n * sizeof(*atts));
            if (atts) {
                memcpy(atts, info->pColorAttachments, n * sizeof(*atts));
                int changed = 0;
                for (uint32_t i = 0; i < n; i++) changed |= imitate_stale(v, &atts[i]);
                if (changed) {
                    ri = *info;
                    ri.pColorAttachments = atts;
                    use = &ri;
                    if (!v->logged_imi && !e->probe) {
                        v->logged_imi = 1;
                        VKB_WARN("errata: view_quarantine (imitated): a view handle value came back on another image; its "
                                 "attachment is left unwritten");
                    }
                }
            }
        }
        pthread_mutex_unlock(&v->lock);
    }
    v->below.vkCmdBeginRendering(cb, use);
    if (atts && atts != local) free(atts);
}

/* ------------------------------------------------------------------ install, destroy */

void vkb_errata_views_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (e->views) return; /* once per device: view_quarantine and view_counts, fix or imitation */
    vkb_errata_views *v = calloc(1, sizeof(*v));
    if (!v) return;
    pthread_mutex_init(&v->lock, NULL);
    v->device = t->device;
    v->owner = e;
    v->below = t->real;
    v->qcap = QUARANTINE_DEFAULT;
    const char *q = vkb_srv_env("VKBRIDGE_VIEW_QUARANTINE");
    if (q && *q) {
        long n = strtol(q, NULL, 0);
        v->qcap = n < 0 ? 0 : n > 65536 ? 65536 : (uint32_t)n;
        VKB_INFO("errata: view_quarantine holds %u views (VKBRIDGE_VIEW_QUARANTINE)", v->qcap);
    }
    v->img_cap = v->qcap / 4 ? v->qcap / 4 : 1;
    if (v->qcap && !(v->q = calloc(v->qcap, sizeof(*v->q)))) v->qcap = 0;
    e->views = v;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < MAX_DEVICES; i++)
        if (!reg[i]) {
            reg[i] = v;
            break;
        }
    pthread_mutex_unlock(&reg_lock);
    ERRATA_HOOK(t, vkCreateImage, v_CreateImage);
    ERRATA_HOOK(t, vkDestroyImage, v_DestroyImage);
    ERRATA_HOOK(t, vkCreateImageView, v_CreateImageView);
    ERRATA_HOOK(t, vkDestroyImageView, v_DestroyImageView);
    ERRATA_HOOK(t, vkCmdBeginRendering, v_CmdBeginRendering);
}

void vkb_errata_views_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_views *v = e->views;
    if (!v) return;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < MAX_DEVICES; i++)
        if (reg[i] == v) reg[i] = NULL;
    pthread_mutex_unlock(&reg_lock);
    pthread_mutex_lock(&v->lock);
    uint32_t held = v->qn, deferred = v->deferred;
    while (v->qn) release_oldest(v);
    for (unsigned b = 0; b < IMG_BUCKETS; b++)
        for (img_rec *r = v->imgs[b], *n; r; r = n) {
            n = r->next;
            if (r->deferred) v->below.vkDestroyImage(v->device, r->h, NULL);
            free(r);
        }
    for (unsigned b = 0; b < VIEW_BUCKETS; b++)
        for (view_rec *r = v->views[b], *n; r; r = n) {
            n = r->next;
            free(r);
        }
    pthread_mutex_unlock(&v->lock);
    if (!e->probe && (ON(e, VKB_ERRATUM_VIEW_COUNTS) || IMI(e, VKB_ERRATUM_VIEW_COUNTS)) && (v->resolved || v->unknown_image || v->imi_counts))
        VKB_INFO("errata: view_counts: %llu view(s) given explicit counts (%llu of 3D images as 2D/2D array), %llu of an image "
                 "not seen; %llu imitated",
                 (unsigned long long)v->resolved, (unsigned long long)v->resolved_3d, (unsigned long long)v->unknown_image,
                 (unsigned long long)v->imi_counts);
    if (!e->probe && (ON(e, VKB_ERRATUM_VIEW_QUARANTINE) || IMI(e, VKB_ERRATUM_VIEW_QUARANTINE)))
        VKB_INFO("errata: view_quarantine: %llu view(s) held back, %llu destroyed late, at most %llu held (of %u); %llu image(s) "
                 "deferred, at most %llu at once (of %u); %u view(s) and %u image(s) released at vkDestroyDevice; %llu stale "
                 "hit(s) imitated",
                 (unsigned long long)v->held_total, (unsigned long long)v->released - held, (unsigned long long)v->held_max, v->qcap,
                 (unsigned long long)v->deferred_total, (unsigned long long)v->deferred_max, v->img_cap, held, deferred,
                 (unsigned long long)v->imi_stale);
    pthread_mutex_destroy(&v->lock);
    free(v->q);
    free(v);
    e->views = NULL;
}
