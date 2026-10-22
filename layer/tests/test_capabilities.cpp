/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests capability (feature, ext, properties, limits) filtering and spoofing.
 *
 * The layer runs on a FakeDriver (test_support.h): a GPU of a Vulkan 1.1 driver
 * without the features the layer emulates.
 */
#include <catch2/catch_all.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "vkb_proto.h"
}

#include "capabilities.h"
#include "gen/layer_dispatch_gen.h"
#include "test_support.h"

namespace {

/* The device extensions as the app sees them. */
std::vector<std::string> enumerate_extensions(LayerFixture &f) {
    uint32_t count = 0;
    REQUIRE(Layer_vkEnumerateDeviceExtensionProperties(
                f.physical_device(), nullptr, &count, nullptr) == VK_SUCCESS);
    std::vector<VkExtensionProperties> exts(count);
    uint32_t written = count;
    REQUIRE(Layer_vkEnumerateDeviceExtensionProperties(
                f.physical_device(), nullptr, &written, exts.data()) ==
            VK_SUCCESS);
    REQUIRE(written == count);

    std::vector<std::string> names;
    names.reserve(count);
    for (const auto &e : exts)
        names.emplace_back(e.extensionName);
    return names;
}

bool has(const std::vector<std::string> &names, const char *name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool has(const char *const *names, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++)
        if (std::strcmp(names[i], name) == 0)
            return true;
    return false;
}

} // namespace

TEST_CASE("the app sees the driver's own device extensions", "[capabilities]") {
    LayerFixture f;

    const auto names = enumerate_extensions(f);

    REQUIRE(has(names, "VK_KHR_swapchain"));
    REQUIRE(has(names, "VK_KHR_surface"));
    REQUIRE(has(names, "VK_KHR_push_descriptor"));
}

TEST_CASE("an extension the driver has only as KHR is offered under both names",
          "[capabilities]") {
    LayerFixture f;

    const auto names = enumerate_extensions(f);

    // The driver has VK_KHR_vertex_attribute_divisor only; the app asks for the
    // EXT name.
    REQUIRE(has(names, "VK_KHR_vertex_attribute_divisor"));
    REQUIRE(has(names, "VK_EXT_vertex_attribute_divisor"));
}

TEST_CASE("the emulated extensions are offered to the app", "[capabilities]") {
    LayerFixture f;

    const auto names = enumerate_extensions(f);

    // The GPU has neither robustness2 nor maintenance5: the layer emulates
    // both.
    REQUIRE(has(names, "VK_EXT_robustness2"));
    REQUIRE(has(names, "VK_KHR_maintenance5"));
    REQUIRE(has(names, "VK_KHR_maintenance6"));
    REQUIRE(has(names, "VK_EXT_depth_clip_enable"));
}

TEST_CASE("the extension list has no duplicates", "[capabilities]") {
    LayerFixture f;

    const auto names = enumerate_extensions(f);

    const std::set<std::string> unique(names.begin(), names.end());
    REQUIRE(unique.size() == names.size());
}

TEST_CASE("a list that does not fit is cut short", "[capabilities]") {
    LayerFixture f;

    uint32_t count = 0;
    REQUIRE(Layer_vkEnumerateDeviceExtensionProperties(
                f.physical_device(), nullptr, &count, nullptr) == VK_SUCCESS);
    REQUIRE(count > 1);

    std::vector<VkExtensionProperties> exts(1);
    uint32_t room = 1;
    REQUIRE(Layer_vkEnumerateDeviceExtensionProperties(
                f.physical_device(), nullptr, &room, exts.data()) ==
            VK_INCOMPLETE);
    REQUIRE(room == 1);
}

TEST_CASE("the capabilities of a device are detected once", "[capabilities]") {
    LayerFixture f;

    VkPhysicalDeviceProperties props{};
    Layer_vkGetPhysicalDeviceProperties(f.physical_device(), &props);
    const int queries = f.driver().extension_queries;
    REQUIRE(queries > 0);

    // We cache the properties and features after the first query
    Layer_vkGetPhysicalDeviceProperties(f.physical_device(), &props);
    VkPhysicalDeviceFeatures features{};
    Layer_vkGetPhysicalDeviceFeatures(f.physical_device(), &features);
    REQUIRE(f.driver().extension_queries == queries);
}

TEST_CASE("the device creation drops the extensions the layer emulates",
          "[capabilities]") {
    LayerFixture f;
    vkb_arena arena{};

    const char *requested[] = {
        "VK_KHR_swapchain",    // the driver has it
        "VK_EXT_robustness2",  // the layer emulates it
        "VK_KHR_maintenance5", // the layer emulates it
    };
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.enabledExtensionCount = 3;
    ci.ppEnabledExtensionNames = requested;

    const uint32_t emu = filter_device_create_info(
        f.physical_device(), f.context().get(), &ci, &arena);
    REQUIRE(emu & VKB_EMU_ROBUSTNESS2);
    REQUIRE(emu & VKB_EMU_MAINT5);

    REQUIRE(has(ci.ppEnabledExtensionNames, ci.enabledExtensionCount,
                "VK_KHR_swapchain"));
    REQUIRE(has(ci.ppEnabledExtensionNames, ci.enabledExtensionCount,
                "VK_KHR_push_descriptor")); // needed by BC
    REQUIRE(!has(ci.ppEnabledExtensionNames, ci.enabledExtensionCount,
                 "VK_EXT_robustness2"));
    REQUIRE(!has(ci.ppEnabledExtensionNames, ci.enabledExtensionCount,
                 "VK_KHR_maintenance5"));

    vkb_arena_free(&arena);
}

TEST_CASE(
    "the device creation enables the extension the driver has only as KHR",
    "[capabilities]") {
    LayerFixture f;
    vkb_arena arena{};

    const char *requested[] = {"VK_EXT_vertex_attribute_divisor"};
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = requested;

    filter_device_create_info(f.physical_device(), f.context().get(), &ci,
                              &arena);

    REQUIRE(ci.enabledExtensionCount == 2);
    REQUIRE(std::strcmp(ci.ppEnabledExtensionNames[0],
                        "VK_KHR_vertex_attribute_divisor") == 0);
    REQUIRE(std::strcmp(ci.ppEnabledExtensionNames[1],
                        "VK_KHR_push_descriptor") == 0);

    vkb_arena_free(&arena);
}

TEST_CASE("the device creation does not ask for what the GPU lacks",
          "[capabilities]") {
    LayerFixture f;
    vkb_arena arena{};

    VkPhysicalDeviceFeatures features{};
    features.shaderClipDistance = VK_TRUE; // the GPU has no clip distance
    features.shaderStorageImageExtendedFormats = VK_FALSE; // Needed by BC

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pEnabledFeatures = &features;

    filter_device_create_info(f.physical_device(), f.context().get(), &ci,
                              &arena);

    REQUIRE(features.shaderClipDistance == VK_FALSE);
    REQUIRE(features.shaderStorageImageExtendedFormats == VK_TRUE);

    vkb_arena_free(&arena);
}

TEST_CASE("an emulated feature is reported as supported", "[capabilities]") {
    LayerFixture f;

    VkPhysicalDeviceFeatures features{};
    Layer_vkGetPhysicalDeviceFeatures(f.physical_device(), &features);

    REQUIRE(features.shaderClipDistance == VK_TRUE);
    REQUIRE(features.shaderCullDistance == VK_TRUE);
    REQUIRE(features.textureCompressionBC == VK_TRUE);
    REQUIRE(features.dualSrcBlend == VK_TRUE);
    REQUIRE(features.fillModeNonSolid == VK_TRUE);
    REQUIRE(features.multiViewport == VK_TRUE);
    REQUIRE(features.multiDrawIndirect == VK_TRUE);
}

TEST_CASE(
    "a feature that is missing and not emulated is reported as unsupported",
    "[capabilities]") {
    LayerFixture f;
    f.set_env("VKBRIDGE_EMU_CLIP", "0");

    VkPhysicalDeviceFeatures features{};
    Layer_vkGetPhysicalDeviceFeatures(f.physical_device(), &features);

    REQUIRE(features.shaderClipDistance == VK_FALSE);
    REQUIRE(features.shaderCullDistance == VK_TRUE);
}

TEST_CASE("the emulated features in the chain are reported as supported",
          "[capabilities]") {
    LayerFixture f;
    f.driver().divisor_features.vertexAttributeInstanceRateDivisor = VK_TRUE;
    f.driver().divisor_features.vertexAttributeInstanceRateZeroDivisor =
        VK_FALSE;

    VkPhysicalDeviceVertexAttributeDivisorFeatures divisor{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES};
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &divisor};

    Layer_vkGetPhysicalDeviceFeatures2(f.physical_device(), &features);

    REQUIRE(divisor.vertexAttributeInstanceRateDivisor == VK_TRUE);
    REQUIRE(divisor.vertexAttributeInstanceRateZeroDivisor == VK_TRUE);
}

TEST_CASE("the app is shown the spoofed GPU", "[capabilities]") {
    LayerFixture f;
    f.set_env("VKBRIDGE_SPOOF_GPU", "nvidia");

    VkPhysicalDeviceProperties props{};
    Layer_vkGetPhysicalDeviceProperties(f.physical_device(), &props);

    REQUIRE(props.vendorID == 0x10de);
    REQUIRE(props.deviceID == 0x1c03);
    REQUIRE(std::strcmp(props.deviceName, "NVIDIA GeForce GTX 1060 6GB") == 0);
}

TEST_CASE("the emulated limits are raised", "[capabilities]") {
    LayerFixture f;

    VkPhysicalDeviceProperties props{};
    Layer_vkGetPhysicalDeviceProperties(f.physical_device(), &props);

    REQUIRE(props.limits.maxViewports >= 16);
    REQUIRE(props.limits.maxFragmentDualSrcAttachments >= 1);
    REQUIRE(props.limits.maxDrawIndirectCount == UINT32_MAX);
}

TEST_CASE("the emulated properties in the chain are reported",
          "[capabilities]") {
    LayerFixture f;
    f.driver().divisor_features.vertexAttributeInstanceRateZeroDivisor =
        VK_FALSE;

    VkPhysicalDeviceVertexAttributeDivisorProperties divisor{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES};
    VkPhysicalDeviceProperties2 props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &divisor};

    Layer_vkGetPhysicalDeviceProperties2(f.physical_device(), &props);

    REQUIRE(divisor.maxVertexAttribDivisor == UINT32_MAX);
    REQUIRE(divisor.supportsNonZeroFirstInstance == VK_TRUE);
}

TEST_CASE("the device-local heaps are capped", "[capabilities]") {
    LayerFixture f;
    f.set_env("VKBRIDGE_HEAP_MAX_MB", "64");

    VkPhysicalDeviceMemoryProperties memory{};
    Layer_vkGetPhysicalDeviceMemoryProperties(f.physical_device(), &memory);

    // device-local memory should be capped to 64MB
    REQUIRE(memory.memoryHeaps[0].size == (64ull << 20));
    REQUIRE(memory.memoryHeaps[1].size == (1ull << 30));
}

TEST_CASE("the budget follows the capped heaps", "[capabilities]") {
    LayerFixture f;
    f.set_env("VKBRIDGE_HEAP_MAX_MB", "64");

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};

    Layer_vkGetPhysicalDeviceMemoryProperties2(f.physical_device(), &memory);

    REQUIRE(memory.memoryProperties.memoryHeaps[0].size == (64ull << 20));
    REQUIRE(budget.heapBudget[0] == (64ull << 20));
    REQUIRE(budget.heapBudget[1] == (1ull << 30));
}

TEST_CASE("an emulated BC format reports the decoded format's features",
          "[capabilities]") {
    LayerFixture f;

    VkFormatProperties props{};
    Layer_vkGetPhysicalDeviceFormatProperties(
        f.physical_device(), VK_FORMAT_BC1_RGB_UNORM_BLOCK, &props);

    REQUIRE(props.optimalTilingFeatures ==
            (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
             VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
             VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
             VK_FORMAT_FEATURE_BLIT_SRC_BIT |
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT));
    REQUIRE(props.linearTilingFeatures == 0);
    REQUIRE(props.bufferFeatures == 0);
}

TEST_CASE("a format the driver has is reported as it is", "[capabilities]") {
    LayerFixture f;

    VkFormatProperties props{};
    Layer_vkGetPhysicalDeviceFormatProperties(f.physical_device(),
                                              VK_FORMAT_R8G8B8A8_UNORM, &props);

    REQUIRE(props.optimalTilingFeatures ==
            f.driver().formats[VK_FORMAT_R8G8B8A8_UNORM].optimalTilingFeatures);
}

TEST_CASE("the format properties chain is filled for an emulated format",
          "[capabilities]") {
    LayerFixture f;

    VkFormatProperties3 props3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &props3};

    Layer_vkGetPhysicalDeviceFormatProperties2(
        f.physical_device(), VK_FORMAT_BC1_RGB_UNORM_BLOCK, &props);

    REQUIRE(props3.optimalTilingFeatures ==
            props.formatProperties.optimalTilingFeatures);
    REQUIRE(props3.linearTilingFeatures == 0);
    REQUIRE(props3.bufferFeatures == 0);
}
