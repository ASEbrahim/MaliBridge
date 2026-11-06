/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The BC conversion budget's auto rate for a Mali GPU, from its device name (server_emu_bcn.c,
 * VKBRIDGE_BC_TX_BUDGET=auto). Only string work, so tests/bcauto_unit.c builds it alone.
 */
#ifndef SERVER_BC_AUTO_H
#define SERVER_BC_AUTO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* auto's rate for a GPU, from its name ("Mali-G615 MC6", "Mali-G720-Immortalis MC12"): its shader
 * cores times a core's speed against a G720 / G925 core, so that 12 such cores get the 32:4 the
 * G720 was calibrated at (FEAT-bc-budget.md). 0: no budget (not a Mali). The count is " MC<n>" or " MP<n>"
 * (the older naming, "Mali-G72 MP3"); a name with none ("Mali-G78", "Mali-G715" as some drivers report
 * them) or one out of range is taken as AUTO_UNKNOWN_CORES of its family, the smallest Mali configuration,
 * so a GPU auto cannot size is never given more than it can transcode (test11 audit: those names got
 * 32:4, the G720 MC12's rate). */
#define AUTO_UNKNOWN_CORES 2
static inline double budget_auto(const char *name, int *fast_ok, char *why, size_t n)
{
    *fast_ok = 0;
    if (strncmp(name, "Mali-G", 6)) {
        snprintf(why, n, "not a Mali GPU (%.40s): no budget", name);
        return 0;
    }
    const char *mc = strstr(name, " MC");
    if (!mc) mc = strstr(name, " MP");
    int cores = mc ? atoi(mc + 3) : 0;
    const char *g = name + 6;
    int num = atoi(g);
    double per;
    int gen5 = !strncmp(g, "1-", 2) || !strncmp(g, "2-", 2); /* the Immortalis G1 / G2 class (D9500 on) */
    if (gen5 || num == 720 || num == 620 || num == 925 || num == 725 || num == 625) per = 1.0;
    else if (num == 710 || num == 610 || num == 715 || num == 615) per = 0.7;
    else if (num == 77 || num == 78 || num == 57 || num == 68) per = 0.5;
    else if (num == 31 || num == 51 || num == 52 || num == 71 || num == 72 || num == 76) per = 0.3;
    else per = 0.5;
    int known = cores > 0 && cores <= 32;
    if (!known) cores = AUTO_UNKNOWN_CORES;
    double rate = 32.0 * cores * per / 12.0;
    if (rate < 2) rate = 2;
    if (rate > 48) rate = 48;
    *fast_ok = !gen5;
    if (known) snprintf(why, n, "%.40s: %d cores x %.1f of a G720 core", name, cores, per);
    else snprintf(why, n, "%.40s: no core count in the name, taken as %d x %.1f", name, cores, per);
    return rate;
}

#endif
