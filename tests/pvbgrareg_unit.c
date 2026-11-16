/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pvbgrareg_unit: the register of dma-bufs holding R8G8B8A8 texels for a B8G8R8A8 client image (pv_bgra_store,
 * PV-07; server/server_pvbgra_reg.c) on its own. memfds stand in for dma-bufs: both are files with an inode of
 * their own, and every fd of one (dup, another process's received copy) refers to the same open file.
 * Prints one line per check and "OK: n checks" / "FAILED: ..." last.
 */
#define _GNU_SOURCE
#include "server_pvbgra_reg.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fails, checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) fails++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

int main(void)
{
    if (!vkb_pvbgra_reg_usable()) {
        printf("SKIP: kernel before 5.3 (nothing is registered there)\n");
        return 0;
    }
    int a = memfd_create("pvbgra-a", MFD_CLOEXEC), b = memfd_create("pvbgra-b", MFD_CLOEXEC);
    if (a < 0 || b < 0) {
        printf("FAILED: memfd_create\n");
        return 1;
    }
    int a2 = dup(a);
    char key[48];
    check(vkb_dmabuf_content_format(a) == 0 && vkb_pvbgra_reg_count() == 0, "nothing registered: every buffer as its client said (0)");
    check(vkb_pvbgra_reg_add(a, 0x1000, 7, key, sizeof(key)) == 1 && strchr(key, ':'), "a buffer of memory 0x1000, device 7 registered");
    check(vkb_dmabuf_content_format(a2) == VKB_DMABUF_CONTENT_RGBA8, "found by another fd of the same buffer: R8G8B8A8 (37)");
    check(vkb_dmabuf_content_format(b) == 0, "another buffer: not found");
    check(vkb_pvbgra_reg_add(a2, 0x1000, 7, NULL, 0) == 1 && vkb_pvbgra_reg_count() == 1, "the same export again: one entry");
    vkb_pvbgra_reg_drop_mem(0x1000, 8);
    check(vkb_pvbgra_reg_find(a) == 1, "the same memory name on another device: not dropped");
    vkb_pvbgra_reg_drop_mem(0x1000, 7);
    check(vkb_pvbgra_reg_find(a) == 0 && vkb_pvbgra_reg_count() == 0, "freed memory: dropped");
    vkb_pvbgra_reg_add(a, 0x1000, 7, NULL, 0);
    vkb_pvbgra_reg_add(b, 0x2000, 9, NULL, 0);
    vkb_pvbgra_reg_drop_table(7);
    check(vkb_pvbgra_reg_find(a) == 0 && vkb_pvbgra_reg_find(b) == 1, "device 7 ended: its buffers dropped, device 9's kept");
    check(vkb_pvbgra_name(a) != 0 && !vkb_pvbgra_named(a), "a file that is no dma-buf takes no name (the server says so; nothing named)");
    check(vkb_pvbgra_reg_add(-1, 0x3000, 9, key, sizeof(key)) == 0 && key[0] == 0 && vkb_dmabuf_content_format(-1) == 0, "no fd: nothing");
    close(a), close(a2), close(b);
    if (fails) {
        printf("FAILED: %d of %d checks\n", fails, checks);
        return 1;
    }
    printf("OK: %d checks\n", checks);
    return 0;
}
