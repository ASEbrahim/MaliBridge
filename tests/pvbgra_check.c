/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pvbgra_check: PV-07 (PowerVR: storage writes to a B8G8R8A8 image land in R8G8B8A8 byte order).
 * Does what gamescope 3.16 does with its output image on DroidDeck's Wayland backend
 * (vulkan_make_output_images + vulkan_composite): a B8G8R8A8_UNORM image, MUTABLE_FORMAT with the
 * view list {B8G8R8A8_UNORM, B8G8R8A8_SRGB}, usage STORAGE | SAMPLED | TRANSFER_SRC (0xd), dma-buf
 * exportable (DRM-modifier tiling with the LINEAR modifier where the driver has modifiers), written by
 * a compute shader that declares the storage image rgba8, with Steam's blue #1A9FFF = (26,159,255).
 * Then it reads the picture every way it leaves that image:
 *   sampled  a second compute pass samples texel (1,1) through the B8G8R8A8_UNORM view (gamescope's
 *            RCAS / pipewire passes): R,G,B as 0-255
 *   raw      vkCmdCopyImageToBuffer of texel (1,1): the bytes in memory, what a dma-buf consumer reads
 *   copy     vkCmdCopyImage into a plain B8G8R8A8_UNORM image, then its texel (1,1) bytes
 *   dmabuf   vkGetMemoryFdKHR(DMA_BUF) of the image's memory: st_dev:st_ino of the fd (the key the
 *            server registers for the compositor), or "none"
 * Prints one line: "RESULT sampled=R,G,B raw=b0b1b2b3 copy=b0b1b2b3 dmabuf=DEV:INO tiling=..." (bytes in
 * memory order). host/pvbgra_test.sh judges it against the server's log.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint32_t write_spv[] = {
#include "pvbgra_write_comp.inc"
};
static const uint32_t sample_spv[] = {
#include "pvbgra_sample_comp.inc"
};

#define FAIL(...) (printf("FAIL: " __VA_ARGS__), printf("\n"), 1)
#define W 8

static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
#define D(n) static PFN_##n n
D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory); D(vkCreateImageView);
D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateShaderModule);
D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout); D(vkCreateComputePipelines); D(vkCreateDescriptorPool);
D(vkAllocateDescriptorSets); D(vkUpdateDescriptorSets); D(vkCreateCommandPool); D(vkAllocateCommandBuffers);
D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets); D(vkCmdDispatch);
D(vkCmdPipelineBarrier); D(vkCmdCopyImageToBuffer); D(vkCmdCopyImage); D(vkQueueSubmit); D(vkQueueWaitIdle);
D(vkGetDeviceQueue); D(vkCreateSampler); D(vkGetMemoryFdKHR); D(vkDestroyDevice);

static VkPhysicalDeviceMemoryProperties mp;
static int no_export; /* the exportable allocation was refused: dmabuf=none */

static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

static VkDeviceMemory bind_image(VkImage img, int exportable)
{
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, VK_NULL_HANDLE};
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, exportable ? (void *)&ex : NULL, mr.size,
                               mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (exportable && vkAllocateMemory(dev, &ai, NULL, &m) != VK_SUCCESS) {
        /* a driver that makes the image but exports no memory for it (lavapipe through the bridge): kept without export */
        m = VK_NULL_HANDLE;
        no_export = 1;
        ai.pNext = &ded;
    }
    if ((!m && vkAllocateMemory(dev, &ai, NULL, &m)) || vkBindImageMemory(dev, img, m, 0)) return VK_NULL_HANDLE;
    return m;
}

static VkBuffer hostbuf(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b;
    if (vkCreateBuffer(dev, &bi, NULL, &b)) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m;
    if (vkAllocateMemory(dev, &ai, NULL, &m) || vkBindBufferMemory(dev, b, m, 0) || vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map)) return VK_NULL_HANDLE;
    memset(*map, 0, (size_t)size);
    return b;
}

static VkPipeline mkpipe(const uint32_t *code, size_t size, VkPipelineLayout pl)
{
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule sm;
    if (vkCreateShaderModule(dev, &mi, NULL, &sm)) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL},
                                      pl, VK_NULL_HANDLE, -1};
    VkPipeline p;
    return vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) ? VK_NULL_HANDLE : p;
}

static void barrier(VkCommandBuffer cb, VkImage i, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                              i, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return FAIL("no libvulkan.so.1");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pvbgra_check", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) return FAIL("instance");
    GI(vkEnumeratePhysicalDevices);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkCreateDevice);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return FAIL("no physical device");
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    const char *want[4] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier",
                           "VK_KHR_image_format_list"};
    int have[4] = {0};
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    for (uint32_t i = 0; i < ne; i++)
        for (int k = 0; k < 4; k++) have[k] |= !strcmp(ep[i].extensionName, want[k]);
    free(ep);
    const char *exts[4];
    uint32_t nx = 0;
    int dmabuf = have[0] && have[1], mods = dmabuf && have[2];
    if (dmabuf) exts[nx++] = want[0], exts[nx++] = want[1];
    if (mods) exts[nx++] = want[2];
    if (mods && have[3]) exts[nx++] = want[3];
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, nx, exts};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return FAIL("device");
    GI(vkGetDeviceProcAddr);
    gdpa = vkGetDeviceProcAddr;
#define GD(f) f = (PFN_##f)gdpa(dev, #f)
    GD(vkCreateImage); GD(vkGetImageMemoryRequirements); GD(vkAllocateMemory); GD(vkBindImageMemory); GD(vkCreateImageView);
    GD(vkCreateBuffer); GD(vkGetBufferMemoryRequirements); GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkCreateShaderModule);
    GD(vkCreateDescriptorSetLayout); GD(vkCreatePipelineLayout); GD(vkCreateComputePipelines); GD(vkCreateDescriptorPool);
    GD(vkAllocateDescriptorSets); GD(vkUpdateDescriptorSets); GD(vkCreateCommandPool); GD(vkAllocateCommandBuffers);
    GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer); GD(vkCmdBindPipeline); GD(vkCmdBindDescriptorSets); GD(vkCmdDispatch);
    GD(vkCmdPipelineBarrier); GD(vkCmdCopyImageToBuffer); GD(vkCmdCopyImage); GD(vkQueueSubmit); GD(vkQueueWaitIdle);
    GD(vkGetDeviceQueue); GD(vkCreateSampler); GD(vkGetMemoryFdKHR); GD(vkDestroyDevice);

    /* gamescope's output image (CVulkanTexture::BInit with bStorage, bFlippable, bTransferSrc, bSampled) */
    const VkFormat list[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
    const uint64_t linear_mod = 0;
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, list};
    VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, &fl, 1, &linear_mod};
    VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, mods ? (void *)&ml : (void *)&fl,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, dmabuf ? (void *)&emi : (void *)&fl, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {W, W, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            mods ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage g;
    VkResult r = vkCreateImage(dev, &ci, NULL, &g);
    if (r) return FAIL("gamescope's image not made (%d)", r);
    VkDeviceMemory gm = bind_image(g, dmabuf);
    if (!gm) return FAIL("gamescope's image memory");
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {0, 0, 0, 0},
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView gv, gsrgb;
    if (vkCreateImageView(dev, &vi, NULL, &gv)) return FAIL("view");
    vi.format = VK_FORMAT_B8G8R8A8_SRGB;
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_SAMPLED_BIT};
    vi.pNext = &vu;
    if (vkCreateImageView(dev, &vi, NULL, &gsrgb)) return FAIL("sRGB view"); /* made as gamescope makes it, not used */
    /* a plain BGRA image to copy into (as CVulkanCmdBuffer::copyImage would) */
    VkImageCreateInfo pci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {W, W, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage p;
    if (vkCreateImage(dev, &pci, NULL, &p) || !bind_image(p, 0)) return FAIL("plain image");

    void *raw = NULL, *cpy = NULL, *smp = NULL;
    VkBuffer rawb = hostbuf(W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &raw), cpyb = hostbuf(W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &cpy);
    VkBuffer smpb = hostbuf(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &smp);
    if (!rawb || !cpyb || !smpb) return FAIL("buffers");

    VkDescriptorSetLayoutBinding wb = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutBinding sb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo wl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &wb};
    VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, sb};
    VkDescriptorSetLayout wdl, sdl;
    if (vkCreateDescriptorSetLayout(dev, &wl, NULL, &wdl) || vkCreateDescriptorSetLayout(dev, &sl, NULL, &sdl)) return FAIL("set layouts");
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &wdl};
    VkPipelineLayout wpl, spl;
    if (vkCreatePipelineLayout(dev, &pli, NULL, &wpl)) return FAIL("layout");
    pli.pSetLayouts = &sdl;
    if (vkCreatePipelineLayout(dev, &pli, NULL, &spl)) return FAIL("layout");
    VkPipeline wp = mkpipe(write_spv, sizeof(write_spv), wpl), sp = mkpipe(sample_spv, sizeof(sample_spv), spl);
    if (!wp || !sp) return FAIL("pipelines");
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 3, ps};
    VkDescriptorPool pool;
    if (vkCreateDescriptorPool(dev, &dpi, NULL, &pool)) return FAIL("pool");
    VkDescriptorSetLayout both[2] = {wdl, sdl};
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 2, both};
    VkDescriptorSet sets[2];
    if (vkAllocateDescriptorSets(dev, &dai, sets)) return FAIL("sets");
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler smpl;
    if (vkCreateSampler(dev, &sci, NULL, &smpl)) return FAIL("sampler");
    VkDescriptorImageInfo wi = {VK_NULL_HANDLE, gv, VK_IMAGE_LAYOUT_GENERAL}, si = {smpl, gv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {smpb, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[0], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &wi, NULL, NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[1], 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &si, NULL, NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[1], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL}};
    vkUpdateDescriptorSets(dev, 3, w, 0, NULL);

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    if (vkCreateCommandPool(dev, &cpi, NULL, &cp)) return FAIL("command pool");
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    if (vkAllocateCommandBuffers(dev, &cai, &cb)) return FAIL("command buffer");
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &cbi);
    const VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, TR = VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(cb, g, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, CS);
    barrier(cb, p, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, TR);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, wp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, wpl, 0, 1, &sets[0], 0, NULL);
    vkCmdDispatch(cb, W / 4, W / 4, 1);
    barrier(cb, g, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            CS, CS | TR);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, sp);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, spl, 0, 1, &sets[1], 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
    vkCmdCopyImageToBuffer(cb, g, VK_IMAGE_LAYOUT_GENERAL, rawb, 1, &bic);
    VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
    vkCmdCopyImage(cb, g, VK_IMAGE_LAYOUT_GENERAL, p, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    barrier(cb, p, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, TR, TR);
    vkCmdCopyImageToBuffer(cb, p, VK_IMAGE_LAYOUT_GENERAL, cpyb, 1, &bic);
    vkEndCommandBuffer(cb);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    if (vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) return FAIL("submit");

    const uint32_t *s = smp;
    const unsigned char *rb = (const unsigned char *)raw + (W + 1) * 4, *cbb = (const unsigned char *)cpy + (W + 1) * 4;
    char key[128] = "none";
    if (dmabuf && !no_export && vkGetMemoryFdKHR) {
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, gm, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1;
        struct stat st;
        if (vkGetMemoryFdKHR(dev, &gi, &fd) == VK_SUCCESS && fd >= 0 && fstat(fd, &st) == 0) {
            /* the name the server gave the dma-buf, as a compositor in another process sees it */
            char p[40], l[64] = "";
            snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
            ssize_t ln = readlink(p, l, sizeof(l) - 1);
            l[ln > 0 ? ln : 0] = 0;
            const char *nm = !strncmp(l, "/dmabuf:", 8) && l[8] ? l + 8 : "-";
            snprintf(key, sizeof(key), "%llu:%llu name=%s", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino, nm);
        }
        if (fd >= 0) close(fd);
    }
    printf("RESULT sampled=%u,%u,%u raw=%02x%02x%02x%02x copy=%02x%02x%02x%02x dmabuf=%s tiling=%s\n", s[0], s[1], s[2], rb[0], rb[1], rb[2], rb[3],
           cbb[0], cbb[1], cbb[2], cbb[3], key, mods ? "drm-linear" : "optimal");
    vkDestroyDevice(dev, NULL);
    return 0;
}
