/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_zink_name (PV-08): PowerVR's driver keeps an application profile for the exact engine name
 * "mesa zink" (what Mesa's Zink puts in VkApplicationInfo). The profile hides features Zink
 * would use - VK_EXT_vertex_input_dynamic_state, sparse resources, pipeline libraries - so Zink
 * (Steam's web helper, Xwayland's glamor, a Zink in the guest) keeps a stale vertex input
 * and text vanishes. The idea is wallentx's (DroidDeck PR #570: rename the engine); this is a
 * probe-gated version of it:
 *
 *   - only when a startup probe sees the profile. On a PowerVR driver (driverID IMAGINATION or
 *     vendor 0x1010) the self-test makes one instance with the engine name "mesa zink" and one
 *     with a neutral name and compares what each shows for the physical device: the core
 *     features, vertexInputDynamicState, graphicsPipelineLibrary and the device extension list.
 *     The profile is seen when the neutral name shows everything "mesa zink" shows and more.
 *     Other drivers are not probed (they cost two more instances at every start for nothing).
 *   - the name passed is one the probe checked: the first of "Mesa zink", "Zink" that shows what
 *     the neutral reference shows, else the reference itself ("MaliBridge Zink").
 *   - only for a client whose engine name is exactly "mesa zink", only at vkCreateInstance; the
 *     server's own notion of a Zink client (t->zink) and the session report keep the app's name.
 *   - logged; VKBRIDGE_ERRATA=+pv_zink_name renames without the probe (the name "Mesa zink"),
 *     =-pv_zink_name never.
 *   - what the profile hid besides vertexInputDynamicState (sparse resources, graphicsPipelineLibrary,
 *     extensions) stays hidden from the renamed instance (its feature and extension queries), so Zink
 *     sees the driver as its vendor meant it for Zink with only the one feature whose loss breaks it
 *     back. That is the form wallentx's patch ships (0011: the rename, then the profile's other hidden
 *     features hidden again); VKBRIDGE_ZINK_KEEP_PROFILE=0 shows everything the neutral name shows.
 *
 * VKBRIDGE_FAKE_DRIVER=powervr (server_driverid.c) is the host imitation: a PowerVR-looking driver
 * that hides those features from instances whose engine name is exactly "mesa zink".
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENGINE_APP "mesa zink"
#define ENGINE_REF "MaliBridge Zink"
static const char *const candidates[] = {"Mesa zink", "Zink"};

/* The probe's record */
static struct {
    int result;       /* VKB_ERRATA_PROBE_* */
    char name[40];    /* the engine name to pass */
    char what[256];   /* what "mesa zink" hides */
    /* what the profile hid that the renamed instance hides again (all but vertexInputDynamicState) */
    int nhide;
    unsigned char hide_core[64];
    int hide_gpl;
    uint32_t nhide_ext;
    char hide_ext[32][VK_MAX_EXTENSION_NAME_SIZE];
} rec;

int vkb_zink_profile_result(void)
{
    return __atomic_load_n(&rec.result, __ATOMIC_ACQUIRE);
}

#define NFEAT ((int)(sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32)))
#define MAX_EXT 512

typedef struct {
    VkBool32 core[NFEAT];
    VkBool32 vidyn, gpl;
    uint32_t next;
    char (*ext)[VK_MAX_EXTENSION_NAME_SIZE];
} snap;

static const char *const core_names[NFEAT] = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader", "tessellationShader",
    "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp",
    "fillModeNonSolid", "depthBounds", "wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy",
    "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize",
    "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat", "shaderUniformBufferArrayDynamicIndexing",
    "shaderSampledImageArrayDynamicIndexing", "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing",
    "shaderClipDistance", "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
    "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D", "sparseResidencyImage3D",
    "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples", "sparseResidency16Samples", "sparseResidencyAliased",
    "variableMultisampleRate", "inheritedQueries"};

static int has_ext(const snap *s, const char *name)
{
    for (uint32_t i = 0; i < s->next; i++)
        if (!strcmp(s->ext[i], name)) return 1;
    return 0;
}

static void snap_free(snap *s)
{
    free(s->ext);
    s->ext = NULL;
}

static int snap_take(const vkb_dispatch *dt, VkPhysicalDevice pd, snap *s)
{
    memset(s, 0, sizeof(*s));
    uint32_t n = 0;
    if (!dt->vkEnumerateDeviceExtensionProperties || dt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) < 0) return -1;
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    s->ext = calloc(n ? n : 1, sizeof(*s->ext));
    if (!e || !s->ext) {
        free(e);
        snap_free(s);
        return -1;
    }
    if (dt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) < 0) {
        free(e);
        snap_free(s);
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) memcpy(s->ext[i], e[i].extensionName, VK_MAX_EXTENSION_NAME_SIZE);
    s->next = n;
    free(e);
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (has_ext(s, "VK_EXT_vertex_input_dynamic_state")) vi.pNext = f2.pNext, f2.pNext = &vi;
    if (has_ext(s, "VK_EXT_graphics_pipeline_library")) gp.pNext = f2.pNext, f2.pNext = &gp;
    if (dt->vkGetPhysicalDeviceFeatures2) dt->vkGetPhysicalDeviceFeatures2(pd, &f2);
    else if (dt->vkGetPhysicalDeviceFeatures) dt->vkGetPhysicalDeviceFeatures(pd, &f2.features);
    else return snap_free(s), -1;
    memcpy(s->core, &f2.features, sizeof(s->core));
    s->vidyn = vi.vertexInputDynamicState;
    s->gpl = gp.graphicsPipelineLibrary;
    return 0;
}

/* Compares a (the app's name) with b (the neutral one): *more: what b shows and a does not; *less:
 * what a shows and b does not (the two make a profile only when less is 0). Returns the number
 * of differences; what (may be NULL) gets the first few. */
static int snap_diff(const snap *a, const snap *b, int *less, char *what, size_t cap)
{
    int n = 0;
    size_t o = 0;
    *less = 0;
#define NOTE(nm) (n++, what && o < cap ? (o += (size_t)snprintf(what + o, cap - o, "%s%s", o ? ", " : "", nm)) : 0)
    for (int i = 0; i < NFEAT; i++) {
        if (a->core[i] == b->core[i]) continue;
        if (a->core[i]) (*less)++;
        else NOTE(core_names[i]);
    }
    if (a->vidyn != b->vidyn) a->vidyn ? (*less)++ : NOTE("vertexInputDynamicState");
    if (a->gpl != b->gpl) a->gpl ? (*less)++ : NOTE("graphicsPipelineLibrary");
    int shown = 0;
    for (uint32_t i = 0; i < b->next; i++)
        if (!has_ext(a, b->ext[i])) {
            if (shown++ < 3) NOTE(b->ext[i]);
            else n++;
        }
    for (uint32_t i = 0; i < a->next; i++)
        if (!has_ext(b, a->ext[i])) (*less)++;
#undef NOTE
    return n;
}

static int make_snap(const char *engine, const uint8_t uuid[VK_UUID_SIZE], uint32_t api, snap *s)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-selftest", 1, engine, 1, api};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkb_global_dt.vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) return -1;
    vkb_dispatch dt;
    vkb_dispatch_load_instance(&dt, vkb_gipa, inst);
    int rc = -1;
    uint32_t n = 0;
    if (dt.vkEnumeratePhysicalDevices && dt.vkGetPhysicalDeviceProperties2 && dt.vkEnumeratePhysicalDevices(inst, &n, NULL) >= 0 && n) {
        if (n > VKB_MAX_PDS) n = VKB_MAX_PDS;
        VkPhysicalDevice pds[VKB_MAX_PDS];
        dt.vkEnumeratePhysicalDevices(inst, &n, pds);
        for (uint32_t i = 0; i < n && rc; i++) {
            VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            dt.vkGetPhysicalDeviceProperties2(pds[i], &p2);
            if (!memcmp(id.deviceUUID, uuid, VK_UUID_SIZE)) rc = snap_take(&dt, pds[i], s);
        }
    }
    dt.vkDestroyInstance(inst, NULL);
    return rc;
}

static int powervr(uint32_t vendor, uint32_t driver_id)
{
    return driver_id == VK_DRIVER_ID_IMAGINATION_PROPRIETARY || (!driver_id && vendor == 0x1010);
}

/* The self-test's call for each physical device (uuid: its deviceUUID) */
void vkb_zink_profile_probe(const uint8_t uuid[VK_UUID_SIZE], uint32_t vendor, uint32_t driver_id, uint32_t api)
{
    if (!powervr(vendor, driver_id)) return;
    const char *force = vkb_srv_env_global("VKBRIDGE_ZINK_PROFILE_PROBE");
    if (force && !strcmp(force, "0")) {
        VKB_INFO("pv_zink_name: profile probe off (VKBRIDGE_ZINK_PROFILE_PROBE=0)");
        return;
    }
    if (api < VK_API_VERSION_1_1) api = VK_API_VERSION_1_1;
    snap a, ref;
    if (make_snap(ENGINE_APP, uuid, api, &a)) {
        VKB_INFO("pv_zink_name: probe could not run (instance with engine \"%s\" failed); decides nothing", ENGINE_APP);
        return;
    }
    if (make_snap(ENGINE_REF, uuid, api, &ref)) {
        snap_free(&a);
        VKB_INFO("pv_zink_name: probe could not run (instance with engine \"%s\" failed); decides nothing", ENGINE_REF);
        return;
    }
    char what[256] = "";
    int less, diff = snap_diff(&a, &ref, &less, what, sizeof(what));
    if (!diff) {
        snprintf(rec.name, sizeof(rec.name), "%s", candidates[0]);
        __atomic_store_n(&rec.result, VKB_ERRATA_PROBE_PASS, __ATOMIC_RELEASE);
        VKB_INFO("pv_zink_name: probe: engine name \"%s\" shows the same features as \"%s\"%s; no profile", ENGINE_APP, ENGINE_REF,
                 less ? " (it shows more, which is not a profile)" : "");
    } else if (less) {
        __atomic_store_n(&rec.result, VKB_ERRATA_PROBE_PASS, __ATOMIC_RELEASE);
        VKB_INFO("pv_zink_name: probe: \"%s\" and \"%s\" differ both ways (%d, %d); not a profile that hides features", ENGINE_APP, ENGINE_REF,
                 diff, less);
    } else {
        /* the profile: pick the first neutral name that shows what the reference shows */
        snprintf(rec.name, sizeof(rec.name), "%s", ENGINE_REF);
        for (size_t c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++) {
            snap cs;
            int l2;
            if (make_snap(candidates[c], uuid, api, &cs)) continue;
            int d2 = snap_diff(&cs, &ref, &l2, NULL, 0);
            snap_free(&cs);
            if (!d2 && !l2) {
                snprintf(rec.name, sizeof(rec.name), "%s", candidates[c]);
                break;
            }
        }
        snprintf(rec.what, sizeof(rec.what), "%s", what);
        /* what the renamed instance hides again: what "mesa zink" lacks and the neutral name has, vertexInputDynamicState
         * (and its extension) aside */
        rec.nhide = 0;
        for (int i = 0; i < NFEAT && i < 64; i++)
            if (!a.core[i] && ref.core[i]) rec.hide_core[i] = 1, rec.nhide++;
        if (!a.gpl && ref.gpl) rec.hide_gpl = 1, rec.nhide++;
        rec.nhide_ext = 0;
        for (uint32_t i = 0; i < ref.next && rec.nhide_ext < 32; i++)
            if (!has_ext(&a, ref.ext[i]) && strcmp(ref.ext[i], "VK_EXT_vertex_input_dynamic_state"))
                memcpy(rec.hide_ext[rec.nhide_ext++], ref.ext[i], VK_MAX_EXTENSION_NAME_SIZE), rec.nhide++;
        __atomic_store_n(&rec.result, VKB_ERRATA_PROBE_FAIL, __ATOMIC_RELEASE);
        VKB_INFO("pv_zink_name: probe: the driver's profile for engine name \"%s\" hides %d thing(s) from Zink: %s%s; \"%s\" shows them", ENGINE_APP,
                 diff, what, diff > 6 ? ", ..." : "", rec.name);
    }
    snap_free(&a);
    snap_free(&ref);
}

/* vkCreateInstance: the engine name to pass the driver for a client with this engine name, or NULL
 * to pass it as it is. why (may be NULL) gets the reason. */
const char *vkb_errata_zink_engine(const char *engine, char *why, size_t cap)
{
    if (why && cap) why[0] = 0;
    if (!engine || strcmp(engine, ENGINE_APP)) return NULL;
    if (!vkb_errata_zink_name_on(why, cap)) return NULL;
    return rec.name[0] ? rec.name : candidates[0];
}

/* the device's install: say what its instance did, once per server */
void vkb_errata_zink_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    (void)t, (void)e;
    vkb_errata_info_once("errata: pv_zink_name: Zink clients' engine name \"%s\" is passed to the driver as \"%s\" (it hides %s) - PV-08", ENGINE_APP,
                         rec.name[0] ? rec.name : candidates[0], rec.what[0] ? rec.what : "nothing the probe saw: forced");
}

/* ------------------------------------------------------------------ the profile kept, vertexInputDynamicState aside */

static int keep_profile(void)
{
    const char *v = vkb_srv_env_global("VKBRIDGE_ZINK_KEEP_PROFILE");
    return !(v && !strcmp(v, "0")) && __atomic_load_n(&rec.result, __ATOMIC_ACQUIRE) == VKB_ERRATA_PROBE_FAIL && rec.nhide > 0;
}

static const vkb_srv_table *zk_inst(void)
{
    const vkb_srv_table *t = vkb_srv_current_table();
    return t && t->is_device ? t->parent : t;
}

static void core_off(VkPhysicalDeviceFeatures *f)
{
    VkBool32 *b = (VkBool32 *)f;
    for (int i = 0; i < NFEAT && i < 64; i++)
        if (rec.hide_core[i]) b[i] = VK_FALSE;
}

static VKAPI_ATTR void VKAPI_CALL zk_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
    const vkb_srv_table *t = zk_inst();
    if (!t || !t->zk_f1) return;
    t->zk_f1(pd, f);
    core_off(f);
}

static VKAPI_ATTR void VKAPI_CALL zk_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    const vkb_srv_table *t = zk_inst();
    if (!t || !t->zk_f2) return;
    t->zk_f2(pd, f);
    core_off(&f->features);
    for (VkBaseOutStructure *b = f->pNext; b; b = b->pNext)
        if (rec.hide_gpl && b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT)
            ((VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *)b)->graphicsPipelineLibrary = VK_FALSE;
}

static int ext_hidden(const char *name)
{
    for (uint32_t i = 0; i < rec.nhide_ext; i++)
        if (!strcmp(rec.hide_ext[i], name)) return 1;
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL zk_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count,
                                                                             VkExtensionProperties *props)
{
    const vkb_srv_table *t = zk_inst();
    if (!t || !t->zk_ext) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer || !rec.nhide_ext) return t->zk_ext(pd, layer, count, props);
    uint32_t n = 0;
    VkResult r = t->zk_ext(pd, NULL, &n, NULL);
    if (r < 0) return r;
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof(*all));
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r = t->zk_ext(pd, NULL, &n, all);
    if (r < 0) {
        free(all);
        return r;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!ext_hidden(all[i].extensionName)) all[k++] = all[i];
    if (!props) {
        *count = k;
        free(all);
        return VK_SUCCESS;
    }
    uint32_t w = *count < k ? *count : k;
    memcpy(props, all, w * sizeof(*all));
    *count = w;
    free(all);
    return w < k ? VK_INCOMPLETE : VK_SUCCESS;
}

void vkb_errata_zink_instance_install(vkb_srv_table *t)
{
    if (!t->zink_renamed || !keep_profile()) {
        if (t->zink_renamed)
            vkb_errata_info_once("errata: pv_zink_name: the renamed instance sees everything the neutral name shows (%s) - PV-08",
                                 rec.nhide ? "VKBRIDGE_ZINK_KEEP_PROFILE=0" : "no probe result saying what the profile hid");
        return;
    }
    if (t->dt.vkGetPhysicalDeviceFeatures) t->zk_f1 = t->dt.vkGetPhysicalDeviceFeatures, t->dt.vkGetPhysicalDeviceFeatures = zk_GetPhysicalDeviceFeatures;
    if (t->dt.vkGetPhysicalDeviceFeatures2)
        t->zk_f2 = t->dt.vkGetPhysicalDeviceFeatures2, t->dt.vkGetPhysicalDeviceFeatures2 = zk_GetPhysicalDeviceFeatures2;
    if (t->dt.vkEnumerateDeviceExtensionProperties)
        t->zk_ext = t->dt.vkEnumerateDeviceExtensionProperties, t->dt.vkEnumerateDeviceExtensionProperties = zk_EnumerateDeviceExtensionProperties;
    vkb_errata_info_once("errata: pv_zink_name: the renamed instance keeps the profile's other %d hidden thing(s) hidden; only "
                         "vertexInputDynamicState is shown - PV-08", rec.nhide);
}
