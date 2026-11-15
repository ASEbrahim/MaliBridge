/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Puts a write fence into a dma-buf (DMA_BUF_IOCTL_IMPORT_SYNC_FILE, Linux 6.0): whoever reads the buffer next waits for
 * it (gamescope polls the dma-buf, DroidDeck's compositor exports the fence as its layer's acquire fence). The
 * presenting thread's early commit (VKBRIDGE_PRESENT_EARLY, wsi_wayland.c) rests on this.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>

struct vkb_dma_buf_import_sync_file {
    uint32_t flags;
    int32_t fd;
};
#define VKB_DMA_BUF_SYNC_WRITE (1u << 1)
#define VKB_DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW('b', 3, struct vkb_dma_buf_import_sync_file)

int vkb_dmabuf_attach_write_fence(int dmabuf_fd, int sync_fd); /* as in vkb_client.h; this file stands alone for the host test */

/* 0 when the sync file is now a write fence of the dma-buf; else the errno (ENOTTY: no such ioctl, EACCES/EPERM: not allowed here). */
int vkb_dmabuf_attach_write_fence(int dmabuf_fd, int sync_fd)
{
    struct vkb_dma_buf_import_sync_file x = {VKB_DMA_BUF_SYNC_WRITE, sync_fd};
    int r;
    do r = ioctl(dmabuf_fd, VKB_DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &x);
    while (r < 0 && errno == EINTR);
    return r < 0 ? errno : 0;
}
