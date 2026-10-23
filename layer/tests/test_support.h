/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fake Vulkan driver the layer can run on, and the
 * fixture that wires it up (the context, the handle registry and the
 * environment the capability model reads).
 */
#pragma once

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "vkb_server.h"
}

#include "layer_ctx.h"

/* A Vulkan driver stub for the layer tests to run on. It answers the queries
 * the capability model makes (features, properties, extensions, formats,
 * memory) and counts them. */
class FakeDriver {
  public:
    FakeDriver();

    std::vector<VkExtensionProperties> extensions;
    VkPhysicalDeviceFeatures features{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceVertexAttributeDivisorFeatures divisor_features{};
    VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing{};
    VkPhysicalDeviceTexelBufferAlignmentProperties texel_alignment{};
    std::map<VkFormat, VkFormatProperties> formats;

    // Tracks number of queries (ext/features/props/etc) actually made to the
    // underlying driver
    int extension_queries = 0;
    int feature_queries = 0;
    int property_queries = 0;
    int format_queries = 0;
    int memory_queries = 0;

    std::vector<VkPhysicalDevice> physical_devices;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    // Tracks the number of creations actually made to the underlying driver
    int instance_creations = 0;
    int instance_destroys = 0;
    int device_creations = 0;
    int device_destroys = 0;
    int allocated_command_buffers = 0;
    int freed_command_buffers = 0;
    std::vector<std::string> last_device_extensions;
    VkPhysicalDeviceFeatures last_device_features{};

    void add_extension(const char *name, uint32_t spec_version = 1);
    bool has_extension(const char *name) const;
    void set_format(VkFormat format, VkFormatFeatureFlags optimal);

    void install(vkb_dispatch &dispatch) const;

    // The "real" entry points (gdpa/gipa) in the fake driver
    PFN_vkVoidFunction instance_proc(const char *name);
    PFN_vkVoidFunction device_proc(const char *name);

    // Tracks vulkan functions asked for but not implemented/whitelisted
    std::set<std::string> unresolved;
};

FakeDriver *&current_fake_driver();

// Whitelisted unsupported functions
const std::set<std::string> &fake_expected_unresolved_names();

// Unsupported functions not already whitelisted - these should be audited to
// see if they need to be implemented in the fake or whitelisted
const std::set<std::string> &fake_unresolved_names();

// The "real" gipa/gdpa with the correct function signatures (just dispatches to
// fake_driver->instance_proc/device_proc)
PFN_vkVoidFunction fake_get_instance_proc_addr(VkInstance instance,
                                               const char *name);
PFN_vkVoidFunction fake_get_device_proc_addr(VkDevice device, const char *name);

// The stub function
PFN_vkVoidFunction fake_driver_only_proc();

VkPhysicalDevice fake_physical_device();

struct LoaderLink {
    VkLayerInstanceCreateInfo instance_info{
        VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO};
    VkLayerInstanceLink instance_link{};
    VkLayerDeviceCreateInfo device_info{
        VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO};
    VkLayerDeviceLink device_link{};

    LoaderLink();
};

// Fake pipeline cache directory for the test
void use_scratch_cache_dir();

// Scoped layer stub for unit tests
class LayerFixture {
  public:
    LayerFixture();
    ~LayerFixture();
    LayerFixture(const LayerFixture &) = delete;
    LayerFixture &operator=(const LayerFixture &) = delete;

    FakeDriver &driver() { return *m_driver; }
    VkPhysicalDevice physical_device() const { return m_pd; }
    const std::shared_ptr<LayerCtx> &context() const { return m_ctx; }
    vkb_srv_table *table() const { return m_ctx->table(); }

    // Sets an environment variable for the time the fixture lives.
    void set_env(const char *name, const char *value);

  private:
    std::unique_ptr<FakeDriver> m_driver;
    std::shared_ptr<LayerCtx> m_ctx;
    VkPhysicalDevice m_pd = VK_NULL_HANDLE;
    std::vector<std::pair<std::string, std::optional<std::string>>> m_env;
};
