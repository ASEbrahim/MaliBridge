/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_support.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
PFN_vkGetInstanceProcAddr vkb_gipa = nullptr;
vkb_dispatch vkb_global_dt{};
int vkb_verbose = 0;

int vkb_log_level(void) { return VKB_LOG_ERROR; }

void vkb_log(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
} // extern "C"

namespace {

FakeDriver *g_driver = nullptr;

VkPhysicalDevice next_physical_device() {
    static uintptr_t next = 0x1000;
    next += 0x1000;
    return reinterpret_cast<VkPhysicalDevice>(next);
}

FakeDriver *driver() { return g_driver; }

VKAPI_ATTR VkResult VKAPI_CALL
fake_EnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *layer_name,
                                        uint32_t *count, VkExtensionProperties *props) {
    FakeDriver *d = driver();
    if (!d || !count) return VK_ERROR_INITIALIZATION_FAILED;
    d->extension_queries++;
    const uint32_t total = static_cast<uint32_t>(d->extensions.size());
    if (!props) {
        *count = total;
        return VK_SUCCESS;
    }
    const uint32_t written = std::min(*count, total);
    std::copy_n(d->extensions.begin(), written, props);
    *count = written;
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_GetPhysicalDeviceFeatures(VkPhysicalDevice,
                                                           VkPhysicalDeviceFeatures *features) {
    FakeDriver *d = driver();
    if (!d || !features) return;
    d->feature_queries++;
    *features = d->features;
}

VKAPI_ATTR void VKAPI_CALL fake_GetPhysicalDeviceFeatures2(VkPhysicalDevice,
                                                            VkPhysicalDeviceFeatures2 *features) {
    FakeDriver *d = driver();
    if (!d || !features) return;
    d->feature_queries++;
    features->features = d->features;
    for (auto *b = static_cast<VkBaseOutStructure *>(features->pNext); b; b = b->pNext) {
        switch (b->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES: {
            auto *div = reinterpret_cast<VkPhysicalDeviceVertexAttributeDivisorFeatures *>(b);
            div->vertexAttributeInstanceRateDivisor = d->divisor_features.vertexAttributeInstanceRateDivisor;
            div->vertexAttributeInstanceRateZeroDivisor = d->divisor_features.vertexAttributeInstanceRateZeroDivisor;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES:
            reinterpret_cast<VkPhysicalDeviceMaintenance5Features *>(b)->maintenance5 = VK_FALSE;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES:
            reinterpret_cast<VkPhysicalDeviceMaintenance6Features *>(b)->maintenance6 = VK_FALSE;
            break;
        default:
            break;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL fake_GetPhysicalDeviceProperties(VkPhysicalDevice,
                                                             VkPhysicalDeviceProperties *props) {
    FakeDriver *d = driver();
    if (!d || !props) return;
    d->property_queries++;
    *props = d->properties;
}

VKAPI_ATTR void VKAPI_CALL fake_GetPhysicalDeviceProperties2(VkPhysicalDevice,
                                                              VkPhysicalDeviceProperties2 *props) {
    FakeDriver *d = driver();
    if (!d || !props) return;
    d->property_queries++;
    props->properties = d->properties;
    for (auto *b = static_cast<VkBaseOutStructure *>(props->pNext); b; b = b->pNext) {
        switch (b->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
            *reinterpret_cast<VkPhysicalDeviceDescriptorIndexingProperties *>(b) = d->descriptor_indexing;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES:
            *reinterpret_cast<VkPhysicalDeviceTexelBufferAlignmentProperties *>(b) = d->texel_alignment;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES: {
            auto *div = reinterpret_cast<VkPhysicalDeviceVertexAttributeDivisorProperties *>(b);
            div->maxVertexAttribDivisor = 65535;
            div->supportsNonZeroFirstInstance = VK_FALSE;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES:
            reinterpret_cast<VkPhysicalDevicePushDescriptorProperties *>(b)->maxPushDescriptors = 0;
            break;
        default:
            break;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL fake_GetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat format,
                                                                   VkFormatProperties *props) {
    FakeDriver *d = driver();
    if (!d || !props) return;
    d->format_queries++;
    auto it = d->formats.find(format);
    *props = (it != d->formats.end()) ? it->second : VkFormatProperties{};
}

VKAPI_ATTR void VKAPI_CALL fake_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat format,
                                                                    VkFormatProperties2 *props) {
    FakeDriver *d = driver();
    if (!d || !props) return;
    d->format_queries++;
    auto it = d->formats.find(format);
    props->formatProperties = (it != d->formats.end()) ? it->second : VkFormatProperties{};
    for (auto *b = static_cast<VkBaseOutStructure *>(props->pNext); b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            auto *p3 = reinterpret_cast<VkFormatProperties3 *>(b);
            p3->linearTilingFeatures = props->formatProperties.linearTilingFeatures;
            p3->optimalTilingFeatures = props->formatProperties.optimalTilingFeatures;
            p3->bufferFeatures = props->formatProperties.bufferFeatures;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL fake_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice,
                                                                   VkPhysicalDeviceMemoryProperties *mem) {
    FakeDriver *d = driver();
    if (!d || !mem) return;
    d->memory_queries++;
    *mem = d->memory;
}

VKAPI_ATTR void VKAPI_CALL fake_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice,
                                                                    VkPhysicalDeviceMemoryProperties2 *mem) {
    FakeDriver *d = driver();
    if (!d || !mem) return;
    d->memory_queries++;
    mem->memoryProperties = d->memory;
    for (auto *b = static_cast<VkBaseOutStructure *>(mem->pNext); b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            auto *budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT *>(b);
            for (uint32_t h = 0; h < d->memory.memoryHeapCount; h++)
                budget->heapBudget[h] = d->memory.memoryHeaps[h].size;
        }
    }
}

} // namespace

FakeDriver::FakeDriver() {
    add_extension("VK_KHR_surface");
    add_extension("VK_KHR_swapchain");
    add_extension("VK_KHR_vertex_attribute_divisor");
    add_extension("VK_KHR_push_descriptor", 2);

    features.robustBufferAccess = VK_TRUE;
    features.shaderStorageImageExtendedFormats = VK_TRUE;
    features.depthClamp = VK_TRUE;

    properties.apiVersion = VK_API_VERSION_1_1;
    properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    properties.vendorID = 0x13b5; /* Arm */
    properties.deviceID = 0x1234;
    std::snprintf(properties.deviceName, sizeof(properties.deviceName), "Fake Mali");
    properties.limits.maxViewports = 1;
    properties.limits.maxFragmentDualSrcAttachments = 0;
    properties.limits.maxDrawIndirectCount = 1;
    properties.limits.maxPerStageDescriptorSampledImages = 128;
    properties.limits.maxDescriptorSetSampledImages = 128;

    divisor_features.vertexAttributeInstanceRateDivisor = VK_TRUE;
    divisor_features.vertexAttributeInstanceRateZeroDivisor = VK_TRUE;

    memory.memoryHeapCount = 2;
    memory.memoryHeaps[0].size = 4ull << 30; /* device-local */
    memory.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    memory.memoryHeaps[1].size = 1ull << 30;
    memory.memoryHeaps[1].flags = 0;
    memory.memoryTypeCount = 1;
    memory.memoryTypes[0].heapIndex = 0;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    /* The formats the BC decoder writes to, and the format a BC texture is sampled as. */
    const VkFormatFeatureFlags storage = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    set_format(VK_FORMAT_R8G8B8A8_UNORM,
               storage | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT);
    set_format(VK_FORMAT_R8_UNORM, storage);
    set_format(VK_FORMAT_R8G8_UNORM, storage);
    set_format(VK_FORMAT_R16G16B16A16_SFLOAT, storage);
}

void FakeDriver::add_extension(const char *name, uint32_t spec_version) {
    VkExtensionProperties e{};
    std::snprintf(e.extensionName, sizeof(e.extensionName), "%s", name);
    e.specVersion = spec_version;
    extensions.push_back(e);
}

bool FakeDriver::has_extension(const char *name) const {
    return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties &e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

void FakeDriver::set_format(VkFormat format, VkFormatFeatureFlags optimal) {
    VkFormatProperties p{};
    p.optimalTilingFeatures = optimal;
    p.linearTilingFeatures = optimal;
    formats[format] = p;
}

void FakeDriver::install(vkb_dispatch &dispatch) const {
    dispatch.vkEnumerateDeviceExtensionProperties = fake_EnumerateDeviceExtensionProperties;
    dispatch.vkGetPhysicalDeviceFeatures = fake_GetPhysicalDeviceFeatures;
    dispatch.vkGetPhysicalDeviceFeatures2 = fake_GetPhysicalDeviceFeatures2;
    dispatch.vkGetPhysicalDeviceProperties = fake_GetPhysicalDeviceProperties;
    dispatch.vkGetPhysicalDeviceProperties2 = fake_GetPhysicalDeviceProperties2;
    dispatch.vkGetPhysicalDeviceFormatProperties = fake_GetPhysicalDeviceFormatProperties;
    dispatch.vkGetPhysicalDeviceFormatProperties2 = fake_GetPhysicalDeviceFormatProperties2;
    dispatch.vkGetPhysicalDeviceMemoryProperties = fake_GetPhysicalDeviceMemoryProperties;
    dispatch.vkGetPhysicalDeviceMemoryProperties2 = fake_GetPhysicalDeviceMemoryProperties2;
}

FakeDriver *&current_fake_driver() { return g_driver; }

LayerFixture::LayerFixture() {
    m_driver = std::make_unique<FakeDriver>();
    m_ctx = std::make_shared<LayerCtx>();
    m_ctx->t = std::make_unique<vkb_srv_table>();
    m_driver->install(m_ctx->t->real);
    m_driver->install(m_ctx->t->dt);
    m_pd = next_physical_device();
    current_fake_driver() = m_driver.get();
    register_handle(m_pd, m_ctx);
}

LayerFixture::~LayerFixture() {
    unregister_handle(m_pd);
    if (current_fake_driver() == m_driver.get())
        current_fake_driver() = nullptr;
    for (auto it = m_env.rbegin(); it != m_env.rend(); ++it) {
        if (it->second)
            setenv(it->first.c_str(), it->second->c_str(), 1);
        else
            unsetenv(it->first.c_str());
    }
}

void LayerFixture::set_env(const char *name, const char *value) {
    const char *old = getenv(name);
    m_env.emplace_back(name, old ? std::optional<std::string>(old) : std::nullopt);
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
}
