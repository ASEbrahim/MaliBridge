/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The page size, as the bridge's two halves use it. A Pixel's developer option runs a 16 KB-page
 * kernel: mapping lengths round up to the kernel's page, the placed-memory alignment the app is told
 * (VK_EXT_map_memory_placed: Wine places mappings at addresses aligned to it) and the alignment of
 * memory the driver imports from the host (a memfd's size) are at least that.
 *
 *   vkb_page_size()         the kernel's page, never below 4096: what mmap / mremap lengths round to
 *   vkb_report_page_size()  the page an app is told about. The same, except in host builds, where
 *                           VKBRIDGE_FAKE_PAGE_SIZE=16384 (a power of two above the real page) reports a
 *                           bigger one, so a 4 KB host can test what a 16 KB kernel makes of the
 *                           reported alignments. Never used for a length handed to mmap.
 *   vkb_import_align(d)     the driver's minImportedHostPointerAlignment d (0: none), raised to the page
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static inline size_t vkb_page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p >= 4096 ? (size_t)p : 4096;
}

static inline size_t vkb_report_page_size(void)
{
    size_t p = vkb_page_size();
#ifndef __ANDROID__
    const char *f = getenv("VKBRIDGE_FAKE_PAGE_SIZE");
    if (f && *f) {
        unsigned long v = strtoul(f, NULL, 0);
        if (v > p && v <= (1ul << 24) && (v & (v - 1)) == 0) p = v;
    }
#endif
    return p;
}

static inline uint64_t vkb_import_align(uint64_t driver)
{
    uint64_t p = vkb_report_page_size();
    return driver > p ? driver : p;
}
