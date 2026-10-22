/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fake Vulkan driver the layer can run on, and the
 * fixture that wires it up (the context, the handle registry and the
 * environment the capability model reads).
 */
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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

    int extension_queries = 0;
    int feature_queries = 0;
    int property_queries = 0;
    int format_queries = 0;
    int memory_queries = 0;

    void add_extension(const char *name, uint32_t spec_version = 1);
    bool has_extension(const char *name) const;
    void set_format(VkFormat format, VkFormatFeatureFlags optimal);

    void install(vkb_dispatch &dispatch) const;
};

FakeDriver *&current_fake_driver();

/* Scoped layer stub for unit tests */
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

    /* Sets an environment variable for the time the fixture lives. */
    void set_env(const char *name, const char *value);

  private:
    std::unique_ptr<FakeDriver> m_driver;
    std::shared_ptr<LayerCtx> m_ctx;
    VkPhysicalDevice m_pd = VK_NULL_HANDLE;
    std::vector<std::pair<std::string, std::optional<std::string>>> m_env;
};
