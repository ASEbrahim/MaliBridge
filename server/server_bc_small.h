/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The memory cap of the small-image rule (server_emu_bcn.c small_take, VKBRIDGE_BC_SMALL_DECODE): how many bytes more than
 * transcoded ones the device's small BC images decoded instead may take, from the device's RAM. Only arithmetic, so
 * tests/bcsmall_unit.c builds it alone.
 *
 * A decoded small image costs 0.5-3.5 bytes a texel more than a transcoded one; the cap bounds that per device (= per game
 * process). Scaled with RAM so that it stays a small, fixed share of what a game gets:
 *   MemTotal < SMALL_MIN_RAM_MB (3 GB: "3 GB and less" phones)  off: such a phone has no room to give
 *   below SMALL_BIGRAM_MB (11 GB: 4, 6, 8 GB phones)             RAM/128: 0.8% of RAM (4 GB ~29 MB, 6 GB ~44 MB, 8 GB ~58 MB).
 *                                                                 A game gets about half of RAM there, so 1.6%, under the
 *                                                                 64 MB floor the auto budget's decode cap gives such phones
 *   from SMALL_BIGRAM_MB (12 and 16 GB tablets)                   RAM/96, at most SMALL_CAP_MAX (12 GB ~117 MB, 16 GB ~158 MB)
 * The rule also stops while memory is tight or low (vkb_mem_tight / vkb_mem_low): MemAvailable under a sixth of RAM.
 */
#ifndef SERVER_BC_SMALL_H
#define SERVER_BC_SMALL_H

#include <stdint.h>

#define SMALL_MIN_RAM_MB 3000u
#define SMALL_BIGRAM_MB 11000u /* MemTotal of a "12 GB" device reads ~11.2-11.6 GB */
#define SMALL_CAP_MAX (256ll << 20)

/* The cap in bytes for a device with ram_mb of MemTotal (0: unknown, taken as the smallest phone that gets one). */
static inline int64_t bc_small_cap(uint64_t ram_mb)
{
    if (!ram_mb) ram_mb = SMALL_MIN_RAM_MB;
    if (ram_mb < SMALL_MIN_RAM_MB) return 0;
    int64_t cap = (int64_t)(ram_mb / (ram_mb >= SMALL_BIGRAM_MB ? 96 : 128)) << 20;
    return cap > SMALL_CAP_MAX ? SMALL_CAP_MAX : cap;
}

#endif
