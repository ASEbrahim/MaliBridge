/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * desc_limits_check: descriptor limits as vkd3d-proton's legacy bindless path wants them (1,000,000
 * sampled images, storage images and storage buffers per stage after bind) on a GPU with fewer
 * (Mali r44: 500,000; the server cuts what reaches the driver, server_emu_desc.c). Set up as vkd3d
 * does: UPDATE_AFTER_BIND + PARTIALLY_BOUND + VARIABLE_DESCRIPTOR_COUNT layouts at 1,000,000, host
 * layouts beside them, sets at a variable count of 1,000,000, descriptors written into both and
 * copied from host to shader sets, some written past Mali's limit; a compute dispatch then reads
 * low-index descriptors back. On the host, VKBRIDGE_FAKE_MISSING=desclimits in the server's
 * environment makes the desktop driver imitate Mali's limits.
 *
 *   cc -O2 -Wall -Ithird_party/Vulkan-Headers-1.4.341/include -Icommon -Icommon/gen -Itests \
 *      -o host/out/desc_limits_check tests/desc_limits_check.c -ldl          (from the repository root)
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT 1000000u       /* vkd3d's VKD3D_MIN_VIEW_DESCRIPTOR_COUNT */
#define MALI_LIMIT 500000u   /* Mali-G720 r44 */

/* glslc -O --target-env=vulkan1.2 of:
 *   #extension GL_EXT_nonuniform_qualifier : require
 *   #extension GL_EXT_samplerless_texture_functions : require
 *   layout(local_size_x = 1) in;
 *   layout(set = 0, binding = 0) uniform utexture2D tex[];
 *   layout(set = 1, binding = 0, r32ui) uniform readonly uimage2D img[];
 *   layout(set = 2, binding = 1, std430) readonly buffer B { uint v; } buf[];
 *   layout(set = 3, binding = 0, std430) writeonly buffer O { uint o[3]; } outp;
 *   layout(push_constant) uniform P { uint i; } pc;
 *   void main() { uint i = pc.i; outp.o[0] = texelFetch(tex[i], ivec2(0), 0).x;
 *                 outp.o[1] = imageLoad(img[i], ivec2(0)).x; outp.o[2] = buf[i].v; } */
static const uint32_t spv_dl_comp[] = {
    0x07230203,0x00010500,0x000d000a,0x0000003e,0x00000000,0x00020011,
    0x00000001,0x00020011,0x000014b6,0x0006000b,0x00000001,0x4c534c47,
    0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x000a000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000b,
    0x00000015,0x00000019,0x0000002a,0x00000036,0x00060010,0x00000004,
    0x00000011,0x00000001,0x00000001,0x00000001,0x00050048,0x00000009,
    0x00000000,0x00000023,0x00000000,0x00030047,0x00000009,0x00000002,
    0x00040047,0x00000012,0x00000006,0x00000004,0x00040048,0x00000013,
    0x00000000,0x00000019,0x00050048,0x00000013,0x00000000,0x00000023,
    0x00000000,0x00030047,0x00000013,0x00000002,0x00040047,0x00000015,
    0x00000022,0x00000003,0x00040047,0x00000015,0x00000021,0x00000000,
    0x00040047,0x00000019,0x00000022,0x00000000,0x00040047,0x00000019,
    0x00000021,0x00000000,0x00040047,0x0000002a,0x00000022,0x00000001,
    0x00040047,0x0000002a,0x00000021,0x00000000,0x00030047,0x0000002a,
    0x00000018,0x00040048,0x00000033,0x00000000,0x00000018,0x00050048,
    0x00000033,0x00000000,0x00000023,0x00000000,0x00030047,0x00000033,
    0x00000002,0x00040047,0x00000036,0x00000022,0x00000002,0x00040047,
    0x00000036,0x00000021,0x00000001,0x00040047,0x0000003d,0x0000000b,
    0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,
    0x00040015,0x00000006,0x00000020,0x00000000,0x0003001e,0x00000009,
    0x00000006,0x00040020,0x0000000a,0x00000009,0x00000009,0x0004003b,
    0x0000000a,0x0000000b,0x00000009,0x00040015,0x0000000c,0x00000020,
    0x00000001,0x0004002b,0x0000000c,0x0000000d,0x00000000,0x00040020,
    0x0000000e,0x00000009,0x00000006,0x0004002b,0x00000006,0x00000011,
    0x00000003,0x0004001c,0x00000012,0x00000006,0x00000011,0x0003001e,
    0x00000013,0x00000012,0x00040020,0x00000014,0x0000000c,0x00000013,
    0x0004003b,0x00000014,0x00000015,0x0000000c,0x00090019,0x00000016,
    0x00000006,0x00000001,0x00000000,0x00000000,0x00000000,0x00000001,
    0x00000000,0x0003001d,0x00000017,0x00000016,0x00040020,0x00000018,
    0x00000000,0x00000017,0x0004003b,0x00000018,0x00000019,0x00000000,
    0x00040020,0x0000001b,0x00000000,0x00000016,0x00040017,0x0000001e,
    0x0000000c,0x00000002,0x0005002c,0x0000001e,0x0000001f,0x0000000d,
    0x0000000d,0x00040017,0x00000020,0x00000006,0x00000004,0x00040020,
    0x00000024,0x0000000c,0x00000006,0x0004002b,0x0000000c,0x00000026,
    0x00000001,0x00090019,0x00000027,0x00000006,0x00000001,0x00000000,
    0x00000000,0x00000000,0x00000002,0x00000021,0x0003001d,0x00000028,
    0x00000027,0x00040020,0x00000029,0x00000000,0x00000028,0x0004003b,
    0x00000029,0x0000002a,0x00000000,0x00040020,0x0000002c,0x00000000,
    0x00000027,0x0004002b,0x0000000c,0x00000032,0x00000002,0x0003001e,
    0x00000033,0x00000006,0x0003001d,0x00000034,0x00000033,0x00040020,
    0x00000035,0x0000000c,0x00000034,0x0004003b,0x00000035,0x00000036,
    0x0000000c,0x00040017,0x0000003b,0x00000006,0x00000003,0x0004002b,
    0x00000006,0x0000003c,0x00000001,0x0006002c,0x0000003b,0x0000003d,
    0x0000003c,0x0000003c,0x0000003c,0x00050036,0x00000002,0x00000004,
    0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x0000000e,
    0x0000000f,0x0000000b,0x0000000d,0x0004003d,0x00000006,0x00000010,
    0x0000000f,0x00050041,0x0000001b,0x0000001c,0x00000019,0x00000010,
    0x0004003d,0x00000016,0x0000001d,0x0000001c,0x0007005f,0x00000020,
    0x00000021,0x0000001d,0x0000001f,0x00002002,0x0000000d,0x00050051,
    0x00000006,0x00000023,0x00000021,0x00000000,0x00060041,0x00000024,
    0x00000025,0x00000015,0x0000000d,0x0000000d,0x0003003e,0x00000025,
    0x00000023,0x00050041,0x0000002c,0x0000002d,0x0000002a,0x00000010,
    0x0004003d,0x00000027,0x0000002e,0x0000002d,0x00060062,0x00000020,
    0x0000002f,0x0000002e,0x0000001f,0x00002000,0x00050051,0x00000006,
    0x00000030,0x0000002f,0x00000000,0x00060041,0x00000024,0x00000031,
    0x00000015,0x0000000d,0x00000026,0x0003003e,0x00000031,0x00000030,
    0x00060041,0x00000024,0x00000038,0x00000036,0x00000010,0x0000000d,
    0x0004003d,0x00000006,0x00000039,0x00000038,0x00060041,0x00000024,
    0x0000003a,0x00000015,0x0000000d,0x00000032,0x0003003e,0x0000003a,
    0x00000039,0x000100fd,0x00010038
};

static VkDevice dev;
static VkQueue queue;
static VkCommandPool cpool;
static VkPhysicalDeviceMemoryProperties mp;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties2); I(vkGetPhysicalDeviceFeatures2);
I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkDestroyInstance); I(vkDestroyDevice); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers);
I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements);
I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory);
I(vkCreateImageView); I(vkCmdPipelineBarrier); I(vkCmdClearColorImage); I(vkCreateShaderModule); I(vkCreateDescriptorSetLayout);
I(vkGetDescriptorSetLayoutSupport); I(vkCreatePipelineLayout); I(vkCreateComputePipelines); I(vkCreateDescriptorPool);
I(vkAllocateDescriptorSets); I(vkUpdateDescriptorSets); I(vkCreateDescriptorUpdateTemplate); I(vkUpdateDescriptorSetWithTemplate);
I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets); I(vkCmdPushConstants); I(vkCmdDispatch); I(vkDestroyDescriptorPool);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* A host-visible, coherent buffer holding one value (or the dispatch's output). */
static VkBuffer make_buffer(uint32_t value, uint32_t **map)
{
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b;
    if (vkCreateBuffer(dev, &ci, NULL, &b)) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size,
                               mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m;
    uint32_t *p;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &m) || vkBindBufferMemory(dev, b, m, 0) ||
        vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&p))
        return VK_NULL_HANDLE;
    memset(p, 0, 256);
    p[0] = value;
    if (map) *map = p;
    return b;
}

/* A 1x1 R32_UINT image cleared to value, left in GENERAL; its view. */
static VkImageView make_image(VkCommandBuffer cb, uint32_t value)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32_UINT, {1, 1, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ci, NULL, &img)) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, 0)};
    VkDeviceMemory m;
    if (vkAllocateMemory(dev, &ai, NULL, &m) || vkBindImageMemory(dev, img, m, 0)) return VK_NULL_HANDLE;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue c = {.uint32 = {value, 0, 0, 0}};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32_UINT,
                                 {0}, range};
    VkImageView v;
    return vkCreateImageView(dev, &vci, NULL, &v) ? VK_NULL_HANDLE : v;
}

/* One of vkd3d's bindless layouts: the big binding last (after `extra` single storage buffers). */
static VkDescriptorSetLayout make_layout(VkDescriptorType type, int extra, int host, VkDescriptorSetLayoutSupport *support)
{
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL},
                                         {extra, type, COUNT, VK_SHADER_STAGE_ALL, NULL}};
    VkDescriptorBindingFlags f[2] = {0, host ? VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
                                            : VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                                                  VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL,
                                                      (uint32_t)extra + 1, f + 1 - extra};
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi,
                                          host ? 0 : VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, (uint32_t)extra + 1,
                                          b + 1 - extra};
    if (support) vkGetDescriptorSetLayoutSupport(dev, &ci, support);
    VkDescriptorSetLayout l;
    return vkCreateDescriptorSetLayout(dev, &ci, NULL, &l) ? VK_NULL_HANDLE : l;
}

static VkDescriptorPool make_pool(int host)
{
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, COUNT}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, COUNT},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, COUNT + 1}};
    VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                     host ? 0 : VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 3, 3, ps};
    VkDescriptorPool p;
    return vkCreateDescriptorPool(dev, &ci, NULL, &p) ? VK_NULL_HANDLE : p;
}

static int alloc_sets(VkDescriptorPool pool, const VkDescriptorSetLayout *layouts, VkDescriptorSet *sets)
{
    uint32_t counts[3] = {COUNT, COUNT, COUNT};
    VkDescriptorSetVariableDescriptorCountAllocateInfo vi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
                                                             NULL, 3, counts};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vi, pool, 3, layouts};
    return vkAllocateDescriptorSets(dev, &ai, sets) == VK_SUCCESS;
}

/* Writes one descriptor of each kind at elem (n copies of it for the sampled image). */
static void write3(const VkDescriptorSet *sets, uint32_t elem, uint32_t n, VkImageView tex, VkImageView img, VkBuffer buf)
{
    VkDescriptorImageInfo ti[8], si = {VK_NULL_HANDLE, img, VK_IMAGE_LAYOUT_GENERAL};
    for (uint32_t i = 0; i < n && i < 8; i++) ti[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, tex, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[0], 0, elem, n, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ti, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[1], 0, elem, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &si, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[2], 1, elem, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL},
    };
    vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
}

static void copy3(const VkDescriptorSet *src, const VkDescriptorSet *dst, uint32_t elem)
{
    VkCopyDescriptorSet c[3];
    for (int i = 0; i < 3; i++)
        c[i] = (VkCopyDescriptorSet){VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, NULL, src[i], i == 2, elem, dst[i], i == 2, elem, 1};
    vkUpdateDescriptorSets(dev, 0, NULL, 3, c);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    char d[512];
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-desc-limits", 1, "vkbridge", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        check(0, "instance", NULL);
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties2); G(vkGetPhysicalDeviceFeatures2);
    G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr);
    G(vkDestroyInstance);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        check(0, "physical device", "none");
        return 1;
    }

    /* What vkd3d-proton checks (vkd3d_bindless_state_init_legacy), both ways it can be asked. */
    VkPhysicalDeviceDescriptorIndexingProperties dip = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &dip};
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    vkGetPhysicalDeviceProperties2(pd, &props);
    snprintf(d, sizeof(d), "%s: per stage after bind %u/%u/%u, per set %u/%u/%u, per stage %u/%u/%u, resources %u", props.properties.deviceName,
             p12.maxPerStageDescriptorUpdateAfterBindSampledImages, p12.maxPerStageDescriptorUpdateAfterBindStorageImages,
             p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers, p12.maxDescriptorSetUpdateAfterBindSampledImages,
             p12.maxDescriptorSetUpdateAfterBindStorageImages, p12.maxDescriptorSetUpdateAfterBindStorageBuffers,
             props.properties.limits.maxPerStageDescriptorSampledImages, props.properties.limits.maxPerStageDescriptorStorageImages,
             props.properties.limits.maxPerStageDescriptorStorageBuffers, p12.maxPerStageUpdateAfterBindResources);
    check(p12.maxPerStageDescriptorUpdateAfterBindSampledImages >= COUNT && p12.maxPerStageDescriptorUpdateAfterBindStorageImages >= COUNT &&
          p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers >= COUNT && p12.maxDescriptorSetUpdateAfterBindSampledImages >= COUNT &&
          p12.maxDescriptorSetUpdateAfterBindStorageBuffers >= COUNT && p12.maxDescriptorSetUpdateAfterBindStorageImages >= COUNT,
          "limits (Vulkan12Properties)", d);
    check(dip.maxPerStageDescriptorUpdateAfterBindSampledImages == p12.maxPerStageDescriptorUpdateAfterBindSampledImages &&
          dip.maxPerStageDescriptorUpdateAfterBindStorageBuffers == p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers &&
          dip.maxDescriptorSetUpdateAfterBindStorageImages == p12.maxDescriptorSetUpdateAfterBindStorageImages,
          "limits (DescriptorIndexingProperties agree)", NULL);

    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f12.descriptorIndexing || !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound ||
        !f12.descriptorBindingVariableDescriptorCount || !f12.descriptorBindingSampledImageUpdateAfterBind ||
        !f12.descriptorBindingStorageImageUpdateAfterBind || !f12.descriptorBindingStorageBufferUpdateAfterBind ||
        !f12.descriptorBindingUpdateUnusedWhilePending) {
        check(0, "descriptor indexing features", NULL);
        return 1;
    }
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.descriptorIndexing = e12.runtimeDescriptorArray = e12.descriptorBindingPartiallyBound = VK_TRUE;
    e12.descriptorBindingVariableDescriptorCount = e12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    e12.descriptorBindingStorageImageUpdateAfterBind = e12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    e12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    e12.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing;
    e12.shaderStorageImageArrayNonUniformIndexing = f12.shaderStorageImageArrayNonUniformIndexing;
    e12.shaderStorageBufferArrayNonUniformIndexing = f12.shaderStorageBufferArrayNonUniformIndexing;
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
    ef.features.shaderSampledImageArrayDynamicIndexing = ef.features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
    ef.features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t nq = 16, qfam;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        check(0, "device", NULL);
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkDestroyDevice); G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer);
    G(vkEndCommandBuffer); G(vkQueueSubmit); G(vkQueueWaitIdle); G(vkCreateBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory);
    G(vkBindBufferMemory); G(vkMapMemory); G(vkCreateImage); G(vkGetImageMemoryRequirements); G(vkBindImageMemory); G(vkCreateImageView);
    G(vkCmdPipelineBarrier); G(vkCmdClearColorImage); G(vkCreateShaderModule); G(vkCreateDescriptorSetLayout);
    G(vkGetDescriptorSetLayoutSupport); G(vkCreatePipelineLayout); G(vkCreateComputePipelines); G(vkCreateDescriptorPool);
    G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets); G(vkCreateDescriptorUpdateTemplate); G(vkUpdateDescriptorSetWithTemplate);
    G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets); G(vkCmdPushConstants); G(vkCmdDispatch); G(vkDestroyDescriptorPool);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);

    /* Layouts as vkd3d makes them: sampled images, storage images, storage buffers (after one
     * auxiliary storage buffer, as its CBV set has); and host layouts of the same size. */
    VkDescriptorSetVariableDescriptorCountLayoutSupport vs = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT};
    VkDescriptorSetLayoutSupport sup = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT, &vs};
    VkDescriptorSetLayout L[4], H[3];
    L[0] = make_layout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0, 0, &sup);
    snprintf(d, sizeof(d), "supported %u, max variable count %u", sup.supported, vs.maxVariableDescriptorCount);
    check(sup.supported && vs.maxVariableDescriptorCount >= COUNT, "vkGetDescriptorSetLayoutSupport at 1,000,000", d);
    L[1] = make_layout(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0, 0, NULL);
    L[2] = make_layout(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 0, NULL);
    VkDescriptorSetLayoutBinding ob = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo oci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &ob};
    if (vkCreateDescriptorSetLayout(dev, &oci, NULL, &L[3])) L[3] = VK_NULL_HANDLE;
    H[0] = make_layout(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0, 1, NULL);
    H[1] = make_layout(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0, 1, NULL);
    H[2] = make_layout(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 1, NULL);
    check(L[0] && L[1] && L[2] && L[3] && H[0] && H[1] && H[2], "set layouts at 1,000,000 (update after bind and host)", NULL);

    VkDescriptorPool up = make_pool(0), hp = make_pool(1);
    check(up && hp, "descriptor pools of 1,000,000", NULL);
    VkDescriptorSet S[4], HS[3];
    int ok = up && hp && alloc_sets(up, L, S) && alloc_sets(hp, H, HS);
    check(ok, "sets at a variable count of 1,000,000", NULL);
    VkDescriptorPoolSize ops = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo opci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ops};
    VkDescriptorPool opool;
    VkDescriptorSetAllocateInfo oai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, 1, &L[3]};
    ok = ok && !vkCreateDescriptorPool(dev, &opci, NULL, &opool) && (oai.descriptorPool = opool) && !vkAllocateDescriptorSets(dev, &oai, &S[3]);
    if (!ok) {
        check(0, "setup", NULL);
        return 1;
    }

    /* Resources: 0x1xxx sampled, 0x2xxx storage images, 0x3xxx buffers; xxx = the index read. */
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkAllocateCommandBuffers(dev, &cai, &cb);
    vkBeginCommandBuffer(cb, &bi);
    VkImageView t2 = make_image(cb, 0x1002), i2 = make_image(cb, 0x2002), t5 = make_image(cb, 0x1005), i5 = make_image(cb, 0x2005);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    uint32_t *res;
    VkBuffer b2 = make_buffer(0x3002, NULL), b5 = make_buffer(0x3005, NULL), bo = make_buffer(0, &res);
    check(t2 && i2 && t5 && i5 && b2 && b5 && bo, "images and buffers", NULL);

    /* Low indices directly; index 5 through the host sets and a copy, as vkd3d moves descriptors
     * from CPU heaps to shader-visible ones; then writes and copies past Mali's 500,000. */
    write3(S, 2, 1, t2, i2, b2);
    write3(HS, 5, 1, t5, i5, b5);
    copy3(HS, S, 5);
    write3(S, MALI_LIMIT - 2, 5, t2, i2, b2);
    write3(S, MALI_LIMIT, 1, t2, i2, b2);
    write3(S, COUNT - 1, 1, t2, i2, b2);
    write3(HS, COUNT - 1, 1, t5, i5, b5);
    copy3(HS, S, COUNT - 1);
    VkDescriptorBufferInfo ob_info = {bo, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet ow = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, S[3], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &ob_info, NULL};
    vkUpdateDescriptorSets(dev, 1, &ow, 0, NULL);
    check(1, "writes and copies up to index 999,999", NULL);

    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 4, L, 1, &pcr};
    VkPipelineLayout pl;
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_dl_comp), spv_dl_comp};
    VkShaderModule sm;
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    VkPipeline pipe;
    ok = !vkCreatePipelineLayout(dev, &plci, NULL, &pl) && !vkCreateShaderModule(dev, &smci, NULL, &sm);
    cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm,
                                                   "main", NULL};
    cpci.layout = pl;
    ok = ok && !vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
    check(ok, "compute pipeline over the bindless sets", NULL);
    if (!ok) return 1;

    /* Index 7 is written after its command buffer is recorded (update after bind), the buffer
     * through a template that also reaches index 999,999. */
    VkCommandBuffer cbs[3];
    cai.commandBufferCount = 3;
    vkAllocateCommandBuffers(dev, &cai, cbs);
    const uint32_t idx[3] = {2, 5, 7};
    for (int i = 0; i < 3; i++) {
        vkBeginCommandBuffer(cbs[i], &bi);
        vkCmdBindPipeline(cbs[i], VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cbs[i], VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 4, S, 0, NULL);
        vkCmdPushConstants(cbs[i], pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &idx[i]);
        vkCmdDispatch(cbs[i], 1, 1, 1);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cbs[i]);
    }
    write3(S, 7, 1, t2, i2, b2);
    VkDescriptorUpdateTemplateEntry te[2] = {{1, 7, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, sizeof(VkDescriptorBufferInfo)},
                                             {1, COUNT - 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sizeof(VkDescriptorBufferInfo), 0}};
    VkDescriptorUpdateTemplateCreateInfo tci = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, NULL, 0, 2, te,
                                                VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, L[2], 0, VK_NULL_HANDLE, 0};
    VkDescriptorUpdateTemplate tmpl;
    VkDescriptorBufferInfo tdata[2] = {{b5, 0, VK_WHOLE_SIZE}, {b5, 0, VK_WHOLE_SIZE}};
    ok = !vkCreateDescriptorUpdateTemplate(dev, &tci, NULL, &tmpl);
    if (ok) vkUpdateDescriptorSetWithTemplate(dev, S[2], tmpl, tdata);
    check(ok, "update template reaching index 999,999", NULL);

    const uint32_t want[3][3] = {{0x1002, 0x2002, 0x3002}, {0x1005, 0x2005, 0x3005}, {0x1002, 0x2002, 0x3005}};
    const char *what[3] = {"dispatch reads index 2 (written)", "dispatch reads index 5 (copied from host sets)",
                           "dispatch reads index 7 (written after bind, template)"};
    for (int i = 0; i < 3; i++) {
        res[0] = res[1] = res[2] = 0xdeadbeef;
        si.pCommandBuffers = &cbs[i];
        VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = vkQueueWaitIdle(queue);
        snprintf(d, sizeof(d), "%d, got %x %x %x", r, res[0], res[1], res[2]);
        check(r == VK_SUCCESS && !memcmp(res, want[i], sizeof(want[i])), what[i], d);
    }

    vkDestroyDescriptorPool(dev, up, NULL);
    vkDestroyDescriptorPool(dev, hp, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
