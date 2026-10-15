/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The bridge's mode: safe (the default) or performance.
 *
 * Safe mode gives anything not positively verified on this exact GPU and driver the proven
 * workaround; performance mode takes the native path wherever the startup probe passed. One
 * switch, read here, decides per mode:
 *   - the probe-gated emulations (server_probe.c decide: in safe mode a probe that has not run yet
 *     or ended in error keeps its emulation; in performance mode only a failed, crashed, hung or
 *     died one does) - both masks made at start, the one in force chosen at each vkbQueryServer
 *     and vkCreateDevice (vkb_pds_select_mode), so a switch needs no re-probe and no restart;
 *   - the driver errata (server_errata.c: each erratum's rule per mode, the probe-gated ones on in
 *     safe mode by driver version where their probe has not passed);
 *   - the compile watchdog's interrupt (server_guard.c: 10 s safe, 20 s performance;
 *     VKBRIDGE_COMPILE_TIMEOUT_S still wins);
 *   - the session report's first line (server_report.c).
 * The same in both modes: wait retry (MK-01), BC transcode quality (auto: the fast encoder only
 * for mip levels of 1024x1024 and up, default's search below them), forced
 * robustBufferAccess, and on the client descriptor batching (off; client/conn.c reads the guest's
 * VKBRIDGE_MODE so a later change there is one line).
 *
 * Where the mode comes from, first found wins:
 *   1. VKBRIDGE_MODE in the game's preset file (server_presets.c), for that game's devices: read
 *      on the client's connection thread;
 *   2. VKBRIDGE_MODE in <cache dir>/env (a developer override, read again at each device);
 *   3. VKBRIDGE_MODE in the server's environment;
 *   4. VKBRIDGE_MODE in the game's built-in preset (on a GPU it was tested on), for that game's
 *      devices;
 *   5. <cache dir>/mode, one word, written by the app from Setup's "Safe mode" switch before it
 *      starts or reuses the server;
 *   6. the default, safe.
 * Values: safe, performance (perf). Anything else is safe, with a warning. Re-read at each
 * vkbQueryServer and vkCreateDevice (one small file); a change is logged with its source.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mode_v = -1;  /* VKB_MODE_*, -1: not read yet */
static int source_v;     /* VKB_MODE_SRC_* */
static char bad_value[40]; /* the unknown value last warned about */

static const char *const source_names[] = {
    [VKB_MODE_SRC_DEFAULT] = "the default",
    [VKB_MODE_SRC_FILE] = "Setup",
    [VKB_MODE_SRC_ENV] = "VKBRIDGE_MODE",
    [VKB_MODE_SRC_CACHE_ENV] = "VKBRIDGE_MODE in <cache dir>/env",
    [VKB_MODE_SRC_PRESET_FILE] = "the game's preset file",
    [VKB_MODE_SRC_PRESET_BUILTIN] = "the game's built-in preset",
};

const char *vkb_mode_name(int mode) { return mode == VKB_MODE_PERFORMANCE ? "performance" : "safe"; }

/* VKB_MODE_*; -1 for an empty value, -2 for an unknown one */
static int parse(const char *v)
{
    while (*v == ' ' || *v == '\t') v++;
    size_t n = strcspn(v, " \t\r\n");
    if (!n) return -1;
    if ((n == 4 && !strncmp(v, "safe", 4))) return VKB_MODE_SAFE;
    if ((n == 11 && !strncmp(v, "performance", 11)) || (n == 4 && !strncmp(v, "perf", 4))) return VKB_MODE_PERFORMANCE;
    return -2;
}

static void unknown(const char *where, const char *v)
{
    char val[sizeof(bad_value)];
    snprintf(val, sizeof(val), "%.*s", (int)strcspn(v, "\r\n"), v);
    if (!strcmp(val, bad_value)) return;
    memcpy(bad_value, val, sizeof(bad_value));
    VKB_WARN("mode: %s is \"%s\", neither safe nor performance; safe", where, val);
}

int vkb_mode_refresh(void)
{
    int mode = -1, src = VKB_MODE_SRC_DEFAULT;
    int es;
    const char *v = vkb_srv_env_src("VKBRIDGE_MODE", &es);
    if (v && *v) {
        src = es == VKB_ENV_SRC_ENV              ? VKB_MODE_SRC_ENV
              : es == VKB_ENV_SRC_PRESET_FILE    ? VKB_MODE_SRC_PRESET_FILE
              : es == VKB_ENV_SRC_PRESET_BUILTIN ? VKB_MODE_SRC_PRESET_BUILTIN
                                                 : VKB_MODE_SRC_CACHE_ENV;
        mode = parse(v);
        if (mode == -2) unknown(source_names[src], v);
        if (mode < 0) mode = VKB_MODE_SAFE;
    }
    const char *dir = vkb_pcache_dir();
    if (mode < 0 && dir) {
        char path[640], line[64];
        snprintf(path, sizeof(path), "%s/mode", dir);
        FILE *f = fopen(path, "re");
        if (f) {
            if (fgets(line, sizeof(line), f)) {
                int m = parse(line);
                if (m == -2) unknown(path, line);
                if (m != -1) {
                    mode = m < 0 ? VKB_MODE_SAFE : m;
                    src = VKB_MODE_SRC_FILE;
                }
            }
            fclose(f);
        }
    }
    if (mode < 0) mode = VKB_MODE_SAFE, src = VKB_MODE_SRC_DEFAULT;
    int was = __atomic_exchange_n(&mode_v, mode, __ATOMIC_ACQ_REL);
    int was_src = __atomic_exchange_n(&source_v, src, __ATOMIC_ACQ_REL);
    if (was != mode || was_src != src)
        VKB_INFO("mode: %s (%s)%s", vkb_mode_name(mode), source_names[src], was < 0 ? "" : was != mode ? " - changed" : "");
    return mode;
}

int vkb_mode(void)
{
    int m = __atomic_load_n(&mode_v, __ATOMIC_ACQUIRE);
    return m < 0 ? vkb_mode_refresh() : m;
}

int vkb_mode_source(void)
{
    vkb_mode();
    return __atomic_load_n(&source_v, __ATOMIC_ACQUIRE);
}

const char *vkb_mode_source_name(void) { return source_names[vkb_mode_source()]; }

/* What the mode changes, for the report's first line */
const char *vkb_mode_summary(int mode)
{
    return mode == VKB_MODE_PERFORMANCE ? "native paths wherever the startup probe did not see them fail"
                                        : "emulations and errata fixes stay on wherever the startup probe has not verified the native path on "
                                          "this GPU and driver";
}
