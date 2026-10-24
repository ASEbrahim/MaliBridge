/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dynrend_check: VK_KHR_dynamic_rendering, as Zink uses it (Mesa 25 and later require it). On a
 * driver without it (Mali's Vulkan 1.1 drivers) the bridge records each rendering as a render pass
 * (server_emu_dynrend.c); host/dynrend_test.sh runs this with that emulation on the desktop driver
 * (VKBRIDGE_FAKE_MISSING=dynrend, the device at Vulkan 1.2) and without it. A 64x64 RGBA8 target,
 * a triangle over its upper-left half (white), read back:
 *   - clear + draw: the clear colour outside, the triangle inside;
 *   - load: a second rendering with LOAD keeps both, a vkCmdClearAttachments rect inside it lands;
 *   - depth: a D16 attachment cleared to 0 hides the triangle (LESS at z 0), cleared to 1 shows it;
 *   - multisampled: 4x colour resolved (AVERAGE) into the target;
 *   - secondary: the draw recorded in a secondary command buffer begun with
 *     VkCommandBufferInheritanceRenderingInfo, executed inside a rendering with
 *     VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
 *   - hole: two colour attachments, the first without a view (VK_FORMAT_UNDEFINED in the pipeline),
 *     the second cleared and written to by vkCmdClearAttachments at index 1;
 *   - vkCmdBindVertexBuffers2 with sizes, the one Zink 26.2 binds with (where the device has it).
 * Prints PASS/FAIL per check and "OK: n of n checks passed" / "FAIL: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* glslc -O --target-env=vulkan1.1 of:
 *   layout(location = 0) in vec2 pos;
 *   void main() { gl_Position = vec4(pos.x + float(gl_InstanceIndex), pos.y, 0.0, 1.0); gl_PointSize = 1.0; } */
static const uint32_t spv_vert[] = {
#include "polygon_vert.inc"
};
/* glslc -O --target-env=vulkan1.1 of: layout(location = 0) out vec4 c; void main() { c = vec4(1.0); } */
static const uint32_t spv_frag[] = {
#include "polygon_frag.inc"
};

#define W 64
#define H 64
#define WHITE 0xffffffffu
#define RED 0xff0000ffu   /* as bytes R,G,B,A read big-endian */
#define GREEN 0x00ff00ffu
#define BLUE 0x0000ffffu
#define YELLOW 0xffff00ffu
#define CYAN 0x00ffffffu

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static int failures, checks;
static VkPhysicalDeviceMemoryProperties mp;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkEnumerateDeviceExtensionProperties);
I(vkGetPhysicalDeviceFeatures2); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice);
I(vkGetDeviceProcAddr); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer);
I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateImageView);
I(vkCreateShaderModule); I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkCmdBindPipeline); I(vkCmdBindVertexBuffers);
I(vkCmdDraw); I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier); I(vkCmdExecuteCommands); I(vkCmdClearAttachments);
I(vkCmdBeginRendering); I(vkCmdEndRendering); I(vkCmdBindVertexBuffers2); I(vkResetCommandBuffer); I(vkDestroyDevice);
I(vkDestroyInstance);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

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
    if (t == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &m) || vkBindBufferMemory(dev, b, m, 0) ||
        vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map))
        return VK_NULL_HANDLE;
    return b;
}

typedef struct {
    VkImage img;
    VkImageView view;
} target;

static int make_target(VkFormat fmt, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect, target *o)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, 1, samples,
                            VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(dev, &ci, NULL, &o->img)) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, o->img, &mr);
    uint32_t t = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_type(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    VkDeviceMemory m;
    if (vkAllocateMemory(dev, &ai, NULL, &m) || vkBindImageMemory(dev, o->img, m, 0)) return 0;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, o->img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0}, {aspect, 0, 1, 0, 1}};
    return vkCreateImageView(dev, &vci, NULL, &o->view) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkCommandPool pool;
static VkCommandBuffer cb, sec;
static VkBuffer rbuf, vbuf;
static uint8_t *pixels;

static void begin(void)
{
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
}

/* The target (in COLOR_ATTACHMENT_OPTIMAL) read back, left in COLOR_ATTACHMENT_OPTIMAL. */
static void finish_and_read(VkImage img)
{
    barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r);
    barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    memset(pixels, 0x5a, W * H * 4);
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
}

static uint32_t px(int x, int y)
{
    const uint8_t *p = pixels + (y * W + x) * 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Each (x, y, colour) of the list; detail names the first wrong one. */
static void expect(const char *what, int n, const int (*pts)[2], const uint32_t *want)
{
    char d[160] = "";
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        uint32_t got = px(pts[i][0], pts[i][1]);
        if (got != want[i]) {
            ok = 0;
            snprintf(d, sizeof(d), "(%d,%d) is %08x, want %08x", pts[i][0], pts[i][1], got, want[i]);
        }
    }
    check(ok, what, d);
}

static VkRenderingAttachmentInfo color_att(VkImageView v, VkAttachmentLoadOp load, VkAttachmentStoreOp store, uint32_t rgba)
{
    VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, v, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, load, store};
    a.clearValue.color.float32[0] = (float)(rgba >> 24 & 0xff) / 255.0f;
    a.clearValue.color.float32[1] = (float)(rgba >> 16 & 0xff) / 255.0f;
    a.clearValue.color.float32[2] = (float)(rgba >> 8 & 0xff) / 255.0f;
    a.clearValue.color.float32[3] = (float)(rgba & 0xff) / 255.0f;
    return a;
}

static VkRenderingInfo rendering(uint32_t ncolor, const VkRenderingAttachmentInfo *colors, const VkRenderingAttachmentInfo *depth,
                                 VkRenderingFlags flags)
{
    return (VkRenderingInfo){VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, flags, {{0, 0}, {W, H}}, 1, 0, ncolor, colors, depth, NULL};
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

static VkPipelineLayout layout;
static VkPipelineShaderStageCreateInfo stages[2];

static VkPipeline pipeline(uint32_t ncolor, const VkFormat *colors, VkFormat depth, VkSampleCountFlagBits samples, uint32_t write_first)
{
    VkVertexInputBindingDescription bd = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription ad = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &bd, 1, &ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, samples};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth != VK_FORMAT_UNDEFINED;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba[2] = {{0}, {0}};
    for (uint32_t i = 0; i < ncolor; i++) cba[i].colorWriteMask = 0xf;
    if (!write_first) cba[0].colorWriteMask = 0;
    if (ncolor > 1) cba[1].colorWriteMask = 0; /* the hole case writes nothing to its second attachment */
    VkPipelineColorBlendStateCreateInfo cb_ = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY,
                                               ncolor, cba};
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, ncolor, colors, depth, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms,
                                       &ds, &cb_, NULL, layout, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (r) printf("vkCreateGraphicsPipelines: %d\n", r);
    return p;
}

static void draw(VkCommandBuffer c, VkPipeline p)
{
    VkDeviceSize off = 0;
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindVertexBuffers(c, 0, 1, &vbuf, &off);
    vkCmdDraw(c, 3, 1, 0, 0);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL: no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dynrend_check", 1, "zink", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL: vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkEnumerateDeviceExtensionProperties); L(vkGetPhysicalDeviceFeatures2);
    L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
    L(vkDestroyInstance);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL: no GPU\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    int core13 = props.apiVersion >= VK_API_VERSION_1_3, core12 = props.apiVersion >= VK_API_VERSION_1_2;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne + 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    int has_dr = 0, has_rp2 = 0, has_dsr = 0, has_eds = 0;
    for (uint32_t i = 0; i < ne; i++) {
        has_dr |= !strcmp(ep[i].extensionName, "VK_KHR_dynamic_rendering");
        has_rp2 |= !strcmp(ep[i].extensionName, "VK_KHR_create_renderpass2");
        has_dsr |= !strcmp(ep[i].extensionName, "VK_KHR_depth_stencil_resolve");
        has_eds |= !strcmp(ep[i].extensionName, "VK_EXT_extended_dynamic_state");
    }
    free(ep);
    printf("GPU: %s, Vulkan %u.%u, VK_KHR_dynamic_rendering %s\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), has_dr ? "listed" : "not listed");
    VkPhysicalDeviceDynamicRenderingFeatures drf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &drf};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    char msg[200];
    snprintf(msg, sizeof(msg), "extension %s, feature %u", has_dr ? "listed" : "missing", drf.dynamicRendering);
    check((core13 || has_dr) && drf.dynamicRendering, "dynamic rendering offered", msg);
    if (!drf.dynamicRendering) goto out;

    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    const char *exts[4];
    uint32_t nx = 0;
    if (!core13) {
        exts[nx++] = "VK_KHR_dynamic_rendering";
        if (!core12 && has_rp2) exts[nx++] = "VK_KHR_create_renderpass2";
        if (!core12 && has_dsr) exts[nx++] = "VK_KHR_depth_stencil_resolve";
    }
    VkPhysicalDeviceDynamicRenderingFeatures en = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, NULL, VK_TRUE};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en, 0, 1, &qci, 0, NULL, nx, exts, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    check(r == VK_SUCCESS, "vkCreateDevice with dynamicRendering", msg);
    if (r) goto out;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory);
    D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkBindImageMemory);
    D(vkCreateImageView); D(vkCreateShaderModule); D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkCmdBindPipeline);
    D(vkCmdBindVertexBuffers); D(vkCmdDraw); D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier); D(vkCmdExecuteCommands);
    D(vkCmdClearAttachments); D(vkResetCommandBuffer); D(vkDestroyDevice);
#undef D
    if (core13) {
        vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRendering");
        vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRendering");
    } else {
        vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR");
        vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRenderingKHR");
    }
    /* Zink's: the core name, then the EXT one (Mesa's dispatch loader) */
    vkCmdBindVertexBuffers2 = (PFN_vkCmdBindVertexBuffers2)vkGetDeviceProcAddr(dev, "vkCmdBindVertexBuffers2");
    if (!vkCmdBindVertexBuffers2 && has_eds) vkCmdBindVertexBuffers2 = (PFN_vkCmdBindVertexBuffers2)vkGetDeviceProcAddr(dev, "vkCmdBindVertexBuffers2EXT");
    check(vkCmdBeginRendering && vkCmdEndRendering, "vkCmdBeginRendering / vkCmdEndRendering resolve", NULL);
    if (!vkCmdBeginRendering || !vkCmdEndRendering) goto out;
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    target t, ms, dep;
    if (!make_target(fmt, VK_SAMPLE_COUNT_1_BIT, cu, VK_IMAGE_ASPECT_COLOR_BIT, &t) ||
        !make_target(fmt, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &ms) ||
        !make_target(VK_FORMAT_D16_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &dep)) {
        check(0, "setup", "images");
        goto out;
    }
    float *vb;
    vbuf = host_buffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&vb);
    rbuf = host_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&pixels);
    if (!vbuf || !rbuf) { check(0, "setup", "buffers"); goto out; }
    const float tri[] = {-1, -1, 1, -1, -1, 1}; /* the upper-left half (x + y < 64) */
    memcpy(vb, tri, sizeof(tri));
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    vkCreatePipelineLayout(dev, &plci, NULL, &layout);
    stages[0] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,
                                                  shader(spv_vert, sizeof(spv_vert)), "main", NULL};
    stages[1] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                  shader(spv_frag, sizeof(spv_frag)), "main", NULL};
    VkPipeline p_plain = pipeline(1, &fmt, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 1);
    VkPipeline p_depth = pipeline(1, &fmt, VK_FORMAT_D16_UNORM, VK_SAMPLE_COUNT_1_BIT, 1);
    VkPipeline p_ms = pipeline(1, &fmt, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_4_BIT, 1);
    const VkFormat hole_fmts[2] = {VK_FORMAT_UNDEFINED, fmt};
    VkPipeline p_hole = pipeline(2, hole_fmts, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0);
    check(p_plain && p_depth && p_ms && p_hole, "pipelines with VkPipelineRenderingCreateInfo", NULL);
    if (!p_plain || !p_depth || !p_ms || !p_hole) goto out;

    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo pai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBufferAllocateInfo sai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1};
    if (vkAllocateCommandBuffers(dev, &pai, &cb) || vkAllocateCommandBuffers(dev, &sai, &sec)) { check(0, "setup", "command buffers"); goto out; }

    static const int pts2[][2] = {{8, 8}, {56, 56}};
    /* 1: clear + draw */
    begin();
    barrier(cb, t.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo a = color_att(t.view, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED);
    VkRenderingInfo ri = rendering(1, &a, NULL, 0);
    vkCmdBeginRendering(cb, &ri);
    draw(cb, p_plain);
    vkCmdEndRendering(cb);
    finish_and_read(t.img);
    expect("clear + draw", 2, pts2, (const uint32_t[]){WHITE, RED});

    /* 2: load, and a clear rect inside the rendering */
    begin();
    a = color_att(t.view, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, 0);
    ri = rendering(1, &a, NULL, 0);
    vkCmdBeginRendering(cb, &ri);
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0, 1, 0, 1}}}};
    VkClearRect cr = {{{48, 0}, {16, 16}}, 0, 1};
    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    vkCmdEndRendering(cb);
    finish_and_read(t.img);
    static const int pts3[][2] = {{8, 8}, {56, 56}, {56, 8}};
    expect("load keeps, vkCmdClearAttachments lands", 3, pts3, (const uint32_t[]){WHITE, RED, GREEN});

    /* 3: depth cleared to 0 (the triangle at z 0 fails LESS), then to 1 (passes) */
    for (int pass = 0; pass < 2; pass++) {
        begin();
        if (!pass) barrier(cb, dep.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        a = color_att(t.view, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, BLUE);
        VkRenderingAttachmentInfo d = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, dep.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                       VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                       VK_ATTACHMENT_STORE_OP_DONT_CARE};
        d.clearValue.depthStencil.depth = pass ? 1.0f : 0.0f;
        ri = rendering(1, &a, &d, 0);
        vkCmdBeginRendering(cb, &ri);
        draw(cb, p_depth);
        vkCmdEndRendering(cb);
        finish_and_read(t.img);
        expect(pass ? "depth cleared to 1: the triangle drawn" : "depth cleared to 0: the triangle hidden", 2, pts2,
               (const uint32_t[]){pass ? WHITE : BLUE, BLUE});
    }

    /* 4: 4x colour, resolved into the target */
    begin();
    barrier(cb, ms.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    a = color_att(ms.view, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, GREEN);
    a.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    a.resolveImageView = t.view;
    a.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ri = rendering(1, &a, NULL, 0);
    vkCmdBeginRendering(cb, &ri);
    draw(cb, p_ms);
    vkCmdEndRendering(cb);
    finish_and_read(t.img);
    expect("4x resolved", 2, pts2, (const uint32_t[]){WHITE, GREEN});

    /* 5: the draw in a secondary command buffer */
    VkCommandBufferInheritanceRenderingInfo iri = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, NULL, 0, 0, 1, &fmt,
                                                   VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT};
    VkCommandBufferInheritanceInfo ii = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &iri};
    VkCommandBufferBeginInfo sbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &ii};
    vkBeginCommandBuffer(sec, &sbi);
    draw(sec, p_plain);
    vkEndCommandBuffer(sec);
    begin();
    a = color_att(t.view, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, BLUE);
    ri = rendering(1, &a, NULL, VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    vkCmdBeginRendering(cb, &ri);
    vkCmdExecuteCommands(cb, 1, &sec);
    vkCmdEndRendering(cb);
    finish_and_read(t.img);
    expect("secondary command buffer", 2, pts2, (const uint32_t[]){WHITE, BLUE});

    /* 6: a hole at colour attachment 0; attachment 1 cleared, then a clear rect at index 1 */
    begin();
    VkRenderingAttachmentInfo two[2] = {color_att(VK_NULL_HANDLE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, 0),
                                        color_att(t.view, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, YELLOW)};
    ri = rendering(2, two, NULL, 0);
    vkCmdBeginRendering(cb, &ri);
    draw(cb, p_hole);
    VkClearAttachment ca1 = {VK_IMAGE_ASPECT_COLOR_BIT, 1, {.color = {.float32 = {0, 1, 1, 1}}}};
    VkClearRect cr1 = {{{0, 48}, {16, 16}}, 0, 1};
    vkCmdClearAttachments(cb, 1, &ca1, 1, &cr1);
    vkCmdEndRendering(cb);
    finish_and_read(t.img);
    static const int pts4[][2] = {{8, 8}, {56, 56}, {8, 56}};
    expect("a hole at colour attachment 0, index 1 cleared", 3, pts4, (const uint32_t[]){YELLOW, YELLOW, CYAN});

    /* 7: vkCmdBindVertexBuffers2 with sizes (Zink's bind) */
    if (vkCmdBindVertexBuffers2) {
        begin();
        a = color_att(t.view, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, RED);
        ri = rendering(1, &a, NULL, 0);
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_plain);
        VkDeviceSize off = 0, size = sizeof(tri);
        vkCmdBindVertexBuffers2(cb, 0, 1, &vbuf, &off, &size, NULL);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        finish_and_read(t.img);
        expect("vkCmdBindVertexBuffers2", 2, pts2, (const uint32_t[]){WHITE, RED});
    } else {
        printf("(no vkCmdBindVertexBuffers2 on this device)\n");
    }

out:
    if (dev) vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) {
        printf("FAIL: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("OK: %d of %d checks passed\n", checks, checks);
    return 0;
}
