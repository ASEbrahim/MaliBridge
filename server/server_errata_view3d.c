/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_view3d_base (PV-04, PowerVR DXT 25.1; CXTP 25.3 is right): a 2D-array view of a 3D image
 * (2D_ARRAY_COMPATIBLE) with baseArrayLayer >= 1, used as a framebuffer attachment of several layers and cleared
 * by the render pass (loadOp CLEAR), writes slices 0..n-1 instead of base..base+n-1 - with
 * VK_REMAINING_ARRAY_LAYERS and with an explicit count alike, so view_counts (MK-07, server_errata_views.c) does
 * not fix it. The startup probe (view3d_layers, server_probe.c) tells the pattern and, in the same run, which
 * forms of the fix work on the driver; the fix goes by those (the probe chooses; the most general proven form wins):
 *
 *   one-slice views (count 1): a one-slice 2D-array view that keeps its base needs nothing; if only a 2D view of
 *     the slice keeps it (or the other way round), the attachment is that view of the same slice ("alias": no
 *     copies, exact); if neither keeps it, the copy form below.
 *   every other view (count >= 2), and one-slice views where no alias works - the copy form: the attachment is a
 *     2D-array image of the bridge's own (the "shadow": the view's format, its mip level's size, one layer per
 *     slice - plain 2D-array images the driver gets right whatever it does with bases). Before the pass the
 *     slices the pass covers (the render area) are copied into it when the pass loads them (loadOp LOAD, or
 *     none), after the pass they are copied back when the pass stores them (storeOp STORE, or the attachment is
 *     the target of a resolve); the 3D image's layouts are what the app expects before and after
 *     (vkb_pv3d_begin / vkb_pv3d_end, which the probe's own sub-cases call too: the form proven on the driver is
 *     the form the fix records). Draws, layered draws (gl_Layer), vkCmdClearAttachments with any layer range and
 *     the loadOp CLEAR of the pass itself all land in the shadow, so they are covered as they are.
 *
 * Covered: render passes (vkCreateFramebuffer, vkCmdBeginRenderPass and 2, imageless framebuffers through
 * VkRenderPassAttachmentBeginInfo, the framebuffer secondary command buffers inherit), dynamic rendering
 * (vkCmdBeginRendering with colour and resolve attachments; a suspended pass copies out at its last part, a
 * resumed one copies nothing in), loadOp CLEAR / LOAD / DONT_CARE / NONE, storeOp STORE / DONT_CARE / NONE.
 *
 * Layered draws: the slice a draw lands in is chosen by the shader (gl_Layer / the Layer built-in, from a vertex,
 * tessellation or geometry shader) or by multiview's view mask; both index the shadow's layers as they would have
 * indexed the view's, so the copy back puts them in base + layer. Only the layers the pass renders are copied: the
 * framebuffer's layers (vkCmdBeginRendering: layerCount), or with multiview up to the highest view of the mask - a
 * view mask with holes copies the whole range in first, so the slices of the holes come back as they were.
 *
 * Reads of the 3D image: after a pass the slices are in the 3D image (the copy back), so sampling, storage reads,
 * copies and later passes see them. Inside a pass Vulkan lets shaders read what the pass rendered only where the
 * attachment is in a layout made for it - GENERAL, ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT or RENDERING_LOCAL_READ
 * (a feedback loop after a barrier inside the pass, or a later subpass sampling what an earlier one rendered).
 * Input attachments cannot name these views at all (a 2D or 2D-array view of a 3D image is not allowed in a
 * descriptor of that type: VUID-VkDescriptorImageInfo-imageView-06712 / -descriptorType-06713). A pass with a
 * copied attachment in such a layout is split where a read can follow a write: at each pipeline barrier recorded
 * inside it and at each next subpass, the pass is ended (every attachment stored: the render pass is a variant
 * that stores all, vkCmdBeginRendering's store ops become STORE), the shadow is copied into the slices, and the
 * pass begins again where it was (a variant that loads all, the same framebuffer, the subpasses before passed
 * through empty; dynamic rendering's attachment locations and input indices set again), so the read sees the 3D
 * image as rendered so far. In such passes the 3D image itself follows the layouts the app's subpasses give the
 * attachment, which its descriptors name. Suspending and resuming such a dynamic rendering pass becomes ending
 * and loading. Not split, with a warning (Vulkan offers no way to end a pass there): a barrier recorded inside a
 * secondary command buffer, and a split point while a query or transform feedback is active in the pass.
 *
 * Not covered, plainly (nothing else is left out):
 *   - the bug's reach beyond attachments (sampling or storage through 2D views of a slice, VK_EXT_image_2d_view_of_3d)
 *     is not probed, so it is not fixed; MaliBridge check's pv_view3d_draws asks the device;
 *   - vkCmdBeginRendering from a secondary command buffer that suspends or resumes across buffers is handled as
 *     the primary form is (its begin and end are paired by the command buffer they are recorded in);
 *   - an imageless framebuffer's attachment info carries the 3D image's create flags (2D_ARRAY_COMPATIBLE); the
 *     bridge's image has none (a 2D-array image cannot) - drivers that compare them would object;
 *   - two command buffers that use one redirected view in flight on different queues at once share its copy;
 *   - views of mip levels are copied by mip; a pass with more than 16 redirected attachments redirects 16.
 *
 * Two more reasons for the copy form, both for the views as attachments of certain passes only (any base layer, any
 * number of slices; nothing else is copied for them):
 *   - feedback loops (PowerVR CXTP 25.3, Pixel 11): a pass whose colour attachment is in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL
 *     or RENDERING_LOCAL_READ layout, with the 2D-array view of a 3D image as that attachment, reads the 3D image
 *     wrong inside the pass (right slices, wrong values), while the same loop into a plain 2D-array image is right
 *     (the probe's sub-cases 5 and 7) and the copy form, split at the barrier, is right too (sub-case 6): on
 *     (VKB_PROBE_FLAG_PV_V3D_FB, _FBCOPY) the copy form is taken for those passes. A later subpass sampling what an earlier
 *     one drew (GENERAL) is right on the driver and is not copied for this reason.
 *   - multiview (PowerVR, any version): a view mask with such a view as attachment faults in the driver, the
 *     startup probe cannot tell it (a fault ends the server), so on PowerVR's driver every render pass and dynamic
 *     rendering with a non-zero view mask that renders into a 2D-array view of a 3D image takes the copy form,
 *     whatever the probe says (the multiview case into a plain 2D-array image, and the copy form, are right on both).
 *
 * A second erratum lives here, sharing the splits (pv_fbloop_split, PV-06, PowerVR CXTP 25.3, Pixel 11): a feedback loop
 * (a colour or depth/stencil attachment in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL or RENDERING_LOCAL_READ layout, a barrier inside the
 * pass, then draws reading the attachment at their own texel) reads wrong values in any image, a plain 2D-array image
 * as much as a 3D image's view, while the pass ended at the barrier and begun again with loadOp LOAD (no copy) is right
 * (the probe's sub-cases 7 and 8). The fix: every render pass and dynamic rendering instance with an attachment in such a
 * layout is split at each pipeline barrier recorded inside it that has the feedback-loop or by-region dependency flag -
 * the split above with nothing to copy (the render pass variants that store / load every attachment, dynamic rendering's
 * stores made STORE and its begin kept; attachment locations and input indices set again; a global memory barrier between the
 * parts). The same limits: a barrier inside a secondary command buffer of such a pass (dynamic rendering's inheritance
 * does not carry the layouts, so only render passes are seen) and a split point under an active query or transform
 * feedback are not split - a one-time warning and a counter. Where a 3D-image view is copied for the same pass (PV-04) the
 * pass is split once, copies included. Imitation (VKBRIDGE_ERRATA_IMITATE=pv_fbloop_split, host): a plain 2D / 2D-array
 * image with the feedback-loop usage, as a colour attachment of a pass with an attachment in a feedback-loop layout, is
 * rendered through the same hidden 2D-array image, copied into the image where a pass ends (so the loop reads the image
 * as it was when the pass began or was last ended: the stale reads of the driver); the fix's splits end the pass, so
 * they flush the hidden image and the loop reads right.
 *
 * Imitation (host): a 3D image's 2D-array view with base layer >= 1 and two or more layers starts at slice 0
 * (explicit counts too) - the PowerVR form. VKBRIDGE_PV3D_IMITATE=array also breaks one-slice 2D-array views,
 * =all 2D views of a slice as well (the probe then reads those sub-cases wrong, and the fix goes to the copy
 * form). The imitation breaks the view wherever it is used, not only as an attachment.
 * VKBRIDGE_PV3D_IMITATE=fb is CXTP's other form: the base layer is right, and the feedback loops of passes rendering
 * into such views read what the 3D image held before the pass (the writes land in a hidden 2D-array image that is copied
 * into the slices at the end of the pass, which is what the fix's copy form does without its splits).
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define BK 1024
#define MAXA 16 /* redirected attachments per pass */
#define MAX_DEVICES 64

#define ON(e) ((e)->on >> VKB_ERRATUM_PV_VIEW3D_BASE & 1)
#define IMI(e) ((e)->imitated >> VKB_ERRATUM_PV_VIEW3D_BASE & 1)
#define ADD(x, n) __atomic_add_fetch(&(x), (n), __ATOMIC_RELAXED)

enum { ACT_NONE, ACT_ALIAS, ACT_SHADOW };
enum { IM_COUNT, IM_ARRAY, IM_ALL, IM_FB };
enum { PC_FB = 1, PC_MV = 2 }; /* what a pass is, for the views that are copied for that reason only */

/* ------------------------------------------------------------------ a small map: handle -> record */

typedef struct node {
    uint64_t k;
    void *p;
    struct node *next;
} node;

typedef struct {
    node *b[BK];
} map;

static unsigned hh(uint64_t k)
{
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    k ^= k >> 29;
    return (unsigned)k & (BK - 1);
}

static void *mget(map *m, uint64_t k)
{
    for (node *n = m->b[hh(k)]; n; n = n->next)
        if (n->k == k) return n->p;
    return NULL;
}

/* puts p under k; the record it replaced, or NULL */
static void *mput(map *m, uint64_t k, void *p)
{
    unsigned h = hh(k);
    for (node *n = m->b[h]; n; n = n->next)
        if (n->k == k) {
            void *old = n->p;
            n->p = p;
            return old;
        }
    node *n = malloc(sizeof(*n));
    if (!n) return NULL;
    *n = (node){k, p, m->b[h]};
    m->b[h] = n;
    return NULL;
}

static void *mdel(map *m, uint64_t k)
{
    for (node **q = &m->b[hh(k)]; *q; q = &(*q)->next)
        if ((*q)->k == k) {
            node *n = *q;
            void *p = n->p;
            *q = n->next;
            free(n);
            return p;
        }
    return NULL;
}

/* ------------------------------------------------------------------ records */

typedef struct {
    VkImageType type;
    VkFormat format;
    VkExtent3D extent;
    uint32_t mips, layers;
    VkImageUsageFlags usage;
} img_rec;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
} shadow;

typedef struct {
    VkImage image;
    VkImageViewType type;
    VkFormat format;
    uint32_t mip, base, count, w, h; /* the slices it covers, its mip level's size */
    VkImageUsageFlags usage;
    int action;
    VkImageView alias;
    shadow sh;
    int cond;   /* PC_*: the passes that take this view through a shadow whatever its base layer (feedback loop, multiview) */
    int arr;    /* a plain 2D / 2D-array image (pv_fbloop_split's imitation), not a 3D image */
    int failed; /* 1 << ACT_*: the alias / shadow could not be made: left as the app made it */
} view_rec;

typedef struct {
    VkRenderPass rp;
    uint32_t w, h, layers, n;
    VkFramebufferCreateFlags flags;
    VkImageView *views; /* NULL: imageless */
    VkFramebuffer alt;  /* with the redirected views, made at first use */
    int alt_tried;
} fb_rec;

typedef struct {
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;
    VkImageLayout init, fin;
    uint8_t resolve; /* the target of a resolve */
} rp_att;

typedef struct {
    uint32_t n, nsub, vmask; /* attachments, subpasses, the subpasses' view masks together (multiview) */
    rp_att *att;
    VkImageLayout *lay;           /* [s * n + a]: attachment a's layout in subpass s as a colour or resolve attachment (UNDEFINED: not one there) */
    int feedback;                 /* some colour or resolve attachment is in a layout shaders may read while it is rendered */
    int fbl;                      /* ... in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL or RENDERING_LOCAL_READ (a loop within the subpass: PC_FB) */
    int fbl_any;                  /* ... or a colour, resolve or depth/stencil attachment is (pv_fbloop_split's passes) */
    VkRenderPass split1, split2;  /* feedback: the same pass storing every attachment, and loading every one (initial = final layout) */
} rp_rec;

typedef struct {
    vkb_pv3d_pair p;
    int copy_in, copy_out;
    uint32_t att;       /* render pass: the attachment's index */
    VkImageLayout cur3; /* the 3D image's layout now (a split pass: the one the app's subpass gives the attachment) */
} pass_item;

#define MAXLOC 32

typedef struct {
    int n, suspending;
    int dyn;                  /* vkCmdBeginRendering */
    int feedback;             /* split where a read can follow a write */
    int subpass, nsub;        /* render pass: where it is */
    VkSubpassContents contents; /* the current subpass's */
    int active;               /* queries and transform feedback active inside it (no split then) */
    uint32_t natt;
    VkImageLayout *lay;       /* feedback render pass: rp_rec.lay's copy */
    uint8_t *blob;            /* feedback: the next part's begin info, encoded */
    size_t len;
    /* feedback dynamic rendering: attachment locations and input indices set inside it, set again after a split */
    int has_loc, has_idx, loc_null, idx_null, has_d, has_s;
    uint32_t nloc, nidx, d_idx, s_idx;
    uint32_t loc[MAXLOC], idx[MAXLOC];
    pass_item it[MAXA];
} pass_rec;

static void pr_free(pass_rec *pr)
{
    if (!pr) return;
    free(pr->lay);
    free(pr->blob);
    free(pr);
}

typedef struct vkb_errata_pv3d {
    pthread_mutex_t lock;
    VkDevice device;
    vkb_errata_dev *owner;
    vkb_dispatch below;
    vkb_pv3d_fns fns;
    VkPhysicalDeviceMemoryProperties mp;
    map imgs, views, fbs, rps, passes, fsec;
    struct {
        int array1, two_d, copy;
        int fb; /* feedback loops in 3D-image views are copied (the probe's sub-cases 5-7), */
        int mv; /* multiview into them too (PowerVR's driver) */
    } pol;
    int imode;
    int nosplit; /* VKBRIDGE_PV3D_NOSPLIT=1 (tests): never split, to show what the splits are for */
    uint64_t fbs_rp, fbs_dyn, fbs_splits, fbs_unsplit; /* pv_fbloop_split: the passes it covers, its splits */
    int logged_fbs, logged_fbs_unsplit;
    uint32_t nbad;    /* views with an action */
    int any_feedback; /* a pass or secondary command buffer that may need splitting was seen (barriers look then) */
    uint64_t imi_views, alias_views, shadow_views, shadow_bytes_est, rp_passes, dyn_passes, copies_in, copies_out, alias_uses, failures;
    uint64_t splits, unsplit, mv_passes, fb_passes;
    int logged_alias, logged_shadow, logged_imi, logged_fail, logged_split, logged_unsplit, logged_mv, logged_fb;
} pv3d;

/* The imitation of CXTP's feedback loops acts where no fix covers them (the fix with its probe's feedback policy does) */
static int mim_fb(const pv3d *v)
{
    return IMI(v->owner) && v->imode == IM_FB && !(ON(v->owner) && v->pol.fb);
}

/* pv_fbloop_split (PV-06): the fix is on / its imitation is */
static int fbs_on(const pv3d *v)
{
    return v->owner->on >> VKB_ERRATUM_PV_FBLOOP_SPLIT & 1;
}

static int fbs_imi(const pv3d *v)
{
    return v->owner->imitated >> VKB_ERRATUM_PV_FBLOOP_SPLIT & 1;
}

/* The fix's code runs: the fix is on, or the feedback-loop imitation is (the same redirection without its splits), or
 * pv_fbloop_split is (or its imitation) */
static int act(const pv3d *v)
{
    return ON(v->owner) || mim_fb(v) || fbs_on(v) || fbs_imi(v);
}

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static pv3d *reg[MAX_DEVICES];

/* The state for a call on device d and, in *out, d's errata (as server_errata_views.c finds it) */
static pv3d *for_device(VkDevice d, vkb_errata_dev **out)
{
    vkb_errata_dev *e = vkb_errata_cur();
    *out = e;
    if (e && e->pv3d && e->pv3d->device == d) return e->pv3d;
    pv3d *v = NULL;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < MAX_DEVICES && !v; i++)
        if (reg[i] && reg[i]->device == d) v = reg[i];
    pthread_mutex_unlock(&reg_lock);
    if (v) *out = v->owner;
    return v;
}

/* ------------------------------------------------------------------ the copies (also the probe's; pv3d_copy.c) */

void vkb_pv3d_begin(const vkb_dispatch *d, VkCommandBuffer cb, const vkb_pv3d_pair *p, int copy_in)
{
    vkb_pv3d_fns f = {d->vkCmdPipelineBarrier, d->vkCmdCopyImage};
    vkb_pv3d_copy_begin(&f, cb, p, copy_in);
}

void vkb_pv3d_end(const vkb_dispatch *d, VkCommandBuffer cb, const vkb_pv3d_pair *p, int copy_out)
{
    vkb_pv3d_fns f = {d->vkCmdPipelineBarrier, d->vkCmdCopyImage};
    vkb_pv3d_copy_end(&f, cb, p, copy_out);
}

/* the layouts in which Vulkan lets shaders read an attachment while the pass renders it */
static int readable(VkImageLayout l)
{
    return l == VK_IMAGE_LAYOUT_GENERAL || l == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT || l == VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
}

/* ... of those, the ones of a loop within a subpass: what a pass reading 3D-image slices it renders is wrong in on CXTP
 * (a later subpass sampling, GENERAL, is right there) */
static int fbl_layout(VkImageLayout l)
{
    return l == VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT || l == VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
}

/* the layers a view mask renders: its highest view + 1 */
static uint32_t mask_layers(uint32_t m)
{
    return m ? 32 - (uint32_t)__builtin_clz(m) : 0;
}

static int mask_holes(uint32_t m)
{
    uint32_t n = mask_layers(m);
    return m && m != (n >= 32 ? 0xffffffffu : (1u << n) - 1);
}

/* ------------------------------------------------------------------ views */

/* the slices a view of a 3D image covers */
static uint32_t slices_of(const img_rec *im, const VkImageViewCreateInfo *ci)
{
    const VkImageSubresourceRange *s = &ci->subresourceRange;
    uint32_t depth = im->type == VK_IMAGE_TYPE_3D ? im->extent.depth >> (s->baseMipLevel < 31 ? s->baseMipLevel : 31) : im->layers;
    if (!depth) depth = 1;
    if (ci->viewType == VK_IMAGE_VIEW_TYPE_2D) return 1;
    if (s->layerCount == VK_REMAINING_ARRAY_LAYERS) return depth > s->baseArrayLayer ? depth - s->baseArrayLayer : 1;
    return s->layerCount;
}

static int slices_view(const img_rec *im, const VkImageViewCreateInfo *ci)
{
    return im->type == VK_IMAGE_TYPE_3D && (ci->viewType == VK_IMAGE_VIEW_TYPE_2D || ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
}

/* pv_fbloop_split's imitation: a view of a plain image (recorded only with the imitation) */
static int plain_view(const img_rec *im, const VkImageViewCreateInfo *ci)
{
    return im->type == VK_IMAGE_TYPE_2D && (ci->viewType == VK_IMAGE_VIEW_TYPE_2D || ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
}

/* what the fix does for a view the app made (pol: what the probe proved) */
static int view_action(const pv3d *v, const img_rec *im, const VkImageViewCreateInfo *ci, uint32_t n)
{
    if (!slices_view(im, ci) || !ci->subresourceRange.baseArrayLayer) return ACT_NONE;
    if (n == 1) {
        int arr = ci->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        int same = arr ? v->pol.array1 : v->pol.two_d, other = arr ? v->pol.two_d : v->pol.array1;
        return same ? ACT_NONE : other ? ACT_ALIAS : v->pol.copy ? ACT_SHADOW : ACT_NONE;
    }
    return v->pol.copy ? ACT_SHADOW : ACT_NONE;
}

/* the passes that take this view through its shadow whatever its base layer (PC_*): feedback loops where the probe saw
 * them wrong (or the imitation is), multiview on PowerVR's driver */
static int view_cond(const pv3d *v, const img_rec *im, const VkImageViewCreateInfo *ci)
{
    if (fbs_imi(v) && plain_view(im, ci)) return PC_FB; /* the imitation's stale loops: in any pass with an attachment in a feedback-loop layout */
    if (!slices_view(im, ci)) return 0;
    return (v->pol.fb || mim_fb(v) ? PC_FB : 0) | (v->pol.mv ? PC_MV : 0);
}

/* the imitation: 1 and *out when this view starts at slice 0 */
static int imitate_view(pv3d *v, const img_rec *im, const VkImageViewCreateInfo *ci, VkImageViewCreateInfo *out)
{
    if (v->imode == IM_FB) return 0; /* CXTP's form: the base layer is right (the loops are what is imitated) */
    if (!slices_view(im, ci) || !ci->subresourceRange.baseArrayLayer) return 0;
    if (ci->viewType == VK_IMAGE_VIEW_TYPE_2D && v->imode != IM_ALL) return 0;
    if (v->imode == IM_COUNT && slices_of(im, ci) < 2) return 0;
    *out = *ci;
    out->subresourceRange.baseArrayLayer = 0;
    return 1;
}

/* the driver's vkCreateImageView with the imitation in front of it. im: the image's record (or NULL). */
static VkResult create_view(pv3d *v, const img_rec *im, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{
    VkImageViewCreateInfo bug;
    const VkImageViewCreateInfo *use = ci;
    if (IMI(v->owner) && im && imitate_view(v, im, ci, &bug)) {
        use = &bug;
        ADD(v->imi_views, 1);
        if (!v->owner->probe && !__atomic_exchange_n(&v->logged_imi, 1, __ATOMIC_RELAXED))
            VKB_WARN("errata: pv_view3d_base (imitated): a %s view of a 3D image from slice %u starts at slice 0",
                     ci->viewType == VK_IMAGE_VIEW_TYPE_2D ? "2D" : "2D_ARRAY", ci->subresourceRange.baseArrayLayer);
    }
    return v->below.vkCreateImageView(v->device, use, a, out);
}

static uint32_t mem_type(const pv3d *v, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < v->mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = v->mp.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & want) != want) continue;
        if (f & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT)) continue;
        return i;
    }
    return UINT32_MAX;
}

static void drop_shadow(pv3d *v, shadow *s)
{
    if (s->view) v->below.vkDestroyImageView(v->device, s->view, NULL);
    if (s->img) v->below.vkDestroyImage(v->device, s->img, NULL);
    if (s->mem) v->below.vkFreeMemory(v->device, s->mem, NULL);
    *s = (shadow){0};
}

/* The shadow of a view (lock held): 1 made. Usage: what the 3D image has of the attachment-ish ones, and both
 * transfer bits; the smallest set the driver takes if that is refused. */
static int make_shadow(pv3d *v, view_rec *vr)
{
    static const VkImageUsageFlags keep = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT; /* the attachment in that layout needs it */
    const VkImageUsageFlags xfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags tries[2] = {(vr->usage & keep) | xfer | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | xfer};
    shadow s = {0};
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    for (int t = 0; t < 2 && r != VK_SUCCESS; t++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, vr->format, {vr->w, vr->h, 1}, 1, vr->count,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, tries[t], VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                VK_IMAGE_LAYOUT_UNDEFINED};
        r = v->below.vkCreateImage(v->device, &ci, NULL, &s.img);
    }
    if (r != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    v->below.vkGetImageMemoryRequirements(v->device, s.img, &mr);
    uint32_t t = mem_type(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = mem_type(v, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    if (t == UINT32_MAX || v->below.vkAllocateMemory(v->device, &ai, NULL, &s.mem) != VK_SUCCESS ||
        v->below.vkBindImageMemory(v->device, s.img, s.mem, 0) != VK_SUCCESS) {
        drop_shadow(v, &s);
        return 0;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, s.img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, vr->format,
                                {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, vr->count}};
    if (v->below.vkCreateImageView(v->device, &vi, NULL, &s.view) != VK_SUCCESS) {
        drop_shadow(v, &s);
        return 0;
    }
    vr->sh = s;
    ADD(v->shadow_views, 1);
    ADD(v->shadow_bytes_est, mr.size);
    return 1;
}

/* The same slice through the other view type (lock held) */
static int make_alias(pv3d *v, view_rec *vr)
{
    int to_2d = vr->type == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, vr->image,
                                to_2d ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY, vr->format, {0, 0, 0, 0},
                                {VK_IMAGE_ASPECT_COLOR_BIT, vr->mip, 1, vr->base, 1}};
    img_rec *im = mget(&v->imgs, (uint64_t)vr->image);
    if (!im || create_view(v, im, &vi, NULL, &vr->alias) != VK_SUCCESS) {
        vr->alias = VK_NULL_HANDLE;
        return 0;
    }
    ADD(v->alias_views, 1);
    return 1;
}

typedef struct {
    VkImageView view;
    int shadow;
    view_rec snap;
} repl;

/* What replaces an app view used as an attachment of a pass of kind ctx (PC_*; lock held): 0 nothing, 1 *r, -1 it could
 * not be made. The shadow where the pass is one that takes the view through it whatever the base (cond), else the
 * view's own action (the base layer bug). */
static int get_repl(pv3d *v, VkImageView app, int ctx, repl *r)
{
    view_rec *vr = app ? mget(&v->views, (uint64_t)app) : NULL;
    if (!vr) return 0;
    int why = vr->cond & ctx;
    int action = why ? ACT_SHADOW : vr->action;
    if (action == ACT_NONE) return 0;
    if (vr->failed & 1 << action) return -1;
    if (action == ACT_ALIAS) {
        if (!vr->alias && !make_alias(v, vr)) return vr->failed |= 1 << action, -1;
        r->view = vr->alias;
        r->shadow = 0;
        if (!__atomic_exchange_n(&v->logged_alias, 1, __ATOMIC_RELAXED))
            VKB_INFO("errata: pv_view3d_base: a %s view of a 3D image from slice %u (%u slice) is rendered through a %s view of the same slice "
                     "(the first such view)", vr->type == VK_IMAGE_VIEW_TYPE_2D ? "2D" : "2D_ARRAY", vr->base, vr->count,
                     vr->type == VK_IMAGE_VIEW_TYPE_2D ? "2D_ARRAY" : "2D");
    } else {
        if (!vr->sh.view && !make_shadow(v, vr)) return vr->failed |= 1 << action, -1;
        r->view = vr->sh.view;
        r->shadow = 1;
        if (v->owner->probe || mim_fb(v) || fbs_imi(v)) { /* the probe's, or the imitation's: nothing to tell */
        } else if (why & PC_MV) {
            if (!__atomic_exchange_n(&v->logged_mv, 1, __ATOMIC_RELAXED))
                VKB_INFO("errata: pv_view3d_base: multiview (view mask) into a %s view of a 3D image (%ux%u, mip %u) from slice %u, %u slice(s), is rendered "
                         "through a 2D-array image of its own, copied in and out - multiview into such a view faults in PowerVR's driver (the first such "
                         "view; the rule for every PowerVR driver)", vr->type == VK_IMAGE_VIEW_TYPE_2D ? "2D" : "2D_ARRAY", vr->w, vr->h, vr->mip, vr->base,
                         vr->count);
        } else if (why & PC_FB) {
            if (!__atomic_exchange_n(&v->logged_fb, 1, __ATOMIC_RELAXED))
                VKB_INFO("errata: pv_view3d_base: a feedback loop into a %s view of a 3D image (%ux%u, mip %u) from slice %u, %u slice(s), is rendered "
                         "through a 2D-array image of its own, copied in and out, the pass split at its barriers (the first such view)",
                         vr->type == VK_IMAGE_VIEW_TYPE_2D ? "2D" : "2D_ARRAY", vr->w, vr->h, vr->mip, vr->base, vr->count);
        } else if (!__atomic_exchange_n(&v->logged_shadow, 1, __ATOMIC_RELAXED))
            VKB_INFO("errata: pv_view3d_base: a %s view of a 3D image (%ux%u, mip %u) from slice %u, %u slice(s), is rendered through a 2D-array image "
                     "of its own, copied in before and out after the pass (the first such view)",
                     vr->type == VK_IMAGE_VIEW_TYPE_2D ? "2D" : "2D_ARRAY", vr->w, vr->h, vr->mip, vr->base, vr->count);
    }
    r->snap = *vr;
    return 1;
}

static void note_failure(pv3d *v)
{
    ADD(v->failures, 1);
    if (!__atomic_exchange_n(&v->logged_fail, 1, __ATOMIC_RELAXED))
        VKB_WARN("errata: pv_view3d_base: a view could not be redirected (its alias or 2D-array image was refused); it is used as the app made it");
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) return vkb_errata_next(1)->vkCreateImage(d, ci, a, out);
    VkResult r = v->below.vkCreateImage(d, ci, a, out);
    int plain = fbs_imi(v) && ci->imageType == VK_IMAGE_TYPE_2D && ci->samples == VK_SAMPLE_COUNT_1_BIT &&
                (ci->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);
    if (r != VK_SUCCESS || (ci->imageType != VK_IMAGE_TYPE_3D && !plain)) return r;
    img_rec *n = calloc(1, sizeof(*n));
    if (!n) return r;
    *n = (img_rec){ci->imageType, ci->format, ci->extent, ci->mipLevels, ci->arrayLayers, ci->usage};
    pthread_mutex_lock(&v->lock);
    free(mput(&v->imgs, (uint64_t)*out, n));
    pthread_mutex_unlock(&v->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks *a)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) {
        vkb_errata_next(1)->vkDestroyImage(d, img, a);
        return;
    }
    if (img) {
        pthread_mutex_lock(&v->lock);
        free(mdel(&v->imgs, (uint64_t)img));
        pthread_mutex_unlock(&v->lock);
    }
    v->below.vkDestroyImage(d, img, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) return vkb_errata_next(1)->vkCreateImageView(d, ci, a, out);
    img_rec im = {0}, *known = NULL;
    pthread_mutex_lock(&v->lock);
    img_rec *r0 = mget(&v->imgs, (uint64_t)ci->image);
    if (r0) im = *r0, known = &im;
    pthread_mutex_unlock(&v->lock);
    VkResult r = create_view(v, known, ci, a, out);
    if (r != VK_SUCCESS || !known || !act(v)) return r;
    uint32_t n = slices_of(&im, ci);
    int ac = view_action(v, &im, ci, n), cond = view_cond(v, &im, ci);
    if (ac == ACT_NONE && !cond) return r;
    view_rec *vr = calloc(1, sizeof(*vr));
    if (!vr) return r;
    const VkImageSubresourceRange *s = &ci->subresourceRange;
    *vr = (view_rec){.image = ci->image, .type = ci->viewType, .format = ci->format, .mip = s->baseMipLevel, .base = s->baseArrayLayer,
                     .count = n, .w = im.extent.width >> (s->baseMipLevel < 31 ? s->baseMipLevel : 31),
                     .h = im.extent.height >> (s->baseMipLevel < 31 ? s->baseMipLevel : 31), .usage = im.usage, .action = ac, .cond = cond,
                     .arr = im.type != VK_IMAGE_TYPE_3D};
    if (!vr->w) vr->w = 1;
    if (!vr->h) vr->h = 1;
    pthread_mutex_lock(&v->lock);
    view_rec *old = mput(&v->views, (uint64_t)*out, vr);
    if (old) {
        drop_shadow(v, &old->sh);
        if (old->alias) v->below.vkDestroyImageView(v->device, old->alias, NULL);
        free(old);
    } else
        __atomic_add_fetch(&v->nbad, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&v->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyImageView(VkDevice d, VkImageView view, const VkAllocationCallbacks *a)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) {
        vkb_errata_next(1)->vkDestroyImageView(d, view, a);
        return;
    }
    if (view) {
        pthread_mutex_lock(&v->lock);
        view_rec *vr = mdel(&v->views, (uint64_t)view);
        if (vr) {
            __atomic_sub_fetch(&v->nbad, 1, __ATOMIC_RELAXED);
            drop_shadow(v, &vr->sh);
            if (vr->alias) v->below.vkDestroyImageView(v->device, vr->alias, NULL);
            free(vr);
        }
        pthread_mutex_unlock(&v->lock);
    }
    v->below.vkDestroyImageView(d, view, a);
}

/* ------------------------------------------------------------------ render passes and framebuffers */

/* lay: [nsub * n], taken */
static void rp_record(pv3d *v, VkRenderPass h, uint32_t n, const rp_att *src, const uint32_t *resolve, uint32_t nres, uint32_t nsub,
                      VkImageLayout *lay, uint32_t vmask, int ds_fbl, VkRenderPass s1, VkRenderPass s2)
{
    rp_rec *r = calloc(1, sizeof(*r));
    if (r) r->att = calloc(n ? n : 1, sizeof(*r->att));
    if (!r || !r->att) {
        if (r) free(r);
        free(lay);
        if (s1) v->below.vkDestroyRenderPass(v->device, s1, NULL);
        if (s2) v->below.vkDestroyRenderPass(v->device, s2, NULL);
        return;
    }
    r->n = n, r->nsub = nsub, r->vmask = vmask, r->lay = lay, r->split1 = s1, r->split2 = s2;
    memcpy(r->att, src, n * sizeof(*src));
    for (uint32_t i = 0; i < nres; i++)
        if (resolve[i] < n) r->att[resolve[i]].resolve = 1;
    for (uint32_t i = 0; lay && i < n * nsub; i++) {
        if (readable(lay[i])) r->feedback = 1;
        if (fbl_layout(lay[i])) r->fbl = 1;
    }
    r->fbl_any = r->fbl || ds_fbl;
    pthread_mutex_lock(&v->lock);
    rp_rec *old = mput(&v->rps, (uint64_t)h, r);
    pthread_mutex_unlock(&v->lock);
    if (old) {
        if (old->split1) v->below.vkDestroyRenderPass(v->device, old->split1, NULL);
        if (old->split2) v->below.vkDestroyRenderPass(v->device, old->split2, NULL);
        free(old->att), free(old->lay), free(old);
    }
}

static void lay_set(VkImageLayout *lay, uint32_t n, uint32_t s, uint32_t a, VkImageLayout l)
{
    if (lay && a < n) lay[s * n + a] = l;
}

static int lay_feedback(const VkImageLayout *lay, uint32_t n)
{
    for (uint32_t i = 0; lay && i < n; i++)
        if (readable(lay[i])) return 1;
    return 0;
}

/* a depth/stencil attachment in a feedback-loop layout (not tracked in lay: it is no copied attachment) */
static int ds_fbl_of(VkImageLayout l)
{
    return fbl_layout(l);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) return vkb_errata_next(1)->vkCreateRenderPass(d, ci, a, out);
    VkResult r = v->below.vkCreateRenderPass(d, ci, a, out);
    if (r != VK_SUCCESS || !act(v)) return r;
    uint32_t n = ci->attachmentCount, ns = ci->subpassCount, vmask = 0;
    rp_att *at = calloc(n ? n : 1, sizeof(*at));
    uint32_t *res = calloc(32, sizeof(*res));
    VkImageLayout *lay = calloc((size_t)(n ? n : 1) * (ns ? ns : 1), sizeof(*lay));
    VkAttachmentDescription *va = calloc(n ? n : 1, sizeof(*va));
    VkRenderPass s1 = VK_NULL_HANDLE, s2 = VK_NULL_HANDLE;
    uint32_t nres = 0, cap = 32;
    int ds_fbl = 0;
    if (!at || !res || !lay || !va) goto out;
    for (uint32_t i = 0; i < n; i++) {
        const VkAttachmentDescription *x = &ci->pAttachments[i];
        at[i] = (rp_att){x->loadOp, x->storeOp, x->initialLayout, x->finalLayout, 0};
    }
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO) {
            const VkRenderPassMultiviewCreateInfo *mv = (const VkRenderPassMultiviewCreateInfo *)p;
            for (uint32_t s = 0; s < mv->subpassCount; s++) vmask |= mv->pViewMasks[s];
        }
    for (uint32_t s = 0; s < ns; s++) {
        const VkSubpassDescription *sp = &ci->pSubpasses[s];
        if (sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) ds_fbl |= ds_fbl_of(sp->pDepthStencilAttachment->layout);
        for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) {
            lay_set(lay, n, s, sp->pColorAttachments[k].attachment, sp->pColorAttachments[k].layout);
            if (!sp->pResolveAttachments || sp->pResolveAttachments[k].attachment == VK_ATTACHMENT_UNUSED) continue;
            lay_set(lay, n, s, sp->pResolveAttachments[k].attachment, sp->pResolveAttachments[k].layout);
            if (nres == cap) {
                uint32_t *nr = realloc(res, (cap *= 2) * sizeof(*res));
                if (!nr) goto out;
                res = nr;
            }
            res[nres++] = sp->pResolveAttachments[k].attachment;
        }
    }
    if (lay_feedback(lay, n * ns) || ds_fbl) { /* the variants a split pass is recorded with (compatible: only ops and layouts differ) */
        VkRenderPassCreateInfo c = *ci;
        c.pAttachments = va;
        for (uint32_t i = 0; i < n; i++) {
            va[i] = ci->pAttachments[i];
            va[i].storeOp = va[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        if (v->below.vkCreateRenderPass(d, &c, NULL, &s1) != VK_SUCCESS) s1 = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < n; i++) {
            va[i].loadOp = va[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            va[i].initialLayout = va[i].finalLayout;
        }
        if (s1 && v->below.vkCreateRenderPass(d, &c, NULL, &s2) != VK_SUCCESS) s2 = VK_NULL_HANDLE;
    }
    rp_record(v, *out, n, at, res, nres, ns, lay, vmask, ds_fbl, s1, s2);
    lay = NULL;
out:
    free(at);
    free(res);
    free(lay);
    free(va);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateRenderPass2(VkDevice d, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) return vkb_errata_next(1)->vkCreateRenderPass2(d, ci, a, out);
    VkResult r = v->below.vkCreateRenderPass2(d, ci, a, out);
    if (r != VK_SUCCESS || !act(v)) return r;
    uint32_t n = ci->attachmentCount, ns = ci->subpassCount, vmask = 0;
    rp_att *at = calloc(n ? n : 1, sizeof(*at));
    uint32_t *res = calloc(32, sizeof(*res));
    VkImageLayout *lay = calloc((size_t)(n ? n : 1) * (ns ? ns : 1), sizeof(*lay));
    VkAttachmentDescription2 *va = calloc(n ? n : 1, sizeof(*va));
    VkAttachmentDescriptionStencilLayout *sl = calloc(n ? n : 1, sizeof(*sl));
    VkRenderPass s1 = VK_NULL_HANDLE, s2 = VK_NULL_HANDLE;
    uint32_t nres = 0, cap = 32;
    int ds_fbl = 0;
    if (!at || !res || !lay || !va || !sl) goto out;
    for (uint32_t i = 0; i < n; i++) {
        const VkAttachmentDescription2 *x = &ci->pAttachments[i];
        at[i] = (rp_att){x->loadOp, x->storeOp, x->initialLayout, x->finalLayout, 0};
    }
    for (uint32_t s = 0; s < ns; s++) {
        const VkSubpassDescription2 *sp = &ci->pSubpasses[s];
        vmask |= sp->viewMask;
        if (sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) ds_fbl |= ds_fbl_of(sp->pDepthStencilAttachment->layout);
        for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) {
            lay_set(lay, n, s, sp->pColorAttachments[k].attachment, sp->pColorAttachments[k].layout);
            if (!sp->pResolveAttachments || sp->pResolveAttachments[k].attachment == VK_ATTACHMENT_UNUSED) continue;
            lay_set(lay, n, s, sp->pResolveAttachments[k].attachment, sp->pResolveAttachments[k].layout);
            if (nres == cap) {
                uint32_t *nr = realloc(res, (cap *= 2) * sizeof(*res));
                if (!nr) goto out;
                res = nr;
            }
            res[nres++] = sp->pResolveAttachments[k].attachment;
        }
    }
    if (lay_feedback(lay, n * ns) || ds_fbl) {
        VkRenderPassCreateInfo2 c = *ci;
        c.pAttachments = va;
        for (uint32_t i = 0; i < n; i++) {
            va[i] = ci->pAttachments[i];
            va[i].storeOp = va[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            /* a separate stencil layout: its own copy, so the second variant can start it where it ends */
            const VkBaseInStructure *p = va[i].pNext;
            for (; p && p->sType != VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT; p = p->pNext) {}
            if (p) {
                sl[i] = *(const VkAttachmentDescriptionStencilLayout *)p;
                sl[i].pNext = NULL;
                va[i].pNext = &sl[i]; /* (any other structure chained there is left out of the variants) */
            }
        }
        if (v->below.vkCreateRenderPass2(d, &c, NULL, &s1) != VK_SUCCESS) s1 = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < n; i++) {
            va[i].loadOp = va[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            va[i].initialLayout = va[i].finalLayout;
            sl[i].stencilInitialLayout = sl[i].stencilFinalLayout;
        }
        if (s1 && v->below.vkCreateRenderPass2(d, &c, NULL, &s2) != VK_SUCCESS) s2 = VK_NULL_HANDLE;
    }
    rp_record(v, *out, n, at, res, nres, ns, lay, vmask, ds_fbl, s1, s2);
    lay = NULL;
out:
    free(at);
    free(res);
    free(lay);
    free(va);
    free(sl);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyRenderPass(VkDevice d, VkRenderPass rp, const VkAllocationCallbacks *a)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) {
        vkb_errata_next(1)->vkDestroyRenderPass(d, rp, a);
        return;
    }
    if (rp) {
        pthread_mutex_lock(&v->lock);
        rp_rec *r = mdel(&v->rps, (uint64_t)rp);
        pthread_mutex_unlock(&v->lock);
        if (r) {
            if (r->split1) v->below.vkDestroyRenderPass(d, r->split1, NULL);
            if (r->split2) v->below.vkDestroyRenderPass(d, r->split2, NULL);
            free(r->att), free(r->lay), free(r);
        }
    }
    v->below.vkDestroyRenderPass(d, rp, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) return vkb_errata_next(1)->vkCreateFramebuffer(d, ci, a, out);
    VkResult r = v->below.vkCreateFramebuffer(d, ci, a, out);
    if (r != VK_SUCCESS || !act(v)) return r;
    fb_rec *f = calloc(1, sizeof(*f));
    if (!f) return r;
    f->rp = ci->renderPass;
    f->w = ci->width, f->h = ci->height, f->layers = ci->layers, f->n = ci->attachmentCount, f->flags = ci->flags;
    if (!(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && ci->attachmentCount) {
        f->views = malloc(ci->attachmentCount * sizeof(*f->views));
        if (!f->views) {
            free(f);
            return r;
        }
        memcpy(f->views, ci->pAttachments, ci->attachmentCount * sizeof(*f->views));
    }
    pthread_mutex_lock(&v->lock);
    fb_rec *old = mput(&v->fbs, (uint64_t)*out, f);
    pthread_mutex_unlock(&v->lock);
    if (old) free(old->views), free(old);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyFramebuffer(VkDevice d, VkFramebuffer fb, const VkAllocationCallbacks *a)
{
    vkb_errata_dev *e;
    pv3d *v = for_device(d, &e);
    if (!v) {
        vkb_errata_next(1)->vkDestroyFramebuffer(d, fb, a);
        return;
    }
    if (fb) {
        pthread_mutex_lock(&v->lock);
        fb_rec *f = mdel(&v->fbs, (uint64_t)fb);
        pthread_mutex_unlock(&v->lock);
        if (f) {
            if (f->alt) v->below.vkDestroyFramebuffer(d, f->alt, NULL);
            free(f->views);
            free(f);
        }
    }
    v->below.vkDestroyFramebuffer(d, fb, a);
}

/* The kind of pass a render pass makes (PC_*; lock held): a feedback loop in a colour attachment's layout, a view mask */
static int rp_ctx(pv3d *v, VkRenderPass h)
{
    rp_rec *r = h ? mget(&v->rps, (uint64_t)h) : NULL;
    return r ? (r->fbl ? PC_FB : 0) | (r->vmask ? PC_MV : 0) : 0;
}

/* The framebuffer with the redirected views (lock held), made once; NULL when none of its views is redirected or
 * one could not be */
static VkFramebuffer fb_alt(pv3d *v, fb_rec *f)
{
    if (f->alt_tried || !f->views) return f->alt;
    f->alt_tried = 1;
    int ctx = rp_ctx(v, f->rp);
    VkImageView *nv = malloc((f->n ? f->n : 1) * sizeof(*nv));
    if (!nv) return VK_NULL_HANDLE;
    uint32_t changed = 0;
    for (uint32_t i = 0; i < f->n; i++) {
        repl r;
        int k = get_repl(v, f->views[i], ctx, &r);
        if (k < 0) {
            free(nv);
            note_failure(v);
            return VK_NULL_HANDLE;
        }
        nv[i] = k ? r.view : f->views[i];
        changed += k > 0;
    }
    if (changed) {
        VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, f->flags, f->rp, f->n, nv, f->w, f->h, f->layers};
        if (v->below.vkCreateFramebuffer(v->device, &ci, NULL, &f->alt) != VK_SUCCESS) {
            f->alt = VK_NULL_HANDLE;
            note_failure(v);
        }
    }
    free(nv);
    return f->alt;
}

static VkRect2D clamp_area(VkRect2D a, uint32_t w, uint32_t h)
{
    int64_t x0 = a.offset.x < 0 ? 0 : a.offset.x, y0 = a.offset.y < 0 ? 0 : a.offset.y;
    int64_t x1 = (int64_t)a.offset.x + a.extent.width, y1 = (int64_t)a.offset.y + a.extent.height;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

/* layers: the shadow's layers the pass renders (0: all) */
static void item_add(pass_rec *pr, const repl *r, VkRect2D area, VkImageLayout l3s, VkImageLayout l3e, VkImageLayout lss, VkImageLayout lse,
                     int copy_in, int copy_out, uint32_t layers, uint32_t att)
{
    if (pr->n == MAXA) return;
    pass_item *it = &pr->it[pr->n++];
    it->p = (vkb_pv3d_pair){r->snap.image, r->snap.sh.img, r->snap.mip, r->snap.base, r->snap.count,
                            clamp_area(area, r->snap.w, r->snap.h), l3s, l3e, lss, lse, layers};
    it->p.arr = r->snap.arr;
    it->copy_in = copy_in;
    it->copy_out = copy_out;
    it->att = att;
    it->cur3 = l3s;
}

static int load_reads(VkAttachmentLoadOp op)
{
    return op == VK_ATTACHMENT_LOAD_OP_LOAD || op == VK_ATTACHMENT_LOAD_OP_NONE;
}

/* The layout item i's 3D image is in now: a previous item's of the same mip level of the same image, or its own */
static VkImageLayout now3(const pass_rec *pr, int i)
{
    for (int j = i - 1; j >= 0; j--)
        if (pr->it[j].p.img3d == pr->it[i].p.img3d && pr->it[j].p.mip == pr->it[i].p.mip) return pr->it[j].cur3;
    return pr->it[i].cur3;
}

/* A split render pass: the layout the app's subpass s gives attachment a, or keep (not one of its attachments) */
static VkImageLayout sub_layout(const pass_rec *pr, uint32_t a, int s, VkImageLayout keep)
{
    if (!pr->lay || a >= pr->natt || s >= pr->nsub) return keep;
    VkImageLayout l = pr->lay[(uint32_t)s * pr->natt + a];
    return l == VK_IMAGE_LAYOUT_UNDEFINED ? keep : l;
}

/* A feedback pass begins: the next part's begin info kept (encoded) */
static void keep_begin(pass_rec *pr, int dyn, const void *info)
{
    vkb_enc e;
    vkb_enc_init(&e);
    if (dyn) vkb_enc_VkRenderingInfo(&e, info);
    else vkb_enc_VkRenderPassBeginInfo(&e, info);
    if (e.oom || !e.buf) {
        vkb_enc_free(&e);
        return;
    }
    pr->blob = e.buf, pr->len = e.len;
}

typedef struct {
    VkRenderPassBeginInfo *bi;
    VkFramebuffer old_fb;
    VkRenderPass old_rp;
    int fb_set, rp_set;
    VkRenderPassAttachmentBeginInfo *abi;
    const VkImageView *old_att;
    VkImageView *tmp;
} rp_undo;

/* Before a render pass begins: the views it renders to redirected in the begin info (restored by rp_restore after
 * the driver's call), the shadows prepared on cb, the pass remembered for its end (and, where reads of the 3D image
 * can follow writes inside it, for its splits). */
static void rp_prepare(pv3d *v, VkCommandBuffer cb, const VkRenderPassBeginInfo *cbi, VkSubpassContents contents, rp_undo *u)
{
    memset(u, 0, sizeof(*u));
    VkRenderPassBeginInfo *bi = (VkRenderPassBeginInfo *)cbi;
    pass_rec *pr = calloc(1, sizeof(*pr));
    if (!pr) return;
    pthread_mutex_lock(&v->lock);
    fb_rec *f = mget(&v->fbs, (uint64_t)bi->framebuffer);
    rp_rec *rp = mget(&v->rps, (uint64_t)bi->renderPass);
    int changed = 0;
    VkRenderPass split2 = VK_NULL_HANDLE;
    if (f && rp) {
        const VkImageView *atts = f->views;
        uint32_t n = f->n;
        VkRenderPassAttachmentBeginInfo *abi = NULL;
        if (!f->views) {
            for (const VkBaseInStructure *s = bi->pNext; s; s = s->pNext)
                if (s->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) abi = (VkRenderPassAttachmentBeginInfo *)s;
            if (abi) atts = abi->pAttachments, n = abi->attachmentCount;
        }
        /* the layers the pass renders: the framebuffer's, or with multiview up to the mask's highest view (holes: copied in) */
        uint32_t nl = rp->vmask ? mask_layers(rp->vmask) : f->layers;
        int holes = mask_holes(rp->vmask);
        VkFramebuffer alt = f->views ? fb_alt(v, f) : VK_NULL_HANDLE;
        int ctx = f->views ? rp_ctx(v, f->rp) : (rp->fbl ? PC_FB : 0) | (rp->vmask ? PC_MV : 0); /* (the alt framebuffer was made for f's own pass) */
        VkImageView *tmp = !f->views && abi ? malloc((n ? n : 1) * sizeof(*tmp)) : NULL;
        if ((f->views ? alt != VK_NULL_HANDLE : tmp != NULL) && atts) {
            for (uint32_t i = 0; i < n; i++) {
                repl r;
                int k = get_repl(v, atts[i], ctx, &r);
                if (tmp) tmp[i] = k > 0 ? r.view : atts[i];
                if (k < 0) {
                    note_failure(v);
                    continue;
                }
                if (k > 0) changed++;
                if (k > 0 && r.shadow && i < rp->n) {
                    const rp_att *a = &rp->att[i];
                    item_add(pr, &r, bi->renderArea, a->init, a->fin, a->init, a->fin,
                             (load_reads(a->load) || holes) && a->init != VK_IMAGE_LAYOUT_UNDEFINED,
                             a->store == VK_ATTACHMENT_STORE_OP_STORE || a->resolve, nl, i);
                }
            }
        }
        if (changed && f->views) {
            u->bi = bi, u->old_fb = bi->framebuffer, u->fb_set = 1;
            bi->framebuffer = alt;
        } else if (changed && tmp) {
            u->abi = abi, u->old_att = abi->pAttachments, u->tmp = tmp;
            abi->pAttachments = tmp, tmp = NULL;
        }
        free(tmp);
        int items = changed && pr->n;
        int fbs = fbs_on(v) && rp->fbl_any; /* a pass pv_fbloop_split splits (with views copied for it or not) */
        if (fbs) ADD(v->fbs_rp, 1);
        if (items || fbs) {
            if (items && (ctx & PC_MV)) ADD(v->mv_passes, 1);
            if (items && (ctx & PC_FB)) ADD(v->fb_passes, 1);
            pr->nsub = (int)rp->nsub;
            pr->contents = contents;
            if ((items ? rp->feedback || fbs : 1) && rp->split1 && rp->split2 && (pr->lay = malloc((size_t)rp->n * rp->nsub * sizeof(*pr->lay)))) {
                memcpy(pr->lay, rp->lay, (size_t)rp->n * rp->nsub * sizeof(*pr->lay));
                pr->natt = rp->n;
                pr->feedback = 1;
                split2 = rp->split2;
                u->bi = bi, u->old_rp = bi->renderPass, u->rp_set = 1;
                bi->renderPass = rp->split1; /* every attachment stored, so a split loses nothing */
                for (int i = 0; i < pr->n; i++) pr->it[i].copy_out = 1;
                v->any_feedback = 1;
            }
        }
    }
    if ((changed && pr->n) || pr->feedback) {
        pr_free(mput(&v->passes, (uint64_t)cb, pr));
        if (pr->n) ADD(v->rp_passes, 1);
    } else {
        pr_free(pr);
        pr = NULL;
    }
    pthread_mutex_unlock(&v->lock);
    if (!pr) return;
    for (int i = 0; i < pr->n; i++) {
        vkb_pv3d_copy_begin(&v->fns, cb, &pr->it[i].p, pr->it[i].copy_in);
        if (pr->it[i].copy_in) ADD(v->copies_in, 1);
    }
    if (pr->feedback) {
        /* the 3D image in the layout the first subpass gives the attachment: what the app's reads of it name */
        for (int i = 0; i < pr->n; i++) {
            VkImageLayout from = now3(pr, i), to = sub_layout(pr, pr->it[i].att, 0, from);
            vkb_pv3d_copy_layout_p(&v->fns, cb, &pr->it[i].p, from, to);
            pr->it[i].cur3 = to;
        }
        VkRenderPass s1 = bi->renderPass;
        bi->renderPass = split2;
        keep_begin(pr, 0, bi);
        bi->renderPass = s1;
    }
}

static void rp_restore(rp_undo *u)
{
    if (u->fb_set) u->bi->framebuffer = u->old_fb;
    if (u->rp_set) u->bi->renderPass = u->old_rp;
    if (u->abi) u->abi->pAttachments = u->old_att;
    free(u->tmp);
}

/* After a pass (or its last part): the slices stored copied back */
static void pass_finish(pv3d *v, VkCommandBuffer cb)
{
    pthread_mutex_lock(&v->lock);
    pass_rec *pr = mdel(&v->passes, (uint64_t)cb);
    pthread_mutex_unlock(&v->lock);
    if (!pr) return;
    if (!pr->suspending)
        for (int i = 0; i < pr->n; i++) {
            vkb_pv3d_pair q = pr->it[i].p;
            q.l3_start = pr->it[i].cur3;
            /* another view of the same mip level of the same image was finished before: the image is in its end layout already */
            for (int j = 0; j < i; j++)
                if (pr->it[j].p.img3d == q.img3d && pr->it[j].p.mip == q.mip) q.l3_start = q.l3_end;
            vkb_pv3d_copy_end(&v->fns, cb, &q, pr->it[i].copy_out);
            if (pr->it[i].copy_out) ADD(v->copies_out, 1);
        }
    pr_free(pr);
}

/* A split: the pass (render pass or dynamic rendering) ended where it is, every shadow copied into its slices,
 * the pass begun again at the same point (pr->subpass: the subpass the app is in now, whose layouts the 3D image
 * takes). Every attachment was stored (split1 / STORE) and is loaded (split2 / LOAD), so nothing is lost. */
/* every copied attachment of the pass is a plain image's (the imitation's), none a 3D image's view (PV-04's) */
static int only_plain(const pass_rec *pr)
{
    for (int i = 0; i < pr->n; i++)
        if (!pr->it[i].p.arr) return 0;
    return 1;
}

static void split(pv3d *v, VkCommandBuffer cb, pass_rec *pr, const char *where)
{
    /* the imitation of CXTP's loops is the fix without its splits; so is the plain imitation without pv_fbloop_split's */
    int imi = mim_fb(v) || (fbs_imi(v) && !fbs_on(v) && pr->n && only_plain(pr));
    /* whose split: pv_fbloop_split's where nothing but plain images (or nothing) is copied for the pass, else PV-04's */
    int mine = fbs_on(v) && only_plain(pr);
    uint64_t *n_split = mine ? &v->fbs_splits : &v->splits, *n_unsplit = mine ? &v->fbs_unsplit : &v->unsplit;
    if (!pr->blob || pr->active || v->nosplit || imi) {
        ADD(*n_unsplit, 1);
        if (!imi && mine && !__atomic_exchange_n(&v->logged_fbs_unsplit, 1, __ATOMIC_RELAXED))
            VKB_WARN("errata: pv_fbloop_split: a pass with an attachment in a feedback-loop layout was not split at a %s (%s): reads of the "
                     "attachment after it inside the pass may be wrong",
                     where, v->nosplit ? "VKBRIDGE_PV3D_NOSPLIT" : pr->active ? "a query or transform feedback is active" : "its begin info could not be kept");
        else if (!imi && !mine && !__atomic_exchange_n(&v->logged_unsplit, 1, __ATOMIC_RELAXED))
            VKB_WARN("errata: pv_view3d_base: a pass rendering a 3D image through a copy was not split at a %s (%s): reads of the 3D image "
                     "after it inside the pass see the slices as they were at its start or its last split",
                     where, v->nosplit ? "VKBRIDGE_PV3D_NOSPLIT" : pr->active ? "a query or transform feedback is active" : "its begin info could not be kept");
        return;
    }
    const vkb_dispatch *d = &v->below;
    if (pr->dyn)
        d->vkCmdEndRendering(cb);
    else {
        for (int s = pr->subpass + 1; s < pr->nsub; s++) d->vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
        d->vkCmdEndRenderPass(cb);
    }
    for (int i = 0; i < pr->n; i++) {
        pass_item *it = &pr->it[i];
        VkImageLayout from = now3(pr, i), to = pr->dyn ? from : sub_layout(pr, it->att, pr->subpass, from);
        vkb_pv3d_copy_sync(&v->fns, cb, &it->p, it->p.ls_end, from, to);
        it->cur3 = to;
        ADD(v->copies_out, 1);
    }
    if (!pr->n) { /* nothing to copy: what the first part wrote is made visible to the second's loads and reads */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        v->fns.barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    vkb_arena arena;
    memset(&arena, 0, sizeof(arena));
    vkb_dec dc;
    vkb_dec_init(&dc, pr->blob, pr->len, &arena, NULL, 0);
    if (pr->dyn) {
        VkRenderingInfo ri;
        memset(&ri, 0, sizeof(ri));
        vkb_dec_VkRenderingInfo(&dc, &ri);
        d->vkCmdBeginRendering(cb, &ri);
        if (pr->has_loc && d->vkCmdSetRenderingAttachmentLocations) {
            VkRenderingAttachmentLocationInfo li = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO, NULL, pr->nloc, pr->loc_null ? NULL : pr->loc};
            d->vkCmdSetRenderingAttachmentLocations(cb, &li);
        }
        if (pr->has_idx && d->vkCmdSetRenderingInputAttachmentIndices) {
            VkRenderingInputAttachmentIndexInfo ii = {VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO, NULL, pr->nidx,
                                                      pr->idx_null ? NULL : pr->idx, pr->has_d ? &pr->d_idx : NULL, pr->has_s ? &pr->s_idx : NULL};
            d->vkCmdSetRenderingInputAttachmentIndices(cb, &ii);
        }
    } else {
        VkRenderPassBeginInfo bi;
        memset(&bi, 0, sizeof(bi));
        vkb_dec_VkRenderPassBeginInfo(&dc, &bi);
        d->vkCmdBeginRenderPass(cb, &bi, pr->subpass ? VK_SUBPASS_CONTENTS_INLINE : pr->contents);
        for (int s = 1; s <= pr->subpass; s++) d->vkCmdNextSubpass(cb, s == pr->subpass ? pr->contents : VK_SUBPASS_CONTENTS_INLINE);
    }
    vkb_arena_free(&arena);
    ADD(*n_split, 1);
    if (mine) {
        if (!__atomic_exchange_n(&v->logged_fbs, 1, __ATOMIC_RELAXED))
            VKB_INFO("errata: pv_fbloop_split: a pass with an attachment in a feedback-loop layout is split at a %s: ended there and begun "
                     "again with its attachments loaded, so the reads after it see what was drawn before (the first such split)", where);
    } else if (!__atomic_exchange_n(&v->logged_split, 1, __ATOMIC_RELAXED))
        VKB_INFO("errata: pv_view3d_base: a pass rendering a 3D image through a copy, with the attachment in a layout shaders may read it in, "
                 "is split at a %s: the slices rendered so far copied into the 3D image before it goes on (the first such split)",
                 where);
}

/* (the fallbacks call what is beneath every imitation, the driver: next_fix would reach these wrappers again, the
 * imitation's when the bug is imitated; the state is gone after vkb_errata_pv3d_destroy, the wrappers stay) */
#define PV(RET_VOID_FALLBACK)                                              \
    vkb_errata_dev *e = vkb_errata_cur();                                  \
    pv3d *v = e ? e->pv3d : NULL;                                          \
    if (!v) {                                                              \
        RET_VOID_FALLBACK;                                                 \
        return;                                                            \
    }                                                                      \
    int fix = fbs_on(v) || (act(v) && __atomic_load_n(&v->nbad, __ATOMIC_RELAXED) != 0)

static VKAPI_ATTR void VKAPI_CALL p_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c)
{
    PV(vkb_errata_next(1)->vkCmdBeginRenderPass(cb, bi, c));
    rp_undo u = {0};
    if (fix) rp_prepare(v, cb, bi, c, &u);
    v->below.vkCmdBeginRenderPass(cb, bi, c);
    rp_restore(&u);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *sb)
{
    PV(vkb_errata_next(1)->vkCmdBeginRenderPass2(cb, bi, sb));
    rp_undo u = {0};
    if (fix) rp_prepare(v, cb, bi, sb ? sb->contents : VK_SUBPASS_CONTENTS_INLINE, &u);
    v->below.vkCmdBeginRenderPass2(cb, bi, sb);
    rp_restore(&u);
}

/* The open pass of cb (render pass or dynamic rendering), or NULL */
static pass_rec *open_pass(pv3d *v, VkCommandBuffer cb)
{
    pthread_mutex_lock(&v->lock);
    pass_rec *pr = mget(&v->passes, (uint64_t)cb);
    pthread_mutex_unlock(&v->lock);
    return pr;
}

/* After the app's next subpass: where a split pass is, and a split (a later subpass may sample what an earlier one rendered) */
static void next_subpass(pv3d *v, VkCommandBuffer cb, VkSubpassContents c)
{
    pass_rec *pr = open_pass(v, cb);
    if (!pr || pr->dyn) return;
    pr->subpass++;
    pr->contents = c;
    if (pr->feedback && pr->n) split(v, cb, pr, "next subpass"); /* (a pass with nothing copied only at its feedback-loop barriers) */
}

static VKAPI_ATTR void VKAPI_CALL p_CmdNextSubpass(VkCommandBuffer cb, VkSubpassContents c)
{
    PV(vkb_errata_next(1)->vkCmdNextSubpass(cb, c));
    v->below.vkCmdNextSubpass(cb, c);
    if (fix) next_subpass(v, cb, c);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdNextSubpass2(VkCommandBuffer cb, const VkSubpassBeginInfo *sb, const VkSubpassEndInfo *se)
{
    PV(vkb_errata_next(1)->vkCmdNextSubpass2(cb, sb, se));
    v->below.vkCmdNextSubpass2(cb, sb, se);
    if (fix) next_subpass(v, cb, sb ? sb->contents : VK_SUBPASS_CONTENTS_INLINE);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdEndRenderPass(VkCommandBuffer cb)
{
    PV(vkb_errata_next(1)->vkCmdEndRenderPass(cb));
    v->below.vkCmdEndRenderPass(cb);
    if (fix) pass_finish(v, cb);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *se)
{
    PV(vkb_errata_next(1)->vkCmdEndRenderPass2(cb, se));
    v->below.vkCmdEndRenderPass2(cb, se);
    if (fix) pass_finish(v, cb);
}

/* A barrier inside a split pass: the pass split first, so what the barrier makes visible is in the 3D image. A pass with
 * nothing copied (pv_fbloop_split's) is split at the barriers with the feedback-loop or by-region dependency flag only. */
static void before_barrier(pv3d *v, VkCommandBuffer cb, VkDependencyFlags df)
{
    if (!__atomic_load_n(&v->any_feedback, __ATOMIC_RELAXED)) return;
    int loop = !!(df & (VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT | VK_DEPENDENCY_BY_REGION_BIT));
    pthread_mutex_lock(&v->lock);
    pass_rec *pr = mget(&v->passes, (uint64_t)cb);
    intptr_t sec = !pr ? (intptr_t)mget(&v->fsec, (uint64_t)cb) : 0; /* 1: PV-04's secondary command buffer, 2: pv_fbloop_split's */
    pthread_mutex_unlock(&v->lock);
    if (pr && pr->feedback) {
        if ((pr->n && !only_plain(pr)) || loop) split(v, cb, pr, "pipeline barrier");
    } else if (sec == 2 && loop) {
        ADD(v->fbs_unsplit, 1);
        if (!__atomic_exchange_n(&v->logged_fbs_unsplit, 1, __ATOMIC_RELAXED))
            VKB_WARN("errata: pv_fbloop_split: a pipeline barrier inside a secondary command buffer of a pass with an attachment in a "
                     "feedback-loop layout cannot split the pass: reads of the attachment after it inside the pass may be wrong");
    } else if (sec == 1) {
        ADD(v->unsplit, 1);
        if (!__atomic_exchange_n(&v->logged_unsplit, 1, __ATOMIC_RELAXED))
            VKB_WARN("errata: pv_view3d_base: a pipeline barrier inside a secondary command buffer of a pass rendering a 3D image through a "
                     "copy, with the attachment in a layout shaders may read it in, cannot split the pass: reads of the 3D image after it "
                     "inside the pass see the slices as they were at its start or its last split");
    }
}

static VKAPI_ATTR void VKAPI_CALL p_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df,
                                                       uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
                                                       const VkImageMemoryBarrier *im)
{
    PV(vkb_errata_next(1)->vkCmdPipelineBarrier(cb, ss, ds, df, nm, m, nb, b, ni, im));
    if (fix) before_barrier(v, cb, df);
    v->below.vkCmdPipelineBarrier(cb, ss, ds, df, nm, m, nb, b, ni, im);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di)
{
    PV(vkb_errata_next(1)->vkCmdPipelineBarrier2(cb, di));
    if (fix) before_barrier(v, cb, di->dependencyFlags);
    v->below.vkCmdPipelineBarrier2(cb, di);
}

/* Queries and transform feedback active inside a split pass: a split would end the pass under them */
static void active_add(pv3d *v, VkCommandBuffer cb, int d)
{
    if (!__atomic_load_n(&v->any_feedback, __ATOMIC_RELAXED)) return;
    pass_rec *pr = open_pass(v, cb);
    if (pr && pr->feedback) pr->active += d;
}

static VKAPI_ATTR void VKAPI_CALL p_CmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f)
{
    PV(vkb_errata_next(1)->vkCmdBeginQuery(cb, p, q, f));
    v->below.vkCmdBeginQuery(cb, p, q, f);
    if (fix) active_add(v, cb, 1);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q)
{
    PV(vkb_errata_next(1)->vkCmdEndQuery(cb, p, q));
    v->below.vkCmdEndQuery(cb, p, q);
    if (fix) active_add(v, cb, -1);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdBeginQueryIndexedEXT(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f, uint32_t i)
{
    PV(vkb_errata_next(1)->vkCmdBeginQueryIndexedEXT(cb, p, q, f, i));
    v->below.vkCmdBeginQueryIndexedEXT(cb, p, q, f, i);
    if (fix) active_add(v, cb, 1);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdEndQueryIndexedEXT(VkCommandBuffer cb, VkQueryPool p, uint32_t q, uint32_t i)
{
    PV(vkb_errata_next(1)->vkCmdEndQueryIndexedEXT(cb, p, q, i));
    v->below.vkCmdEndQueryIndexedEXT(cb, p, q, i);
    if (fix) active_add(v, cb, -1);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdBeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b,
                                                                 const VkDeviceSize *o)
{
    PV(vkb_errata_next(1)->vkCmdBeginTransformFeedbackEXT(cb, first, n, b, o));
    v->below.vkCmdBeginTransformFeedbackEXT(cb, first, n, b, o);
    if (fix) active_add(v, cb, 1);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdEndTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b,
                                                               const VkDeviceSize *o)
{
    PV(vkb_errata_next(1)->vkCmdEndTransformFeedbackEXT(cb, first, n, b, o));
    v->below.vkCmdEndTransformFeedbackEXT(cb, first, n, b, o);
    if (fix) active_add(v, cb, -1);
}

/* A secondary command buffer inherits the framebuffer of the pass it runs in: the redirected one. A command buffer
 * begun again starts with no open pass. */
static VKAPI_ATTR VkResult VKAPI_CALL p_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    vkb_errata_dev *e = vkb_errata_cur();
    pv3d *v = e ? e->pv3d : NULL;
    if (!v) return vkb_errata_next(1)->vkBeginCommandBuffer(cb, bi);
    VkCommandBufferInheritanceInfo *inh = (VkCommandBufferInheritanceInfo *)bi->pInheritanceInfo;
    VkFramebuffer old = VK_NULL_HANDLE, alt = VK_NULL_HANDLE;
    if (fbs_on(v) || (act(v) && __atomic_load_n(&v->nbad, __ATOMIC_RELAXED))) {
        pthread_mutex_lock(&v->lock);
        pass_rec *stale = mdel(&v->passes, (uint64_t)cb);
        mdel(&v->fsec, (uint64_t)cb);
        if (inh && (bi->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
            fb_rec *f = inh->framebuffer ? mget(&v->fbs, (uint64_t)inh->framebuffer) : NULL;
            if (f) alt = fb_alt(v, f);
            rp_rec *rp = inh->renderPass ? mget(&v->rps, (uint64_t)inh->renderPass) : NULL;
            if (rp && rp->feedback && (alt || !inh->framebuffer)) {
                mput(&v->fsec, (uint64_t)cb, (void *)1);
                v->any_feedback = 1;
            } else if (rp && fbs_on(v) && rp->fbl_any) {
                mput(&v->fsec, (uint64_t)cb, (void *)2);
                v->any_feedback = 1;
            }
        }
        pthread_mutex_unlock(&v->lock);
        pr_free(stale);
        if (alt) old = inh->framebuffer, inh->framebuffer = alt;
    }
    VkResult r = v->below.vkBeginCommandBuffer(cb, bi);
    if (alt) inh->framebuffer = old;
    return r;
}

/* ------------------------------------------------------------------ dynamic rendering */

static VKAPI_ATTR void VKAPI_CALL p_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *info)
{
    PV(vkb_errata_next(1)->vkCmdBeginRendering(cb, info));
    uint32_t nc = info->colorAttachmentCount;
    int fbl_here = 0; /* an attachment in a feedback-loop layout (colour, depth, stencil) */
    for (uint32_t i = 0; i < nc; i++)
        if (fbl_layout(info->pColorAttachments[i].imageLayout)) fbl_here = 1;
    int ctx_fb = fbl_here;
    if (info->pDepthAttachment && fbl_layout(info->pDepthAttachment->imageLayout)) fbl_here = 1;
    if (info->pStencilAttachment && fbl_layout(info->pStencilAttachment->imageLayout)) fbl_here = 1;
    int fbs = fbs_on(v) && fbl_here; /* a pass pv_fbloop_split splits */
    VkRenderingAttachmentInfo *col = fix && (nc || fbs) && (fbs || (act(v) && __atomic_load_n(&v->nbad, __ATOMIC_RELAXED))) ?
                                     malloc(2 * (nc ? nc : 1) * sizeof(*col)) : NULL;
    if (!col) {
        v->below.vkCmdBeginRendering(cb, info);
        return;
    }
    if (nc) memcpy(col, info->pColorAttachments, nc * sizeof(*col));
    VkRenderingInfo ri = *info;
    ri.pColorAttachments = col;
    pass_rec *pr = calloc(1, sizeof(*pr));
    int changed = 0, fail = 0, feedback = 0;
    uint32_t nl = info->viewMask ? mask_layers(info->viewMask) : info->layerCount;
    int holes = mask_holes(info->viewMask);
    int resuming = !!(info->flags & VK_RENDERING_RESUMING_BIT);
    int ctx = info->viewMask ? PC_MV : 0;
    if (ctx_fb) ctx |= PC_FB;
    if (fbs) ADD(v->fbs_dyn, 1);
    pthread_mutex_lock(&v->lock);
    for (uint32_t i = 0; pr && i < nc; i++) {
        VkRenderingAttachmentInfo *at = &col[i];
        repl r;
        int k = get_repl(v, at->imageView, ctx, &r);
        if (k < 0) fail = 1;
        if (k > 0) {
            at->imageView = r.view;
            changed++;
            if (r.shadow) {
                item_add(pr, &r, info->renderArea, at->imageLayout, at->imageLayout, at->imageLayout, at->imageLayout,
                         (load_reads(at->loadOp) || holes) && at->imageLayout != VK_IMAGE_LAYOUT_UNDEFINED,
                         at->storeOp == VK_ATTACHMENT_STORE_OP_STORE, nl, i);
                feedback |= readable(at->imageLayout);
            }
        }
        k = at->resolveMode != VK_RESOLVE_MODE_NONE ? get_repl(v, at->resolveImageView, ctx, &r) : 0;
        if (k < 0) fail = 1;
        if (k > 0) {
            at->resolveImageView = r.view;
            changed++;
            if (r.shadow) {
                item_add(pr, &r, info->renderArea, at->resolveImageLayout, at->resolveImageLayout, at->resolveImageLayout,
                         at->resolveImageLayout, holes && at->resolveImageLayout != VK_IMAGE_LAYOUT_UNDEFINED, 1, nl, i);
                feedback |= readable(at->resolveImageLayout);
            }
        }
    }
    int items = changed && pr && pr->n;
    if (items || (pr && fbs)) {
        pr->dyn = 1;
        if (items && (ctx & PC_MV)) ADD(v->mv_passes, 1);
        if (items && (ctx & PC_FB)) ADD(v->fb_passes, 1);
        feedback = feedback || fbs;
        pr->feedback = feedback;
        pr->suspending = !feedback && (info->flags & VK_RENDERING_SUSPENDING_BIT);
        if (feedback) v->any_feedback = 1;
        pr_free(mput(&v->passes, (uint64_t)cb, pr));
        if (items) ADD(v->dyn_passes, 1);
    } else {
        pr_free(pr);
        pr = NULL;
    }
    pthread_mutex_unlock(&v->lock);
    if (fail) note_failure(v);
    VkRenderingAttachmentInfo dep, ste;
    if (pr && pr->feedback) {
        /* split where reads can follow writes: every attachment stored, a suspend / resume becomes an end / a load
         * (the parts in between hold no copies then) */
        ri.flags &= ~(VkRenderingFlags)(VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT);
        for (uint32_t i = 0; i < nc; i++) {
            col[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (resuming) col[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        if (info->pDepthAttachment) {
            dep = *info->pDepthAttachment;
            dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (resuming) dep.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ri.pDepthAttachment = &dep;
        }
        if (info->pStencilAttachment) {
            ste = *info->pStencilAttachment;
            ste.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (resuming) ste.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ri.pStencilAttachment = &ste;
        }
        for (int i = 0; i < pr->n; i++) {
            pr->it[i].copy_out = 1;
            if (resuming && pr->it[i].p.l3_start != VK_IMAGE_LAYOUT_UNDEFINED) pr->it[i].copy_in = 1;
        }
        /* the next part loads everything */
        VkRenderingInfo nx = ri;
        VkRenderingAttachmentInfo *c2 = col + nc, d2, s2;
        memcpy(c2, col, nc * sizeof(*c2));
        for (uint32_t i = 0; i < nc; i++) c2[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        nx.pColorAttachments = c2;
        if (ri.pDepthAttachment) d2 = *ri.pDepthAttachment, d2.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, nx.pDepthAttachment = &d2;
        if (ri.pStencilAttachment) s2 = *ri.pStencilAttachment, s2.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, nx.pStencilAttachment = &s2;
        keep_begin(pr, 1, &nx);
    }
    if (pr && (pr->feedback || !resuming))
        for (int i = 0; i < pr->n; i++) {
            vkb_pv3d_copy_begin(&v->fns, cb, &pr->it[i].p, pr->it[i].copy_in);
            if (pr->it[i].copy_in) ADD(v->copies_in, 1);
        }
    v->below.vkCmdBeginRendering(cb, &ri);
    free(col);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdEndRendering(VkCommandBuffer cb)
{
    PV(vkb_errata_next(1)->vkCmdEndRendering(cb));
    v->below.vkCmdEndRendering(cb);
    if (fix) pass_finish(v, cb);
}

/* Dynamic rendering's local-read state, which a new begin resets: kept in a split pass to be set again */
static VKAPI_ATTR void VKAPI_CALL p_CmdSetRenderingAttachmentLocations(VkCommandBuffer cb, const VkRenderingAttachmentLocationInfo *li)
{
    PV(vkb_errata_next(1)->vkCmdSetRenderingAttachmentLocations(cb, li));
    v->below.vkCmdSetRenderingAttachmentLocations(cb, li);
    pass_rec *pr = fix && __atomic_load_n(&v->any_feedback, __ATOMIC_RELAXED) ? open_pass(v, cb) : NULL;
    if (!pr || !pr->feedback || !pr->dyn || li->colorAttachmentCount > MAXLOC) return;
    pr->has_loc = 1;
    pr->nloc = li->colorAttachmentCount;
    pr->loc_null = !li->pColorAttachmentLocations;
    if (li->pColorAttachmentLocations) memcpy(pr->loc, li->pColorAttachmentLocations, pr->nloc * sizeof(*pr->loc));
}

static VKAPI_ATTR void VKAPI_CALL p_CmdSetRenderingInputAttachmentIndices(VkCommandBuffer cb, const VkRenderingInputAttachmentIndexInfo *ii)
{
    PV(vkb_errata_next(1)->vkCmdSetRenderingInputAttachmentIndices(cb, ii));
    v->below.vkCmdSetRenderingInputAttachmentIndices(cb, ii);
    pass_rec *pr = fix && __atomic_load_n(&v->any_feedback, __ATOMIC_RELAXED) ? open_pass(v, cb) : NULL;
    if (!pr || !pr->feedback || !pr->dyn || ii->colorAttachmentCount > MAXLOC) return;
    pr->has_idx = 1;
    pr->nidx = ii->colorAttachmentCount;
    pr->idx_null = !ii->pColorAttachmentInputIndices;
    if (ii->pColorAttachmentInputIndices) memcpy(pr->idx, ii->pColorAttachmentInputIndices, pr->nidx * sizeof(*pr->idx));
    pr->has_d = !!ii->pDepthInputAttachmentIndex;
    pr->has_s = !!ii->pStencilInputAttachmentIndex;
    if (pr->has_d) pr->d_idx = *ii->pDepthInputAttachmentIndex;
    if (pr->has_s) pr->s_idx = *ii->pStencilInputAttachmentIndex;
}

/* ------------------------------------------------------------------ install, destroy */

/* The wrappers, once per device: the fix and the imitation share them (each fix above its imitation inside) */
static pv3d *setup(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (e->pv3d) return e->pv3d;
    pv3d *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    pthread_mutex_init(&v->lock, NULL);
    v->device = t->device;
    v->owner = e;
    v->below = t->real;
    v->fns = (vkb_pv3d_fns){t->real.vkCmdPipelineBarrier, t->real.vkCmdCopyImage};
    if (t->real.vkGetPhysicalDeviceMemoryProperties) t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &v->mp);
    const char *im = vkb_srv_env("VKBRIDGE_PV3D_IMITATE");
    v->imode = im && !strcmp(im, "all") ? IM_ALL : im && !strcmp(im, "array") ? IM_ARRAY : IM_COUNT;
    if (im && !strcmp(im, "fb")) v->imode = IM_FB;
    const char *ns = vkb_srv_env("VKBRIDGE_PV3D_NOSPLIT");
    v->nosplit = ns && *ns == '1';
    e->pv3d = v;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < MAX_DEVICES; i++)
        if (!reg[i]) {
            reg[i] = v;
            break;
        }
    pthread_mutex_unlock(&reg_lock);
    ERRATA_HOOK(t, vkCreateImage, p_CreateImage);
    ERRATA_HOOK(t, vkDestroyImage, p_DestroyImage);
    ERRATA_HOOK(t, vkCreateImageView, p_CreateImageView);
    ERRATA_HOOK(t, vkDestroyImageView, p_DestroyImageView);
    ERRATA_HOOK(t, vkCreateRenderPass, p_CreateRenderPass);
    ERRATA_HOOK(t, vkCreateRenderPass2, p_CreateRenderPass2);
    ERRATA_HOOK(t, vkDestroyRenderPass, p_DestroyRenderPass);
    ERRATA_HOOK(t, vkCreateFramebuffer, p_CreateFramebuffer);
    ERRATA_HOOK(t, vkDestroyFramebuffer, p_DestroyFramebuffer);
    ERRATA_HOOK(t, vkCmdBeginRenderPass, p_CmdBeginRenderPass);
    ERRATA_HOOK(t, vkCmdBeginRenderPass2, p_CmdBeginRenderPass2);
    ERRATA_HOOK(t, vkCmdEndRenderPass, p_CmdEndRenderPass);
    ERRATA_HOOK(t, vkCmdEndRenderPass2, p_CmdEndRenderPass2);
    ERRATA_HOOK(t, vkBeginCommandBuffer, p_BeginCommandBuffer);
    if (t->real.vkCmdBeginRendering) ERRATA_HOOK(t, vkCmdBeginRendering, p_CmdBeginRendering);
    if (t->real.vkCmdEndRendering) ERRATA_HOOK(t, vkCmdEndRendering, p_CmdEndRendering);
    /* the splits of passes where shaders may read the 3D image while it is rendered */
    ERRATA_HOOK(t, vkCmdNextSubpass, p_CmdNextSubpass);
    ERRATA_HOOK(t, vkCmdNextSubpass2, p_CmdNextSubpass2);
    ERRATA_HOOK(t, vkCmdPipelineBarrier, p_CmdPipelineBarrier);
    ERRATA_HOOK(t, vkCmdPipelineBarrier2, p_CmdPipelineBarrier2);
    ERRATA_HOOK(t, vkCmdBeginQuery, p_CmdBeginQuery);
    ERRATA_HOOK(t, vkCmdEndQuery, p_CmdEndQuery);
    ERRATA_HOOK(t, vkCmdBeginQueryIndexedEXT, p_CmdBeginQueryIndexedEXT);
    ERRATA_HOOK(t, vkCmdEndQueryIndexedEXT, p_CmdEndQueryIndexedEXT);
    ERRATA_HOOK(t, vkCmdBeginTransformFeedbackEXT, p_CmdBeginTransformFeedbackEXT);
    ERRATA_HOOK(t, vkCmdEndTransformFeedbackEXT, p_CmdEndTransformFeedbackEXT);
    ERRATA_HOOK(t, vkCmdSetRenderingAttachmentLocations, p_CmdSetRenderingAttachmentLocations);
    ERRATA_HOOK(t, vkCmdSetRenderingInputAttachmentIndices, p_CmdSetRenderingInputAttachmentIndices);
    return v;
}

void vkb_errata_pv3d_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    setup(t, e);
}

void vkb_errata_pv3d_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    pv3d *v = setup(t, e);
    if (!v) return;
    int forced = e->forced >> VKB_ERRATUM_PV_VIEW3D_BASE & 1;
    int seen = vkb_probe_flag(VKB_PROBE_FLAG_PV_VIEW3D);
    /* the probe chooses; forcing the fix on a driver the probe did not fault assumes the copy form works */
    v->pol.array1 = seen && vkb_probe_flag(VKB_PROBE_FLAG_PV_V3D_ARRAY1);
    v->pol.two_d = seen && vkb_probe_flag(VKB_PROBE_FLAG_PV_V3D_2D1);
    v->pol.copy = seen ? vkb_probe_flag(VKB_PROBE_FLAG_PV_V3D_COPY) : forced;
    /* feedback loops: the second pattern, copied where the same probe run proved the copy form right for them */
    int fb_seen = vkb_probe_flag(VKB_PROBE_FLAG_PV_V3D_FB);
    v->pol.fb = fb_seen && vkb_probe_flag(VKB_PROBE_FLAG_PV_V3D_FBCOPY);
    v->pol.mv = e->pvr; /* multiview faults in PowerVR's driver: the copy form, whatever the probe says */
    if (v->pol.fb)
        vkb_errata_info_once("errata: pv_view3d_base: feedback loops (a colour attachment in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL or RENDERING_LOCAL_READ layout) "
                             "into a 2D-array view of a 3D image read wrong here and right in a plain 2D-array image and through a copy (probe view3d_layers): "
                             "such passes are rendered through a 2D-array image of their own, split at their barriers - PV-04");
    else if (fb_seen)
        vkb_errata_info_once("errata: pv_view3d_base: feedback loops into a 2D-array view of a 3D image read wrong here, and the copy form was not proven "
                             "right for them: record only, those passes stay as the app made them - PV-04");
    if (v->pol.mv)
        vkb_errata_info_once("errata: pv_view3d_base: PowerVR's driver: multiview (a view mask) into a 2D-array view of a 3D image faults in it (CXTP 25.3, DXT "
                             "24.3/25.1), no probe can tell: such passes take the copy form (a 2D-array image of their own, copied in and out) whatever the "
                             "probe says; other passes are untouched - PV-04");
    if (!seen && !forced && (v->pol.mv || fb_seen)) {
        vkb_errata_info_once("errata: pv_view3d_base: the base-layer bug was not seen (probe view3d_layers: no pattern): views from slice 1 on stay as the app "
                             "made them - PV-04");
        return;
    }
    const char *how_one = v->pol.array1 ? "kept as they are (the driver keeps their base layer)"
                          : v->pol.two_d ? "rendered through a 2D view of the slice"
                          : v->pol.copy ? "rendered through a 2D-array image of their own"
                                        : "left as they are (no form proven)";
    const char *how_many = v->pol.copy ? "rendered through a 2D-array image of their own, copied in before and out after each pass"
                                       : "left as they are (no form proven)";
    if (!v->pol.copy && !v->pol.array1 && !v->pol.two_d)
        vkb_errata_info_once("errata: pv_view3d_base: no form of the fix was proven on this driver (one-slice 2D-array view %s, 2D view of a slice %s, copy through a "
                             "2D-array image %s): record only, views stay as the app made them - PV-04",
                             "no", "no", "no");
    else
        vkb_errata_info_once("errata: pv_view3d_base: forms proven by the probe: one-slice 2D-array view %s, 2D view of a slice %s, copy through a 2D-array "
                             "image %s%s; one-slice views from slice 1 on: %s; multi-slice views from slice 1 on: %s - PV-04",
                             v->pol.array1 ? "keeps its base" : "ignores it", v->pol.two_d ? "keeps its base" : "ignores it",
                             v->pol.copy ? "right" : "not proven", forced && !seen ? " (assumed: VKBRIDGE_ERRATA=+pv_view3d_base)" : "",
                             how_one, how_many);
}

/* pv_fbloop_split (PV-06): the same wrappers; the passes it splits are decided at their begin */
void vkb_errata_pvfbl_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    setup(t, e);
}

void vkb_errata_pvfbl_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (!setup(t, e)) return;
    vkb_errata_info_once("errata: pv_fbloop_split: feedback loops (an attachment in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL or RENDERING_LOCAL_READ layout, a barrier "
                         "inside the pass) read wrong here and right with the pass ended at the barrier and begun again (probe view3d_layers): such passes are "
                         "split at each in-pass barrier with the feedback-loop or by-region dependency flag, their attachments stored and loaded - PV-06");
}

void vkb_errata_pv3d_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    pv3d *v = e->pv3d;
    if (!v) return;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < MAX_DEVICES; i++)
        if (reg[i] == v) reg[i] = NULL;
    pthread_mutex_unlock(&reg_lock);
    pthread_mutex_lock(&v->lock);
    for (unsigned b = 0; b < BK; b++) {
        for (node *n = v->views.b[b], *nx; n; n = nx) {
            nx = n->next;
            view_rec *vr = n->p;
            drop_shadow(v, &vr->sh);
            if (vr->alias) v->below.vkDestroyImageView(v->device, vr->alias, NULL);
            free(vr);
            free(n);
        }
        for (node *n = v->fbs.b[b], *nx; n; n = nx) {
            nx = n->next;
            fb_rec *f = n->p;
            if (f->alt) v->below.vkDestroyFramebuffer(v->device, f->alt, NULL);
            free(f->views);
            free(f);
            free(n);
        }
        for (node *n = v->rps.b[b], *nx; n; n = nx) {
            nx = n->next;
            rp_rec *r = n->p;
            if (r->split1) v->below.vkDestroyRenderPass(v->device, r->split1, NULL);
            if (r->split2) v->below.vkDestroyRenderPass(v->device, r->split2, NULL);
            free(r->att);
            free(r->lay);
            free(r);
            free(n);
        }
        for (node *n = v->fsec.b[b], *nx; n; n = nx) {
            nx = n->next;
            free(n);
        }
        for (node *n = v->imgs.b[b], *nx; n; n = nx) {
            nx = n->next;
            free(n->p);
            free(n);
        }
        for (node *n = v->passes.b[b], *nx; n; n = nx) {
            nx = n->next;
            pr_free(n->p);
            free(n);
        }
    }
    pthread_mutex_unlock(&v->lock);
    if (!e->probe && (ON(e) || IMI(e)) && (v->alias_views || v->shadow_views || v->imi_views || v->failures))
        VKB_INFO("errata: pv_view3d_base: %llu view(s) rendered through a 2D view / 2D_ARRAY view of their slice, %llu through a 2D-array image of "
                 "their own (%llu bytes); %llu render pass(es) and %llu dynamic rendering pass(es) redirected, %llu copies in, %llu copies out; "
                 "%llu split(s) for reads inside a pass, %llu not split; %llu failure(s); %llu view(s) imitated; "
                 "%llu multiview pass(es) and %llu feedback-loop pass(es) among them",
                 (unsigned long long)v->alias_views, (unsigned long long)v->shadow_views, (unsigned long long)v->shadow_bytes_est,
                 (unsigned long long)v->rp_passes, (unsigned long long)v->dyn_passes, (unsigned long long)v->copies_in,
                 (unsigned long long)v->copies_out, (unsigned long long)v->splits, (unsigned long long)v->unsplit,
                 (unsigned long long)v->failures, (unsigned long long)v->imi_views,
                 (unsigned long long)v->mv_passes, (unsigned long long)v->fb_passes);
    if (!e->probe && fbs_on(v) && (v->fbs_rp || v->fbs_dyn))
        VKB_INFO("errata: pv_fbloop_split: %llu render pass(es) and %llu dynamic rendering pass(es) with an attachment in a feedback-loop layout, "
                 "%llu split(s) at a barrier, %llu not split",
                 (unsigned long long)v->fbs_rp, (unsigned long long)v->fbs_dyn, (unsigned long long)v->fbs_splits, (unsigned long long)v->fbs_unsplit);
    pthread_mutex_destroy(&v->lock);
    free(v);
    e->pv3d = NULL;
}
