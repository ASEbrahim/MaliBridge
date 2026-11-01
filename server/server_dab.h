/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dma-buf over AHardwareBuffers (VKB_EMU_DMABUF_AHB): the register of the pages the bridge handed
 * out as dma-bufs (server_dab_reg.c, no Vulkan in it) and the emulation (server_dmabuf_ahb.c).
 */
#ifndef SERVER_DAB_H
#define SERVER_DAB_H

#include <stdint.h>
#include "vkb_dmabuf_ahb.h"

/* ------------------------------------------------------------------ the register */

typedef struct dab_entry dab_entry;

/* How the register holds and lends the AHardwareBuffers (Android; NULL on the host). */
void dab_reg_set_ahb_ops(void (*acquire)(void *ahb), void (*release)(void *ahb));
/* New pages: the entry owns ahb (one reference) and fd (the pages' dma-buf); refs = 1. image may be
 * NULL (a buffer, or memory for no one image). NULL when fd cannot be described (fstat). */
dab_entry *dab_reg_add(void *ahb, int fd, uint64_t bytes, const vkb_dmabuf_ahb_info *image);
/* The entry whose dma-buf fd is (the same open file, whatever the fd number), with one more
 * reference; NULL when fd is not one of the register's. */
dab_entry *dab_reg_find(int fd);
void dab_reg_ref(dab_entry *e);
void dab_reg_put(dab_entry *e); /* the last reference frees the pages */
int dab_reg_fd(const dab_entry *e);
void *dab_reg_ahb(const dab_entry *e);
uint64_t dab_reg_bytes(const dab_entry *e);
const vkb_dmabuf_ahb_info *dab_reg_image(const dab_entry *e); /* has_image 0 when none */
unsigned dab_reg_count(void);
/* 1 when the kernel gives every dma-buf one inode (Linux before 5.3: Android 10-11 Kirin phones):
 * two fds are then told apart by their open file (flock), not by the inode. */
int dab_reg_shared_inode(void);
void dab_reg_note_shared_inode(int shared); /* the start's check saw it; tests force it */
/* 1 when fds a and b are the same open file (the same dma-buf). */
int dab_same_file(int a, int b);

/* The compositor's question (vkb_dmabuf_ahb.h). */
int vkb_dmabuf_ahb_lookup(int fd, vkb_dmabuf_ahb_info *out);

#endif
