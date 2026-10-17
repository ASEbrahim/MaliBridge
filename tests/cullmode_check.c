/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cullmode_check: a dynamic cull mode (vkCmdSetCullMode) must not cull points and lines (MK-16, Arm
 * erratum 3786517: Mali drivers below r52 do, and those draws vanish), and must still cull the
 * triangles drawn after them. Pipelines with VK_DYNAMIC_STATE_CULL_MODE, created with cull NONE,
 * draw into a 4x4 RGBA8 target:
 *   1  line list, cull FRONT_AND_BACK set after the bind: the line on pixel row 1 is drawn (green)
 *   2  point list, cull BACK set before the bind: the point on pixel (1, 1) is drawn (blue)
 *   3  FRONT_AND_BACK, a line, then a triangle pipeline: the line drawn, the triangle (red) culled
 *   4  FRONT_AND_BACK, a triangle (culled), then a line pipeline: the line drawn, no red
 * Host imitation (server): VKBRIDGE_ERRATA_IMITATE=cull_nontri drops point and line draws while a
 * cull mode other than NONE is set dynamically.
 *
 * Shaders: tests/shaders/cullmode/cullmode.vert (glslc -O --target-env=vulkan1.1 -mfmt=num ->
 * cullmode_vert.inc) and dynstate_check's fragment shader.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t spv_vert[] = {
#include "cullmode_vert.inc"
};
static const uint32_t spv_frag[] = {
#include "dynstate_frag.inc"
};

#define W 4

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkGetDeviceQueue); I(vkCreateCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkFreeCommandBuffers); I(vkQueueSubmit);
I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory);
I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateImageView); I(vkCreateRenderPass);
I(vkCreateFramebuffer); I(vkCreateShaderModule); I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkCmdBeginRenderPass);
I(vkCmdEndRenderPass); I(vkCmdBindPipeline); I(vkCmdPushConstants); I(vkCmdDraw); I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier);
I(vkCmdSetCullMode);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static VkPhysicalDeviceMemoryProperties mp;

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkRenderPass rp;
static VkPipelineLayout layout;
static VkPipelineShaderStageCreateInfo stages[2];
static VkCommandPool cpool;
static VkFramebuffer fb;
static VkImage color;
static VkBuffer rbuf;
static uint8_t *rb;

static VkPipeline pipeline(VkPrimitiveTopology topo)
{
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo, 0};
    VkViewport vp = {0, 0, W, W, 0, 1};
    VkRect2D sc = {{0, 0}, {W, W}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    const VkDynamicState d[] = {VK_DYNAMIC_STATE_CULL_MODE};
    VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 1, d};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms,
                                       NULL, &cbs, &dsi, layout, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (r) printf("     vkCreateGraphicsPipelines: %d\n", r);
    return p;
}

static VkCommandBuffer begin_case(void)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, W}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    return cb;
}

enum { TRI, LINE, POINT };

static void draw(VkCommandBuffer cb, int mode, float r, float g, float b)
{
    float pc[8] = {r, g, b, 1, (float)mode, 0, 0, 0};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cb, mode == TRI ? 3 : mode == LINE ? 2 : 1, 1, 0, 0);
}

static const uint8_t RED[4] = {255, 0, 0, 255}, GREEN[4] = {0, 255, 0, 255}, BLUE[4] = {0, 0, 255, 255};

/* Ends the case: pixel (x, y) must be want, and no pixel red (a triangle that should be culled) */
static void end_case(VkCommandBuffer cb, const char *what, int x, int y, const uint8_t want[4])
{
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
    vkCmdCopyImageToBuffer(cb, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    memset(rb, 0x55, W * W * 4);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) {
        check(0, what, "submit");
        return;
    }
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
    char msg[160] = "";
    const uint8_t *p = rb + (y * W + x) * 4;
    int ok = !memcmp(p, want, 4), red = 0;
    for (int i = 0; i < W * W; i++) red += !memcmp(rb + i * 4, RED, 4);
    if (!ok) snprintf(msg, sizeof(msg), "pixel (%d, %d) is (%u %u %u %u), wanted (%u %u %u %u)", x, y, p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3]);
    if (red) snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "%s%d red pixel(s): a triangle was not culled", ok ? "" : "; ", red);
    check(ok && !red, what, msg);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "cullmode_check", 1, "dxvk", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceQueueFamilyProperties);
    L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("GPU: %s, Vulkan %u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
    if (props.apiVersion < VK_API_VERSION_1_3) { printf("SKIP a Vulkan 1.3 device is needed\n0 checks, 0 failed\n"); return 0; }

    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    char msg[64];
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    check(r == VK_SUCCESS, "vkCreateDevice", msg);
    if (r) goto out;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkFreeCommandBuffers); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule);
    D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkCmdBeginRenderPass); D(vkCmdEndRenderPass); D(vkCmdBindPipeline);
    D(vkCmdPushConstants); D(vkCmdDraw); D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier); D(vkCmdSetCullMode);
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* target and readback */
    VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ici2 = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, cfmt, {W, W, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements mr;
    VkDeviceMemory m;
    if (vkCreateImage(dev, &ici2, NULL, &color)) { check(0, "setup", "image"); goto out; }
    vkGetImageMemoryRequirements(dev, color, &mr);
    uint32_t t = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_type(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    if (vkAllocateMemory(dev, &mai, NULL, &m) || vkBindImageMemory(dev, color, m, 0)) { check(0, "setup", "image memory"); goto out; }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(dev, &bci, NULL, &rbuf)) { check(0, "setup", "buffer"); goto out; }
    vkGetBufferMemoryRequirements(dev, rbuf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &m) || vkBindBufferMemory(dev, rbuf, m, 0) ||
        vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&rb)) {
        check(0, "setup", "readback memory");
        goto out;
    }
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, color, VK_IMAGE_VIEW_TYPE_2D, cfmt, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    vkCreateImageView(dev, &vci, NULL, &view);
    VkAttachmentDescription att = {0, cfmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &cref, NULL, NULL, 0, NULL};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
    vkCreateRenderPass(dev, &rpci, NULL, &rp);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, W, 1};
    vkCreateFramebuffer(dev, &fci, NULL, &fb);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    vkCreatePipelineLayout(dev, &plci, NULL, &layout);
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_vert), spv_vert};
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &smci, NULL, &vs);
    smci.codeSize = sizeof(spv_frag), smci.pCode = spv_frag;
    vkCreateShaderModule(dev, &smci, NULL, &fs);
    stages[0] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL};
    stages[1] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL};
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);
    VkPipeline tri = pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST), line = pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST),
               point = pipeline(VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    if (!tri || !line || !point) { check(0, "setup", "pipelines"); goto out; }

    VkCommandBuffer cb = begin_case();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, line);
    vkCmdSetCullMode(cb, VK_CULL_MODE_FRONT_AND_BACK);
    draw(cb, LINE, 0, 1, 0);
    end_case(cb, "line list, cull FRONT_AND_BACK set after the bind: the line drawn", 2, 1, GREEN);

    cb = begin_case();
    vkCmdSetCullMode(cb, VK_CULL_MODE_BACK_BIT);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, point);
    draw(cb, POINT, 0, 0, 1);
    end_case(cb, "point list, cull BACK set before the bind: the point drawn", 1, 1, BLUE);

    cb = begin_case();
    vkCmdSetCullMode(cb, VK_CULL_MODE_FRONT_AND_BACK);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, line);
    draw(cb, LINE, 0, 1, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tri);
    draw(cb, TRI, 1, 0, 0);
    end_case(cb, "FRONT_AND_BACK, a line, then a triangle pipeline: the line drawn, the triangle culled", 2, 1, GREEN);

    cb = begin_case();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tri);
    vkCmdSetCullMode(cb, VK_CULL_MODE_FRONT_AND_BACK);
    draw(cb, TRI, 1, 0, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, line);
    draw(cb, LINE, 0, 1, 0);
    end_case(cb, "FRONT_AND_BACK, a culled triangle, then a line pipeline: the line drawn", 2, 1, GREEN);
out:
    if (failures)
        printf("FAIL: %d of %d checks failed\n", failures, checks);
    else
        printf("OK: %d of %d checks passed\n", checks, checks);
    return failures ? 1 : 0;
}
