/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Which driver runs the GPU: Arm's own (the Mali driver the errata, the SPIR-V fixes and the
 * dedicated-allocation hint are written for) or another one with Arm's vendor ID. Mesa's PanVK
 * reports vendorID 0x13B5 like Arm's driver, and its driverVersion is Mesa's (Mesa 26.3.0 is
 * VK_MAKE_VERSION(26, 3, 0), which decodes as "r26p3", inside the errata's ranges), so Arm's driver
 * is told by VkPhysicalDeviceDriverProperties.driverID == VK_DRIVER_ID_ARM_PROPRIETARY. Where the
 * driver cannot say (Vulkan 1.0/1.1 without VK_KHR_driver_properties), the vendor ID decides, as it
 * did before driverID was asked.
 *
 * The driverID is asked once per physical device, by (vendorID, deviceID, driverVersion), and
 * remembered for the server process: the self-test asks through its own instance (the loader's
 * version), so a device made later through an app's Vulkan 1.0 instance, which cannot ask, gets
 * the self-test's answer. Two drivers giving different answers for the same three values make the
 * answer unknown (the vendor ID decides again).
 *
 * VKBRIDGE_FAKE_DRIVER (host tests), beneath everything the server does (vkb_gipa: the self-test,
 * the probes and the devices all see it): the GPU reports vendorID 0x13B5 and
 *   panvk  driverID MESA_PANVK, Mesa 26.3.0's driverVersion, driverName "panvk"
 *   arm    driverID ARM_PROPRIETARY, driverVersion r44p1, driverInfo "v1.r44p1-01eac0"
 *   arm11  as arm, as a Vulkan 1.1 driver without VK_KHR_driver_properties (driverID unknown)
 *   samsung  vendorID 0x144D and driverID SAMSUNG_PROPRIETARY, the rest the driver's (Xclipse)
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARM_VENDOR 0x13B5u
#define AMBIGUOUS 0xffffffffu

static struct {
    uint32_t vendor, device, version, id;
} known[16];
static int nknown;
static pthread_mutex_t known_lock = PTHREAD_MUTEX_INITIALIZER;

static void remember(uint32_t vendor, uint32_t device, uint32_t version, uint32_t id)
{
    pthread_mutex_lock(&known_lock);
    int i = 0;
    while (i < nknown && !(known[i].vendor == vendor && known[i].device == device && known[i].version == version)) i++;
    if (i < nknown) {
        if (known[i].id != id && known[i].id != AMBIGUOUS) {
            VKB_WARN("GPU driver: vendor 0x%04x device 0x%04x driver 0x%08x answered driverID %u, then %u; Arm's driver is told by "
                     "the vendor ID for it", vendor, device, version, known[i].id, id);
            known[i].id = AMBIGUOUS;
        }
    } else if (nknown < (int)(sizeof(known) / sizeof(known[0]))) {
        known[i].vendor = vendor, known[i].device = device, known[i].version = version, known[i].id = id;
        nknown++;
    }
    pthread_mutex_unlock(&known_lock);
}

static uint32_t recall(uint32_t vendor, uint32_t device, uint32_t version)
{
    uint32_t id = 0;
    pthread_mutex_lock(&known_lock);
    for (int i = 0; i < nknown; i++)
        if (known[i].vendor == vendor && known[i].device == device && known[i].version == version) id = known[i].id;
    pthread_mutex_unlock(&known_lock);
    return id == AMBIGUOUS ? 0 : id;
}

static int has_driver_properties(const vkb_dispatch *dt, VkPhysicalDevice pd)
{
    uint32_t n = 0;
    if (!dt->vkEnumerateDeviceExtensionProperties || dt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkExtensionProperties *e = calloc(n, sizeof(*e));
    if (!e) return 0;
    int found = 0;
    if (dt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) >= 0)
        for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(e[i].extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
    free(e);
    return found;
}

uint32_t vkb_driver_id(const vkb_dispatch *dt, VkPhysicalDevice pd, uint32_t inst_api)
{
    VkPhysicalDeviceProperties p;
    dt->vkGetPhysicalDeviceProperties(pd, &p);
    uint32_t id = recall(p.vendorID, p.deviceID, p.driverVersion);
    if (id) return id;
    uint32_t api = p.apiVersion < inst_api ? p.apiVersion : inst_api;
    if (!dt->vkGetPhysicalDeviceProperties2 || inst_api < VK_API_VERSION_1_1 || (api < VK_API_VERSION_1_2 && !has_driver_properties(dt, pd)))
        return 0;
    VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
    dt->vkGetPhysicalDeviceProperties2(pd, &p2);
    if (dp.driverID) remember(p.vendorID, p.deviceID, p.driverVersion, dp.driverID);
    return recall(p.vendorID, p.deviceID, p.driverVersion);
}

int vkb_driver_is_arm(const VkPhysicalDeviceProperties *p, uint32_t driver_id)
{
    if (driver_id) remember(p->vendorID, p->deviceID, p->driverVersion, driver_id);
    uint32_t id = recall(p->vendorID, p->deviceID, p->driverVersion);
    return id ? id == VK_DRIVER_ID_ARM_PROPRIETARY : p->vendorID == ARM_VENDOR;
}

int vkb_arm_driver(const vkb_dispatch *dt, VkPhysicalDevice pd, uint32_t inst_api)
{
    VkPhysicalDeviceProperties p;
    dt->vkGetPhysicalDeviceProperties(pd, &p);
    return vkb_driver_is_arm(&p, vkb_driver_id(dt, pd, inst_api));
}

const char *vkb_driver_id_name(uint32_t id)
{
    switch (id) {
    case 0: return "unknown";
    case VK_DRIVER_ID_ARM_PROPRIETARY: return "ARM_PROPRIETARY";
    case VK_DRIVER_ID_MESA_PANVK: return "MESA_PANVK";
    case VK_DRIVER_ID_MESA_RADV: return "MESA_RADV";
    case VK_DRIVER_ID_MESA_LLVMPIPE: return "MESA_LLVMPIPE";
    case VK_DRIVER_ID_MESA_TURNIP: return "MESA_TURNIP";
    case VK_DRIVER_ID_QUALCOMM_PROPRIETARY: return "QUALCOMM_PROPRIETARY";
    case VK_DRIVER_ID_SAMSUNG_PROPRIETARY: return "SAMSUNG_PROPRIETARY";
    case VK_DRIVER_ID_MESA_VENUS: return "MESA_VENUS";
    default: return "other";
    }
}

void vkb_driver_log(const vkb_dispatch *dt, VkPhysicalDevice pd, uint32_t inst_api)
{
    VkPhysicalDeviceProperties p;
    dt->vkGetPhysicalDeviceProperties(pd, &p);
    uint32_t id = vkb_driver_id(dt, pd, inst_api);
    int arm = vkb_driver_is_arm(&p, id);
    if (id)
        VKB_INFO("GPU driver: driverID %u (%s): %s", id, vkb_driver_id_name(id),
                 arm ? "Arm's driver; its errata and fixes apply"
                 : p.vendorID == ARM_VENDOR ? "not Arm's driver although the vendor is Arm; Arm's errata and fixes stay off"
                                            : "not Arm's driver");
    else
        VKB_INFO("GPU driver: driverID unknown (Vulkan %u.%u, no VK_KHR_driver_properties): %s", VK_API_VERSION_MAJOR(p.apiVersion),
                 VK_API_VERSION_MINOR(p.apiVersion), arm ? "Arm's driver by the vendor ID" : "not Arm's driver by the vendor ID");
}

/* ------------------------------------------------------------------ VKBRIDGE_FAKE_DRIVER */

enum { FAKE_OFF, FAKE_PANVK, FAKE_ARM, FAKE_ARM11, FAKE_SAMSUNG };

static struct {
    int on;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetPhysicalDeviceProperties props;
    PFN_vkGetPhysicalDeviceProperties2 props2, props2khr;
    PFN_vkEnumerateDeviceExtensionProperties exts;
} fk;

static void fake_props(VkPhysicalDeviceProperties *p)
{
    if (fk.on == FAKE_SAMSUNG) {
        p->vendorID = 0x144D;
        return;
    }
    p->vendorID = ARM_VENDOR;
    if (fk.on == FAKE_PANVK) p->driverVersion = VK_MAKE_VERSION(26, 3, 0);
    else p->driverVersion = (44u << 22) | (1u << 12);
    if (fk.on == FAKE_ARM11 && p->apiVersion > VK_API_VERSION_1_1) p->apiVersion = VK_MAKE_API_VERSION(0, 1, 1, VK_API_VERSION_PATCH(p->apiVersion));
}

static void fake_chain(void *next)
{
    for (VkBaseOutStructure *b = next; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *)b;
            if (fk.on == FAKE_PANVK) {
                d->driverID = VK_DRIVER_ID_MESA_PANVK;
                snprintf(d->driverName, sizeof(d->driverName), "panvk");
                snprintf(d->driverInfo, sizeof(d->driverInfo), "Mesa 26.3.0");
            } else if (fk.on == FAKE_SAMSUNG) {
                d->driverID = VK_DRIVER_ID_SAMSUNG_PROPRIETARY;
                snprintf(d->driverName, sizeof(d->driverName), "Samsung Proprietary driver");
            } else if (fk.on == FAKE_ARM) {
                d->driverID = VK_DRIVER_ID_ARM_PROPRIETARY;
                snprintf(d->driverName, sizeof(d->driverName), "Mali");
                snprintf(d->driverInfo, sizeof(d->driverInfo), "v1.r44p1-01eac0");
            } else { /* a driver without the structure leaves it as it was given */
                d->driverID = 0;
                d->driverName[0] = d->driverInfo[0] = 0;
            }
        }
}

static VKAPI_ATTR void VKAPI_CALL fk_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    fk.props(pd, p);
    fake_props(p);
}

static VKAPI_ATTR void VKAPI_CALL fk_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    fk.props2(pd, p);
    fake_props(&p->properties);
    fake_chain(p->pNext);
}

static VKAPI_ATTR void VKAPI_CALL fk_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    fk.props2khr(pd, p);
    fake_props(&p->properties);
    fake_chain(p->pNext);
}

/* arm11: VK_KHR_driver_properties taken off the driver's list */
static VKAPI_ATTR VkResult VKAPI_CALL fk_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count,
                                                                            VkExtensionProperties *out)
{
    if (layer) return fk.exts(pd, layer, count, out);
    uint32_t n = 0;
    VkResult r = fk.exts(pd, NULL, &n, NULL);
    if (r < 0) return r;
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof(*all));
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = fk.exts(pd, NULL, &n, all);
    if (r < 0) {
        free(all);
        return r;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(all[i].extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) all[k++] = all[i];
    r = VK_SUCCESS;
    if (out) {
        if (*count < k) k = *count, r = VK_INCOMPLETE;
        memcpy(out, all, k * sizeof(*all));
    }
    *count = k;
    free(all);
    return r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fk_GetInstanceProcAddr(VkInstance inst, const char *n)
{
    PFN_vkVoidFunction f = fk.gipa(inst, n);
    if (!f || !inst) return f;
    if (!strcmp(n, "vkGetPhysicalDeviceProperties")) {
        fk.props = (PFN_vkGetPhysicalDeviceProperties)f;
        return (PFN_vkVoidFunction)fk_GetPhysicalDeviceProperties;
    }
    if (!strcmp(n, "vkGetPhysicalDeviceProperties2")) {
        fk.props2 = (PFN_vkGetPhysicalDeviceProperties2)f;
        return (PFN_vkVoidFunction)fk_GetPhysicalDeviceProperties2;
    }
    if (!strcmp(n, "vkGetPhysicalDeviceProperties2KHR")) {
        fk.props2khr = (PFN_vkGetPhysicalDeviceProperties2)f;
        return (PFN_vkVoidFunction)fk_GetPhysicalDeviceProperties2KHR;
    }
    if (fk.on == FAKE_ARM11 && !strcmp(n, "vkEnumerateDeviceExtensionProperties")) {
        fk.exts = (PFN_vkEnumerateDeviceExtensionProperties)f;
        return (PFN_vkVoidFunction)fk_EnumerateDeviceExtensionProperties;
    }
    return f;
}

PFN_vkGetInstanceProcAddr vkb_driverid_imitate_gipa(PFN_vkGetInstanceProcAddr gipa)
{
    const char *v = vkb_srv_env("VKBRIDGE_FAKE_DRIVER");
    if (!v || !*v) return gipa;
    fk.on = !strcmp(v, "panvk") ? FAKE_PANVK : !strcmp(v, "arm") ? FAKE_ARM : !strcmp(v, "arm11") ? FAKE_ARM11
            : !strcmp(v, "samsung") ? FAKE_SAMSUNG : FAKE_OFF;
    if (!fk.on) {
        VKB_WARN("VKBRIDGE_FAKE_DRIVER=%s means nothing (panvk, arm, arm11 or samsung)", v);
        return gipa;
    }
    fk.gipa = gipa;
    if (fk.on == FAKE_SAMSUNG) VKB_INFO("imitating Samsung's driver (VKBRIDGE_FAKE_DRIVER=%s): vendor 0x144D, driverID SAMSUNG_PROPRIETARY", v);
    else
        VKB_INFO("imitating %s (VKBRIDGE_FAKE_DRIVER=%s): vendor 0x13B5", fk.on == FAKE_PANVK ? "Mesa PanVK 26.3.0" : fk.on == FAKE_ARM ? "Arm's driver r44p1"
                                                                          : "Arm's driver r44p1 as Vulkan 1.1 without VK_KHR_driver_properties", v);
    return fk_GetInstanceProcAddr;
}
