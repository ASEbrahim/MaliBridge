/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dbmeta_unit: the register of dma-buf export layouts (server/server_dmabuf_meta.c), built alone.
 * It answers an importer that passes back an export's plane layout, whatever modifier it names
 * (Mali r56: a 1668x2173 BGRA8 AFBC export laid out at row pitch 27,648 / 3,760,128 bytes was
 * imported as LINEAR with that pitch, and Mali asked 60,058,128 bytes of the dma-buf).
 *   1 an export's layout matches, with the size given or left 0
 *   2 another size, pitch or offset: not answered
 *   3 another format, extent, mips or layers: not answered
 *   4 a LINEAR export is never answered (the importer's LINEAR is the truth)
 *   5 the newest export of a shape is the one given
 *   6 the register keeps the newest 64: the oldest is forgotten
 * Prints PASS/FAIL lines and "OK: 0 failures" / "FAILED: n failures".
 */
#include "server_dmabuf_meta.h"

#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

#define AFBC 0x0820000000000101ull
#define OTHER 0x0820000000000102ull

static vkb_meta_shape mk(uint32_t fmt, uint32_t w, uint32_t h, uint64_t mod, uint64_t off, uint64_t pitch, uint64_t size)
{
    vkb_meta_shape s;
    memset(&s, 0, sizeof(s));
    s.format = fmt;
    s.width = w;
    s.height = h;
    s.mips = 1;
    s.layers = 1;
    s.modifier = mod;
    s.layout.offset = off;
    s.layout.rowPitch = pitch;
    s.layout.size = size;
    return s;
}

/* One call at a time, as the checks below use it (C has no address of a returned struct). */
static vkb_meta_shape *q(uint32_t fmt, uint32_t w, uint32_t h, uint64_t mod, uint64_t off, uint64_t pitch, uint64_t size)
{
    static vkb_meta_shape s;
    s = mk(fmt, w, h, mod, off, pitch, size);
    return &s;
}

int main(void)
{
    vkb_meta_reset();
    vkb_meta_note(q(44, 1668, 2173, AFBC, 0, 27648, 3760128));

    /* 1 */
    check(vkb_meta_match(q(44, 1668, 2173, 0, 0, 27648, 0)) == AFBC, "the export's layout is answered (size 0)");
    check(vkb_meta_match(q(44, 1668, 2173, 0, 0, 27648, 3760128)) == AFBC, "the export's layout is answered (size given)");

    /* 2 */
    check(vkb_meta_match(q(44, 1668, 2173, 0, 0, 27648, 3760129)) == 0, "another size is not");
    check(vkb_meta_match(q(44, 1668, 2173, 0, 0, 6720, 0)) == 0, "a linear pitch is not");
    check(vkb_meta_match(q(44, 1668, 2173, 0, 4096, 27648, 0)) == 0, "another offset is not");

    /* 3 */
    check(vkb_meta_match(q(43, 1668, 2173, 0, 0, 27648, 0)) == 0, "another format is not");
    check(vkb_meta_match(q(44, 1668, 2172, 0, 0, 27648, 0)) == 0, "another extent is not");
    vkb_meta_shape m = mk(44, 1668, 2173, 0, 0, 27648, 0);
    m.mips = 2;
    check(vkb_meta_match(&m) == 0, "another mip count is not");
    m = mk(44, 1668, 2173, 0, 0, 27648, 0);
    m.layers = 2;
    check(vkb_meta_match(&m) == 0, "another layer count is not");

    /* 4 */
    vkb_meta_reset();
    vkb_meta_note(q(44, 1668, 2173, 0, 0, 6720, 14602560));
    check(vkb_meta_match(q(44, 1668, 2173, 0, 0, 6720, 0)) == 0, "a LINEAR export is not answered");
    vkb_meta_note(q(44, 1668, 2173, AFBC, 0, 27648, 3760128));
    check(vkb_meta_match(q(44, 1668, 2173, 0, 0, 27648, 0)) == AFBC, "the compressed export of the shape is");

    /* 5 */
    vkb_meta_note(q(44, 1668, 2173, OTHER, 0, 27648, 3760128));
    check(vkb_meta_match(q(44, 1668, 2173, 0, 0, 27648, 0)) == OTHER, "the newest export of the shape is given");

    /* 6 */
    vkb_meta_reset();
    vkb_meta_note(q(44, 1668, 2173, AFBC, 0, 27648, 3760128));
    for (unsigned i = 0; i < 64; i++) vkb_meta_note(q(44, 100 + i, 100, OTHER, 0, 1000, 1000));
    check(vkb_meta_match(q(44, 1668, 2173, 0, 0, 27648, 0)) == 0, "the oldest export is forgotten");
    check(vkb_meta_match(q(44, 163, 100, 0, 0, 1000, 0)) == OTHER, "the newest is still answered");

    printf(fails ? "FAILED: %d failures\n" : "OK: 0 failures\n", fails);
    return fails != 0;
}
