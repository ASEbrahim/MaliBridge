/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 */
#include "capabilities.h"
#include "layer_crash.h"
#include "layer_ctx.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "server_emu.h"
#include "vkb_server.h"
}

#define VKB_LAYER_EXPORT __attribute__((visibility("default")))

namespace {

constexpr const char *LAYER_NAME = "VK_LAYER_MALIBRIDGE_compat";
constexpr const char *LAYER_DESCRIPTION =
    "MaliBridge Compatibility & Feature Emulation Layer for Arm Mali";
constexpr uint32_t LAYER_IMPL_VERSION = 1;
constexpr uint32_t LAYER_SPEC_VERSION = VK_API_VERSION_1_3;

/* The loader's link info in a create info's pNext chain: the layer takes the next layer's
 * GetInstanceProcAddr (and GetDeviceProcAddr) from it, and passes the chain on (the chain is
 * the loader's, and the layer changes it). */
template <typename LinkInfo>
LinkInfo *find_link_info(const void *chain, VkStructureType type) {
    for (auto *info = const_cast<LinkInfo *>(static_cast<const LinkInfo *>(chain));
         info; info = const_cast<LinkInfo *>(static_cast<const LinkInfo *>(info->pNext))) {
        if (info->sType == type && info->function == VK_LAYER_LINK_INFO)
            return info;
    }
    return nullptr;
}

/* The process the layer runs in, as the bridge's server knows a client's (vkb_srv_proc): its name
 * is what the per-process fixes match (gs_hide's VKBRIDGE_GS_HIDE_PROCS) and what the
 * logs say. */
vkb_srv_proc *layer_proc() {
    static vkb_srv_proc proc;
    static std::once_flag once;
    std::call_once(once, [] {
        pthread_mutex_init(&proc.olock, nullptr);
        pthread_cond_init(&proc.ocond, nullptr);
        proc.pid = static_cast<int>(getpid());
        if (FILE *f = std::fopen("/proc/self/comm", "r")) {
            if (std::fgets(proc.comm, sizeof(proc.comm), f))
                proc.comm[std::strcspn(proc.comm, "\n")] = 0;
            std::fclose(f);
        }
    });
    return &proc;
}

/* A directory this process can write: the stuck-pipeline list, <cache dir>/env and the SPIR-V
 * dumps live there. Winlator and its forks set HOME and TMPDIR inside their imagefs, and no
 * XDG_CACHE_HOME; /data/local/tmp is the shell's, not an app's. First that can be made and
 * written wins. */
bool writable_dir(const std::string &dir) {
    if (dir.empty() || vkb_mkdirs(dir.c_str()) != 0)
        return false;
    struct stat st;
    return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && access(dir.c_str(), W_OK | X_OK) == 0;
}

void choose_cache_dir() {
    if (vkb_pcache_dir())
        return;
    auto env = [](const char *name) -> std::string {
        const char *v = getenv(name);
        return v && *v ? v : "";
    };
    const std::string xdg = env("XDG_CACHE_HOME"), home = env("HOME"), tmp = env("TMPDIR");
    const std::string candidates[] = {
        env("VKBRIDGE_CACHE_DIR"),
        xdg.empty() ? "" : xdg + "/malibridge",
        home.empty() ? "" : home + "/.cache/malibridge",
        tmp.empty() ? "" : tmp + "/malibridge",
        "/data/local/tmp/malibridge_cache",
        "/tmp/malibridge",
    };
    for (const auto &dir : candidates) {
        if (writable_dir(dir)) {
            vkb_pcache_set_dir(dir.c_str());
            return;
        }
    }
    VKB_WARN("layer: no writable cache directory (VKBRIDGE_CACHE_DIR, XDG_CACHE_HOME, HOME, TMPDIR); stuck pipelines "
             "are not remembered");
}

/* A context with a server table of its own. */
std::shared_ptr<LayerCtx> make_context() {
    auto ctx = std::make_shared<LayerCtx>();
    ctx->t = std::make_unique<vkb_srv_table>();
    pthread_mutex_init(&ctx->t->lock, nullptr);
    return ctx;
}

} // namespace

#define VKB_LAYER_GEN_IMPLEMENTATION
// Implements all pass-through dispatches to MaliBridge's server emulation
#include "gen/layer_dispatch_gen.h"
#undef VKB_LAYER_GEN_IMPLEMENTATION

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pCount, VkPhysicalDevice *pDevices) {
    auto ctx = get_scoped_ctx(instance);
    VkResult res =
        ctx->t->real.vkEnumeratePhysicalDevices(instance, pCount, pDevices);
    if (pDevices && (res == VK_SUCCESS || res == VK_INCOMPLETE))
        for (uint32_t i = 0; i < *pCount; ++i)
            register_handle(pDevices[i], ctx.ctx);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
    VkCommandBuffer *pCommandBuffers) {
    auto ctx = get_scoped_ctx(device);
    VkResult result = ctx->t->dt.vkAllocateCommandBuffers(device, pAllocateInfo,
                                                          pCommandBuffers);
    if (result == VK_SUCCESS && pCommandBuffers) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            register_handle(pCommandBuffers[i], ctx.ctx);
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL Layer_vkFreeCommandBuffers(
    VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
    const VkCommandBuffer *pCommandBuffers) {
    auto ctx = get_scoped_ctx(device);
    ctx->t->dt.vkFreeCommandBuffers(device, commandPool, commandBufferCount,
                                    pCommandBuffers);
    if (pCommandBuffers) {
        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            unregister_handle(pCommandBuffers[i]);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetDeviceQueue(VkDevice device,
                                                  uint32_t queueFamilyIndex,
                                                  uint32_t queueIndex,
                                                  VkQueue *pQueue) {
    auto ctx = get_scoped_ctx(device);
    ctx->t->dt.vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    if (pQueue && *pQueue) {
        register_handle(*pQueue, ctx.ctx);
    }
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue) {
    auto ctx = get_scoped_ctx(device);
    ctx->t->dt.vkGetDeviceQueue2(device, pQueueInfo, pQueue);
    if (pQueue && *pQueue) {
        register_handle(*pQueue, ctx.ctx);
    }
}

extern "C" VKB_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName);
extern "C" VKB_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName);

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    auto *layer_info = find_link_info<VkLayerInstanceCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO);
    if (!layer_info)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa =
        layer_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto next_create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    layer_info->u.pLayerInfo = layer_info->u.pLayerInfo->pNext;

    VkResult result = next_create_instance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS)
        return result;

    auto ctx = make_context();
    ctx->gipa = next_gipa;
    ctx->t->instance = *pInstance;
    ctx->t->api_version = pCreateInfo->pApplicationInfo
                              ? pCreateInfo->pApplicationInfo->apiVersion
                              : VK_API_VERSION_1_0;

    vkb_dispatch_load_instance(&ctx->t->real, next_gipa, *pInstance);
    ctx->t->dt = ctx->t->real;
    vkb_gipa = next_gipa;
    vkb_global_dt = ctx->t->real;

    static std::once_flag cache_once;
    std::call_once(cache_once, [] {
        choose_cache_dir();
        vkb_srv_env_reload(); /* <cache dir>/env, as the bridge reads it at start */
    });

    auto *t = ctx->t.get();
    t->proc = layer_proc();
    vkb_emu_a1_layer(t->proc->comm); /* the a1 gate (server_apicap.c): Steam mode's own processes only */
    vkb_table_register(t);
    /* The bridge's instance fixes (server_instance.c), those that apply in-process: the extension
     * hiding and fp16_hide (MK-20) in vkb_emu_instance_install, gs_hide over them */
    vkb_emu_instance_install(t);
    vkb_errata_gs_instance_install(t);

    register_handle(*pInstance, ctx);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Layer_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    auto ctx = get_scoped_ctx(instance);
    if (!ctx)
        return;
    ctx->t->real.vkDestroyInstance(instance, pAllocator);
    unregister_handle(instance);
    vkb_table_unregister(ctx->t.get());
    pthread_mutex_destroy(&ctx->t->lock);
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    auto parent = get_scoped_ctx(physicalDevice);
    if (!parent)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto *layer_info = find_link_info<VkLayerDeviceCreateInfo>(
        pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO);
    if (!layer_info)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa =
        layer_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa =
        layer_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    auto next_create_device = reinterpret_cast<PFN_vkCreateDevice>(
        next_gipa(parent->t->instance, "vkCreateDevice"));
    layer_info->u.pLayerInfo = layer_info->u.pLayerInfo->pNext;

    /* The create info the driver gets: the emulated extensions are taken out of it, and the
     * features the GPU lacks are not asked for. */
    vkb_srv_env_reload(); /* <cache dir>/env again at each device, as the bridge does */
    VkDeviceCreateInfo ci_copy = *pCreateInfo;
    vkb_arena arena{};
    uint32_t emu = filter_device_create_info(physicalDevice, parent.ctx.get(),
                                             &ci_copy, &arena);

    VkResult res =
        next_create_device(physicalDevice, &ci_copy, pAllocator, pDevice);
    vkb_arena_free(&arena);
    if (res != VK_SUCCESS || !pDevice || !*pDevice)
        return res;

    auto ctx = make_context();
    ctx->parent = parent.ctx;
    ctx->device = *pDevice;
    ctx->gdpa = next_gdpa;
    ctx->gipa = next_gipa;

    auto *dev_table = ctx->t.get();
    dev_table->is_device = 1;
    dev_table->proc = parent->t->proc;
    dev_table->device = *pDevice;
    dev_table->physical = physicalDevice;
    dev_table->instance = parent->t->instance;
    dev_table->parent = parent->t.get();
    dev_table->api_version = parent->t->api_version;

    /* The instance's dispatch first: it has the physical device queries the
     * errata and the emulation ask for (the driver version, the formats); the
     * device's own functions are added on top of it. The server's device table
     * is built the same way. */
    dev_table->real = parent->t->real;
    vkb_dispatch_load_device(&dev_table->real, next_gdpa, *pDevice);
    dev_table->dt = dev_table->real;
    vkb_table_register(dev_table);

    ScopedLayerContext scope(ctx);
    layer_crash_install(); /* before the guard: its contained crashes need the handler */
    vkb_guard_device_init(dev_table);
    vkb_errata_device_init(dev_table);
    vkb_emu_device_init(dev_table, emu);
    vkb_pools_device_init(dev_table);
    vkb_nulldesc_device_init(dev_table);
    vkb_dsmap_device_init(dev_table);
    vkb_spvfix_device_init(dev_table);
    vkb_bvmap_device_init(dev_table);

    register_handle(*pDevice, ctx);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Layer_vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks *pAllocator) {
    auto ctx = get_scoped_ctx(device);
    if (!ctx)
        return;
    auto *t = ctx->t.get();

    vkb_emu_device_quiesce(t);
    vkb_bvmap_device_destroy(t);
    vkb_spvfix_device_destroy(t);
    vkb_dsmap_device_destroy(t);
    vkb_nulldesc_device_destroy(t);
    vkb_pools_device_destroy(t);
    vkb_emu_device_destroy(t);
    vkb_errata_device_destroy(t);
    vkb_guard_device_destroy(t);

    t->real.vkDestroyDevice(device, pAllocator);
    unregister_handle(device);
    vkb_table_unregister(t);
    pthread_mutex_destroy(&t->lock);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
Layer_vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    if (!pName)
        return nullptr;
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(Layer_vkGetDeviceProcAddr);

    PFN_vkVoidFunction device_proc = layer_lookup_device_proc(pName);
    if (device_proc)
        return device_proc;

    auto ctx = get_scoped_ctx(device);
    return (ctx && ctx->gdpa) ? ctx->gdpa(device, pName) : nullptr;
}

extern "C" VKB_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    return Layer_vkGetDeviceProcAddr(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
Layer_vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName)
        return nullptr;
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(
            Layer_vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(Layer_vkGetDeviceProcAddr);

    PFN_vkVoidFunction instance_proc = layer_lookup_instance_proc(pName);
    if (instance_proc)
        return instance_proc;

    PFN_vkVoidFunction device_proc = layer_lookup_device_proc(pName);
    if (device_proc)
        return device_proc;

    auto ctx = get_scoped_ctx(instance);
    return (ctx && ctx->gipa) ? ctx->gipa(instance, pName) : nullptr;
}

extern "C" VKB_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return Layer_vkGetInstanceProcAddr(instance, pName);
}

extern "C" VKB_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *pVersionStruct) {
    if (!pVersionStruct ||
        pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
        pVersionStruct->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount,
    VkLayerProperties *pProperties) {
    (void)physicalDevice;
    (void)pProperties;
    if (pPropertyCount)
        *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumeratePhysicalDeviceGroups(
    VkInstance instance, uint32_t *pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties) {
    auto ctx = get_scoped_ctx(instance);
    if (!ctx || !ctx->t->real.vkEnumeratePhysicalDeviceGroups)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = ctx->t->real.vkEnumeratePhysicalDeviceGroups(
        instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    // An app that enumerates only through groups needs its physical devices known, as
    // vkEnumeratePhysicalDevices does
    if (pPhysicalDeviceGroupProperties && (res == VK_SUCCESS || res == VK_INCOMPLETE))
        for (uint32_t g = 0; g < *pPhysicalDeviceGroupCount; ++g)
            for (uint32_t i = 0; i < pPhysicalDeviceGroupProperties[g].physicalDeviceCount; ++i)
                register_handle(pPhysicalDeviceGroupProperties[g].physicalDevices[i], ctx.ctx);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumerateInstanceLayerProperties(
    uint32_t *pPropertyCount, VkLayerProperties *pProperties) {
    if (!pPropertyCount)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProperties) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    if (*pPropertyCount < 1)
        return VK_INCOMPLETE;
    VkLayerProperties props{};
    std::snprintf(props.layerName, sizeof(props.layerName), "%s", LAYER_NAME);
    std::snprintf(props.description, sizeof(props.description), "%s",
                  LAYER_DESCRIPTION);
    props.implementationVersion = LAYER_IMPL_VERSION;
    props.specVersion = LAYER_SPEC_VERSION;
    *pProperties = props;
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

extern "C" VKB_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                   VkLayerProperties *pProperties) {
    return Layer_vkEnumerateInstanceLayerProperties(pPropertyCount,
                                                    pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount,
    VkExtensionProperties *pProperties) {
    (void)pProperties;
    if (pLayerName && std::strcmp(pLayerName, LAYER_NAME) == 0) {
        if (pPropertyCount)
            *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    return VK_ERROR_LAYER_NOT_PRESENT;
}

extern "C" VKB_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *pLayerName,
                                       uint32_t *pPropertyCount,
                                       VkExtensionProperties *pProperties) {
    return Layer_vkEnumerateInstanceExtensionProperties(
        pLayerName, pPropertyCount, pProperties);
}
