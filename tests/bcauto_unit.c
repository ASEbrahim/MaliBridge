/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * bcauto_unit: the BC conversion budget's auto rate from a GPU's name (server/server_bc_auto.h), built alone.
 * Audit of test11 (T11-PROOF-AUDIT row 165): names without " MC" ("Mali-G78", "Mali-G72 MP3") got 32:4,
 * the G720 MC12's rate, the most auto gives; auto must never give a GPU more than its family's smallest one.
 *   1 names with " MC<n>": the rates the Ultra A/B plan and bcbudget_test use (G720/G925 MC12 32, G615 MC6
 *     11.2, G77 MC9 12, G57 MC2 2.7, G68 MC4 5.3), the Immortalis G1 never encoded fast
 *   2 names with " MP<n>" (the Bifrost/Valhall naming): counted as cores (G72 MP3 2.4, G76 MP10 8)
 *   3 no core count ("Mali-G78", "Mali-G715", "Mali-G52", "Mali-G925-Immortalis"): the family's 2-core
 *     rate, never 32
 *   4 a count out of range (MC0, MC64): as no count
 *   5 for every family and every count 2-32: no count never gets more than a named count
 *   6 not a Mali: 0 (no budget)
 * Prints PASS/FAIL lines and "OK: 0 failures" / "FAILED: n failures".
 */
#include "server_bc_auto.h"

#include <math.h>
#include <stdio.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static double rate_of(const char *name, int *fast)
{
    char why[160];
    int f = -1;
    double r = budget_auto(name, &f, why, sizeof(why));
    if (fast) *fast = f;
    printf("     %-28s -> %.2f (%s)\n", name, r, why);
    return r;
}

static void want(const char *name, double rate, int fast)
{
    char what[160];
    int f;
    double r = rate_of(name, &f);
    snprintf(what, sizeof(what), "%s: %.1f, fast search %s", name, rate, fast ? "allowed" : "never");
    check(fabs(r - rate) < 0.05 && f == fast, what);
}

int main(void)
{
    /* 1 */
    want("Mali-G720-Immortalis MC12", 32.0, 1);
    want("Mali-G925-Immortalis MC12", 32.0, 1);
    want("Mali-G615 MC6", 11.2, 1);
    want("Mali-G77 MC9", 12.0, 1);
    want("Mali-G57 MC2", 32.0 * 2 * 0.5 / 12, 1);
    want("Mali-G68 MC4", 32.0 * 4 * 0.5 / 12, 1);
    want("Mali-G1-Ultra MC12", 32.0, 0);
    /* 2 */
    want("Mali-G72 MP3", 2.4, 1);
    want("Mali-G76 MP10", 8.0, 1);
    /* 3 */
    want("Mali-G78", 32.0 * 2 * 0.5 / 12, 1);
    want("Mali-G715", 32.0 * 2 * 0.7 / 12, 1);
    want("Mali-G52", 2.0, 1);
    want("Mali-G925-Immortalis", 32.0 * 2 * 1.0 / 12, 1);
    /* 4 */
    want("Mali-G615 MC0", 32.0 * 2 * 0.7 / 12, 1);
    want("Mali-G720-Immortalis MC64", 32.0 * 2 * 1.0 / 12, 1);
    /* 5 */
    {
        static const char *const fam[] = {"Mali-G1-Ultra", "Mali-G720-Immortalis", "Mali-G925-Immortalis", "Mali-G620", "Mali-G710",
                                          "Mali-G615", "Mali-G715", "Mali-G77", "Mali-G78", "Mali-G57", "Mali-G68", "Mali-G52",
                                          "Mali-G72", "Mali-G76", "Mali-G31", "Mali-G99"};
        int bad = 0;
        for (size_t i = 0; i < sizeof(fam) / sizeof(fam[0]); i++) {
            char why[160], nm[64];
            int f;
            double none = budget_auto(fam[i], &f, why, sizeof(why));
            for (int c = 2; c <= 32; c++) {
                snprintf(nm, sizeof(nm), "%s MC%d", fam[i], c);
                double named = budget_auto(nm, &f, why, sizeof(why));
                if (none > named + 1e-9) {
                    if (!bad) printf("     %s with no count %.2f > %s %.2f\n", fam[i], none, nm, named);
                    bad++;
                }
            }
        }
        check(bad == 0, "no core count never gets more than the same GPU with 2-32 cores named");
    }
    /* 6 */
    want("Adreno (TM) 750", 0, 0);
    want("PowerVR D-Series DXT-48-1536", 0, 0);
    want("AMD Radeon RX 7800 XT (RADV NAVI32)", 0, 0);

    if (fails) printf("FAILED: %d failures\n", fails);
    else printf("OK: 0 failures\n");
    return fails != 0;
}
