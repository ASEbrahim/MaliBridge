/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * Steam mode's own processes at Vulkan 1.1 on Arm job-manager Malis (/#15).
 *
 * Steam's game mode shows black on Arm job-manager Malis whose driver offers VK_EXT_extended_dynamic_state
 * (Galaxy A16, Tab S9 FE, F15, A54, Tab S10 Lite: r38p1, Vulkan 1.3; the Anbernic RG556: Unisoc T820,
 * G57 r40p0, Vulkan 1.3, AHardwareBuffer pages): gamescope's frames reach DroidDeck's compositor, the
 * bridge logs no error, and the picture is black. The frames on screen are Xwayland's pixmaps, written
 * by Zink inside Xwayland, and Zink's draws lose their picture on these drivers when it uses that
 * extension's state (server_emu.c a1_decide.
 * Where the picture shows: the Galaxy S21 (G78 r38p0, Vulkan 1.1) whose driver has no such extension;
 * a Redmi (G57 MC2, job manager, r54p1, Vulkan 1.3, the extension offered: a player log, Big Picture and
 * a game from it), so Arm's r54 drivers are past it. The Galaxy S10 and Note10+ phones (G76 r38p1 beta,
 * Vulkan 1.1 to apps, the extension offered) died before Big Picture drew (#11, gs_hide); at 1.1 Zink
 * still takes the extension, so they are inside the gate too.
 *
 * vkb_black_gate: Arm's own driver (not PanVK), a job manager GPU below r54 (unknown: inside) or a CSF GPU
 * on a known release below r44 (, a player log: Dimensity 9200's Immortalis-G715 on the
 * same r38p1-01eac0 driver as the job-manager devices, black the same way; r44p1 and later CSF drivers
 * show Steam, an unknown CSF release stays outside), and either the driver offers VK_EXT_extended_dynamic_state (not hidden by VKBRIDGE_SERVER_HIDE_EXTS /
 * VKBRIDGE_FAKE_MISSING) or apps are shown Vulkan 1.3 or later (where it is core).
 * There, by default, the processes named Xwayland, steam and steamwebhelper are shown Vulkan 1.1 and
 * gamescope 1.2 (at 1.1 gamescope finds no device: "failed to find physical device" on the host, as on
 * r32p1 in #10), where apps are shown more than that. Games and every other process are never capped.
 * The driver's extensions stay offered (timeline semaphores, dynamic rendering etc. as extensions, like
 * the S21's): the client caps the apiVersion it reports and its device version (client/physdev.c
 * cap_api), so core 1.2/1.3 names stay hidden unless their extension is enabled.
 * CSF Malis on r44 or later (or an unknown release), job-manager ones on r54 and later, drivers without the
 * extension and other vendors: no change.
 *
 * Switches (<cache dir>/env or the server's environment):
 *   VKBRIDGE_STEAM_API_CAP=0 / off      no cap anywhere (the off switch)
 *   VKBRIDGE_STEAM_API_CAP=1.1|1.2|1.3  that cap anywhere, the gate or not (gamescope at least 1.2)
 *   VKBRIDGE_STEAM_API_CAP_GAMESCOPE=1.x  gamescope's own cap (=0: gamescope uncapped)
 * One line per server process says the cap is on and why ("api cap: ...").
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

/* The first release whose job-manager driver draws Zink's extended dynamic state right (a player log, r54p1). */
#define BLACK_FIXED_RELEASE 54
/* CSF GPUs: the oldest release seen drawing it right (r44p1: Galaxy Tab S10+); r38p1 on the Immortalis-G715
 * is black like the job-manager devices on that driver. r32p1 CSF drivers have neither the
 * extension nor Vulkan 1.3, so the rest of the gate keeps them out. */
#define BLACK_FIXED_RELEASE_CSF 44

/* 1 where Steam's game mode shows black: Arm's driver on a job manager GPU below r54 or a CSF one below r44, with
 * VK_EXT_extended_dynamic_state offered or Vulkan 1.3 to apps (the first GPU asked decides; *why: for the log). */
int vkb_black_gate(const vkb_srv_table *t, VkPhysicalDevice pd, const char **why)
{
    pthread_mutex_lock(&gate_lock);
    if (gate_state < 0 && t && pd) {
        int arm = 0, jm = 0, rel = vkb_errata_arm_release_pd(t, pd, &arm, &jm);
        uint32_t api = apps_api(t, pd);
        int old = jm ? rel < BLACK_FIXED_RELEASE /* unknown (-1) counts as old */
                     : rel >= 0 && rel < BLACK_FIXED_RELEASE_CSF; /* CSF: a known release only */
        int eds1 = arm && old ? driver_offers(t, pd, "VK_EXT_extended_dynamic_state") : 0;
        gate_state = arm && old && (eds1 || VK_API_VERSION_MINOR(api) >= 3);
        char r[16] = "r?";
        if (rel >= 0) snprintf(r, sizeof(r), "r%d", rel);
        snprintf(gate_why, sizeof(gate_why), "%s driver %s, %s, Vulkan 1.%u to apps, %s%s", arm ? "Arm" : "not Arm's", r,
                 jm ? "job manager" : "CSF", VK_API_VERSION_MINOR(api),
                 eds1 ? "VK_EXT_extended_dynamic_state offered" : "no VK_EXT_extended_dynamic_state",
                 arm && !old ? (jm ? ", r54 or later" : rel >= 0 ? ", r44 or later" : ", release unknown") : "");
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

/* gamescope's minor for the others' minor m: VKBRIDGE_STEAM_API_CAP_GAMESCOPE, else at least 1.2 (#10) */
static int gamescope_minor(int m)
{
    int gm = cap_value(vkb_srv_env_global("VKBRIDGE_STEAM_API_CAP_GAMESCOPE"));
    if (gm >= 0) return gm;
    return m < 2 ? 2 : m;
}

/* 1 for Steam mode's own processes: gamescope, Xwayland, steamwebhelper, steam */
int vkb_steam_own_process(const char *comm)
{
    return comm && (!strncmp(comm, "gamescope", 9) || !strcmp(comm, "Xwayland") || !strcmp(comm, "steamwebhelper") ||
                    !strcmp(comm, "steam"));
}

uint32_t vkb_steam_api_cap(const vkb_srv_table *t, VkPhysicalDevice pd)
{
    const vkb_srv_proc *pr = t ? t->proc : NULL;
    const char *comm = pr && pr->comm[0] ? pr->comm : "";
    int gs = !strncmp(comm, "gamescope", 9);
    if (!vkb_steam_own_process(comm)) return 0;
    const char *v = vkb_srv_env_global("VKBRIDGE_STEAM_API_CAP");
    int minor = cap_value(v);
    const char *why = NULL;
    if (minor == -2) {
        static int warned;
        if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
            VKB_WARN("api cap: VKBRIDGE_STEAM_API_CAP=%s is not 1.1, 1.2, 1.3, 0 or off; no cap", v);
        return 0;
    }
    if (minor < 0) minor = vkb_black_gate(t, pd, &why) ? 1 : 0; /* the default: on where the black screen is */
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
