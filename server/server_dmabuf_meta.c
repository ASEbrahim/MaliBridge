/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MaliBridge - server side (bionic on the device, glibc for host tests).
 *
 * Why this register exists (Mali-G2-Ultra-NX MC16, driver r56, 2026-11-08): Zink exported a
 * 1668x2173 BGRA8 dma-buf at DRM-modifier tiling. The driver chose the AFBC modifier
 * 0x0820000000000101 and gave it 3,760,128 bytes (1728x2176 at 8bpp, the 16x16 superblocks'
 * payload), and vkGetImageSubresourceLayout answered offset 0, rowPitch 27,648 (108 superblocks
 * x 256 bytes), size 3,760,128. The same process then imported the dma-buf into an image that
 * named the LINEAR modifier (0) with that rowPitch: Mali's explicit-layout path read the pitch
 * as a pixel row's and asked 27,648 x 2172 + 6,672 = 60,058,128 bytes of a 3,760,128-byte
 * dma-buf, and vkAllocateMemory failed with VK_ERROR_INVALID_EXTERNAL_HANDLE (-1000072003);
 * Zink gave up on the surface and Gecko fell back to wl_shm. An explicit image at the AFBC
 * modifier with the full layout is refused outright (VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_
 * PLANE_LAYOUT_EXT); at that modifier with the rowPitch alone it is laid out exactly as the
 * export (3,760,128 bytes, same layout).
 *
 * No API the server answers can tell an importer the modifier when its caller names LINEAR, so
 * the layout is what identifies the export. The register holds the few dozen newest exports:
 * a layout that matches one is that export's, and the image can be made at the export's modifier
 * without the explicit plane layout (server_emu_layer.c, import_modifier_rescue).
 */
#include "server_dmabuf_meta.h"

#include <pthread.h>
#include <string.h>

#define META_SHAPES 64

static struct {
    pthread_mutex_t lock;
    vkb_meta_shape s[META_SHAPES];
    unsigned next;
} meta = {.lock = PTHREAD_MUTEX_INITIALIZER};

void vkb_meta_note(const vkb_meta_shape *s)
{
    if (!s || !s->width || !s->height || !s->layout.rowPitch) return;
    pthread_mutex_lock(&meta.lock);
    meta.s[meta.next++ % META_SHAPES] = *s;
    pthread_mutex_unlock(&meta.lock);
}

uint64_t vkb_meta_match(const vkb_meta_shape *s)
{
    uint64_t mod = 0;
    if (!s || !s->width || !s->height || !s->layout.rowPitch) return 0;
    pthread_mutex_lock(&meta.lock);
    /* newest first: a later export of the same shape is the one an importer has seen */
    for (unsigned i = 0; i < META_SHAPES; i++) {
        const vkb_meta_shape *e = &meta.s[(meta.next + META_SHAPES - 1 - i) % META_SHAPES];
        if (!e->width) continue;
        if (e->format != s->format || e->width != s->width || e->height != s->height || e->mips != s->mips ||
            e->layers != s->layers)
            continue;
        if (e->layout.offset != s->layout.offset || e->layout.rowPitch != s->layout.rowPitch) continue;
        if (s->layout.size && e->layout.size != s->layout.size) continue;
        if (!e->modifier) continue; /* a LINEAR export: the importer's LINEAR is the truth */
        mod = e->modifier;
        break;
    }
    pthread_mutex_unlock(&meta.lock);
    return mod;
}

void vkb_meta_reset(void)
{
    pthread_mutex_lock(&meta.lock);
    memset(meta.s, 0, sizeof(meta.s));
    meta.next = 0;
    pthread_mutex_unlock(&meta.lock);
}
