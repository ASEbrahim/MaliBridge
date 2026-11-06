/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 ASEbrahim and MaliBridge contributors */
#ifndef VKB_SERVER_DSHAPE_H
#define VKB_SERVER_DSHAPE_H

#include <stdint.h>

/* The shapes of dma-buf images made without the driver's handle type (server_dmabuf_shape.c): an importer that
 * names no modifier is laid out like its LINEAR exporter. */
typedef struct vkb_dshape {
    const void *owner; /* the vkb_emu_device */
    uint64_t image;    /* 0: empty slot */
    uint32_t format, width, height;
    int linear;        /* LINEAR tiling (else OPTIMAL) */
    uint64_t size;     /* the image's memory requirement */
    uint64_t pitch;    /* LINEAR: its row pitch */
} vkb_dshape;

void vkb_dshape_note(const vkb_dshape *s);
void vkb_dshape_forget(const void *owner, uint64_t image);
void vkb_dshape_forget_owner(const void *owner);
/* 1 and *out the LINEAR export to lay an OPTIMAL image of this format and size (optimal_size bytes) out like: one that the
 * OPTIMAL image does not fit in, with no OPTIMAL export of the shape live; 0 otherwise. */
int vkb_dshape_linear_export(uint32_t format, uint32_t width, uint32_t height, uint64_t optimal_size, vkb_dshape *out);

#endif
