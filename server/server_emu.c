/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Feature emulation: entry point. What the GPU lacks and the bridge can make up for is decided
 * here per physical device; the client reports those features as present, the server strips
 * them from vkCreateDevice and installs wrappers in the device's dispatch table.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

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
    for (uint32_t i = 0; i < n; i++)
        if (!vkb_emu_ext_hidden(all[i].extensionName)) all[kept++] = all[i];
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

void vkb_emu_instance_install(vkb_srv_table *t)
{
    if (!vkb_emu_hiding_exts() || !t->real.vkEnumerateDeviceExtensionProperties) return;
    t->dt.vkEnumerateDeviceExtensionProperties = hide_EnumerateDeviceExtensionProperties;
    VKB_ONCE(VKB_LOG_INFO, "device extensions hidden from clients: VKBRIDGE_FAKE_MISSING=%s VKBRIDGE_SERVER_HIDE_EXTS=%s",
             fake_list() ? fake_list() : "", hide_list() ? hide_list() : "");
}

uint32_t vkb_emu_detect(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t *missing)
{
    VkPhysicalDeviceFeatures f;
    idt->vkGetPhysicalDeviceFeatures(pd, &f);
    uint32_t n = 0;
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int divisor = 0, depth_clip = 0, maint5 = 0, robust2 = 0, maint6 = 0, lson_khr = 0, lson_ext = 0, push = 0;
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
        if (token(fake, "multiviewport")) f.multiViewport = VK_FALSE;
        if (token(fake, "multidraw")) f.multiDrawIndirect = VK_FALSE;
        if (token(fake, "maint6")) maint6 = 0;
        if (token(fake, "push")) push = 0;
        if (token(fake, "loadstorenone")) lson_khr = 0;
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
    /* dma-buf export of images (gamescope's nested backend needs it): emulated with dma-heap pages
     * where the driver imports dma-bufs into images but exports none (Mali). */
    if (!env_off("VKBRIDGE_EMU_DMABUF_EXPORT") && vkb_mem_dmaheap_available()) {
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
        if (!can_export && can_import) emu |= VKB_EMU_DMABUF_EXPORT;
    }
    /* OPAQUE_FD external memory on dma-bufs (server_opaque.c): DXVK's D3D11 shared resources. */
    if (!env_off("VKBRIDGE_EMU_OPAQUEFD") && vkb_opaque_wanted(idt, pd, emu)) emu |= VKB_EMU_OPAQUE_FD;
    /* What DXVK 3 requires and the GPU lacks (server_emu_compat.c). Vulkan 1.4 has maintenance6. */
    if (!f.dualSrcBlend && !env_off("VKBRIDGE_EMU_DUALSRC")) emu |= VKB_EMU_DUAL_SRC;
    if (!f.fillModeNonSolid && !env_off("VKBRIDGE_EMU_FILLMODE")) emu |= VKB_EMU_FILL_MODE;
    if (!f.multiViewport && !env_off("VKBRIDGE_EMU_MULTIVIEWPORT")) emu |= VKB_EMU_MULTI_VIEWPORT;
    /* DXVK refuses a device without multiDrawIndirect; Mali-G57 on r32p1/r38p1 has none (Lee Gao's
     * compat_layer README). Indirect draws are split into single ones (server_emu_compat.c). */
    if (!f.multiDrawIndirect && !env_off("VKBRIDGE_EMU_MULTIDRAW")) emu |= VKB_EMU_MULTI_DRAW;
    if (!maint6 && (props.apiVersion < VK_API_VERSION_1_4 || (fake && token(fake, "maint6"))) && !env_off("VKBRIDGE_EMU_MAINT6"))
        emu |= VKB_EMU_MAINT6;
    if (!lson_khr && lson_ext) emu |= VKB_EMU_LOADSTORE_NONE;
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
    if (!f.dualSrcBlend) miss |= VKB_EMU_DUAL_SRC;
    if (!f.fillModeNonSolid) miss |= VKB_EMU_FILL_MODE;
    if (!f.multiViewport) miss |= VKB_EMU_MULTI_VIEWPORT;
    if (!f.multiDrawIndirect) miss |= VKB_EMU_MULTI_DRAW;
    if (emu & VKB_EMU_MAINT6) miss |= VKB_EMU_MAINT6;
    if (emu & VKB_EMU_PUSH_DESC) miss |= VKB_EMU_PUSH_DESC;
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
    if (emu & VKB_EMU_ROBUSTNESS2) {
        chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
        /* What robustness the GPU has stays on; robustBufferAccess2 asks for at least that. */
        VkPhysicalDeviceFeatures *rf = find_features(ci);
        const char *nr = vkb_srv_env("VKBRIDGE_NO_FORCED_ROBUSTNESS");
        if (rf && !(nr && *nr && strcmp(nr, "0"))) rf->robustBufferAccess = VK_TRUE;
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

void vkb_emu_device_destroy(vkb_srv_table *dev)
{
    if (!dev->emu) return;
    vkb_emu_uninstall(dev);
    pthread_mutex_destroy(&dev->emu->lock);
    free(dev->emu);
    dev->emu = NULL;
}
