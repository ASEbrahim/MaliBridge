/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * bcsmall_unit: the small-image rule's memory cap from the device's RAM (server/server_bc_small.h), built alone.
 *   1 the RAM classes: 2 GB phones off; 4 / 6 / 8 GB phones RAM/128 (under the 64 MB the flat cap gave them); 12 / 16 GB
 *     tablets RAM/96 (the S10+ and the Ultra keep the 116 / 158 MB they had in the device rounds); capped at 256 MB
 *   2 never more for less RAM: the cap does not fall as RAM grows, over 1-64 GB in 64 MB steps
 *   3 unknown RAM (0): the smallest class that gets a cap, never off-by-default and never a tablet's cap
 * Prints PASS/FAIL lines and "OK: 0 failures" / "FAILED: n failures".
 */
#include "server_bc_small.h"

#include <stdio.h>

static int fails;
static void check(int ok, const char *what, uint64_t ram, int64_t cap)
{
    printf("%s %s (MemTotal %llu MB: %lld MB)\n", ok ? "PASS" : "FAIL", what, (unsigned long long)ram, (long long)(cap >> 20));
    if (!ok) fails++;
}

int main(void)
{
    const struct {
        uint64_t ram;
        int64_t lo, hi; /* MB */
        const char *what;
    } c[] = {{1800, 0, 0, "1 2 GB phone: off"},
             {2900, 0, 0, "1 3 GB phone reading under 3000 MB: off"},
             {3700, 28, 29, "1 4 GB phone: RAM/128"},
             {5600, 43, 44, "1 6 GB phone: RAM/128"},
             {7400, 57, 58, "1 8 GB phone (A34 class): RAM/128, under the flat 64 MB"},
             {10900, 85, 85, "1 just under 11 GB: RAM/128"},
             {11200, 116, 117, "1 12 GB tablet (S10+): RAM/96 as before"},
             {15245, 158, 158, "1 16 GB tablet (Ultra, a player log MemTotal): RAM/96 as before"},
             {65536, 256, 256, "1 64 GB: at most 256 MB"}};
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        int64_t cap = bc_small_cap(c[i].ram);
        check(cap >= (c[i].lo << 20) && cap <= ((c[i].hi + 1) << 20) - 1, c[i].what, c[i].ram, cap);
    }
    int64_t prev = 0;
    uint64_t bad = 0;
    for (uint64_t ram = 1024; ram <= 65536; ram += 64) {
        int64_t cap = bc_small_cap(ram);
        if (cap < prev && !bad) bad = ram;
        prev = cap;
    }
    check(!bad, "2 never less cap for more RAM", bad, bad ? bc_small_cap(bad) : prev);
    int64_t u = bc_small_cap(0);
    check(u > 0 && u <= bc_small_cap(7400), "3 unknown RAM: the smallest class that gets one", 0, u);
    if (fails) printf("FAILED: %d failures\n", fails);
    else printf("OK: 0 failures\n");
    return fails != 0;
}
