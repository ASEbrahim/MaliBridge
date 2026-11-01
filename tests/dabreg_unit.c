/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dabreg_unit: the register of dma-bufs handed out over AHardwareBuffers (server/server_dab_reg.c),
 * built alone. Regular files stand in for dma-bufs: two open()s of one file are two open files on
 * one inode, as two dma-bufs are on a kernel before 5.3, where every dma-buf shares one inode.
 *   1 an fd is found by any dup of it, another file is not
 *   2 one inode for all (the kernel before 5.3): a dup found, the other open file of that inode not
 *   3 two entries on one inode switch the shared-inode test on by themselves
 *   4 references: the pages (fd, buffer) go with the last one; the buffer released once
 *   5 the compositor's question: ABI and size checked, the image described, a reference lent
 * Prints PASS/FAIL lines and "OK: 0 failures" / "FAILED: n failures".
 */
#define _GNU_SOURCE
#include "server_dab.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static int acquired, released;
static void acq(void *a) { (void)a; acquired++; }
static void rel(void *a) { (void)a; released++; }


int main(void)
{
    char path[] = "/tmp/dabreg-XXXXXX";
    int base = mkstemp(path);
    if (base < 0) return 2;
    if (write(base, "pages", 5) != 5) return 2;
    char path2[] = "/tmp/dabreg2-XXXXXX";
    int other_file = mkstemp(path2);
    dab_reg_set_ahb_ops(acq, rel);

    /* 1 */
    int a = open(path, O_RDWR);
    int a_dup = dup(a);
    vkb_dmabuf_ahb_info img;
    memset(&img, 0, sizeof(img));
    img.has_image = 1;
    img.format = 44;
    img.width = 320;
    img.height = 200;
    img.tiling = 1;
    img.row_pitch = 1280;
    static int buf_a;
    dab_entry *ea = dab_reg_add(&buf_a, a, 65536, &img);
    check(ea != NULL, "1 an entry made");
    dab_entry *f = dab_reg_find(a_dup);
    check(f == ea, "1 found through a dup");
    dab_reg_put(f);
    check(dab_reg_find(other_file) == NULL, "1 another file not found");
    check(!dab_reg_shared_inode(), "1 one entry: no shared inode noticed");

    /* 2: another open file of the same inode, as a second dma-buf before Linux 5.3 */
    int b = open(path, O_RDWR);
    check(dab_reg_find(b) == ea, "2 (per-inode kernel) the same inode is the same dma-buf");
    dab_reg_put(ea);
    dab_reg_note_shared_inode(1);
    check(dab_reg_find(b) == NULL, "2 (shared inode) another open file of that inode not found");
    f = dab_reg_find(a_dup);
    check(f == ea, "2 (shared inode) a dup still found");
    dab_reg_put(f);
    check(dab_same_file(a, a_dup) == 1 && dab_same_file(a, b) == 0, "2 dab_same_file: dup yes, other open file no");
    dab_reg_note_shared_inode(0);

    /* 3 */
    static int buf_b;
    dab_entry *eb = dab_reg_add(&buf_b, b, 4096, NULL);
    check(eb && dab_reg_shared_inode(), "3 two entries on one inode: shared inode noticed");
    f = dab_reg_find(dup(b));
    check(f == eb, "3 the second entry found by its own dup");
    dab_reg_put(f);
    f = dab_reg_find(a_dup);
    check(f == ea, "3 the first entry still found by its dup");
    dab_reg_put(f);
    check(dab_reg_count() == 2, "3 two entries");

    /* 4 */
    dab_reg_ref(ea);
    dab_reg_put(ea);
    check(dab_reg_count() == 2 && released == 0, "4 a reference less: still there");
    dab_reg_put(eb);
    check(dab_reg_count() == 1 && released == 1 && fcntl(b, F_GETFD) < 0, "4 the last reference: fd closed, buffer released");

    /* 5 */
    vkb_dmabuf_ahb_info q;
    memset(&q, 0, sizeof(q));
    q.abi = VKB_DMABUF_AHB_ABI + 1;
    q.struct_size = sizeof(q);
    check(vkb_dmabuf_ahb_lookup(a_dup, &q) == -1, "5 another ABI refused");
    q.abi = VKB_DMABUF_AHB_ABI;
    q.struct_size = sizeof(q) - 8;
    check(vkb_dmabuf_ahb_lookup(a_dup, &q) == -1, "5 a smaller struct refused");
    q.struct_size = sizeof(q);
    check(vkb_dmabuf_ahb_lookup(other_file, &q) == 0, "5 not one of the bridge's: 0");
    int before = acquired;
    int r = vkb_dmabuf_ahb_lookup(a_dup, &q);
    check(r == 1 && q.ahb == &buf_a && q.bytes == 65536 && q.has_image && q.width == 320 && q.height == 200 && q.format == 44 &&
              q.row_pitch == 1280 && q.abi == VKB_DMABUF_AHB_ABI && q.struct_size == sizeof(q) && acquired == before + 1,
          "5 found: buffer, size and image as recorded, one reference lent");
    dab_reg_put(ea);
    check(dab_reg_count() == 0 && released == 2, "5 the first entry gone with its last reference");
    unlink(path);
    unlink(path2);
    if (fails) printf("FAILED: %d failures\n", fails);
    else printf("OK: 0 failures\n");
    return fails != 0;
}
