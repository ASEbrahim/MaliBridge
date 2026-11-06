/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * VK_KHR_imageless_framebuffer on a driver without it (VKB_EMU_IMAGELESS: Mali r18-r26, Vulkan 1.1,
 * e.g. the HUAWEI P40 Pro's Mali-G76 at 1.1.97). Mesa's Zink requires it (26.0.8, DroidDeck's Ubuntu
 * runtime: zink_device_info.py marks it required, and a release build says nothing but "failed to
 * load driver: zink"), and Zink draws Steam's UI: Steam stopped at "glXChooseVisual failed".
 *
 *   - vkCreateFramebuffer with VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT: nothing reaches the driver. A record
 *     (render pass, size, layers, attachment count) is made and its address is the handle the app gets:
 *     unique while it lives, and never passed to the driver (debug-utils names stay in the client).
 *     Every other framebuffer goes through untouched.
 *   - vkCmdBeginRenderPass / vkCmdBeginRenderPass2: a begin without VkRenderPassAttachmentBeginInfo goes
 *     straight through (the spec requires that structure for an imageless framebuffer and forbids a
 *     non-empty one otherwise). With it, the record's real framebuffer for those views: found by
 *     comparing the views (no allocation, no wait), made once with vkCreateFramebuffer on a miss. The
 *     structure is taken out of the chain; the rest of the begin is the app's.
 *   - vkBeginCommandBuffer: an inherited framebuffer that is one of ours becomes VK_NULL_HANDLE (allowed).
 *   - vkDestroyImageView: every real framebuffer made with that view is destroyed with it (the spec
 *     forbids destroying a view a pending command buffer uses, and ours is used only where the view is).
 *   - vkDestroyFramebuffer of ours: its real framebuffers destroyed, the record freed. What is left goes
 *     with the device.
 * Installed over the other emulations (server_emu_layer.c), so the BC sink views and the errata fixes
 * beneath see ordinary framebuffers with their views. VKBRIDGE_EMU_IMAGELESS=0 turns it off.
 *
 * Our own code from the Vulkan specification (no other implementation looked at).
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdlib.h>
#include <string.h>

#define FB_BUCKETS 256
#define VIEW_BUCKETS 1024

struct il_ent;

/* One view of a real framebuffer, in the view map (doubly linked: unlinked in place). */
typedef struct il_link {
    VkImageView view;
    struct il_ent *ent;
    struct il_link *next, **pprev;
} il_link;

/* A real framebuffer of an imageless one, for one set of views. */
typedef struct il_ent {
    struct il_fb *fb;
    VkFramebuffer real;
    struct il_ent *next; /* in fb->ents */
    uint32_t n;
    VkImageView *views;  /* n, after links[] in the same allocation */
    il_link links[];
} il_ent;

/* An imageless framebuffer: its address is the app's handle. */
typedef struct il_fb {
    VkRenderPass rp;
    uint32_t w, h, layers, n;
    VkFramebufferCreateFlags flags; /* the app's, without IMAGELESS */
    il_ent *ents;
    struct il_fb *next; /* hash chain */
} il_fb;

typedef struct il_state {
    pthread_mutex_t lock;
    il_fb *fbs[FB_BUCKETS];
    il_link *views[VIEW_BUCKETS];
    uint32_t live; /* imageless framebuffers alive: 0 keeps the view destroy and inheritance checks lock-free */
    PFN_vkCreateFramebuffer create_fb;
    PFN_vkDestroyFramebuffer destroy_fb;
    PFN_vkDestroyImageView destroy_view;
    PFN_vkCmdBeginRenderPass begin_rp;
    PFN_vkCmdBeginRenderPass2 begin_rp2;
    PFN_vkBeginCommandBuffer begin_cb;
    uint64_t begins, made, view_freed, fbs_made, failed;
} il_state;

static il_state *states[4096];

static il_state *st(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < 4096 ? states[t->id] : NULL;
}

static unsigned hk(uint64_t k, unsigned mask)
{
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    return (unsigned)(k >> 40) & mask;
}

#define FB_H(h) hk((uint64_t)(uintptr_t)(h), FB_BUCKETS - 1)
#define VIEW_H(v) hk((uint64_t)(uintptr_t)(v), VIEW_BUCKETS - 1)

/* Ours (under the lock): the record whose address is h. */
static il_fb *fb_find(il_state *s, VkFramebuffer h)
{
    for (il_fb *f = s->fbs[FB_H(h)]; f; f = f->next)
        if ((VkFramebuffer)(uintptr_t)f == h) return f;
    return NULL;
}

/* A real framebuffer destroyed and its record unlinked from the view map and its imageless one (lock held). */
static void ent_free(il_state *s, VkDevice device, il_ent *e)
{
    for (il_ent **pp = &e->fb->ents; *pp; pp = &(*pp)->next)
        if (*pp == e) {
            *pp = e->next;
            break;
        }
    for (uint32_t i = 0; i < e->n; i++) {
        il_link *l = &e->links[i];
        *l->pprev = l->next;
        if (l->next) l->next->pprev = l->pprev;
    }
    s->destroy_fb(device, e->real, NULL);
    free(e);
}

/* ------------------------------------------------------------------ framebuffers and views */

static VKAPI_ATTR VkResult VKAPI_CALL il_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a,
                                                           VkFramebuffer *out)
{
    il_state *s = st();
    if (!ci || !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) return s->create_fb(device, ci, a, out);
    il_fb *f = calloc(1, sizeof(*f));
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    f->rp = ci->renderPass;
    f->w = ci->width;
    f->h = ci->height;
    f->layers = ci->layers;
    f->n = ci->attachmentCount;
    f->flags = ci->flags & ~(VkFramebufferCreateFlags)VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
    VkFramebuffer h = (VkFramebuffer)(uintptr_t)f;
    pthread_mutex_lock(&s->lock);
    f->next = s->fbs[FB_H(h)];
    s->fbs[FB_H(h)] = f;
    s->live++;
    uint64_t nth = ++s->fbs_made;
    pthread_mutex_unlock(&s->lock);
    if (nth == 1)
        VKB_INFO("imageless framebuffer emulation: first imageless framebuffer (%u attachment(s), %ux%u, %u layer(s))", f->n, f->w, f->h,
                 f->layers);
    *out = h;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL il_DestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks *a)
{
    il_state *s = st();
    if (fb && __atomic_load_n(&s->live, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&s->lock);
        for (il_fb **pp = &s->fbs[FB_H(fb)]; *pp; pp = &(*pp)->next)
            if ((VkFramebuffer)(uintptr_t)*pp == fb) {
                il_fb *f = *pp;
                *pp = f->next;
                while (f->ents) ent_free(s, device, f->ents);
                free(f);
                s->live--;
                pthread_mutex_unlock(&s->lock);
                return;
            }
        pthread_mutex_unlock(&s->lock);
    }
    s->destroy_fb(device, fb, a);
}

static VKAPI_ATTR void VKAPI_CALL il_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *a)
{
    il_state *s = st();
    if (view && __atomic_load_n(&s->live, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&s->lock);
        for (;;) {
            il_link *l = s->views[VIEW_H(view)];
            while (l && l->view != view) l = l->next;
            if (!l) break;
            ent_free(s, device, l->ent);
            s->view_freed++;
        }
        pthread_mutex_unlock(&s->lock);
    }
    s->destroy_view(device, view, a);
}

/* ------------------------------------------------------------------ render passes */

/* The real framebuffer for an imageless one and these views (made on the first use); VK_NULL_HANDLE: none. */
static VkFramebuffer real_for(il_state *s, il_fb *f, uint32_t n, const VkImageView *views)
{
    for (il_ent *e = f->ents; e; e = e->next)
        if (e->n == n && !memcmp(e->views, views, n * sizeof(*views))) return e->real;
    vkb_srv_table *t = vkb_srv_current_table();
    il_ent *e = calloc(1, sizeof(*e) + n * (sizeof(il_link) + sizeof(VkImageView)));
    if (!e) return VK_NULL_HANDLE;
    e->views = (VkImageView *)(void *)&e->links[n];
    memcpy(e->views, views, n * sizeof(*views));
    e->n = n;
    e->fb = f;
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, f->flags, f->rp, n, e->views, f->w, f->h, f->layers};
    VkResult r = s->create_fb(t->device, &fci, NULL, &e->real);
    if (r != VK_SUCCESS) {
        free(e);
        s->failed++;
        VKB_ONCE(VKB_LOG_ERROR, "imageless framebuffer emulation: vkCreateFramebuffer failed (%d); the render pass is dropped (logged once)", r);
        return VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < n; i++) {
        il_link *l = &e->links[i];
        l->view = views[i];
        l->ent = e;
        il_link **head = &s->views[VIEW_H(views[i])];
        l->next = *head;
        l->pprev = head;
        if (*head) (*head)->pprev = &l->next;
        *head = l;
    }
    e->next = f->ents;
    f->ents = e;
    s->made++;
    return e->real;
}

/* bi's framebuffer made real (bi is the server's copy: changed in place). 0: ours, but none could be made (drop the pass). */
static int resolve(il_state *s, VkRenderPassBeginInfo *bi)
{
    if (!bi || !bi->pNext) return 1;
    const VkRenderPassAttachmentBeginInfo *ab = NULL;
    for (const VkBaseInStructure *b = bi->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) ab = (const VkRenderPassAttachmentBeginInfo *)b;
    if (!ab) return 1;
    pthread_mutex_lock(&s->lock);
    il_fb *f = fb_find(s, bi->framebuffer);
    if (!f) {
        /* the driver's own framebuffer with the structure (an empty one is allowed): it goes through as it came */
        pthread_mutex_unlock(&s->lock);
        return 1;
    }
    uint32_t n = ab->attachmentCount;
    if (n != f->n || (n && !ab->pAttachments)) {
        VKB_ONCE(VKB_LOG_ERROR, "imageless framebuffer emulation: a begin with %u view(s) for a framebuffer of %u; the render pass is dropped (logged once)",
                 n, f->n);
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    VkFramebuffer real = real_for(s, f, n, ab->pAttachments);
    s->begins++;
    pthread_mutex_unlock(&s->lock);
    if (!real) return 0;
    vkb_emu_chain_take(bi, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO);
    bi->framebuffer = real;
    return 1;
}

/* A dropped pass leaves the command buffer outside a render pass: its draws are refused by the driver as
 * any outside one would be. It only happens when the driver cannot make the framebuffer (out of memory). */
static VKAPI_ATTR void VKAPI_CALL il_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c)
{
    il_state *s = st();
    if (!resolve(s, (VkRenderPassBeginInfo *)bi)) return;
    s->begin_rp(cb, bi, c);
}

static VKAPI_ATTR void VKAPI_CALL il_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *sb)
{
    il_state *s = st();
    if (!resolve(s, (VkRenderPassBeginInfo *)bi)) return;
    s->begin_rp2(cb, bi, sb);
}

static VKAPI_ATTR VkResult VKAPI_CALL il_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info)
{
    il_state *s = st();
    VkCommandBufferInheritanceInfo *ii = info ? (VkCommandBufferInheritanceInfo *)info->pInheritanceInfo : NULL;
    if (ii && ii->framebuffer && __atomic_load_n(&s->live, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&s->lock);
        if (fb_find(s, ii->framebuffer)) ii->framebuffer = VK_NULL_HANDLE; /* the server's copy */
        pthread_mutex_unlock(&s->lock);
    }
    return s->begin_cb(cb, info);
}

/* ------------------------------------------------------------------ install */

void vkb_emu_imageless_install(vkb_srv_table *dev)
{
    if (!(dev->emu->flags & VKB_EMU_IMAGELESS) || dev->id >= 4096) return;
    vkb_dispatch *dt = &dev->dt;
    if (!dt->vkCreateFramebuffer || !dt->vkDestroyFramebuffer || !dt->vkDestroyImageView || !dt->vkCmdBeginRenderPass ||
        !dt->vkBeginCommandBuffer) {
        VKB_ERR("imageless framebuffer emulation: the device's table lacks the framebuffer calls; not installed on device %u", dev->id);
        return;
    }
    il_state *s = calloc(1, sizeof(*s));
    if (!s) return;
    pthread_mutex_init(&s->lock, NULL);
    s->create_fb = dt->vkCreateFramebuffer;
    s->destroy_fb = dt->vkDestroyFramebuffer;
    s->destroy_view = dt->vkDestroyImageView;
    s->begin_rp = dt->vkCmdBeginRenderPass;
    s->begin_rp2 = dt->vkCmdBeginRenderPass2;
    s->begin_cb = dt->vkBeginCommandBuffer;
    states[dev->id] = s;
    dt->vkCreateFramebuffer = il_CreateFramebuffer;
    dt->vkDestroyFramebuffer = il_DestroyFramebuffer;
    dt->vkDestroyImageView = il_DestroyImageView;
    dt->vkCmdBeginRenderPass = il_CmdBeginRenderPass;
    if (s->begin_rp2) dt->vkCmdBeginRenderPass2 = il_CmdBeginRenderPass2;
    dt->vkBeginCommandBuffer = il_BeginCommandBuffer;
    VKB_INFO("imageless framebuffers emulated on device %u: a framebuffer per set of views, made at its first render pass", dev->id);
}

void vkb_emu_imageless_uninstall(vkb_srv_table *dev)
{
    if (dev->id >= 4096 || !states[dev->id]) return;
    il_state *s = states[dev->id];
    states[dev->id] = NULL;
    if (s->fbs_made)
        VKB_INFO("imageless framebuffer emulation, device %u: %llu begin(s), %llu framebuffer(s) made, %llu freed by a view destroy%s", dev->id,
                 (unsigned long long)s->begins, (unsigned long long)s->made, (unsigned long long)s->view_freed,
                 s->failed ? ", some could not be made" : "");
    for (int i = 0; i < FB_BUCKETS; i++)
        while (s->fbs[i]) {
            il_fb *f = s->fbs[i];
            s->fbs[i] = f->next;
            while (f->ents) ent_free(s, dev->device, f->ents);
            free(f);
        }
    pthread_mutex_destroy(&s->lock);
    free(s);
}
