/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The shapes of the dma-buf images the server has made without the driver's dma-buf handle type
 * (server_emu_layer.c), kept so that an importer can be laid out like its exporter.
 *
 * Why: a driver that refuses dma-buf images at plain tiling (Mali r32p1, r44p1, r49p1) gets its
 * exports from the bridge's own pages, and an exporter that asked for LINEAR tiling (Steam's
 * Chromium) has a dma-buf of the LINEAR image's size. An importer that names no modifier (gamescope
 * on a driver without VK_EXT_image_drm_format_modifier) asks for OPTIMAL tiling with an implicit
 * layout, and Mali sizes that larger than the LINEAR dma-buf (r49p1, 1564x720 BGRA8: 5193728 bytes
 * against 4558848, VK_ERROR_INVALID_EXTERNAL_HANDLE, Steam's window never shown). At image creation
 * the dma-buf is not known, so the register says whether a live LINEAR export of the importer's
 * format and size exists that the OPTIMAL image cannot fit in, and whether anything else of that
 * shape (an OPTIMAL export) could be what is imported. Answered only when the first holds and the
 * second does not: every import that works today is left as it is.
 * No Vulkan here: the unit test (tests/dshape_unit.c) builds it alone.
 */
#include "server_dshape.h"

#include <pthread.h>
#include <string.h>

#define SHAPES 128

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static vkb_dshape slot[SHAPES];
static uint64_t seq; /* the order slots were noted in */
static uint64_t order[SHAPES];
static int live; /* slots in use: a destroy with none live takes no lock */

void vkb_dshape_note(const vkb_dshape *s)
{
    pthread_mutex_lock(&lock);
    int pick = 0;
    for (int i = 0; i < SHAPES; i++) {
        if (!slot[i].image) {
            pick = i;
            break;
        }
        if (order[i] < order[pick]) pick = i;
    }
    if (!slot[pick].image) __atomic_fetch_add(&live, 1, __ATOMIC_RELAXED);
    slot[pick] = *s;
    order[pick] = ++seq;
    pthread_mutex_unlock(&lock);
}

void vkb_dshape_forget(const void *owner, uint64_t image)
{
    if (!__atomic_load_n(&live, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SHAPES; i++)
        if (slot[i].image == image && slot[i].owner == owner) memset(&slot[i], 0, sizeof(slot[i])), __atomic_fetch_sub(&live, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&lock);
}

void vkb_dshape_forget_owner(const void *owner)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SHAPES; i++)
        if (slot[i].image && slot[i].owner == owner) memset(&slot[i], 0, sizeof(slot[i])), __atomic_fetch_sub(&live, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&lock);
}

int vkb_dshape_linear_export(uint32_t format, uint32_t width, uint32_t height, uint64_t optimal_size, vkb_dshape *out)
{
    int found = -1, other = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SHAPES; i++) {
        const vkb_dshape *s = &slot[i];
        if (!s->image || s->format != format || s->width != width || s->height != height) continue;
        if (!s->linear) {
            other = 1; /* an OPTIMAL export of this shape: the import may be of that one */
            break;
        }
        if (s->size >= optimal_size) continue; /* the OPTIMAL image fits in it: an import of it works as it is */
        if (found < 0) {
            found = i;
        } else if (s->pitch != slot[found].pitch) {
            other = 1; /* LINEAR exports of this shape that disagree on the layout: not told apart */
            break;
        } else if (order[i] > order[found]) {
            found = i;
        }
    }
    int ok = found >= 0 && !other;
    if (ok) *out = slot[found];
    pthread_mutex_unlock(&lock);
    return ok;
}
