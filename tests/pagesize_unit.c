/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pagesize_unit: common/vkb_page.h, the page size the bridge's two halves use (a Pixel's 16 KB-page
 * developer option): the kernel's page for lengths; the page an app is told about, which a host build
 * can raise with VKBRIDGE_FAKE_PAGE_SIZE to stand in for a 16 KB kernel; the import alignment raised
 * to it. No driver.
 */
#include "vkb_page.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
static void check(int ok, const char *what, unsigned long a, unsigned long b)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s (%lu, %lu)\n", ok ? "PASS" : "FAIL", what, a, b);
}

int main(void)
{
    const size_t real = vkb_page_size();
    unsetenv("VKBRIDGE_FAKE_PAGE_SIZE");
    check(real >= 4096 && (real & (real - 1)) == 0, "the kernel's page is a power of two, 4096 or more", real, 0);
    check(vkb_report_page_size() == real, "no fake: the page told is the real one", vkb_report_page_size(), real);
    check(vkb_import_align(0) == real && vkb_import_align(4096) == real, "no driver alignment or 4096: the page", vkb_import_align(0), real);
    check(vkb_import_align(real * 4) == real * 4, "a driver alignment above the page is kept", vkb_import_align(real * 4), real * 4);
    /* a 16 KB kernel on this host */
    setenv("VKBRIDGE_FAKE_PAGE_SIZE", "16384", 1);
    const size_t fake = real > 16384 ? real : 16384;
    check(vkb_report_page_size() == fake, "VKBRIDGE_FAKE_PAGE_SIZE=16384: told 16384 (or the real page, if bigger)", vkb_report_page_size(), fake);
    check(vkb_page_size() == real, "the page for mmap lengths stays the real one", vkb_page_size(), real);
    check(vkb_import_align(0) == fake && vkb_import_align(4096) == fake, "the import alignment follows the page told", vkb_import_align(4096), fake);
    check(vkb_import_align(65536) == 65536, "a larger driver alignment still wins", vkb_import_align(65536), 65536);
    /* values that mean nothing are ignored */
    const char *bad[] = {"12288", "1024", "0", "abc", "-4096", "134217728", ""};
    int ignored = 1;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        setenv("VKBRIDGE_FAKE_PAGE_SIZE", bad[i], 1);
        if (vkb_report_page_size() != real) ignored = 0, printf("  not ignored: \"%s\" -> %zu\n", bad[i], vkb_report_page_size());
    }
    check(ignored, "not a power of two, not above the page, too big, unreadable: ignored", ignored, 1);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures != 0;
}
