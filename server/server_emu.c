/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Feature emulation: entry point. What the GPU lacks and the bridge can make up for is decided
 * here per physical device; the client reports those features as present, the server strips
 * them from vkCreateDevice and installs wrappers in the device's dispatch table.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int token(const char *list, const char *t)
{
    size_t n = strlen(t);
    for (const char *p = list; p && (p = strstr(p, t)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return 1;
    return 0;
}

/* VKBRIDGE_FAKE_MISSING, taken once: from the process environment (host tests) or from
 * <cache dir>/env, which the server reads before its startup self-test. The in-app server tests
 * once per app process, so on a tablet a change in the file needs the app restarted (force stop);
 * a snapshot keeps vkCreateDevice consistent with what the self-test decided. With it a newer Mali
 * can take an older one's paths, e.g. the G925 the G720's (docs: w3-stall-report). */
static const char *fake_list(void)
{
    static const char *snap;
    static int taken;
    if (!__atomic_load_n(&taken, __ATOMIC_ACQUIRE)) {
        const char *v = vkb_srv_env("VKBRIDGE_FAKE_MISSING");
        snap = v && *v ? strdup(v) : NULL;
        __atomic_store_n(&taken, 1, __ATOMIC_RELEASE);
    }
    return snap;
}

int vkb_emu_faked(const char *t)
{
    const char *fake = fake_list();
    return fake && token(fake, t);
}

static int env_off(const char *name)
{
    const char *v = vkb_srv_env(name);
    return v && !strcmp(v, "0");
}

/* Feature tokens of VKBRIDGE_FAKE_MISSING and the driver extensions they stand for. */
static const struct {
    const char *token, *ext[2];
} fake_exts[] = {
    {"divisor", {"VK_EXT_vertex_attribute_divisor", "VK_KHR_vertex_attribute_divisor"}},
    {"depthclip", {"VK_EXT_depth_clip_enable", NULL}},
    {"maint5", {"VK_KHR_maintenance5", NULL}},
    {"maint6", {"VK_KHR_maintenance6", NULL}},
    {"push", {"VK_KHR_push_descriptor", NULL}},
    {"robustness2", {"VK_EXT_robustness2", "VK_KHR_robustness2"}},
    {"loadstorenone", {"VK_KHR_load_store_op_none", NULL}},
    {"dynrend", {"VK_KHR_dynamic_rendering", NULL}},
    {"vidyn", {"VK_EXT_vertex_input_dynamic_state", NULL}},
};

/* A driver extension the clients are not shown: named in VKBRIDGE_FAKE_MISSING (as itself or
 * through a feature token above) or in VKBRIDGE_SERVER_HIDE_EXTS (same snapshot rules; the
 * client's own VKBRIDGE_HIDE_EXTS is separate). What an emulation offers in its place the client
 * adds itself. */
static const char *hide_list(void)
{
    static const char *snap;
    static int taken;
    if (!__atomic_load_n(&taken, __ATOMIC_ACQUIRE)) {
        const char *v = vkb_srv_env("VKBRIDGE_SERVER_HIDE_EXTS");
        snap = v && *v ? strdup(v) : NULL;
        __atomic_store_n(&taken, 1, __ATOMIC_RELEASE);
    }
    return snap;
}

int vkb_emu_ext_hidden(const char *name)
{
    const char *hide = hide_list(), *fake = fake_list();
    if (hide && token(hide, name)) return 1;
    if (!fake) return 0;
    if (token(fake, name)) return 1;
    for (size_t i = 0; i < sizeof(fake_exts) / sizeof(fake_exts[0]); i++)
        if (token(fake, fake_exts[i].token))
            for (int j = 0; j < 2; j++)
                if (fake_exts[i].ext[j] && !strcmp(fake_exts[i].ext[j], name)) return 1;
    return 0;
}

/*
 * VKBRIDGE_HIDE_CALIBRATED_TS: VK_EXT_calibrated_timestamps (and its KHR promotion) hidden from a
 * game. GameNative's wrapper hides the EXT name on every Arm device (GameNative/mesa wrapper-25,
 * wrapper_physical_device.c), with no reason given in its code or history, and Lee Gao's wrapper
 * does not; no failure with it is known here, so it is opt-in, per game through a preset
 * (server_presets.c): "1" hides both names, "ext" the EXT name only (as GameNative does, which
 * leaves the same entry points under the KHR name). Read on the client's connection thread at each
 * enumeration, so that game's preset applies.
 */
static int calts_hidden(const char *name)
{
    if (strcmp(name, "VK_EXT_calibrated_timestamps") && strcmp(name, "VK_KHR_calibrated_timestamps")) return 0;
    const char *v = vkb_srv_env("VKBRIDGE_HIDE_CALIBRATED_TS");
    if (!v || !*v || !strcmp(v, "0")) return 0;
    if (!strcmp(v, "ext")) return !strcmp(name, "VK_EXT_calibrated_timestamps");
    return 1;
}

int vkb_emu_hiding_exts(void)
{
    return hide_list() != NULL || fake_list() != NULL;
}

/* The device extensions a client sees, without the hidden ones (vkb_emu_ext_hidden). */
static VKAPI_ATTR VkResult VKAPI_CALL hide_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count,
                                                                              VkExtensionProperties *props)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (!t || layer) return t ? t->real.vkEnumerateDeviceExtensionProperties(pd, layer, count, props) : VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0;
    VkResult r = t->real.vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof(*all));
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = t->real.vkEnumerateDeviceExtensionProperties(pd, NULL, &n, all);
    if (r < 0) {
        free(all);
        return r;
    }
    uint32_t kept = 0;
    const char *erratum = vkb_errata_hidden_ext(t, pd);
    const vkb_srv_table *inst = t->is_device ? t->parent : t;
    /* Zink gets VK_EXT_vertex_input_dynamic_state on every driver (server_vidyn.c): the driver's,
     * or the server's in its place; and no graphics pipeline libraries where the server makes
     * pipeline variants (a library's vertex input cannot be made static). */
    int vidyn = vkb_vidyn_mode(inst, pd), no_gpl = vidyn && vkb_vidyn_variants(inst, pd);
    /* With that off (VKBRIDGE_EMU_VIDYN=0), Zink on a driver that needs the packed vertex format
     * emulations: its vertex formats at pipeline creation, where the vertex shader can be fitted to
     * them (vkb_emu_bgr10_for). */
    const vkb_pd_knowledge *k = inst && inst->zink && !vidyn ? vkb_pd_lookup(&t->real, pd) : NULL;
    int hide_vi_dyn = k && (k->info.emu & (VKB_EMU_BGR10_VTX | VKB_EMU_UF11_VTX));
    int have_vi_dyn = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *e = all[i].extensionName;
        int is_vi = !strcmp(e, "VK_EXT_vertex_input_dynamic_state");
        if (is_vi && vidyn == VKB_VIDYN_EMULATED) continue; /* the server's below */
        if (is_vi && vidyn == VKB_VIDYN_NATIVE) {
            all[kept++] = all[i], have_vi_dyn = 1;
            continue;
        }
        if (!vkb_emu_ext_hidden(e) && !(erratum && !strcmp(e, erratum)) && !calts_hidden(e) && !(hide_vi_dyn && is_vi) &&
            !(no_gpl && !strcmp(e, "VK_EXT_graphics_pipeline_library")))
            all[kept++] = all[i];
    }
    if (vidyn == VKB_VIDYN_EMULATED || (vidyn == VKB_VIDYN_NATIVE && !have_vi_dyn)) {
        VkExtensionProperties *grown = realloc(all, (n + 1) * sizeof(*all));
        if (grown) {
            all = grown;
            memset(&all[kept], 0, sizeof(all[kept]));
            snprintf(all[kept].extensionName, sizeof(all[kept].extensionName), "VK_EXT_vertex_input_dynamic_state");
            all[kept++].specVersion = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_SPEC_VERSION;
        }
    }
    if (hide_vi_dyn) VKB_ONCE(VKB_LOG_INFO, "emulation: VK_EXT_vertex_input_dynamic_state hidden from Zink (packed vertex fetch emulated)");
    if (vidyn)
        VKB_ONCE(VKB_LOG_INFO, "emulation: VK_EXT_vertex_input_dynamic_state offered to Zink: %s%s",
                 vidyn == VKB_VIDYN_EMULATED ? "the server's (not in the driver)" : "the driver's",
                 no_gpl ? "; graphics pipeline libraries hidden from it (pipeline variants)" : "");
    if (!props) {
        *count = kept;
        free(all);
        return VK_SUCCESS;
    }
    uint32_t w = *count < kept ? *count : kept;
    memcpy(props, all, w * sizeof(*props));
    *count = w;
    free(all);
    return w < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

/* VKBRIDGE_FAKE_MISSING=planarext (host tests): the Mali-G925's refusal of
 * VK_IMAGE_CREATE_EXTENDED_USAGE_BIT on multi-planar (YCbCr) formats, which DXVK sets on every NV12
 * texture: the format queries answer VK_ERROR_FORMAT_NOT_SUPPORTED, image creation fails
 * (driver_create_image). The client takes the flag off where the format covers the usage
 * (client/emu.c, vkb_emu_planar_ext_usage). */
int vkb_emu_planar_ext_refused(VkFormat f, VkImageCreateFlags flags)
{
    static int fake = -1;
    if (fake < 0) fake = vkb_emu_faked("planarext");
    if (!fake || !(flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)) return 0;
    return (f >= VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM && f <= VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM) ||
           (f >= VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16 && f <= VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16) ||
           (f >= VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16 && f <= VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16) ||
           (f >= VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM && f <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM) ||
           (f >= VK_FORMAT_G8_B8R8_2PLANE_444_UNORM && f <= VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
}

static PFN_vkGetPhysicalDeviceImageFormatProperties planar_next1;
static PFN_vkGetPhysicalDeviceImageFormatProperties2 planar_next2;

static VKAPI_ATTR VkResult VKAPI_CALL planar_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat format, VkImageType type,
                                                                                   VkImageTiling tiling, VkImageUsageFlags usage,
                                                                                   VkImageCreateFlags flags, VkImageFormatProperties *props)
{
    if (vkb_emu_planar_ext_refused(format, flags)) {
        memset(props, 0, sizeof(*props));
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return planar_next1(pd, format, type, tiling, usage, flags, props);
}

static VKAPI_ATTR VkResult VKAPI_CALL planar_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                                    const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                                    VkImageFormatProperties2 *props)
{
    if (vkb_emu_planar_ext_refused(info->format, info->flags)) {
        memset(&props->imageFormatProperties, 0, sizeof(props->imageFormatProperties));
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return planar_next2(pd, info, props);
}

/* depthClamp (VKB_EMU_DEPTH_CLAMP) is reported here as well as by the client (client/emu.c), so a
 * guest whose client library predates the flag sees it too. */
static PFN_vkGetPhysicalDeviceFeatures clamp_next1;
static PFN_vkGetPhysicalDeviceFeatures2 clamp_next2;

static int clamp_on(VkPhysicalDevice pd)
{
    vkb_srv_table *t = vkb_srv_current_table();
    const vkb_pd_knowledge *k = t ? vkb_pd_lookup(&t->real, pd) : NULL;
    return k && (k->info.emu & VKB_EMU_DEPTH_CLAMP);
}

static VKAPI_ATTR void VKAPI_CALL clamp_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
    clamp_next1(pd, f);
    if (clamp_on(pd)) f->depthClamp = VK_TRUE;
}

static VKAPI_ATTR void VKAPI_CALL clamp_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    clamp_next2(pd, f);
    if (clamp_on(pd)) f->features.depthClamp = VK_TRUE;
}

void vkb_emu_instance_install(vkb_srv_table *t)
{
    if (t->dt.vkGetPhysicalDeviceFeatures) {
        clamp_next1 = t->dt.vkGetPhysicalDeviceFeatures;
        t->dt.vkGetPhysicalDeviceFeatures = clamp_GetPhysicalDeviceFeatures;
    }
    if (t->dt.vkGetPhysicalDeviceFeatures2) {
        clamp_next2 = t->dt.vkGetPhysicalDeviceFeatures2;
        t->dt.vkGetPhysicalDeviceFeatures2 = clamp_GetPhysicalDeviceFeatures2;
    }
    if (vkb_emu_faked("planarext")) {
        if (t->dt.vkGetPhysicalDeviceImageFormatProperties) {
            planar_next1 = t->dt.vkGetPhysicalDeviceImageFormatProperties;
            t->dt.vkGetPhysicalDeviceImageFormatProperties = planar_GetPhysicalDeviceImageFormatProperties;
        }
        if (t->dt.vkGetPhysicalDeviceImageFormatProperties2) {
            planar_next2 = t->dt.vkGetPhysicalDeviceImageFormatProperties2;
            t->dt.vkGetPhysicalDeviceImageFormatProperties2 = planar_GetPhysicalDeviceImageFormatProperties2;
        }
        VKB_ONCE(VKB_LOG_INFO, "imitating Mali's refusal of extended usage on multi-planar formats (VKBRIDGE_FAKE_MISSING=planarext)");
    }
    /* always: fp16_hide (MK-20) decides per query whether 16-bit floats are offered */
    vkb_errata_fp16_instance_install(t);
    if (!t->real.vkEnumerateDeviceExtensionProperties) return;
    /* always: the driver errata hide by GPU and driver version (vkb_errata_hidden_ext) */
    t->dt.vkEnumerateDeviceExtensionProperties = hide_EnumerateDeviceExtensionProperties;
    if (vkb_emu_hiding_exts()) VKB_ONCE(VKB_LOG_INFO, "device extensions hidden from clients: VKBRIDGE_FAKE_MISSING=%s VKBRIDGE_SERVER_HIDE_EXTS=%s",
             fake_list() ? fake_list() : "", hide_list() ? hide_list() : "");
}

/* ------------------------------------------------------------------ robustness2 without its robust access */

/*
 * The Mali-G715 on r54p3 (Pixel 8 / 8 Pro / 8a, Android 17; driver 0x0d803000) has VK_EXT_robustness2
 * with nullDescriptor but neither robustBufferAccess2 nor robustImageAccess2. DXVK 3 requires
 * robustBufferAccess2 (dxvk_device_info.cpp, the robustness2 entries of the feature list) and drops
 * an adapter without it; with no adapter left its DXGI factory is never made, so every D3D9-12 game
 * fails at CreateDXGIFactory (E_FAIL): NieR's "No Graphic Memory", Dead Space's
 * "CreateDXGIFactory1(IDXGIFactory4) E_FAIL", Steam's d3ddriverquery64 exit code 1. vkd3d-proton
 * requires both (device.c, "Robustness2 features not supported").
 * The bridge reports them (VKB_EMU_ROBUST2_ACCESS) and, as for the full robustness2 emulation,
 * turns on the robustness the GPU has instead: core robustBufferAccess for robustBufferAccess2,
 * robustImageAccess (Vulkan 1.3 / VK_EXT_image_robustness) for robustImageAccess2. What differs:
 * an out-of-bounds read may give an in-bounds value rather than zero, and the bounds are the
 * descriptor's range rounded as robustBufferAccess allows. Null descriptors stay the driver's own.
 */
int vkb_emu_robust2_driver(const vkb_dispatch *dt, VkPhysicalDevice pd, VkBool32 *buf, VkBool32 *img, VkBool32 *null_desc)
{
    *buf = *img = *null_desc = VK_FALSE;
    if (!dt->vkGetPhysicalDeviceFeatures2) return 0;
    VkPhysicalDeviceRobustness2FeaturesEXT r2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &r2};
    dt->vkGetPhysicalDeviceFeatures2(pd, &f2);
    *buf = r2.robustBufferAccess2;
    *img = r2.robustImageAccess2;
    *null_desc = r2.nullDescriptor;
    return 1;
}

/* The app's robustness2 features in a device request (NULL: none) */
static VkPhysicalDeviceRobustness2FeaturesEXT *robust2_request(const VkDeviceCreateInfo *ci)
{
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) return (VkPhysicalDeviceRobustness2FeaturesEXT *)b;
    return NULL;
}

int vkb_emu_robust2_image_stand_in(vkb_srv_table *inst, VkPhysicalDevice pd, uint32_t emu, const VkDeviceCreateInfo *ci)
{
    if (!(emu & VKB_EMU_ROBUST2_ACCESS)) return 0;
    const VkPhysicalDeviceRobustness2FeaturesEXT *r = robust2_request(ci);
    if (!r || !r->robustImageAccess2) return 0;
    VkBool32 b, i, n;
    return vkb_emu_robust2_driver(&inst->real, pd, &b, &i, &n) && !i;
}

/*
 * VKBRIDGE_FAKE_MISSING=robust2access (host tests): that driver's robustness2 imitated. The feature
 * query answers robustBufferAccess2 and robustImageAccess2 false, and a device that asks for
 * either is refused with VK_ERROR_FEATURE_NOT_PRESENT, as a driver may.
 */
static struct {
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetPhysicalDeviceFeatures2 f2, f2khr;
    PFN_vkCreateDevice create;
} r2imit;

static void r2imit_clear(void *next)
{
    for (VkBaseOutStructure *b = next; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT *r = (VkPhysicalDeviceRobustness2FeaturesEXT *)b;
            r->robustBufferAccess2 = r->robustImageAccess2 = VK_FALSE;
        }
}

static VKAPI_ATTR void VKAPI_CALL r2imit_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    r2imit.f2(pd, f);
    r2imit_clear(f->pNext);
}

static VKAPI_ATTR void VKAPI_CALL r2imit_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    r2imit.f2khr(pd, f);
    r2imit_clear(f->pNext);
}

static VKAPI_ATTR VkResult VKAPI_CALL r2imit_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a,
                                                          VkDevice *out)
{
    const VkPhysicalDeviceRobustness2FeaturesEXT *r = robust2_request(ci);
    if (r && (r->robustBufferAccess2 || r->robustImageAccess2)) {
        VKB_WARN("imitated robustness2: device refused (robustBufferAccess2 %u, robustImageAccess2 %u asked; the driver has neither)",
                 r->robustBufferAccess2, r->robustImageAccess2);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    return r2imit.create(pd, ci, a, out);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL r2imit_GetInstanceProcAddr(VkInstance inst, const char *n)
{
    PFN_vkVoidFunction f = r2imit.gipa(inst, n);
    if (!f || !inst) return f;
    if (!strcmp(n, "vkGetPhysicalDeviceFeatures2")) {
        r2imit.f2 = (PFN_vkGetPhysicalDeviceFeatures2)f;
        return (PFN_vkVoidFunction)r2imit_GetPhysicalDeviceFeatures2;
    }
    if (!strcmp(n, "vkGetPhysicalDeviceFeatures2KHR")) {
        r2imit.f2khr = (PFN_vkGetPhysicalDeviceFeatures2)f;
        return (PFN_vkVoidFunction)r2imit_GetPhysicalDeviceFeatures2KHR;
    }
    if (!strcmp(n, "vkCreateDevice")) {
        r2imit.create = (PFN_vkCreateDevice)f;
        return (PFN_vkVoidFunction)r2imit_CreateDevice;
    }
    return f;
}

PFN_vkGetInstanceProcAddr vkb_emu_imitate_gipa(PFN_vkGetInstanceProcAddr gipa)
{
    if (!vkb_emu_faked("robust2access")) return gipa;
    r2imit.gipa = gipa;
    VKB_INFO("imitating the Mali-G715 r54p3's robustness2: nullDescriptor only (VKBRIDGE_FAKE_MISSING=robust2access)");
    return r2imit_GetInstanceProcAddr;
}

/* The driver offers VK_EXT_vertex_input_dynamic_state (VKBRIDGE_FAKE_MISSING=vidyn: as if not). */
int vkb_emu_driver_vi_dyn(const vkb_dispatch *d, VkPhysicalDevice pd)
{
    const char *fake = fake_list();
    if (fake && token(fake, "vidyn")) return 0;
    uint32_t n = 0;
    d->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    if (!e) return 1;
    d->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(e[i].extensionName, "VK_EXT_vertex_input_dynamic_state");
    free(e);
    return found;
}

/* Whether this instance's clients get VKB_EMU_BGR10_VTX (the GPU needing it is the caller's check):
 * where the driver offers VK_EXT_vertex_input_dynamic_state, only Zink, for which the vertex input
 * of vkCmdSetVertexInputEXT is made static in pipeline variants (server_vidyn.c), or from which the
 * extension is hidden (VKBRIDGE_EMU_VIDYN=0). */
int vkb_emu_bgr10_for(const vkb_srv_table *inst, VkPhysicalDevice pd)
{
    if (!inst) return 0;
    if (inst->zink) return 1;
    return !vkb_emu_driver_vi_dyn(&inst->real, pd);
}

uint32_t vkb_emu_detect(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t *missing)
{
    VkPhysicalDeviceFeatures f;
    idt->vkGetPhysicalDeviceFeatures(pd, &f);
    uint32_t n = 0;
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int divisor = 0, depth_clip = 0, maint5 = 0, robust2 = 0, maint6 = 0, lson_khr = 0, lson_ext = 0, push = 0, dynrend = 0, rp2 = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(e[i].extensionName, "VK_EXT_vertex_attribute_divisor") ||
            !strcmp(e[i].extensionName, "VK_KHR_vertex_attribute_divisor"))
            divisor = 1;
        if (!strcmp(e[i].extensionName, "VK_EXT_depth_clip_enable")) depth_clip = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_maintenance5")) maint5 = 1;
        if (!strcmp(e[i].extensionName, "VK_EXT_robustness2") || !strcmp(e[i].extensionName, "VK_KHR_robustness2")) robust2 = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_maintenance6")) maint6 = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_push_descriptor")) push = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_load_store_op_none")) lson_khr = 1;
        if (!strcmp(e[i].extensionName, "VK_EXT_load_store_op_none")) lson_ext = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_dynamic_rendering")) dynrend = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_create_renderpass2")) rp2 = 1;
    }
    free(e);
    /* VKBRIDGE_FAKE_MISSING=bc,divisor,clip,cull,depthclip pretends the GPU lacks them (host tests);
     * divisorzero: a divisor without vertexAttributeInstanceRateZeroDivisor (Mali-G925). */
    const char *fake = fake_list();
    if (fake) {
        if (token(fake, "bc")) f.textureCompressionBC = VK_FALSE;
        if (token(fake, "divisor")) divisor = 0;
        if (token(fake, "clip")) f.shaderClipDistance = VK_FALSE;
        if (token(fake, "cull")) f.shaderCullDistance = VK_FALSE;
        if (token(fake, "depthclip")) depth_clip = 0;
        if (token(fake, "maint5")) maint5 = 0;
        if (token(fake, "robustness2")) robust2 = 0;
        if (token(fake, "dualsrc")) f.dualSrcBlend = VK_FALSE;
        if (token(fake, "fillmode")) f.fillModeNonSolid = VK_FALSE;
        if (token(fake, "depthclamp")) f.depthClamp = VK_FALSE;
        if (token(fake, "multiviewport")) f.multiViewport = VK_FALSE;
        if (token(fake, "multidraw")) f.multiDrawIndirect = VK_FALSE;
        if (token(fake, "maint6")) maint6 = 0;
        if (token(fake, "push")) push = 0;
        if (token(fake, "loadstorenone")) lson_khr = 0;
        if (token(fake, "dynrend")) dynrend = 0;
    }
    uint32_t emu = 0;
    VkPhysicalDeviceProperties props;
    idt->vkGetPhysicalDeviceProperties(pd, &props);
    /* vkd3d-proton (every D3D12 game) wants vertexAttributeInstanceRateZeroDivisor too. Mali-G925 on
     * Android 16 has the KHR extension without it (and without supportsNonZeroFirstInstance): such a
     * divisor is taken as missing and emulated whole - the emulation does both, at any firstInstance. */
    if (divisor && idt->vkGetPhysicalDeviceFeatures2 && props.apiVersion >= VK_API_VERSION_1_1) {
        VkPhysicalDeviceVertexAttributeDivisorFeatures df = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &df};
        idt->vkGetPhysicalDeviceFeatures2(pd, &f2);
        int zero = df.vertexAttributeInstanceRateZeroDivisor && !(fake && token(fake, "divisorzero"));
        if (!df.vertexAttributeInstanceRateDivisor || !zero) {
            VKB_ONCE(VKB_LOG_INFO, "vertex attribute divisor: the driver's lacks %s; emulated instead",
                     df.vertexAttributeInstanceRateDivisor ? "vertexAttributeInstanceRateZeroDivisor" : "vertexAttributeInstanceRateDivisor");
            divisor = 0;
        }
    }
    /* Vulkan 1.4 has maintenance5 in core. */
    if (!maint5 && (props.apiVersion < VK_API_VERSION_1_4 || (fake && token(fake, "maint5"))) && !env_off("VKBRIDGE_EMU_MAINT5"))
        emu |= VKB_EMU_MAINT5;
    if (!f.textureCompressionBC && vkb_emu_bcn_supported(idt, pd) && !env_off("VKBRIDGE_EMU_BCN")) emu |= VKB_EMU_BCN;
    if (!divisor && !env_off("VKBRIDGE_EMU_DIVISOR")) emu |= VKB_EMU_DIVISOR;
    if (!f.shaderClipDistance && !env_off("VKBRIDGE_EMU_CLIP")) emu |= VKB_EMU_CLIP_DISTANCE;
    if (!f.shaderCullDistance && !env_off("VKBRIDGE_EMU_CULL")) emu |= VKB_EMU_CULL_DISTANCE;
    if (!depth_clip && f.depthClamp && !env_off("VKBRIDGE_EMU_DEPTHCLIP")) emu |= VKB_EMU_DEPTH_CLIP;
    /* DXVK will not start without robustness2. Mali r44 lacks it; GameNative's wrapper reports it
     * (robustBufferAccess2, robustImageAccess2, nullDescriptor) without enforcing it, and DXVK
     * games run - out-of-bounds reads and null descriptors are then up to the driver. */
    if (!robust2 && f.robustBufferAccess && !env_off("VKBRIDGE_EMU_ROBUSTNESS2")) emu |= VKB_EMU_ROBUSTNESS2;
    /* The driver's robustness2 without robustBufferAccess2 / robustImageAccess2 (Mali-G715 r54p3):
     * those two reported, the driver's null descriptors kept (vkb_emu_robust2_driver). */
    int r2_partial = 0;
    if (robust2 && props.apiVersion >= VK_API_VERSION_1_1) {
        VkBool32 rb, ri, rn;
        if (vkb_emu_robust2_driver(idt, pd, &rb, &ri, &rn) && (!rb || !ri)) {
            r2_partial = 1;
            int on = rn && f.robustBufferAccess && !env_off("VKBRIDGE_EMU_ROBUST2ACCESS");
            if (on) emu |= VKB_EMU_ROBUST2_ACCESS;
            VKB_ONCE(VKB_LOG_INFO, "robustness2: the driver's has robustBufferAccess2 %u, robustImageAccess2 %u, nullDescriptor %u; %s", rb, ri,
                     rn, on ? "the missing ones reported, the GPU's own robustness on in their place (DXVK 3 and vkd3d-proton require them)"
                     : !rn ? "not reported (no nullDescriptor either)"
                     : !f.robustBufferAccess ? "not reported (no robustBufferAccess to stand in)"
                     : "not reported (VKBRIDGE_EMU_ROBUST2ACCESS=0): DXVK 3 and vkd3d-proton will not use this GPU");
        }
    }
    /* dma-buf export of images (gamescope's nested backend needs it): emulated with dma-heap pages
     * where the driver imports dma-bufs into images but exports none (Mali); AHardwareBuffer pages
     * where there is no dma-heap (vkb_mem_export_backing). */
    if (!env_off("VKBRIDGE_EMU_DMABUF_EXPORT") && vkb_mem_export_backing()) {
        int can_export = 0, can_import = 0;
        if (idt->vkGetPhysicalDeviceImageFormatProperties2) {
            /* Asked as gamescope asks: a linear DRM modifier (Mali answers import questions only so). */
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, NULL,
                                                                0 /* DRM_FORMAT_MOD_LINEAR */, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
            VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi,
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
            VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, VK_FORMAT_R8G8B8A8_UNORM,
                                                   VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0};
            VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
            if (idt->vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip) == VK_SUCCESS) {
                VkExternalMemoryFeatureFlags ef = ep.externalMemoryProperties.externalMemoryFeatures;
                can_export = (ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
                can_import = (ef & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
            }
        }
        if (fake && token(fake, "dmabufexport")) can_export = 0;
        if (!can_export && can_import) {
            emu |= VKB_EMU_DMABUF_EXPORT;
            VKB_ONCE(VKB_LOG_INFO, "dma-buf export: emulated, pages from the %s", vkb_mem_export_backing_name());
        }
    }
    /* OPAQUE_FD external memory on dma-bufs (server_opaque.c): DXVK's D3D11 shared resources. */
    if (!env_off("VKBRIDGE_EMU_OPAQUEFD") && vkb_opaque_wanted(idt, pd, emu)) emu |= VKB_EMU_OPAQUE_FD;
    /* What DXVK 3 requires and the GPU lacks (server_emu_compat.c). Vulkan 1.4 has maintenance6. */
    if (!f.dualSrcBlend && !env_off("VKBRIDGE_EMU_DUALSRC")) emu |= VKB_EMU_DUAL_SRC;
    if (!f.fillModeNonSolid && !env_off("VKBRIDGE_EMU_FILLMODE")) emu |= VKB_EMU_FILL_MODE;
    if (!f.depthClamp && !env_off("VKBRIDGE_EMU_DEPTHCLAMP")) emu |= VKB_EMU_DEPTH_CLAMP;
    if (!f.multiViewport && !env_off("VKBRIDGE_EMU_MULTIVIEWPORT")) emu |= VKB_EMU_MULTI_VIEWPORT;
    /* DXVK refuses a device without multiDrawIndirect; Mali-G57 on r32p1/r38p1 has none (Lee Gao's
     * compat_layer README). Indirect draws are split into single ones (server_emu_compat.c). */
    if (!f.multiDrawIndirect && !env_off("VKBRIDGE_EMU_MULTIDRAW")) emu |= VKB_EMU_MULTI_DRAW;
    if (!maint6 && (props.apiVersion < VK_API_VERSION_1_4 || (fake && token(fake, "maint6"))) && !env_off("VKBRIDGE_EMU_MAINT6"))
        emu |= VKB_EMU_MAINT6;
    if (!lson_khr && lson_ext) emu |= VKB_EMU_LOADSTORE_NONE;
    /* Zink (Mesa 25 and later: Steam's UI, every OpenGL game) refuses a device without dynamic
     * rendering; Mali's Vulkan 1.1 drivers (r32p1) lack it. Renderings become render passes
     * (server_emu_dynrend.c), made with vkCreateRenderPass2 (core in 1.2, an extension before). */
    int dynrend_missing = !dynrend && (props.apiVersion < VK_API_VERSION_1_3 || (fake && token(fake, "dynrend")));
    if (dynrend_missing && (rp2 || props.apiVersion >= VK_API_VERSION_1_2) && !env_off("VKBRIDGE_EMU_DYNREND")) emu |= VKB_EMU_DYN_RENDER;
    /* Zink offers OpenGL 3.3 only with GL_ARB_vertex_type_2_10_10_10_rev, which wants every packed
     * 10-10-10-2 vertex format. Mali (G720 r44p1, G68, G57, G925 ...) lacks the BGRA signed one,
     * A2R10G10B10_SNORM_PACK32, and has A2B10G10R10_SNORM: Zink stopped at OpenGL 3.2, and LÖVE 12 and
     * Godot 3 games would not start ("does not support any of the supported OpenGL versions", issue #5).
     * Fetched as the RGBA format, with red and blue swapped where the vertex shader reads it
     * (server_emu_shader.c). A format set at record time (VK_EXT_vertex_input_dynamic_state, r48 and
     * later; below r48 the errata hide it) cannot reach the shader: for Zink such a vertex input is
     * made static in a pipeline variant (server_vidyn.c), and every other client keeps the
     * extension and goes without the emulation (vkb_emu_bgr10_for). */
    /* (r32p1 lacks the BGRA USCALED and SSCALED ones too) */
    int bgr10_missing = 0;
    static const VkFormat bgr10[] = {VK_FORMAT_A2R10G10B10_SNORM_PACK32, VK_FORMAT_A2R10G10B10_USCALED_PACK32,
                                     VK_FORMAT_A2R10G10B10_SSCALED_PACK32};
    for (size_t i = 0; i < sizeof(bgr10) / sizeof(bgr10[0]); i++) {
        VkFormatProperties bgr, rgb;
        idt->vkGetPhysicalDeviceFormatProperties(pd, bgr10[i], &bgr);
        idt->vkGetPhysicalDeviceFormatProperties(pd, vkb_bgr10_rgba(bgr10[i]), &rgb);
        if (fake && token(fake, "bgr10vtx")) bgr.bufferFeatures &= ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        bgr10_missing |= !(bgr.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) && (rgb.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    }
    if (bgr10_missing && !env_off("VKBRIDGE_EMU_BGR10VTX")) emu |= VKB_EMU_BGR10_VTX; /* per client: vkb_emu_bgr10_for */
    /* B10G11R11_UFLOAT vertex fetch: Mali before r49 (G720/G615 r44, G610/G710 r32, G57/G68 r38) lacks it,
     * and Zink offers OpenGL 4.4 only with GL_ARB_vertex_type_10f_11f_11f_rev. Fetched as R32_UINT and
     * unpacked in the vertex shader (server_emu_shader.c), with the same per-client rule as above. */
    int uf11_missing = 0;
    {
        VkFormatProperties uf, u32;
        idt->vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &uf);
        idt->vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32_UINT, &u32);
        if (fake && token(fake, "uf11vtx")) uf.bufferFeatures &= ~(VkFormatFeatureFlags)VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        uf11_missing = !(uf.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) && (u32.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
    }
    if (uf11_missing && !env_off("VKBRIDGE_EMU_UF11VTX")) emu |= VKB_EMU_UF11_VTX;
    /* vkd3d-proton (every D3D12 game) refuses a device without push descriptors (Vulkan 1.4 has them). */
    if (!push && (props.apiVersion < VK_API_VERSION_1_4 || (fake && token(fake, "push"))) && !env_off("VKBRIDGE_EMU_PUSH")) emu |= VKB_EMU_PUSH_DESC;
    /* vkd3d-proton 3 refuses a device whose storage texel buffers need more than texel alignment
     * (Deep Rock Galactic DX12, 2026-10-11: "Lacking support for single texel alignment"). Mali
     * r44 wants 64 bytes for storage (uniform is fine). Reported as supported; D3D12 views mostly
     * start at 0 or on large boundaries, and a misaligned one is logged (server_emu_layer.c). */
    int texel_single = 1;
    if (idt->vkGetPhysicalDeviceProperties2 && props.apiVersion >= VK_API_VERSION_1_3) {
        VkPhysicalDeviceTexelBufferAlignmentProperties ta = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ta};
        idt->vkGetPhysicalDeviceProperties2(pd, &p2);
        texel_single = ta.storageTexelBufferOffsetSingleTexelAlignment || ta.storageTexelBufferOffsetAlignmentBytes == 1;
    }
    if (fake && token(fake, "texelalign")) texel_single = 0;
    if (!texel_single && !env_off("VKBRIDGE_EMU_TEXELALIGN")) emu |= VKB_EMU_TEXEL_ALIGN;
    /* vkd3d-proton's bindless path without descriptor buffers wants 1,000,000 sampled images,
     * storage images and storage buffers per stage after bind (Deep Rock Galactic DX12,
     * 2026-10-11: "Insufficient descriptor indexing support"); Mali r44 allows 500,000. Reported;
     * what reaches the driver is cut to its limits (server_emu_desc.c). */
    int desc_short = 0;
    if (props.apiVersion >= VK_API_VERSION_1_2) {
        uint32_t dl[2][3];
        vkb_emu_desc_limits_get(idt, pd, dl);
        for (int i = 0; i < 3; i++) desc_short |= dl[1][i] && dl[1][i] < 1000000;
    }
    if (desc_short && !env_off("VKBRIDGE_EMU_DESCLIMITS")) emu |= VKB_EMU_DESC_LIMITS;
    /* PointSize in geometry/tessellation stages: stripped when the feature is missing. */
    if (!f.shaderTessellationAndGeometryPointSize && (f.geometryShader || f.tessellationShader)) emu |= VKB_EMU_POINT_SIZE;
    uint32_t miss = 0;
    if (!f.textureCompressionBC) miss |= VKB_EMU_BCN;
    if (!divisor) miss |= VKB_EMU_DIVISOR;
    if (!f.shaderClipDistance) miss |= VKB_EMU_CLIP_DISTANCE;
    if (!f.shaderCullDistance) miss |= VKB_EMU_CULL_DISTANCE;
    if (!depth_clip) miss |= VKB_EMU_DEPTH_CLIP;
    if (emu & VKB_EMU_MAINT5) miss |= VKB_EMU_MAINT5;
    if (!robust2) miss |= VKB_EMU_ROBUSTNESS2;
    if (r2_partial) miss |= VKB_EMU_ROBUST2_ACCESS;
    if (!f.dualSrcBlend) miss |= VKB_EMU_DUAL_SRC;
    if (!f.fillModeNonSolid) miss |= VKB_EMU_FILL_MODE;
    if (!f.depthClamp) miss |= VKB_EMU_DEPTH_CLAMP;
    if (!f.multiViewport) miss |= VKB_EMU_MULTI_VIEWPORT;
    if (!f.multiDrawIndirect) miss |= VKB_EMU_MULTI_DRAW;
    if (emu & VKB_EMU_MAINT6) miss |= VKB_EMU_MAINT6;
    if (emu & VKB_EMU_PUSH_DESC) miss |= VKB_EMU_PUSH_DESC;
    if (dynrend_missing) miss |= VKB_EMU_DYN_RENDER;
    if (bgr10_missing) miss |= VKB_EMU_BGR10_VTX;
    if (uf11_missing) miss |= VKB_EMU_UF11_VTX;
    if (!texel_single) miss |= VKB_EMU_TEXEL_ALIGN;
    if (desc_short) miss |= VKB_EMU_DESC_LIMITS;
    *missing = miss;
    return emu & VKB_EMU_IMPLEMENTED;
}

/* Remove a structure from a pNext chain (the chain lives in the request's arena). */
static void chain_remove(VkDeviceCreateInfo *ci, VkStructureType t)
{
    VkBaseOutStructure *prev = (VkBaseOutStructure *)ci;
    while (prev->pNext) {
        if (prev->pNext->sType == t) prev->pNext = prev->pNext->pNext;
        else prev = prev->pNext;
    }
}

static VkPhysicalDeviceFeatures *find_features(VkDeviceCreateInfo *ci)
{
    if (ci->pEnabledFeatures) return (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) return &((VkPhysicalDeviceFeatures2 *)b)->features;
    return NULL;
}

void vkb_emu_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, uint32_t emu, VkDeviceCreateInfo *ci, vkb_arena *a)
{
    VkPhysicalDeviceFeatures real;
    inst->real.vkGetPhysicalDeviceFeatures(pd, &real);
    VkPhysicalDeviceFeatures *f = find_features(ci);
    if ((emu & (VKB_EMU_BCN | VKB_EMU_DEPTH_CLIP)) && !f) {
        /* The BC decoder writes R8/RG8 storage images: it needs this feature on, so the request
         * gets a features structure if it had none. */
        f = vkb_arena_alloc(a, sizeof(*f));
        ci->pEnabledFeatures = f;
    }
    if (f) {
        /* pEnabledFeatures came from the request: it is ours to change. Anything the client asked
         * for that the GPU does not have is either emulated or would fail the call. */
        VkBool32 *want = (VkBool32 *)f;
        const VkBool32 *have = (const VkBool32 *)&real;
        for (size_t i = 0; i < sizeof(*f) / sizeof(VkBool32); i++) {
            if (want[i] && !have[i]) want[i] = VK_FALSE;
        }
        if (emu & VKB_EMU_BCN) f->shaderStorageImageExtendedFormats = VK_TRUE;
        /* BC images transcoded to these (server_emu_bcn.c) need them on. */
        if ((emu & VKB_EMU_BCN) && vkb_emu_bcn_transcode_wanted()) {
            f->textureCompressionETC2 |= real.textureCompressionETC2;
            f->textureCompressionASTC_LDR |= real.textureCompressionASTC_LDR;
        }
        if (emu & VKB_EMU_DEPTH_CLIP) f->depthClamp = VK_TRUE;
    }
    /* BC6H transcoded to ASTC HDR needs its feature on: in the app's Vulkan 1.3 features if it gave
     * them (where the feature's own structure may not also be), else in that structure. */
    if ((emu & VKB_EMU_BCN) && vkb_emu_bcn_astc_hdr(&inst->real, pd, inst->api_version)) {
        VkPhysicalDeviceTextureCompressionASTCHDRFeatures *hf = NULL;
        VkPhysicalDeviceVulkan13Features *v13 = NULL;
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) v13 = (VkPhysicalDeviceVulkan13Features *)b;
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES)
                hf = (VkPhysicalDeviceTextureCompressionASTCHDRFeatures *)b;
        }
        if (v13) v13->textureCompressionASTC_HDR = VK_TRUE;
        else if (hf) hf->textureCompressionASTC_HDR = VK_TRUE;
        else if ((hf = vkb_arena_alloc(a, sizeof(*hf)))) {
            memset(hf, 0, sizeof(*hf));
            hf->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES;
            hf->textureCompressionASTC_HDR = VK_TRUE;
            hf->pNext = (void *)ci->pNext;
            ci->pNext = hf;
        }
    }
    if (emu & VKB_EMU_DIVISOR) {
        chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT);
        /* Vulkan 1.4 has the features in core: the driver sees no divisor at all. */
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) {
                VkPhysicalDeviceVulkan14Features *v = (VkPhysicalDeviceVulkan14Features *)b;
                v->vertexAttributeInstanceRateDivisor = v->vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
            }
    }
    if (emu & VKB_EMU_DEPTH_CLIP) chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT);
    if (emu & VKB_EMU_MAINT5) chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES);
    if (emu & VKB_EMU_MAINT6) chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES);
    if (emu & VKB_EMU_DYN_RENDER) chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
    if (emu & VKB_EMU_ROBUSTNESS2) {
        chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
        /* What robustness the GPU has stays on; robustBufferAccess2 asks for at least that. */
        VkPhysicalDeviceFeatures *rf = find_features(ci);
        const char *nr = vkb_srv_env("VKBRIDGE_NO_FORCED_ROBUSTNESS");
        if (rf && !(nr && *nr && strcmp(nr, "0"))) rf->robustBufferAccess = VK_TRUE;
    }
    VkPhysicalDeviceRobustness2FeaturesEXT *r2 = (emu & VKB_EMU_ROBUST2_ACCESS) ? robust2_request(ci) : NULL;
    if (r2) {
        /* What the driver lacks is taken off; robustBufferAccess2 asks for at least core robust
         * buffer access (robustImageAccess2's stand-in: server_instance.c). */
        VkBool32 hb, hi, hn;
        vkb_emu_robust2_driver(&inst->real, pd, &hb, &hi, &hn);
        int wb = r2->robustBufferAccess2 && !hb, wi = r2->robustImageAccess2 && !hi;
        if (wb) r2->robustBufferAccess2 = VK_FALSE;
        if (wi) r2->robustImageAccess2 = VK_FALSE;
        VkPhysicalDeviceFeatures *rf = find_features(ci);
        const char *nr = vkb_srv_env("VKBRIDGE_NO_FORCED_ROBUSTNESS");
        int forced = wb && rf && !(nr && *nr && strcmp(nr, "0"));
        if (forced) rf->robustBufferAccess = VK_TRUE;
        if (wb || wi)
            VKB_INFO("vkCreateDevice: robustness2's%s%s taken off (the driver lacks them; VKB_EMU_ROBUST2_ACCESS)%s",
                     wb ? " robustBufferAccess2" : "", wi ? " robustImageAccess2" : "", forced ? "; robustBufferAccess on" : "");
    }
}

void vkb_emu_device_init(vkb_srv_table *dev, uint32_t emu)
{
    if (!emu) return;
    vkb_emu_device *e = calloc(1, sizeof(*e));
    e->flags = emu;
    e->dev = dev;
    pthread_mutex_init(&e->lock, NULL);
    dev->emu = e;
    vkb_emu_install(dev);
}

void vkb_emu_device_quiesce(vkb_srv_table *dev)
{
    if (dev->emu) vkb_emu_bcn_quiesce(dev);
}

void vkb_emu_device_destroy(vkb_srv_table *dev)
{
    if (!dev->emu) return;
    vkb_emu_uninstall(dev);
    pthread_mutex_destroy(&dev->emu->lock);
    free(dev->emu);
    dev->emu = NULL;
}
