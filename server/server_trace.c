/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Evidence: with <pipeline cache dir>/trace present, the first commands of each device that touch
 * images (copies, blits, clears, dispatches, image barriers) and its submissions go to the log, with
 * the images' handles and layouts - what a program actually asks the GPU to do with the images it
 * shares (tablet run 13: gamescope's output images stayed empty while it presented 13 times a
 * second). Off unless the file is there; a debugging aid, not a feature.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRACE_MAX 600

typedef struct vkb_trace {
    vkb_srv_table *t;
    unsigned n;
    PFN_vkCmdCopyImage copy;
    PFN_vkCmdCopyImage2 copy2;
    PFN_vkCmdBlitImage blit;
    PFN_vkCmdBlitImage2 blit2;
    PFN_vkCmdClearColorImage clear;
    PFN_vkCmdDispatch dispatch;
    PFN_vkCmdPipelineBarrier barrier;
    PFN_vkCmdPipelineBarrier2 barrier2;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
} vkb_trace;

static vkb_trace *traces[4096];

static vkb_trace *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < 4096 ? traces[t->id] : NULL;
}

static int take(vkb_trace *x)
{
    return __atomic_fetch_add(&x->n, 1, __ATOMIC_RELAXED) < TRACE_MAX;
}

#define PID(x) ((x)->t->proc ? (x)->t->proc->pid : 0)
#define H(h) ((unsigned long long)(uintptr_t)(h))

static void VKAPI_CALL tr_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                       const VkImageCopy *r)
{
    vkb_trace *x = cur();
    if (take(x))
        VKB_INFO("trace pid %d cb %llx: CopyImage %llx (layout %d) -> %llx (layout %d), %u region(s), first %ux%u", PID(x), H(cb), H(src), sl,
                 H(dst), dl, n, n ? r[0].extent.width : 0, n ? r[0].extent.height : 0);
    x->copy(cb, src, sl, dst, dl, n, r);
}

static void VKAPI_CALL tr_CmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2 *i)
{
    vkb_trace *x = cur();
    if (take(x))
        VKB_INFO("trace pid %d cb %llx: CopyImage2 %llx (layout %d) -> %llx (layout %d)", PID(x), H(cb), H(i->srcImage), i->srcImageLayout,
                 H(i->dstImage), i->dstImageLayout);
    x->copy2(cb, i);
}

static void VKAPI_CALL tr_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                                       const VkImageBlit *r, VkFilter f)
{
    vkb_trace *x = cur();
    if (take(x)) VKB_INFO("trace pid %d cb %llx: BlitImage %llx (layout %d) -> %llx (layout %d)", PID(x), H(cb), H(src), sl, H(dst), dl);
    x->blit(cb, src, sl, dst, dl, n, r, f);
}

static void VKAPI_CALL tr_CmdBlitImage2(VkCommandBuffer cb, const VkBlitImageInfo2 *i)
{
    vkb_trace *x = cur();
    if (take(x))
        VKB_INFO("trace pid %d cb %llx: BlitImage2 %llx -> %llx", PID(x), H(cb), H(i->srcImage), H(i->dstImage));
    x->blit2(cb, i);
}

static void VKAPI_CALL tr_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue *c, uint32_t n,
                                             const VkImageSubresourceRange *r)
{
    vkb_trace *x = cur();
    if (take(x))
        VKB_INFO("trace pid %d cb %llx: ClearColorImage %llx (layout %d) to %.2f %.2f %.2f %.2f", PID(x), H(cb), H(img), l, c->float32[0],
                 c->float32[1], c->float32[2], c->float32[3]);
    x->clear(cb, img, l, c, n, r);
}

static void VKAPI_CALL tr_CmdDispatch(VkCommandBuffer cb, uint32_t gx, uint32_t gy, uint32_t gz)
{
    vkb_trace *x = cur();
    if (take(x)) VKB_INFO("trace pid %d cb %llx: Dispatch %ux%ux%u", PID(x), H(cb), gx, gy, gz);
    x->dispatch(cb, gx, gy, gz);
}

static void VKAPI_CALL tr_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags f,
                                             uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
                                             const VkImageMemoryBarrier *im)
{
    vkb_trace *x = cur();
    for (uint32_t i = 0; i < ni; i++)
        if (take(x))
            VKB_INFO("trace pid %d cb %llx: barrier image %llx layout %d -> %d, queue family %u -> %u", PID(x), H(cb), H(im[i].image),
                     im[i].oldLayout, im[i].newLayout, im[i].srcQueueFamilyIndex, im[i].dstQueueFamilyIndex);
    x->barrier(cb, ss, ds, f, nm, m, nb, b, ni, im);
}

static void VKAPI_CALL tr_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *d)
{
    vkb_trace *x = cur();
    for (uint32_t i = 0; i < d->imageMemoryBarrierCount; i++)
        if (take(x))
            VKB_INFO("trace pid %d cb %llx: barrier2 image %llx layout %d -> %d, queue family %u -> %u", PID(x), H(cb),
                     H(d->pImageMemoryBarriers[i].image), d->pImageMemoryBarriers[i].oldLayout, d->pImageMemoryBarriers[i].newLayout,
                     d->pImageMemoryBarriers[i].srcQueueFamilyIndex, d->pImageMemoryBarriers[i].dstQueueFamilyIndex);
    x->barrier2(cb, d);
}

static VkResult VKAPI_CALL tr_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
    vkb_trace *x = cur();
    for (uint32_t i = 0; i < n; i++)
        if (take(x))
            VKB_INFO("trace pid %d: QueueSubmit %u command buffer(s) (first %llx), %u wait, %u signal semaphore(s), fence %llx", PID(x),
                     s[i].commandBufferCount, s[i].commandBufferCount ? H(s[i].pCommandBuffers[0]) : 0ULL, s[i].waitSemaphoreCount,
                     s[i].signalSemaphoreCount, H(f));
    VkResult r = x->submit(q, n, s, f);
    if (r != VK_SUCCESS) VKB_ERR("trace pid %d: QueueSubmit returned %d", PID(x), r);
    return r;
}

static VkResult VKAPI_CALL tr_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{
    vkb_trace *x = cur();
    for (uint32_t i = 0; i < n; i++)
        if (take(x))
            VKB_INFO("trace pid %d: QueueSubmit2 %u command buffer(s) (first %llx), %u wait, %u signal, fence %llx", PID(x),
                     s[i].commandBufferInfoCount, s[i].commandBufferInfoCount ? H(s[i].pCommandBufferInfos[0].commandBuffer) : 0ULL,
                     s[i].waitSemaphoreInfoCount, s[i].signalSemaphoreInfoCount, H(f));
    VkResult r = x->submit2(q, n, s, f);
    if (r != VK_SUCCESS) VKB_ERR("trace pid %d: QueueSubmit2 returned %d", PID(x), r);
    return r;
}

void vkb_trace_device_init(vkb_srv_table *t)
{
    const char *dir = vkb_pcache_dir();
    char path[600];
    snprintf(path, sizeof(path), "%s/trace", dir ? dir : "");
    if (!dir || access(path, F_OK) != 0 || t->id >= 4096) return;
    vkb_trace *x = calloc(1, sizeof(*x));
    if (!x) return;
    vkb_dispatch *dt = &t->dt;
    x->t = t;
    x->copy = dt->vkCmdCopyImage;
    x->copy2 = dt->vkCmdCopyImage2;
    x->blit = dt->vkCmdBlitImage;
    x->blit2 = dt->vkCmdBlitImage2;
    x->clear = dt->vkCmdClearColorImage;
    x->dispatch = dt->vkCmdDispatch;
    x->barrier = dt->vkCmdPipelineBarrier;
    x->barrier2 = dt->vkCmdPipelineBarrier2;
    x->submit = dt->vkQueueSubmit;
    x->submit2 = dt->vkQueueSubmit2;
    if (x->copy) dt->vkCmdCopyImage = tr_CmdCopyImage;
    if (x->copy2) dt->vkCmdCopyImage2 = tr_CmdCopyImage2;
    if (x->blit) dt->vkCmdBlitImage = tr_CmdBlitImage;
    if (x->blit2) dt->vkCmdBlitImage2 = tr_CmdBlitImage2;
    if (x->clear) dt->vkCmdClearColorImage = tr_CmdClearColorImage;
    if (x->dispatch) dt->vkCmdDispatch = tr_CmdDispatch;
    if (x->barrier) dt->vkCmdPipelineBarrier = tr_CmdPipelineBarrier;
    if (x->barrier2) dt->vkCmdPipelineBarrier2 = tr_CmdPipelineBarrier2;
    if (x->submit) dt->vkQueueSubmit = tr_QueueSubmit;
    if (x->submit2) dt->vkQueueSubmit2 = tr_QueueSubmit2;
    traces[t->id] = x;
    VKB_INFO("device %u: command trace on (first %u commands)", t->id, TRACE_MAX);
}

void vkb_trace_device_destroy(vkb_srv_table *t)
{
    if (t->id >= 4096 || !traces[t->id]) return;
    vkb_trace *x = traces[t->id];
    vkb_dispatch *dt = &t->dt;
    if (x->copy) dt->vkCmdCopyImage = x->copy;
    if (x->copy2) dt->vkCmdCopyImage2 = x->copy2;
    if (x->blit) dt->vkCmdBlitImage = x->blit;
    if (x->blit2) dt->vkCmdBlitImage2 = x->blit2;
    if (x->clear) dt->vkCmdClearColorImage = x->clear;
    if (x->dispatch) dt->vkCmdDispatch = x->dispatch;
    if (x->barrier) dt->vkCmdPipelineBarrier = x->barrier;
    if (x->barrier2) dt->vkCmdPipelineBarrier2 = x->barrier2;
    if (x->submit) dt->vkQueueSubmit = x->submit;
    if (x->submit2) dt->vkQueueSubmit2 = x->submit2;
    traces[t->id] = NULL;
    free(x);
}
