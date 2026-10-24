/*
 * VK_LAYER_MALIBRIDGE_devclamp (host only, for devprofile_check.sh): sits beneath the Khronos
 * Profiles layer, which makes the desktop driver ANSWER queries as a phone's GPU does (its
 * features, extensions, properties, formats). The desktop driver cannot always CREATE a device with
 * what the phone has and it lacks (ASTC HDR, ARM extensions, a feature RADV leaves out): such a
 * vkCreateDevice would fail with VK_ERROR_FEATURE_NOT_PRESENT / EXTENSION_NOT_PRESENT where the phone
 * would succeed. This layer takes those off at vkCreateDevice - features the driver reports false,
 * extensions it does not list - and writes each one it took off to $VKB_DEVCLAMP_LOG (stderr when
 * unset), so the check can tell the driver's gaps from the phone's. Queries pass through untouched.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "devclamp_table.h"

#define EXPORT __attribute__((visibility("default")))
#define N_STRUCTS (sizeof dc_structs / sizeof dc_structs[0])

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static VkInstance last_instance;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    const char *p = getenv("VKB_DEVCLAMP_LOG");
    FILE *f = p && *p ? fopen(p, "a") : stderr;
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "devclamp: ");
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    if (f != stderr) fclose(f);
}

static const struct dc_struct *find(VkStructureType s)
{
    for (size_t i = 0; i < N_STRUCTS; i++)
        if (dc_structs[i].stype == s) return &dc_structs[i];
    return NULL;
}

/* The i-th name of a comma-separated list, into buf */
static const char *nth(const char *list, unsigned i, char *buf, size_t n)
{
    const char *p = list;
    for (; i && p; i--) p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL;
    if (!p) return "?";
    size_t l = strcspn(p, ",");
    if (l >= n) l = n - 1;
    memcpy(buf, p, l);
    buf[l] = 0;
    return buf;
}

static const char *const core_names =
    "robustBufferAccess,fullDrawIndexUint32,imageCubeArray,independentBlend,geometryShader,tessellationShader,"
    "sampleRateShading,dualSrcBlend,logicOp,multiDrawIndirect,drawIndirectFirstInstance,depthClamp,depthBiasClamp,"
    "fillModeNonSolid,depthBounds,wideLines,largePoints,alphaToOne,multiViewport,samplerAnisotropy,"
    "textureCompressionETC2,textureCompressionASTC_LDR,textureCompressionBC,occlusionQueryPrecise,"
    "pipelineStatisticsQuery,vertexPipelineStoresAndAtomics,fragmentStoresAndAtomics,"
    "shaderTessellationAndGeometryPointSize,shaderImageGatherExtended,shaderStorageImageExtendedFormats,"
    "shaderStorageImageMultisample,shaderStorageImageReadWithoutFormat,shaderStorageImageWriteWithoutFormat,"
    "shaderUniformBufferArrayDynamicIndexing,shaderSampledImageArrayDynamicIndexing,"
    "shaderStorageBufferArrayDynamicIndexing,shaderStorageImageArrayDynamicIndexing,shaderClipDistance,"
    "shaderCullDistance,shaderFloat64,shaderInt64,shaderInt16,shaderResourceResidency,shaderResourceMinLod,"
    "sparseBinding,sparseResidencyBuffer,sparseResidencyImage2D,sparseResidencyImage3D,sparseResidency2Samples,"
    "sparseResidency4Samples,sparseResidency8Samples,sparseResidency16Samples,sparseResidencyAliased,"
    "variableMultisampleRate,inheritedQueries";

/* want &= have, over n VkBool32s; logs what it clears */
static unsigned clamp_bools(VkBool32 *want, const VkBool32 *have, unsigned n, const char *sname, const char *names)
{
    unsigned cleared = 0;
    char buf[96];
    for (unsigned i = 0; i < n; i++)
        if (want[i] && !have[i]) {
            want[i] = VK_FALSE;
            cleared++;
            logf_("feature %s.%s taken off (the desktop driver lacks it)", sname, nth(names, i, buf, sizeof buf));
        }
    return cleared;
}

static VKAPI_ATTR VkResult VKAPI_CALL dc_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out)
{
    VkLayerInstanceCreateInfo *li = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
        li = (VkLayerInstanceCreateInfo *)li->pNext;
    if (!li) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    li->u.pLayerInfo = li->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkResult r = create(ci, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&lock);
        next_gipa = gipa;
        last_instance = *out;
        pthread_mutex_unlock(&lock);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL dc_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out)
{
    VkLayerDeviceCreateInfo *li = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
        li = (VkLayerDeviceCreateInfo *)li->pNext;
    if (!li) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    li->u.pLayerInfo = li->u.pLayerInfo->pNext;
    VkInstance inst = last_instance;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    PFN_vkGetPhysicalDeviceFeatures gf = (PFN_vkGetPhysicalDeviceFeatures)gipa(inst, "vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
    PFN_vkEnumerateDeviceExtensionProperties ee = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    pthread_mutex_lock(&lock);
    next_gdpa = gdpa;
    pthread_mutex_unlock(&lock);

    /* extensions the driver lists */
    uint32_t ne = 0;
    ee(pd, NULL, &ne, NULL);
    VkExtensionProperties *have = calloc(ne + 1, sizeof *have);
    ee(pd, NULL, &ne, have);
    const char **exts = calloc(ci->enabledExtensionCount + 1, sizeof *exts);
    uint32_t nx = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        int found = 0;
        for (uint32_t j = 0; j < ne && !found; j++) found = !strcmp(have[j].extensionName, ci->ppEnabledExtensionNames[i]);
        if (found) exts[nx++] = ci->ppEnabledExtensionNames[i];
        else logf_("extension %s taken off (the desktop driver lacks it)", ci->ppEnabledExtensionNames[i]);
    }

    /* the chain, copied: feature structs clamped to the driver's, the others as they are */
    VkDeviceCreateInfo dci = *ci;
    dci.enabledExtensionCount = nx;
    dci.ppEnabledExtensionNames = exts;
    void *copies[512];
    unsigned nc = 0;
    VkBaseOutStructure head = {0}, *tail = &head;
    VkPhysicalDeviceFeatures2 q2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, NULL, {0}};
    VkBaseOutStructure *qtail = (VkBaseOutStructure *)&q2;
    void *qs[512];
    for (const VkBaseInStructure *b = ci->pNext; b && nc < 512; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            /* the loader's own: kept, copied by size */
            VkLayerDeviceCreateInfo *c = malloc(sizeof *c);
            memcpy(c, b, sizeof *c);
            copies[nc] = c; qs[nc] = NULL; nc++;
            tail->pNext = (VkBaseOutStructure *)c; tail = (VkBaseOutStructure *)c;
            continue;
        }
        size_t size;
        const struct dc_struct *s = find(b->sType);
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) size = sizeof(VkPhysicalDeviceFeatures2);
        else if (s) size = s->size;
        else {
            logf_("structure %d in vkCreateDevice's chain unknown here; dropped", (int)b->sType);
            continue;
        }
        VkBaseOutStructure *c = malloc(size);
        memcpy(c, b, size);
        c->pNext = NULL;
        copies[nc] = c;
        qs[nc] = NULL;
        if (s && s->feature) {
            VkBaseOutStructure *q = calloc(1, size);
            q->sType = b->sType;
            qtail->pNext = q; qtail = q;
            qs[nc] = q;
        }
        nc++;
        tail->pNext = c; tail = c;
    }
    dci.pNext = head.pNext;
    gf2(pd, &q2);
    unsigned cleared = 0;
    for (unsigned i = 0; i < nc; i++) {
        VkBaseOutStructure *c = copies[i];
        if (c->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            VkPhysicalDeviceFeatures real;
            gf(pd, &real);
            cleared += clamp_bools((VkBool32 *)&((VkPhysicalDeviceFeatures2 *)c)->features, (const VkBool32 *)&real,
                                   sizeof real / sizeof(VkBool32), "VkPhysicalDeviceFeatures", core_names);
        } else if (qs[i]) {
            const struct dc_struct *s = find(c->sType);
            /* the VkBool32 members (not the size: structs are padded to 8 bytes) */
            unsigned n = 1;
            for (const char *q = s->bools; *q; q++) n += *q == ',';
            cleared += clamp_bools((VkBool32 *)(c + 1), (const VkBool32 *)((VkBaseOutStructure *)qs[i] + 1), n, s->name, s->bools);
        }
    }
    VkPhysicalDeviceFeatures pef;
    if (ci->pEnabledFeatures) {
        VkPhysicalDeviceFeatures real;
        gf(pd, &real);
        pef = *ci->pEnabledFeatures;
        cleared += clamp_bools((VkBool32 *)&pef, (const VkBool32 *)&real, sizeof real / sizeof(VkBool32), "VkPhysicalDeviceFeatures", core_names);
        dci.pEnabledFeatures = &pef;
    }
    VkResult r = create(pd, &dci, a, out);
    logf_("vkCreateDevice: %d (%u of %u extensions, %u feature(s) taken off)", r, nx, ci->enabledExtensionCount, cleared);
    for (unsigned i = 0; i < nc; i++) { free(copies[i]); free(qs[i]); }
    free(exts);
    free(have);
    return r;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL dc_GetDeviceProcAddr(VkDevice d, const char *n)
{
    if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)dc_GetDeviceProcAddr;
    return next_gdpa ? next_gdpa(d, n) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL dc_GetInstanceProcAddr(VkInstance inst, const char *n)
{
    if (!strcmp(n, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)dc_GetInstanceProcAddr;
    if (!strcmp(n, "vkCreateInstance")) return (PFN_vkVoidFunction)dc_CreateInstance;
    if (!strcmp(n, "vkCreateDevice")) return (PFN_vkVoidFunction)dc_CreateDevice;
    if (!strcmp(n, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)dc_GetDeviceProcAddr;
    return next_gipa ? next_gipa(inst, n) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = dc_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = dc_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
