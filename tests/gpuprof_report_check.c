/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * gpuprof_report_check: frames recorded the way vkd3d-proton records them, for host/gpuprof_report_test.sh (the pass profiler,
 * server/server_gpuprof.c, VKBRIDGE_GPU_PROF). Each frame: a command buffer begun one-time-submit, a dynamic rendering
 * with a draw and a vkCmdDrawIndirectCount, a vkCmdDispatchBase, ended, submitted, waited. Argument 1 is how the
 * command buffers are handled, argument 2 the seconds to run (at ~200 frames a second):
 *   reuse    one command buffer begun again each frame (DXVK's way: the profiler's original case)
 *   fresh    a new command buffer every frame, none freed or begun twice (so none ever reaches a second begin)
 *   freed    a new command buffer every frame, freed after its wait
 *   inherit  one command buffer begun again each frame with a non-NULL pInheritanceInfo, as some apps pass for a primary
 *   idle     the device and nothing recorded (the profiler's heartbeat)
 * Prints "OK: <n> frames" last, or "FAIL: ...".
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 64
#define H 64

static const uint32_t spv_vert[] = {
#include "polygon_vert.inc"
};
static const uint32_t spv_frag[] = {
#include "polygon_frag.inc"
};
/* void main() {} with local_size 1,1,1 */
static const uint32_t spv_comp[] = {0x07230203, 0x00010000, 0, 6,          0,          0x00020011, 1,          0x0003000e, 0,          1,
                                    0x0005000f, 5,          4, 0x6e69616d, 0,          0x00060010, 4,          17,         1,          1,
                                    1,          0x00020013, 2, 0x00030021, 3,          2,          0x00050036, 2,          4,          0,
                                    3,          0x000200f8, 5, 0x000100fd, 0x00010038};

static PFN_vkGetInstanceProcAddr gipa;
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceFeatures2);
I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkFreeCommandBuffers); I(vkBeginCommandBuffer);
I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateImageView);
I(vkCreateShaderModule); I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkCreateComputePipelines); I(vkCmdBindPipeline);
I(vkCmdBindVertexBuffers); I(vkCmdDraw); I(vkCmdDrawIndirectCount); I(vkCmdDispatchBase); I(vkCmdPipelineBarrier);
I(vkCmdBeginRendering); I(vkCmdEndRendering); I(vkResetCommandBuffer);
#undef I

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bci, NULL, &b)) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b, &r);
    uint32_t t = find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    VkDeviceMemory m;
    if (t == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &m) || vkBindBufferMemory(dev, b, m, 0) || vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map))
        return VK_NULL_HANDLE;
    return b;
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "reuse";
    int secs = argc > 2 ? atoi(argv[2]) : 7;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL: no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gpuprof_report_check", 1, "vkd3d", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL: vkCreateInstance\n"); return 1; }
    VkPhysicalDevice pd;
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceFeatures2); L(vkGetPhysicalDeviceQueueFamilyProperties);
    L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL: no GPU\n"); return 1; }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering || !f12.drawIndirectCount) { printf("FAIL: needs dynamicRendering and drawIndirectCount\n"); return 1; }
    uint32_t nq = 8, qfam = 0;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    while (qfam < nq && !((qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT))) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, NULL};
    e13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
    e12.drawIndirectCount = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e12, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("FAIL: vkCreateDevice\n"); return 1; }
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkFreeCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory);
    D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkBindImageMemory); D(vkCreateImageView);
    D(vkCreateShaderModule); D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkCreateComputePipelines); D(vkCmdBindPipeline);
    D(vkCmdBindVertexBuffers); D(vkCmdDraw); D(vkCmdDrawIndirectCount); D(vkCmdDispatchBase); D(vkCmdPipelineBarrier);
    D(vkCmdBeginRendering); D(vkCmdEndRendering); D(vkResetCommandBuffer);
#undef D
    VkQueue queue;
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    if (!strcmp(mode, "idle")) {
        sleep((unsigned)secs);
        printf("OK: idle %d s\n", secs);
        return 0;
    }

    /* target, buffers, pipelines */
    VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {W, H, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkImageView view;
    vkCreateImage(dev, &ic, NULL, &img);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    uint32_t mt = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = find_type(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VkDeviceMemory mem;
    vkAllocateMemory(dev, &mai, NULL, &mem);
    vkBindImageMemory(dev, img, mem, 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(dev, &vci, NULL, &view);
    float *vp;
    uint32_t *ip;
    VkBuffer vbuf = host_buffer(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&vp);
    VkBuffer ibuf = host_buffer(1024, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, (void **)&ip);
    uint32_t *cp;
    VkBuffer cbuf = host_buffer(64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, (void **)&cp);
    if (!vbuf || !ibuf || !cbuf) { printf("FAIL: buffers\n"); return 1; }
    static const float tri[6] = {-1, -1, 1, -1, -1, 1};
    memcpy(vp, tri, sizeof(tri));
    for (int i = 0; i < 8; i++) { ip[i * 4] = 3; ip[i * 4 + 1] = 1; ip[i * 4 + 2] = 0; ip[i * 4 + 3] = 0; }
    cp[0] = 4;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    vkCreatePipelineLayout(dev, &plci, NULL, &layout);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, shader(spv_vert, sizeof(spv_vert)), "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader(spv_frag, sizeof(spv_frag)), "main", NULL}};
    VkVertexInputBindingDescription bd = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription ad = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &bd, 1, &ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vpt = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vpt, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &cba};
    VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &cfmt, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms, &ds, &cbs,
                                        NULL, layout, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline gp, cpipe;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &gp)) { printf("FAIL: graphics pipeline\n"); return 1; }
    VkComputePipelineCreateInfo cci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                       {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                        shader(spv_comp, sizeof(spv_comp)), "main", NULL},
                                       layout, VK_NULL_HANDLE, -1};
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cci, NULL, &cpipe)) { printf("FAIL: compute pipeline\n"); return 1; }

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    int fresh = !strcmp(mode, "fresh"), freed = !strcmp(mode, "freed"), inherit = !strcmp(mode, "inherit");
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int frames = 0;
    for (;;) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (t1.tv_sec - t0.tv_sec >= secs) break;
        if (fresh || freed || !cb) vkAllocateCommandBuffers(dev, &cai, &cb);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                       inherit ? &inh : NULL};
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                        VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {W, H}}, 1, 0, 1, &ca, NULL, NULL};
        VkDeviceSize off = 0;
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
        vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdDrawIndirectCount(cb, ibuf, 0, cbuf, 0, 8, 16);
        vkCmdEndRendering(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
        vkCmdDispatchBase(cb, 0, 0, 0, 2, 1, 1);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        if (freed) { vkFreeCommandBuffers(dev, pool, 1, &cb); cb = VK_NULL_HANDLE; }
        frames++;
        usleep(5000);
    }
    printf("OK: %d frames (%s)\n", frames, mode);
    return 0;
}
