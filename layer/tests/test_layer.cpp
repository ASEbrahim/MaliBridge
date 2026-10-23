/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <catch2/catch_all.hpp>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include "vkb_server.h"
}

#include "gen/layer_dispatch_gen.h"
#include "layer_ctx.h"
#include "test_support.h"

namespace {

/* A layer with an instance (and, on demand, a device) on top of a FakeDriver: the loader's
 * chain, the driver's own instance and device, and the handles the layer keeps for them. */
class LayerStack {
public:
    LayerStack() {
        use_scratch_cache_dir();
        m_driver = std::make_unique<FakeDriver>();
        m_driver->physical_devices.push_back(fake_physical_device());
        current_fake_driver() = m_driver.get();

        VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create_info.pNext = &m_link.instance_info;
        m_result = Layer_vkCreateInstance(&create_info, nullptr, &m_instance);
        if (m_result != VK_SUCCESS)
            return;

        /* The physical devices the layer hands out are the ones it keeps a context for. */
        uint32_t count = 0;
        Layer_vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        Layer_vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
        if (!devices.empty())
            m_physical_device = devices[0];
    }

    ~LayerStack() {
        if (m_device)
            Layer_vkDestroyDevice(m_device, nullptr);
        if (m_instance)
            Layer_vkDestroyInstance(m_instance, nullptr);
        if (current_fake_driver() == m_driver.get())
            current_fake_driver() = nullptr;
    }

    LayerStack(const LayerStack &) = delete;
    LayerStack &operator=(const LayerStack &) = delete;

    FakeDriver &driver() { return *m_driver; }
    VkResult result() const { return m_result; }
    VkInstance instance() const { return m_instance; }
    VkDevice device() const { return m_device; }
    VkPhysicalDevice physical_device() const { return m_physical_device; }

    /* The device, through the loader's chain. */
    VkResult create_device() {
        VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create_info.pNext = &m_link.device_info;
        return Layer_vkCreateDevice(m_physical_device, &create_info, nullptr, &m_device);
    }

private:
    std::unique_ptr<FakeDriver> m_driver;
    LoaderLink m_link;
    VkResult m_result = VK_ERROR_INITIALIZATION_FAILED;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
};

} // namespace

/* ------------------------------------------------------------------ the instance */

TEST_CASE("the layer creates the instance through the loader's chain", "[layer]") {
    LayerStack stack;

    REQUIRE(stack.result() == VK_SUCCESS);
    REQUIRE(stack.instance() != VK_NULL_HANDLE);
    REQUIRE(stack.driver().instance_creations == 1);
    /* The layer keeps a context for the instance. */
    auto ctx = get_ctx(stack.instance());
    REQUIRE(ctx != nullptr);
    REQUIRE(ctx->t != nullptr);
    REQUIRE(ctx->t->instance == stack.instance());
}

TEST_CASE("the layer refuses an instance without the loader's link info", "[layer]") {
    use_scratch_cache_dir();
    auto driver = std::make_unique<FakeDriver>();
    current_fake_driver() = driver.get();

    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance = VK_NULL_HANDLE;
    REQUIRE(Layer_vkCreateInstance(&create_info, nullptr, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    REQUIRE(driver->instance_creations == 0);

    current_fake_driver() = nullptr;
}

TEST_CASE("the layer destroys the instance", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);

    Layer_vkDestroyInstance(stack.instance(), nullptr);

    REQUIRE(stack.driver().instance_destroys == 1);
    REQUIRE(get_ctx(stack.instance()) == nullptr);
}

TEST_CASE("the layer keeps a context for the physical devices it hands out", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);

    REQUIRE(stack.physical_device() != VK_NULL_HANDLE);
    REQUIRE(get_ctx(stack.physical_device()) != nullptr);
}

/* ------------------------------------------------------------------ the device */

TEST_CASE("the layer creates the device through the loader's chain", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);

    REQUIRE(stack.create_device() == VK_SUCCESS);
    REQUIRE(stack.device() != VK_NULL_HANDLE);
    REQUIRE(stack.driver().device_creations == 1);

    auto ctx = get_ctx(stack.device());
    REQUIRE(ctx != nullptr);
    REQUIRE(ctx->device == stack.device());
    /* The device's context hangs under the instance's. */
    REQUIRE(ctx->parent != nullptr);
    REQUIRE(ctx->parent.get() == get_ctx(stack.instance()).get());
}

TEST_CASE("the layer refuses a device without the loader's link info", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);

    VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice device = VK_NULL_HANDLE;
    REQUIRE(Layer_vkCreateDevice(stack.physical_device(), &create_info, nullptr, &device) ==
            VK_ERROR_INITIALIZATION_FAILED);
    REQUIRE(stack.driver().device_creations == 0);
}

TEST_CASE("the layer refuses a device of a physical device it does not know", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);

    LoaderLink link;
    VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create_info.pNext = &link.device_info;
    VkDevice device = VK_NULL_HANDLE;
    REQUIRE(Layer_vkCreateDevice(fake_physical_device(), &create_info, nullptr, &device) ==
            VK_ERROR_INITIALIZATION_FAILED);
    REQUIRE(stack.driver().device_creations == 0);
}

TEST_CASE("the driver gets the create info without the emulated extensions", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);

    LoaderLink link;
    const char *requested[] = {"VK_KHR_swapchain", "VK_EXT_robustness2"};
    VkDeviceCreateInfo create_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create_info.pNext = &link.device_info;
    create_info.enabledExtensionCount = 2;
    create_info.ppEnabledExtensionNames = requested;

    VkDevice device = VK_NULL_HANDLE;
    REQUIRE(Layer_vkCreateDevice(stack.physical_device(), &create_info, nullptr, &device) ==
            VK_SUCCESS);

    const auto &names = stack.driver().last_device_extensions;
    const auto has = [&names](const char *name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    REQUIRE(has("VK_KHR_swapchain"));    /* the driver has it */
    REQUIRE(!has("VK_EXT_robustness2")); /* the layer emulates it: the driver must not see it */
}

TEST_CASE("the layer destroys the device", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);
    REQUIRE(stack.create_device() == VK_SUCCESS);

    Layer_vkDestroyDevice(stack.device(), nullptr);

    REQUIRE(stack.driver().device_destroys == 1);
    REQUIRE(get_ctx(stack.device()) == nullptr);
}

/* ------------------------------------------------------------------ the handles the layer keeps */

TEST_CASE("the layer keeps a context for the command buffers it allocates", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);
    REQUIRE(stack.create_device() == VK_SUCCESS);

    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = reinterpret_cast<VkCommandPool>(0x1000);
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 2;
    VkCommandBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    REQUIRE(Layer_vkAllocateCommandBuffers(stack.device(), &info, buffers) == VK_SUCCESS);
    REQUIRE(get_ctx(buffers[0]) != nullptr);
    REQUIRE(get_ctx(buffers[1]) != nullptr);

    Layer_vkFreeCommandBuffers(stack.device(), info.commandPool, 2, buffers);
    REQUIRE(get_ctx(buffers[0]) == nullptr);
    REQUIRE(get_ctx(buffers[1]) == nullptr);
}

TEST_CASE("the layer keeps a context for the queue it hands out", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);
    REQUIRE(stack.create_device() == VK_SUCCESS);

    VkQueue queue = VK_NULL_HANDLE;
    Layer_vkGetDeviceQueue(stack.device(), 0, 0, &queue);
    REQUIRE(queue != VK_NULL_HANDLE);
    REQUIRE(get_ctx(queue) != nullptr);

    VkQueue queue2 = VK_NULL_HANDLE;
    Layer_vkGetDeviceQueue2(stack.device(), nullptr, &queue2);
    REQUIRE(queue2 != VK_NULL_HANDLE);
    REQUIRE(get_ctx(queue2) != nullptr);
}

/* ------------------------------------------------------------------ the names the loader asks for */

TEST_CASE("the layer's GetInstanceProcAddr hands out its own entry points", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);

    REQUIRE(Layer_vkGetInstanceProcAddr(stack.instance(), "vkGetPhysicalDeviceFeatures") != nullptr);
    REQUIRE(Layer_vkGetInstanceProcAddr(stack.instance(), "vkEnumerateDeviceExtensionProperties") != nullptr);
    REQUIRE(Layer_vkGetInstanceProcAddr(stack.instance(), "vkGetInstanceProcAddr") ==
            reinterpret_cast<PFN_vkVoidFunction>(Layer_vkGetInstanceProcAddr));
    REQUIRE(Layer_vkGetInstanceProcAddr(stack.instance(), "vkGetDeviceProcAddr") ==
            reinterpret_cast<PFN_vkVoidFunction>(Layer_vkGetDeviceProcAddr));
    REQUIRE(Layer_vkGetInstanceProcAddr(stack.instance(), nullptr) == nullptr);
}

TEST_CASE("the layer's GetDeviceProcAddr hands out its own entry points", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);
    REQUIRE(stack.create_device() == VK_SUCCESS);

    REQUIRE(Layer_vkGetDeviceProcAddr(stack.device(), "vkAllocateCommandBuffers") != nullptr);
    REQUIRE(Layer_vkGetDeviceProcAddr(stack.device(), "vkGetDeviceProcAddr") ==
            reinterpret_cast<PFN_vkVoidFunction>(Layer_vkGetDeviceProcAddr));
    REQUIRE(Layer_vkGetDeviceProcAddr(stack.device(), nullptr) == nullptr);
}

TEST_CASE("a name the layer does not know is passed on to the driver", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);
    REQUIRE(stack.create_device() == VK_SUCCESS);

    REQUIRE(Layer_vkGetInstanceProcAddr(stack.instance(), "vkFakeDriverOnly") ==
            fake_driver_only_proc());
    REQUIRE(Layer_vkGetDeviceProcAddr(stack.device(), "vkFakeDriverOnly") ==
            fake_driver_only_proc());
}

/* ------------------------------------------------------------------ the loader's interface */

TEST_CASE("the loader's interface version is negotiated", "[layer]") {
    VkNegotiateLayerInterface version{};
    version.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    version.loaderLayerInterfaceVersion = 2;

    REQUIRE(vkNegotiateLoaderLayerInterfaceVersion(&version) == VK_SUCCESS);
    REQUIRE(version.loaderLayerInterfaceVersion == 2);
    REQUIRE(version.pfnGetInstanceProcAddr != nullptr);
    REQUIRE(version.pfnGetDeviceProcAddr != nullptr);
}

TEST_CASE("the loader's interface version is refused when it is too old", "[layer]") {
    VkNegotiateLayerInterface version{};
    version.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    version.loaderLayerInterfaceVersion = 1;

    REQUIRE(vkNegotiateLoaderLayerInterfaceVersion(&version) == VK_ERROR_INITIALIZATION_FAILED);
    REQUIRE(vkNegotiateLoaderLayerInterfaceVersion(nullptr) == VK_ERROR_INITIALIZATION_FAILED);
}

/* ------------------------------------------------------------------ what the layer says about itself */

TEST_CASE("the layer's own properties are listed", "[layer]") {
    uint32_t count = 0;
    REQUIRE(Layer_vkEnumerateInstanceLayerProperties(&count, nullptr) == VK_SUCCESS);
    REQUIRE(count == 1);

    VkLayerProperties props{};
    uint32_t room = 1;
    REQUIRE(Layer_vkEnumerateInstanceLayerProperties(&room, &props) == VK_SUCCESS);
    REQUIRE(room == 1);
    REQUIRE(std::strcmp(props.layerName, "VK_LAYER_MALIBRIDGE_compat") == 0);
    REQUIRE(props.implementationVersion == 1);
    REQUIRE(props.specVersion == VK_API_VERSION_1_3);
}

TEST_CASE("a list of the layer's properties that does not fit is refused", "[layer]") {
    VkLayerProperties props{};
    uint32_t room = 0;
    REQUIRE(Layer_vkEnumerateInstanceLayerProperties(&room, &props) == VK_INCOMPLETE);
    REQUIRE(Layer_vkEnumerateInstanceLayerProperties(nullptr, &props) ==
            VK_ERROR_INITIALIZATION_FAILED);
}

TEST_CASE("the layer has no instance extensions of its own", "[layer]") {
    uint32_t count = 1;
    REQUIRE(Layer_vkEnumerateInstanceExtensionProperties("VK_LAYER_MALIBRIDGE_compat", &count,
                                                         nullptr) == VK_SUCCESS);
    REQUIRE(count == 0);

    /* Another layer's name is not the layer's to answer for. */
    REQUIRE(Layer_vkEnumerateInstanceExtensionProperties("VK_LAYER_OTHER", &count, nullptr) ==
            VK_ERROR_LAYER_NOT_PRESENT);
}

TEST_CASE("the layer has no device layer properties", "[layer]") {
    uint32_t count = 1;
    REQUIRE(Layer_vkEnumerateDeviceLayerProperties(VK_NULL_HANDLE, &count, nullptr) == VK_SUCCESS);
    REQUIRE(count == 0);
}

TEST_CASE("the physical device groups are the driver's", "[layer]") {
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);

    uint32_t count = 0;
    REQUIRE(Layer_vkEnumeratePhysicalDeviceGroups(stack.instance(), &count, nullptr) == VK_SUCCESS);
    REQUIRE(count == 1);

    VkPhysicalDeviceGroupProperties group{};
    uint32_t room = 1;
    REQUIRE(Layer_vkEnumeratePhysicalDeviceGroups(stack.instance(), &room, &group) == VK_SUCCESS);
    REQUIRE(group.physicalDeviceCount == 1);
    REQUIRE(group.physicalDevices[0] == stack.physical_device());
}

/* ------------------------------------------------------------------ the fake driver's own limits */

TEST_CASE("the fake driver is asked for nothing new", "[layer][support]") {
    /* The instance and the device fill the dispatch tables: the loader asks the driver for
     * every Vulkan function there is. The fake driver answers the ones the layer's own path
     * needs; the rest are in the whitelist (fake_driver_names.h). A name outside it is logged
     * when it is asked for: the layer, or the server under it, may be about to call it. */
    LayerStack stack;
    REQUIRE(stack.result() == VK_SUCCESS);
    REQUIRE(stack.create_device() == VK_SUCCESS);

    const auto &expected = fake_expected_unresolved_names();
    std::vector<std::string> unknown;
    for (const auto &name : fake_unresolved_names())
        if (!expected.count(name))
            unknown.push_back(name);
    INFO("the fake driver was asked for " << unknown.size() << " names outside the whitelist");
    REQUIRE(unknown.empty());
}
