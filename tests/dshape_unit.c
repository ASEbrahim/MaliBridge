/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dshape_unit: the register of dma-buf image shapes (server/server_dmabuf_shape.c), built alone. It decides
 * whether an OPTIMAL importer that names no modifier is laid out like a LINEAR exporter (Mali r49p1,
 * gamescope's 1564x720 imports of Steam's windows asked 5193728 bytes of dma-bufs of 4558848).
 *   1 the a player log numbers: three LINEAR exports of 1564x720, an OPTIMAL image needing 5193728: answered
 *   2 another format, another size: not answered
 *   3 an OPTIMAL image that fits in the LINEAR export (it works today): not answered
 *   4 an OPTIMAL export of the same shape live: not answered; gone with its image, answered again
 *   5 LINEAR exports of the shape that disagree on the pitch: not answered
 *   6 the newest of equal exports is the one given; a destroyed image is forgotten
 *   7 a device's images go with it; another device's stay
 * Prints PASS/FAIL lines and "OK: 0 failures" / "FAILED: n failures".
 */
#include "server_dshape.h"

#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static int dev_a, dev_b;
static vkb_dshape mk(const void *owner, uint64_t image, uint32_t fmt, uint32_t w, uint32_t h, int linear, uint64_t size, uint64_t pitch)
{
    vkb_dshape s = {owner, image, fmt, w, h, linear, size, pitch};
    return s;
}

int main(void)
{
    vkb_dshape out, s;
    memset(&out, 0, sizeof(out));

    /* 1 */
    for (uint64_t i = 1; i <= 3; i++) {
        s = mk(&dev_a, 0x1000 + i, 44, 1564, 720, 1, 4558848, 6272);
        vkb_dshape_note(&s);
    }
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 1, "1 5193728 asked, LINEAR exports of 4558848 live: answered");
    check(out.linear && out.size == 4558848 && out.pitch == 6272, "1 the answer is the LINEAR export's size and pitch");

    /* 2 */
    check(vkb_dshape_linear_export(37, 1564, 720, 5193728, &out) == 0, "2 another format: not answered");
    check(vkb_dshape_linear_export(44, 1564, 721, 5193728, &out) == 0, "2 another height: not answered");
    check(vkb_dshape_linear_export(44, 1280, 720, 5193728, &out) == 0, "2 another width: not answered");

    /* 3 */
    check(vkb_dshape_linear_export(44, 1564, 720, 4558848, &out) == 0, "3 the OPTIMAL image just fits: not answered (the import works as it is)");
    check(vkb_dshape_linear_export(44, 1564, 720, 4000000, &out) == 0, "3 the OPTIMAL image is smaller: not answered");

    /* 4 */
    s = mk(&dev_a, 0x2001, 44, 1564, 720, 0, 5193728, 0);
    vkb_dshape_note(&s);
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 0, "4 an OPTIMAL export of the shape is live: not answered (it may be what is imported)");
    vkb_dshape_forget(&dev_a, 0x2001);
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 1, "4 the OPTIMAL one destroyed: answered again");
    vkb_dshape_forget(&dev_b, 0x1001); /* another device's image of the same handle: nothing */
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 1, "4 a forget by another owner changes nothing");

    /* 5 */
    s = mk(&dev_a, 0x3001, 44, 1564, 720, 1, 4558848, 6400);
    vkb_dshape_note(&s);
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 0, "5 LINEAR exports that disagree on the pitch: not answered");
    vkb_dshape_forget(&dev_a, 0x3001);
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 1, "5 the odd one destroyed: answered");

    /* 6 */
    s = mk(&dev_a, 0x4001, 44, 1564, 720, 1, 4559872, 6272);
    vkb_dshape_note(&s);
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 1 && out.image == 0x4001, "6 equal pitch, a newer export: the newest is given");
    vkb_dshape_forget(&dev_a, 0x4001);
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 1 && out.size == 4558848, "6 the newest destroyed: an older one is given");

    /* 7 */
    s = mk(&dev_b, 0x5001, 44, 800, 600, 1, 1949696, 3200);
    vkb_dshape_note(&s);
    vkb_dshape_forget_owner(&dev_a);
    check(vkb_dshape_linear_export(44, 1564, 720, 5193728, &out) == 0, "7 the device destroyed: its images are gone");
    check(vkb_dshape_linear_export(44, 800, 600, 2200000, &out) == 1, "7 another device's image stays");
    vkb_dshape_forget_owner(&dev_b);
    check(vkb_dshape_linear_export(44, 800, 600, 2200000, &out) == 0, "7 and goes with its device");

    /* the register's slots are reused: 200 images in, the newest 128 kept, none broken */
    for (uint64_t i = 1; i <= 200; i++) {
        s = mk(&dev_a, 0x9000 + i, 44, 640, 480, 1, 1000000 + i, 2560);
        vkb_dshape_note(&s);
    }
    check(vkb_dshape_linear_export(44, 640, 480, 2000000, &out) == 1 && out.image == 0x9000 + 200, "8 slots reused: the newest export is given");

    printf("%s %d failures\n", fails ? "FAILED:" : "OK:", fails);
    return fails ? 1 : 0;
}
