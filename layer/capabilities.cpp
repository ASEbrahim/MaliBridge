/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gen/layer_dispatch_gen.h"
#include "layer_ctx.h"
#include "capabilities.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
#include "vkb_caps.h"
#include "vkb_ext_alias.h"
#include "vkb_server.h"
}

namespace {

struct PdCaps {
    vkb_pd_caps caps{};
    std::vector<VkExtensionProperties> exts; // real supported extensions
};

// Room for the extensions the layer adds to the driver's own
constexpr uint32_t kExtensionCapacityBuffer = 16;

template <typename Fn>
void for_each_pNext(void *chain, Fn &&fn) {
    for (auto *b = static_cast<VkBaseOutStructure *>(chain); b; b = b->pNext)
        fn(b);
}

std::vector<VkExtensionProperties> query_device_extensions(vkb_srv_table *t,
                                                            VkPhysicalDevice pd,
                                                            const char *layer_name) {
    uint32_t count = 0;
    t->real.vkEnumerateDeviceExtensionProperties(pd, layer_name, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    if (count > 0) {
        t->real.vkEnumerateDeviceExtensionProperties(pd, layer_name, &count, exts.data());
        exts.resize(count);
    }
    return exts;
}

/* Cache of all feature/prop/extensions queries */
class CapabilityCache {
public:
    const PdCaps &get(VkPhysicalDevice pd, vkb_srv_table *t) {
        std::scoped_lock lock(m_lock);
        auto it = m_caps.find(pd);
        if (it != m_caps.end())
            return it->second;
        return m_caps.emplace(pd, detect(pd, t)).first->second;
    }

private:
    static PdCaps detect(VkPhysicalDevice pd, vkb_srv_table *t) {
        PdCaps p;
        p.exts = query_device_extensions(t, pd, nullptr);

        uint32_t missing = 0;
        p.caps.emu = vkb_emu_detect(&t->real, pd, &missing);
        p.caps.missing = missing;

        t->real.vkGetPhysicalDeviceProperties(pd, &p.caps.real_props);

        p.caps.aliased = vkb_caps_compute_aliased_mask(p.exts.data(),
                                                        static_cast<uint32_t>(p.exts.size()));

        const char *spoof = vkb_srv_env_global("VKBRIDGE_SPOOF_GPU");
        p.caps.spoof_gpu = vkb_caps_spoof_gpu(spoof, nullptr);
        if (p.caps.spoof_gpu) {
            const char *name = p.caps.real_props.deviceName;
            VKB_ONCE(VKB_LOG_INFO,
                     "GPU shown to the app as %s (vendor 0x%04x, device 0x%04x), not %s (vendor 0x%04x, device 0x%04x)",
                     p.caps.spoof_gpu == VKB_SPOOF_NVIDIA ? "NVIDIA GeForce GTX 1060 6GB"
                                                          : "AMD Radeon RX 480 Graphics",
                     p.caps.spoof_gpu == VKB_SPOOF_NVIDIA ? 0x10de : 0x1002,
                     p.caps.spoof_gpu == VKB_SPOOF_NVIDIA ? 0x1c03 : 0x67df,
                     name, p.caps.real_props.vendorID, p.caps.real_props.deviceID);
        }

        p.caps.heap_max_mb = vkb_caps_heap_max_mb(vkb_srv_env_global("VKBRIDGE_HEAP_MAX_MB"), nullptr);

        return p;
    }

    std::mutex m_lock;
    std::unordered_map<VkPhysicalDevice, PdCaps> m_caps;
};

CapabilityCache &capability_cache() {
    static CapabilityCache cache;
    return cache;
}

const PdCaps &pd_caps(VkPhysicalDevice pd, vkb_srv_table *t) {
    return capability_cache().get(pd, t);
}

// Real supported format properties
void layer_get_fmt(VkPhysicalDevice pd, VkFormat format, VkFormatProperties *out, void *user) {
    auto *table = static_cast<vkb_srv_table *>(user);
    table->real.vkGetPhysicalDeviceFormatProperties(pd, format, out);
}

void layer_get_props2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *out, void *user) {
    auto *table = static_cast<vkb_srv_table *>(user);
    table->real.vkGetPhysicalDeviceProperties2(pd, out);
}

} // namespace

uint32_t filter_device_create_info(VkPhysicalDevice physicalDevice,
                                   LayerCtx *parent,
                                   VkDeviceCreateInfo *pCreateInfo,
                                   vkb_arena *arena) {
    const PdCaps &p = pd_caps(physicalDevice, parent->table());

    vkb_caps_filter_device_create_extensions(p.caps.emu, p.exts.data(),
                                              static_cast<uint32_t>(p.exts.size()),
                                              pCreateInfo, arena,
                                              /*faked=*/nullptr, nullptr);

    vkb_emu_device_create_info(parent->table(), physicalDevice, p.caps.emu,
                               pCreateInfo, arena);
    return p.caps.emu;
}

VKAPI_ATTR VkResult VKAPI_CALL Layer_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char *pLayerName, uint32_t *pCount,
    VkExtensionProperties *pProperties) {
    auto ctx = get_scoped_ctx(physicalDevice);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());

    auto exts = query_device_extensions(ctx->table(), physicalDevice, pLayerName);
    const uint32_t real_count = static_cast<uint32_t>(exts.size());

    const uint32_t cap = real_count + kExtensionCapacityBuffer + VKB_EXT_ALIAS_COUNT;
    exts.resize(cap);

    const uint32_t count = vkb_caps_append_device_extensions(&p.caps, exts.data(),
                                                              real_count, cap);

    if (!pProperties) {
        *pCount = count;
        return VK_SUCCESS;
    }

    const uint32_t n = std::min(*pCount, count);
    std::copy_n(exts.data(), n, pProperties);
    *pCount = n;
    return (n < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures) {
    auto ctx = get_scoped_ctx(physicalDevice);
    ctx->table()->real.vkGetPhysicalDeviceFeatures(physicalDevice, pFeatures);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());
    vkb_caps_patch_features(&p.caps, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures) {
    auto ctx = get_scoped_ctx(physicalDevice);
    ctx->table()->real.vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());
    vkb_caps_patch_features(&p.caps, &pFeatures->features);

    for_each_pNext(pFeatures->pNext, [&](VkBaseOutStructure *b) {
        vkb_caps_patch_features_chain(&p.caps, b, /*is_remote_bridge=*/0, false);
    });
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties *pFormatProperties) {
    auto ctx = get_scoped_ctx(physicalDevice);
    ctx->table()->real.vkGetPhysicalDeviceFormatProperties(physicalDevice, format,
                                                            pFormatProperties);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());
    vkb_caps_format_properties(&p.caps, physicalDevice, format, pFormatProperties,
                               nullptr, layer_get_fmt, ctx->table());
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice, VkFormat format,
    VkFormatProperties2 *pFormatProperties) {
    auto ctx = get_scoped_ctx(physicalDevice);
    ctx->table()->real.vkGetPhysicalDeviceFormatProperties2(physicalDevice, format,
                                                             pFormatProperties);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());
    vkb_caps_format_properties(&p.caps, physicalDevice, format,
                               &pFormatProperties->formatProperties, pFormatProperties,
                               layer_get_fmt, ctx->table());
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    auto ctx = get_scoped_ctx(physicalDevice);
    ctx->table()->dt.vkGetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());
    // Cap device-local heap to VKBRIDGE_HEAP_MAX_MB
    vkb_caps_patch_memory_heaps(&p.caps, pMemoryProperties, nullptr);
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    auto ctx = get_scoped_ctx(physicalDevice);
    ctx->table()->dt.vkGetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());

    VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget = nullptr;
    for_each_pNext(pMemoryProperties->pNext, [&](VkBaseOutStructure *b) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT *>(b);
    });
    vkb_caps_patch_memory_heaps(&p.caps, &pMemoryProperties->memoryProperties, budget);
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties) {
    auto ctx = get_scoped_ctx(physicalDevice);
    ctx->table()->real.vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());
    vkb_caps_patch_properties(&p.caps, pProperties, 0);
}

VKAPI_ATTR void VKAPI_CALL Layer_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties) {
    auto ctx = get_scoped_ctx(physicalDevice);
    ctx->table()->real.vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
    const PdCaps &p = pd_caps(physicalDevice, ctx->table());
    vkb_caps_patch_properties(&p.caps, &pProperties->properties, 0);

    for_each_pNext(pProperties->pNext, [&](VkBaseOutStructure *b) {
        vkb_caps_patch_properties_chain(&p.caps, physicalDevice, b, layer_get_props2,
                                        ctx->table());
    });
}
