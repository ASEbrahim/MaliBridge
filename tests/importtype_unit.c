/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * importtype_unit: the dma-buf import's memory type fallback (server/server_import_type.c) on its
 * own, over memory tables of real drivers (no Vulkan driver involved):
 *   - PowerVR DXT / CXTP (Pixel 10 Pro, 11 Pro; 2026-10-29): 0x1, 0x7, 0xb, 0x11, 0x21
 *   - Mali-G720 r44p1 (the dma-buf takes only the host-visible cached type)
 *   - a table with a coherent-only host type
 * Each case: the type asked, the dma-buf's mask (vkGetMemoryFdPropertiesKHR), the resource's mask;
 * the answer must be the nearest type that keeps host-visible and coherent, never a lazily allocated
 * or protected type while another fits, -1 when the asked type is allowed already or nothing fits.
 * Prints "OK: ..." / "FAILED: ..." last. host/Makefile builds it; host/run_tests.sh runs it.
 */
#include "../server/server_import_type.c"

#include <stdio.h>
#include <string.h>

#define DL VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
#define HV VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
#define HC VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
#define CA VK_MEMORY_PROPERTY_HOST_CACHED_BIT
#define LZ VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT
#define PR VK_MEMORY_PROPERTY_PROTECTED_BIT

static VkPhysicalDeviceMemoryProperties table(int n, const uint32_t *flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof(mp));
    mp.memoryTypeCount = (uint32_t)n;
    mp.memoryHeapCount = 1;
    for (int i = 0; i < n; i++) mp.memoryTypes[i].propertyFlags = flags[i];
    return mp;
}

static int fails, cases;

static void expect(const char *name, const VkPhysicalDeviceMemoryProperties *mp, uint32_t asked, uint32_t fd, uint32_t req, int want)
{
    const char *why;
    int got = vkb_import_type_pick(mp, asked, fd, req, &why);
    cases++;
    if (got != want) {
        fails++;
        printf("  FAIL %s: asked %u, dma-buf 0x%x, resource 0x%x: got %d (%s), want %d\n", name, asked, fd, req, got, why, want);
    } else
        printf("  ok   %s: asked %u -> %d (%s)\n", name, asked, got, *why ? why : "unchanged");
}

int main(void)
{
    /* PowerVR: 0 DL, 1 DL+HV+HC, 2 DL+HV+cached, 3 DL+lazy, 4 DL+protected */
    const uint32_t pvr_f[5] = {DL, DL | HV | HC, DL | HV | CA, DL | LZ, DL | PR};
    VkPhysicalDeviceMemoryProperties pvr = table(5, pvr_f);
    expect("pvr: allowed already", &pvr, 1, 0x6, UINT32_MAX, -1);
    expect("pvr: coherent host type asked, only the cached one listed: the best there is", &pvr, 1, 0x4, UINT32_MAX, 2);
    expect("pvr: cached host type asked, only the coherent one listed: host access kept", &pvr, 2, 0x2, UINT32_MAX, 1);
    expect("pvr: device-local asked, only host types listed: the nearest (fewest extra flags)", &pvr, 0, 0x6, UINT32_MAX, 1);
    expect("pvr: device-local asked, the resource allows only the cached one", &pvr, 0, 0x6, 0x4, 2);
    expect("pvr: lazily allocated and protected never while a normal one fits", &pvr, 0, 0x18 | 0x4, UINT32_MAX, 2);
    expect("pvr: only lazily allocated and protected listed: none taken", &pvr, 0, 0x18, UINT32_MAX, -1);
    expect("pvr: nothing in both masks", &pvr, 0, 0x6, 0x1, -1);
    expect("pvr: empty fd properties", &pvr, 0, 0, UINT32_MAX, -1);
    expect("pvr: asked type out of range", &pvr, 9, 0x6, UINT32_MAX, -1);
    /* Mali-G720 r44p1: 0 DL, 1 DL+HV+cached (the only type a dma-buf lists), 2 DL+HV+HC */
    const uint32_t mali_f[3] = {DL, DL | HV | CA, DL | HV | HC};
    VkPhysicalDeviceMemoryProperties mali = table(3, mali_f);
    expect("mali: device-local asked, the host cached type listed (the existing move)", &mali, 0, 0x2, 0x3, 1);
    expect("mali: resource allows type 0 only: nothing to move to", &mali, 0, 0x2, 0x1, -1);
    expect("mali: coherent asked, cached-only listed: the best there is", &mali, 2, 0x2, UINT32_MAX, 1);
    /* two device-local types: the one with the asked type's other flags wins */
    const uint32_t two_f[4] = {DL, DL | HV | HC, HV | HC | CA, DL | HV | HC | CA};
    VkPhysicalDeviceMemoryProperties two = table(4, two_f);
    expect("two: host-visible kept, device-local-ness preferred", &two, 1, 0xc, UINT32_MAX, 3);
    expect("two: device-local asked, same-flags type absent: the fewest extras", &two, 0, 0xa, UINT32_MAX, 1);
    /* a lower-indexed type without coherence listed beside one with it: the coherent one wins */
    const uint32_t coh_f[3] = {DL | HV | CA, DL | HV | HC, DL | HV | HC | CA};
    VkPhysicalDeviceMemoryProperties coh = table(3, coh_f);
    expect("coh: coherent asked, a cached-only and a coherent type listed: the coherent one", &coh, 2, 0x3, UINT32_MAX, 1);
    printf("%s: %d cases, %d failed\n", fails ? "FAILED" : "OK", cases, fails);
    return fails ? 1 : 0;
}
