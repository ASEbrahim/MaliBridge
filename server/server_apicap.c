/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Steam mode's own processes (Xwayland, steam, steamwebhelper, gamescope) shown a lower Vulkan version on the gate.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include "server_emu.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static int gate_state = -1; /* -1 undecided */
static char gate_why[160];

/* The Vulkan version apps are shown on pd: the promotion's choice (server_promote.c), else the driver's. */
static uint32_t apps_api(const vkb_srv_table *t, VkPhysicalDevice pd)
{
    VkPhysicalDeviceProperties p;
    t->real.vkGetPhysicalDeviceProperties(pd, &p);
    uint32_t api = p.apiVersion;
    if (t->real.vkGetPhysicalDeviceProperties2 && t->api_version >= VK_API_VERSION_1_1) {
        VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        t->real.vkGetPhysicalDeviceProperties2(pd, &p2);
        uint32_t offered, drv;
        const char *missing;
        if (vkb_promote_describe(id.deviceUUID, &offered, &drv, &missing)) api = offered;
    }
    return api;
}

/* 1 if the driver behind pd lists name (the server's own env hiding aside) */
static int driver_offers(const vkb_srv_table *t, VkPhysicalDevice pd, const char *name)
{
    if (!t->real.vkEnumerateDeviceExtensionProperties || vkb_emu_ext_hidden_by_env(name)) return 0;
    uint32_t n = 0;
    if (t->real.vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkExtensionProperties *e = calloc(n, sizeof(*e));
    if (!e) return 0;
    int found = 0;
    if (t->real.vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) >= 0)
        for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(e[i].extensionName, name);
    free(e);
    return found;
}

/* The first release outside the gate (r54p1). */
#define BLACK_FIXED_RELEASE 54

/* 1 where the gate is on (the first GPU asked decides; *why: for the log). */
int vkb_black_gate(const vkb_srv_table *t, VkPhysicalDevice pd, const char **why)
{
    pthread_mutex_lock(&gate_lock);
    if (gate_state < 0 && t && pd) {
        int arm = 0, jm = 0, rel = vkb_errata_arm_release_pd(t, pd, &arm, &jm);
        uint32_t api = apps_api(t, pd);
        int eds1 = arm && jm ? driver_offers(t, pd, "VK_EXT_extended_dynamic_state") : 0;
        int old = rel < BLACK_FIXED_RELEASE; /* unknown (-1) counts as old */
        gate_state = arm && jm && old && (eds1 || VK_API_VERSION_MINOR(api) >= 3);
        char r[16] = "r?";
        if (rel >= 0) snprintf(r, sizeof(r), "r%d", rel);
        snprintf(gate_why, sizeof(gate_why), "%s driver %s, %s, Vulkan 1.%u to apps, %s%s", arm ? "Arm" : "not Arm's", r,
                 jm ? "job manager" : "CSF", VK_API_VERSION_MINOR(api),
                 eds1 ? "VK_EXT_extended_dynamic_state offered" : "no VK_EXT_extended_dynamic_state",
                 arm && jm && !old ? ", r54 or later" : "");
    }
    int on = gate_state > 0;
    if (why) *why = gate_state >= 0 ? gate_why : "not decided";
    pthread_mutex_unlock(&gate_lock);
    return on;
}

/* "1.x" -> its minor, "0"/"off" -> 0, unset -> -1, anything else -> -2 */
static int cap_value(const char *v)
{
    unsigned ma, mi;
    if (!v || !*v) return -1;
    if (!strcmp(v, "0") || !strcmp(v, "off")) return 0;
    if (sscanf(v, "%u.%u", &ma, &mi) == 2 && ma == 1 && mi >= 1 && mi <= 4) return (int)mi;
    return -2;
}

/* gamescope's minor for the others' minor m: its own switch, else at least 1.2 */
static int gamescope_minor(int m)
{
    int gm = cap_value(vkb_srv_env_global("VKBRIDGE_STEAM_API_CAP_GAMESCOPE"));
    if (gm >= 0) return gm;
    return m < 2 ? 2 : m;
}

uint32_t vkb_steam_api_cap(const vkb_srv_table *t, VkPhysicalDevice pd)
{
    const vkb_srv_proc *pr = t ? t->proc : NULL;
    const char *comm = pr && pr->comm[0] ? pr->comm : "";
    int gs = !strncmp(comm, "gamescope", 9);
    if (!gs && strcmp(comm, "Xwayland") && strcmp(comm, "steamwebhelper") && strcmp(comm, "steam")) return 0;
    const char *v = vkb_srv_env_global("VKBRIDGE_STEAM_API_CAP");
    int minor = cap_value(v);
    const char *why = NULL;
    if (minor == -2) {
        static int warned;
        if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
            VKB_WARN("api cap: VKBRIDGE_STEAM_API_CAP=%s is not 1.1, 1.2, 1.3, 0 or off; no cap", v);
        return 0;
    }
    if (minor < 0) minor = vkb_black_gate(t, pd, &why) ? 1 : 0; /* the default: on where the gate is */
    if (!minor) return 0;
    int others = minor, gm = gamescope_minor(minor);
    if (gs) {
        if (!gm) return 0;
        minor = gm;
    }
    uint32_t api = apps_api(t, pd), cap = VK_MAKE_API_VERSION(0, 1, (uint32_t)minor, 0);
    if (api <= cap) return 0; /* already at or below it */
    static int said;
    if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED)) {
        char g[32];
        if (gm) snprintf(g, sizeof(g), "gamescope 1.%d", gm);
        else snprintf(g, sizeof(g), "gamescope uncapped");
        VKB_INFO("api cap: Xwayland, steam and steamwebhelper are shown Vulkan 1.%d, %s, games 1.%u (%s%s; "
                 "VKBRIDGE_STEAM_API_CAP=0 turns it off)", others, g, VK_API_VERSION_MINOR(api),
                 why ? why : "VKBRIDGE_STEAM_API_CAP=", why ? "" : v);
    }
    VKB_DBG("api cap: %s pid %d is shown Vulkan 1.%d, not 1.%u", comm, pr ? pr->pid : 0, minor, VK_API_VERSION_MINOR(api));
    return cap;
}
