/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim
 *
 * Implicit sync for dma-bufs on kernels that have none (Linux before 6.0: Android's 5.10 and 5.15).
 *
 * Zink hands a frame to another process (Steam's browser drawing its window over DRI3, Xwayland's
 * window pixmaps) by attaching the batch's end to the dma-buf: right after the submit it exports the
 * batch's signal semaphore as a sync file (vkGetSemaphoreFdKHR, SYNC_FD) and fetches the memory's
 * dma-buf (vkGetMemoryFdKHR) to put that sync file in it with DMA_BUF_IOCTL_IMPORT_SYNC_FILE. Whoever
 * reads the buffer next (Xwayland, gamescope, the app's compositor) waits on the dma-buf's fences.
 * Those two ioctls came with Linux 6.0. On a 5.x kernel the import fails (ENOTTY, unchecked in a
 * release build), the dma-buf carries no fence, and the reader takes the buffer while the GPU is
 * still drawing it: parts of the frame missing at random, a whole frame near-black at times
 * (Galaxy Tab S9 FE+, kernel 5.15; the Kirin 9000's 5.10).
 * Mali's own driver attaches nothing to an imported dma-buf either.
 *
 * Here: when a dma-buf export follows a sync-file export on the same thread (Zink's pattern) and the
 * kernel has no DMA_BUF_IOCTL_EXPORT_SYNC_FILE, the dma-buf export returns only once that sync
 * file has signalled, i.e. once the batch that drew the frame is done on the GPU. Zink's flush (the
 * one before the frame goes to the X server) waits for this, so the frame leaves finished. Waits are
 * bounded (VKBRIDGE_DMABUF_SYNC_MS, default 1000; 0 off), so a batch that never ends cannot stop the
 * app. A sync-file export that no dma-buf export follows (an app's own explicit sync) is not waited
 * on: the next submit on the thread drops it. On kernels with the ioctls nothing changes.
 *
 * VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1 (tests, and a device check on a 6.x kernel): the kernel is taken
 * as one without the ioctls, and the app gets fd -1 from such sync-file exports ("already
 * signalled", which the spec allows), so its own ioctl attaches nothing: a 5.x kernel's behaviour.
 * Both switches also come from the server (<cache dir>/env, VKB_KNOB_DMABUF_SYNC_*); the guest's
 * own environment wins.
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "gen/vkb_client_gen.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* <linux/dma-buf.h> of 6.0, not in every header set the client is built with */
struct vkb_dma_buf_export_sync_file {
    uint32_t flags;
    int32_t fd;
};
#define VKB_DMA_BUF_SYNC_READ (1u << 0)
#define VKB_DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct vkb_dma_buf_export_sync_file)

#define DS_DEFAULT_MS 1000

static __thread int ds_pending = -1; /* the thread's last sync-file export (a dup), not yet paired */

/* -1: not known yet, 0: the kernel has the sync-file ioctls, 1: it has not (or faked) */
static int ds_old_kernel = -1;
static uint64_t ds_waits, ds_wait_ns, ds_wait_max_ns, ds_timeouts;

static uint64_t ds_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The wait's bound in ms, 0: off */
static int ds_ms(void)
{
    static int v = -2;
    int cur = __atomic_load_n(&v, __ATOMIC_RELAXED);
    if (cur != -2) return cur;
    int known, ms = DS_DEFAULT_MS;
    const char *e = getenv("VKBRIDGE_DMABUF_SYNC_MS");
    if (e && *e) {
        long l = strtol(e, NULL, 10);
        ms = l < 0 ? 0 : l > 60000 ? 60000 : (int)l;
        known = 1;
    } else {
        int k = vkb_client_knob(VKB_KNOB_DMABUF_SYNC_SHIFT, &known);
        if (k == 0) ms = 0;
    }
    if (known) __atomic_store_n(&v, ms, __ATOMIC_RELAXED);
    return ms;
}

static int ds_fake_old(void)
{
    static int v = -1;
    int cur = __atomic_load_n(&v, __ATOMIC_RELAXED);
    if (cur >= 0) return cur;
    int known = 1, r;
    const char *e = getenv("VKBRIDGE_DMABUF_SYNC_FAKE_OLD");
    if (e && *e) r = strcmp(e, "0") != 0;
    else r = vkb_client_knob(VKB_KNOB_DMABUF_SYNC_FAKE_SHIFT, &known) == 1;
    if (known) __atomic_store_n(&v, r, __ATOMIC_RELAXED);
    return r;
}

/* Asks the kernel once, on a real dma-buf: DMA_BUF_IOCTL_EXPORT_SYNC_FILE of its read fences. */
static int ds_kernel_is_old(int dmabuf_fd)
{
    int v = __atomic_load_n(&ds_old_kernel, __ATOMIC_ACQUIRE);
    if (v >= 0) return v;
    if (ds_fake_old()) {
        v = 1;
    } else {
        struct vkb_dma_buf_export_sync_file x = {VKB_DMA_BUF_SYNC_READ, -1};
        int r;
        do r = ioctl(dmabuf_fd, VKB_DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &x);
        while (r < 0 && errno == EINTR);
        if (r == 0) {
            if (x.fd >= 0) close(x.fd);
            v = 0;
        } else if (errno == ENOTTY || errno == EINVAL || errno == ENOSYS) {
            v = 1;
        } else {
            return 0; /* not a dma-buf after all, or a passing error: ask again next time */
        }
    }
    int was = -1;
    if (__atomic_compare_exchange_n(&ds_old_kernel, &was, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && v)
        VKB_WARN("emulation: implicit sync for dma-bufs: the kernel has no dma-buf sync files%s (Linux before 6.0), so a frame "
                 "this process hands to another (Xwayland, gamescope) is finished on the GPU before it goes "
                 "(VKBRIDGE_DMABUF_SYNC_MS=0 turns it off)", ds_fake_old() ? " (VKBRIDGE_DMABUF_SYNC_FAKE_OLD)" : "");
    return __atomic_load_n(&ds_old_kernel, __ATOMIC_ACQUIRE);
}

static void ds_drop(void)
{
    if (ds_pending >= 0) close(ds_pending);
    ds_pending = -1;
}

void vkb_dmabuf_sync_submit(void)
{
    ds_drop();
}

void vkb_dmabuf_sync_semaphore_export(const VkSemaphoreGetFdInfoKHR *info, int *pFd)
{
    if (!info || info->handleType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT || !pFd || *pFd < 0) return;
    if (!ds_ms() || __atomic_load_n(&ds_old_kernel, __ATOMIC_ACQUIRE) == 0) return;
    ds_drop();
    ds_pending = fcntl(*pFd, F_DUPFD_CLOEXEC, 0);
    if (ds_pending >= 0 && ds_fake_old()) {
        close(*pFd); /* a 5.x kernel's behaviour: the app's own attach finds nothing to attach */
        *pFd = -1;
    }
}

void vkb_dmabuf_sync_memory_export(const VkMemoryGetFdInfoKHR *info, int fd)
{
    if (ds_pending < 0) return;
    if (!info || info->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT || fd < 0) return;
    int ms = ds_ms();
    if (!ms || !ds_kernel_is_old(fd)) {
        ds_drop();
        return;
    }
    uint64_t t0 = ds_now_ns(), end = t0 + (uint64_t)ms * 1000000ull;
    struct pollfd p = {ds_pending, POLLIN, 0};
    int r;
    for (;;) {
        uint64_t now = ds_now_ns();
        int left = now >= end ? 0 : (int)((end - now + 999999) / 1000000);
        r = poll(&p, 1, left);
        if (r >= 0 || errno != EINTR) break;
    }
    uint64_t d = ds_now_ns() - t0;
    uint64_t n = __atomic_add_fetch(&ds_waits, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ds_wait_ns, d, __ATOMIC_RELAXED);
    uint64_t mx = __atomic_load_n(&ds_wait_max_ns, __ATOMIC_RELAXED);
    while (d > mx && !__atomic_compare_exchange_n(&ds_wait_max_ns, &mx, d, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (r == 0) {
        uint64_t t = __atomic_add_fetch(&ds_timeouts, 1, __ATOMIC_RELAXED);
        if (t <= 4 || (t & (t - 1)) == 0)
            VKB_WARN("implicit sync: a frame's GPU work was not done after %d ms; handed over unfinished (%llu so far; "
                     "VKBRIDGE_DMABUF_SYNC_MS)", ms, (unsigned long long)t);
    }
    if (n == 1 || (n & (n - 1)) == 0)
        VKB_INFO("implicit sync: %llu frame(s) finished before being handed over, %.2f ms on average, %.2f ms at most, %llu not in time",
                 (unsigned long long)n, (double)__atomic_load_n(&ds_wait_ns, __ATOMIC_RELAXED) / 1e6 / (double)n,
                 (double)__atomic_load_n(&ds_wait_max_ns, __ATOMIC_RELAXED) / 1e6,
                 (unsigned long long)__atomic_load_n(&ds_timeouts, __ATOMIC_RELAXED));
    ds_drop();
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetSemaphoreFdKHR(VkDevice device, const VkSemaphoreGetFdInfoKHR *pGetFdInfo, int *pFd)
{
    VkResult r = vkb_wire_vkGetSemaphoreFdKHR(device, pGetFdInfo, pFd);
    if (r == VK_SUCCESS) vkb_dmabuf_sync_semaphore_export(pGetFdInfo, pFd);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *pGetFdInfo, int *pFd)
{
    VkResult r = vkb_wire_vkGetMemoryFdKHR(device, pGetFdInfo, pFd);
    if (r == VK_SUCCESS && pFd) vkb_dmabuf_sync_memory_export(pGetFdInfo, *pFd);
    return r;
}
