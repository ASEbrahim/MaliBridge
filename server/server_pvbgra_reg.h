/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The register of dma-bufs holding R8G8B8A8 texels for a B8G8R8A8 client image (pv_bgra_store, PV-07;
 * server_pvbgra_reg.c). No Vulkan types: the unit test builds it alone.
 */
#ifndef VKB_SERVER_PVBGRA_REG_H
#define VKB_SERVER_PVBGRA_REG_H

#include <stddef.h>
#include <stdint.h>

#include "../common/vkb_dmabuf_ahb.h" /* VKB_DMABUF_CONTENT_RGBA8, vkb_dmabuf_content_format */

/* 1 where a dma-buf is told by (st_dev, st_ino): Linux 5.3 and later */
int vkb_pvbgra_reg_usable(void);
/* Registers fd's dma-buf as the export of memory mem (any 64-bit name) of device table: 1 registered (or already),
 * 0 not (no fd, an old kernel). key, when given, gets "dev:ino". */
int vkb_pvbgra_reg_add(int fd, uint64_t mem, uint32_t table, char *key, size_t cap);
void vkb_pvbgra_reg_drop_mem(uint64_t mem, uint32_t table);
void vkb_pvbgra_reg_drop_table(uint32_t table);
int vkb_pvbgra_reg_find(int fd);
unsigned vkb_pvbgra_reg_count(void);
/* DMA_BUF_SET_NAME VKB_DMABUF_RGBA8_NAME: 0 or the errno; and whether fd's dma-buf has that name */
int vkb_pvbgra_name(int fd);
int vkb_pvbgra_named(int fd);
/* The compositor's question (default visibility): VKB_DMABUF_CONTENT_RGBA8 for a registered buffer, else 0 */
uint32_t vkb_dmabuf_content_format(int fd);

#endif
