/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * polygon_check: polygon mode LINE and POINT (wireframe). DXVK 3 needs fillModeNonSolid; on a GPU
 * without it (Mali-G720) the bridge reports it and draws the triangles' edges itself
 * (server_emu_polygon.c). Small triangles are drawn into a 64x64 target and read back: an edge's
 * middle must be lit (within a pixel), a triangle's inside must stay dark, and a wireframe lights
 * far fewer pixels than the filled triangle.
 *
 *   - LINE, vkCmdDraw, a triangle list
 *   - LINE, vkCmdDrawIndexed, 16-bit indices from firstIndex 3 with vertexOffset 1, two instances
 *   - LINE, vkCmdDrawIndexed, a 32-bit strip with a primitive restart (no edge across it)
 *   - LINE, vkCmdDraw, a fan (its inner diagonal drawn)
 *   - POINT, vkCmdDraw: the corners lit, the edges' middles dark
 *   - a solid pipeline's indexed draw after a wireframe one in the same render pass: the app's
 *     index buffer and pipeline are back (inside lit)
 *   - one recorded command buffer submitted twice, the indices changed on the host in between:
 *     the second picture follows the new indices
 *   - dynamic topology and restart (vkd3d-proton's way): a strip with a restart, then a fan, from
 *     one pipeline (Vulkan 1.3 devices)
 *   - dynamic vertex stride (created 16, set 8 with vkCmdBindVertexBuffers2 before a second bind;
 *     the buffer bound first with vkCmdBindVertexBuffers): the edges where the stride 8 puts them.
 *     The emulation binds its variant and the app's pipeline around the draw, and in safe mode
 *     each bind has MK-04's dyn_reemit send the stride again (Vulkan 1.3 devices)
 *   - FILL, the control: the inside lit
 *   - the command pool and command buffers made before the first non-solid pipeline exists (as an
 *     app's are): a primary's indexed LINE draw, and the same draw recorded in a secondary command
 *     buffer executed inside the primary's render pass (DXVK 3's tiler mode records render passes
 *     so; Half-Life 2's wireframe drew solid on Mali-G720 until secondaries were handled)
 * Culling is off throughout (the emulation draws every triangle's edges; see the server file).
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

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceFeatures);
I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkDestroyInstance); I(vkDestroyDevice); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers);
I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer);
I(vkGetBufferMemoryRequirements); I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage);
I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateImageView); I(vkCreateRenderPass); I(vkCreateFramebuffer);
I(vkCreateShaderModule); I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass);
I(vkCmdBindPipeline); I(vkCmdBindVertexBuffers); I(vkCmdBindIndexBuffer); I(vkCmdDraw); I(vkCmdDrawIndexed);
I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier); I(vkDeviceWaitIdle); I(vkFlushMappedMemoryRanges);
I(vkCmdSetPrimitiveTopology); I(vkCmdSetPrimitiveRestartEnable); I(vkCmdExecuteCommands); I(vkCmdBindVertexBuffers2);
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

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

/* Pixel centre (x, y) in clip space (the target is 64x64; instance 1 is 32 pixels to the right). */
#define PX(x) ((float)(x) / 32.0f - 1.0f)

/* Vertex buffer: 0 junk, 1-3 triangle A (legs along the top and left), 4-7 a square for the fan,
 * 8-10 triangle B (lower half), 11 junk. */
static const float verts[][2] = {
    {PX(60.5), PX(60.5)},
    {PX(4.5), PX(4.5)}, {PX(28.5), PX(4.5)}, {PX(4.5), PX(28.5)},
    {PX(4.5), PX(4.5)}, {PX(28.5), PX(4.5)}, {PX(28.5), PX(28.5)}, {PX(4.5), PX(28.5)},
    {PX(4.5), PX(36.5)}, {PX(28.5), PX(36.5)}, {PX(4.5), PX(60.5)},
    {PX(60.5), PX(60.5)},
};

static uint8_t *pixels; /* RGBA8 readback */

static int lit(int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && pixels[(y * W + x) * 4] > 128; }

static int near_lit(int x, int y)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (lit(x + dx, y + dy)) return 1;
    return 0;
}

static int count_lit(int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += lit(x, y);
    return n;
}

/* Triangle A's picture at x offset ox: the edges' middles lit, the inside dark, few pixels. */
static int wire_a(int ox, char *msg, size_t len)
{
    struct { int x, y; const char *what; } edge[] = {{16, 4, "top edge"}, {4, 16, "left edge"}, {16, 16, "diagonal"}};
    for (int i = 0; i < 3; i++)
        if (!near_lit(ox + edge[i].x, edge[i].y)) {
            snprintf(msg, len, "x+%d: %s (%d,%d) dark", ox, edge[i].what, ox + edge[i].x, edge[i].y);
            return 0;
        }
    if (lit(ox + 10, 10) || lit(ox + 9, 14)) {
        snprintf(msg, len, "x+%d: inside (%d,10) lit: filled, not wireframe", ox, ox + 10);
        return 0;
    }
    int n = count_lit(ox, 0, ox + 32, 32);
    if (n < 40 || n > 140) {
        snprintf(msg, len, "x+%d: %d pixels lit (a wireframe lights about 70, the filled triangle about 300)", ox, n);
        return 0;
    }
    return 1;
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "polygon_check", 1, "DXVK", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceFeatures);
    L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
    L(vkDestroyInstance);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("GPU: %s\n", props.deviceName);
    char msg[256];
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(pd, &have);
    check(have.fillModeNonSolid, "fillModeNonSolid reported", NULL);
    if (!have.fillModeNonSolid) goto out;

    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceFeatures en = {0};
    en.fillModeNonSolid = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, &en};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    check(r == VK_SUCCESS, "vkCreateDevice with fillModeNonSolid", msg);
    if (r) goto out;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule);
    D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkCmdBeginRenderPass); D(vkCmdEndRenderPass); D(vkCmdBindPipeline);
    D(vkCmdBindVertexBuffers); D(vkCmdBindIndexBuffer); D(vkCmdDraw); D(vkCmdDrawIndexed); D(vkCmdCopyImageToBuffer);
    D(vkCmdPipelineBarrier); D(vkDeviceWaitIdle); D(vkFlushMappedMemoryRanges); D(vkCmdExecuteCommands);
    int core13 = props.apiVersion >= VK_API_VERSION_1_3;
    if (core13) {
        D(vkCmdSetPrimitiveTopology);
        D(vkCmdSetPrimitiveRestartEnable);
        D(vkCmdBindVertexBuffers2);
        core13 = vkCmdSetPrimitiveTopology && vkCmdSetPrimitiveRestartEnable && vkCmdBindVertexBuffers2;
    }
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* ---- target, render pass, buffers */
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkMemoryRequirements mr;
    VkDeviceMemory imem;
    if (vkCreateImage(dev, &imci, NULL, &img)) { check(0, "setup", "image"); goto out; }
    vkGetImageMemoryRequirements(dev, img, &mr);
    uint32_t t = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_type(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    if (vkAllocateMemory(dev, &mai, NULL, &imem) || vkBindImageMemory(dev, img, imem, 0)) { check(0, "setup", "image memory"); goto out; }
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    vkCreateImageView(dev, &vci, NULL, &view);
    VkAttachmentDescription att = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
    VkRenderPass rp;
    vkCreateRenderPass(dev, &rpci, NULL, &rp);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 1};
    VkFramebuffer fb;
    vkCreateFramebuffer(dev, &fci, NULL, &fb);
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);

    float *vb;
    VkBuffer vbuf = host_buffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&vb);
    uint16_t *ib16;
    VkBuffer ibuf16 = host_buffer(64, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, (void **)&ib16);
    uint32_t *ib32;
    VkBuffer ibuf32 = host_buffer(64, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, (void **)&ib32);
    VkBuffer rbuf = host_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&pixels);
    if (!vbuf || !ibuf16 || !ibuf32 || !rbuf) { check(0, "setup", "buffers"); goto out; }
    memcpy(vb, verts, sizeof(verts));
    /* 16-bit: three junk indices, then triangle A relative to vertexOffset 1; then triangle B (absolute). */
    const uint16_t i16[] = {9, 9, 9, 0, 1, 2, 8, 9, 10};
    memcpy(ib16, i16, sizeof(i16));
    /* 32-bit strip: triangle A, a restart, triangle B. */
    const uint32_t i32[] = {1, 2, 3, 0xffffffffu, 8, 9, 10};
    memcpy(ib32, i32, sizeof(i32));

    /* ---- the pool and two command buffers before any non-solid pipeline exists */
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VkCommandPool cpool;
    if (vkCreateCommandPool(dev, &cpci, NULL, &cpool)) { check(0, "setup", "command pool"); goto out; }
    VkCommandBuffer early, second;
    VkCommandBufferAllocateInfo eai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBufferAllocateInfo sai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1};
    if (vkAllocateCommandBuffers(dev, &eai, &early) || vkAllocateCommandBuffers(dev, &sai, &second)) {
        check(0, "setup", "early command buffers");
        goto out;
    }

    /* ---- pipelines: LINE list, LINE strip with restart, LINE fan, POINT list, FILL list */
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, shader(spv_vert, sizeof(spv_vert)), "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader(spv_frag, sizeof(spv_frag)), "main", NULL}};
    VkVertexInputBindingDescription bd = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription ad = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &bd, 1, &ad};
    VkViewport vp = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    enum { P_LIST, P_STRIP, P_FAN, P_POINT, P_FILL, P_DYN, P_STRIDE, NP };
    static const struct { VkPrimitiveTopology topo; VkBool32 restart; VkPolygonMode mode; const char *name; } pdesc[NP] = {
        {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_POLYGON_MODE_LINE, "LINE, triangle list"},
        {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 1, VK_POLYGON_MODE_LINE, "LINE, triangle strip with primitive restart"},
        {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, 0, VK_POLYGON_MODE_LINE, "LINE, triangle fan"},
        {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_POLYGON_MODE_POINT, "POINT, triangle list"},
        {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_POLYGON_MODE_FILL, "FILL, triangle list"},
        {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_POLYGON_MODE_LINE, "LINE, dynamic topology and restart"},
        {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_POLYGON_MODE_LINE, "LINE, dynamic vertex stride"},
    };
    VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    VkPipelineDynamicStateCreateInfo dsci = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyns};
    VkDynamicState dyn_stride = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    VkPipelineDynamicStateCreateInfo dsci_stride = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 1, &dyn_stride};
    VkPipeline pipe[NP];
    for (int i = 0; i < NP; i++) {
        if ((i == P_DYN || i == P_STRIDE) && !core13) {
            pipe[i] = VK_NULL_HANDLE;
            continue;
        }
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, pdesc[i].topo,
                                                     pdesc[i].restart};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = pdesc[i].mode;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms,
                                             NULL, &cbs, i == P_DYN ? &dsci : i == P_STRIDE ? &dsci_stride : NULL, pl, rp, 0, VK_NULL_HANDLE, -1};
        bd.stride = i == P_STRIDE ? 16 : 8; /* the dynamic one is created wrong: the draw sets 8 */
        r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe[i]);
        bd.stride = 8;
        snprintf(msg, sizeof(msg), "VkResult %d", r);
        char what[96];
        snprintf(what, sizeof(what), "pipeline: %s", pdesc[i].name);
        check(r == VK_SUCCESS, what, msg);
        if (r) goto out;
    }
    enum { C_LIST, C_IDX16, C_STRIP, C_FAN, C_POINT, C_RESTORE, C_RESUBMIT, C_DYN, C_STRIDE, C_EARLY, C_SECONDARY, C_FILL, NC };
    for (int c = 0; c < NC; c++) {
        if ((c == C_DYN || c == C_STRIDE) && !core13) {
            printf("SKIP %s: a Vulkan 1.1 device\n", c == C_DYN ? "dynamic topology" : "dynamic vertex stride");
            continue;
        }
        VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cb;
        if (c == C_EARLY || c == C_SECONDARY) cb = early;
        else vkAllocateCommandBuffers(dev, &cbai, &cb);
        VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                         c == C_RESUBMIT ? 0 : VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &cbbi);
        VkClearValue clear = {.color = {.float32 = {0, 0, 0, 0}}};
        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, H}}, 1, &clear};
        VkDeviceSize zero = 0;
        if (c == C_SECONDARY) {
            /* The render pass's draws in a secondary (begun before the pipelines existed too). */
            VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, rp, 0, fb, VK_FALSE, 0, 0};
            VkCommandBufferBeginInfo sbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &inh};
            vkBeginCommandBuffer(second, &sbi);
            vkCmdBindVertexBuffers(second, 0, 1, &vbuf, &zero);
            vkCmdBindPipeline(second, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_LIST]);
            vkCmdBindIndexBuffer(second, ibuf16, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(second, 3, 2, 3, 1, 0);
            vkEndCommandBuffer(second);
        }
        vkCmdBeginRenderPass(cb, &rbi, c == C_SECONDARY ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
        if (c != C_SECONDARY) vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &zero);
        const char *what = "";
        switch (c) {
        case C_LIST:
            what = "LINE, vkCmdDraw, triangle list";
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_LIST]);
            vkCmdDraw(cb, 3, 1, 1, 0);
            break;
        case C_IDX16:
            what = "LINE, vkCmdDrawIndexed, 16-bit, firstIndex 3, vertexOffset 1, 2 instances";
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_LIST]);
            vkCmdBindIndexBuffer(cb, ibuf16, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cb, 3, 2, 3, 1, 0);
            break;
        case C_STRIP:
            what = "LINE, vkCmdDrawIndexed, 32-bit strip with a restart";
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_STRIP]);
            vkCmdBindIndexBuffer(cb, ibuf32, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, 7, 1, 0, 0, 0);
            break;
        case C_FAN:
            what = "LINE, vkCmdDraw, triangle fan";
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_FAN]);
            vkCmdDraw(cb, 4, 1, 4, 0);
            break;
        case C_POINT:
            what = "POINT, vkCmdDraw, triangle list";
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_POINT]);
            vkCmdDraw(cb, 3, 1, 1, 0);
            break;
        case C_RESTORE:
            what = "LINE indexed, then FILL indexed from the app's index buffer";
            vkCmdBindIndexBuffer(cb, ibuf16, 0, VK_INDEX_TYPE_UINT16);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_LIST]);
            vkCmdDrawIndexed(cb, 3, 1, 3, 1, 0);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_FILL]);
            vkCmdDrawIndexed(cb, 3, 1, 6, 0, 0);
            break;
        case C_RESUBMIT:
            what = "LINE indexed, one command buffer submitted twice, indices changed in between";
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_LIST]);
            vkCmdBindIndexBuffer(cb, ibuf32, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, 3, 1, 0, 0, 0);
            break;
        case C_DYN:
            what = "LINE, dynamic topology: a 32-bit strip with a restart, then a fan";
            vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
            vkCmdSetPrimitiveRestartEnable(cb, VK_TRUE);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_DYN]);
            vkCmdBindIndexBuffer(cb, ibuf32, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, 7, 1, 0, 0, 0);
            vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
            vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
            vkCmdDraw(cb, 4, 1, 4, 1); /* instance 1: 32 pixels to the right */
            break;
        case C_STRIDE: {
            what = "LINE, vkCmdDraw, dynamic vertex stride 8 set before a second bind (created 16)";
            VkDeviceSize eight = 8;
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_STRIDE]);
            vkCmdBindVertexBuffers2(cb, 0, 1, &vbuf, &zero, NULL, &eight);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_FILL]); /* static stride 8 */
            vkCmdBindVertexBuffers2(cb, 0, 1, &vbuf, &zero, NULL, &eight);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_STRIDE]);
            vkCmdDraw(cb, 3, 1, 1, 0);
            break;
        }
        case C_EARLY:
            what = "LINE, vkCmdDrawIndexed, a command buffer allocated before the pipelines";
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_LIST]);
            vkCmdBindIndexBuffer(cb, ibuf16, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cb, 3, 2, 3, 1, 0);
            break;
        case C_SECONDARY:
            what = "LINE, vkCmdDrawIndexed in a secondary command buffer, both allocated before the pipelines";
            vkCmdExecuteCommands(cb, 1, &second);
            break;
        case C_FILL:
            what = "FILL, vkCmdDraw (control)";
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[P_FILL]);
            vkCmdDraw(cb, 3, 1, 1, 0);
            break;
        }
        vkCmdEndRenderPass(cb);
        VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        memset(pixels, 0x55, W * H * 4);
        if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) { check(0, what, "submit"); goto out; }
        int ok = 1;
        msg[0] = 0;
        switch (c) {
        case C_LIST:
        case C_STRIDE:
            ok = wire_a(0, msg, sizeof(msg));
            break;
        case C_IDX16:
        case C_EARLY:
        case C_SECONDARY:
            ok = wire_a(0, msg, sizeof(msg)) && wire_a(32, msg, sizeof(msg));
            break;
        case C_STRIP:
            /* Triangle A and triangle B (A moved down 32 rows): edges, insides dark, nothing between. */
            ok = wire_a(0, msg, sizeof(msg));
            if (ok && (!near_lit(16, 36) || !near_lit(4, 48) || !near_lit(16, 48) || lit(10, 42))) {
                ok = 0;
                snprintf(msg, sizeof(msg), "triangle B after the restart: edges (16,36) %d (4,48) %d (16,48) %d, inside (10,42) %d",
                         near_lit(16, 36), near_lit(4, 48), near_lit(16, 48), lit(10, 42));
            }
            if (ok && count_lit(8, 29, 30, 35)) {
                ok = 0;
                snprintf(msg, sizeof(msg), "%d pixels lit between the triangles: an edge across the restart", count_lit(8, 29, 30, 35));
            }
            break;
        case C_DYN:
            ok = wire_a(0, msg, sizeof(msg));
            if (ok && (!near_lit(16, 36) || !near_lit(4, 48) || !near_lit(16, 48) || lit(10, 42) || count_lit(8, 29, 30, 35))) {
                ok = 0;
                snprintf(msg, sizeof(msg), "strip's triangle B: edges %d %d %d, inside %d, %d pixels between", near_lit(16, 36),
                         near_lit(4, 48), near_lit(16, 48), lit(10, 42), count_lit(8, 29, 30, 35));
            }
            if (ok && (!near_lit(48, 4) || !near_lit(60, 16) || !near_lit(48, 28) || !near_lit(36, 16) || !near_lit(48, 16) ||
                       lit(54, 10) || lit(42, 22))) {
                ok = 0;
                snprintf(msg, sizeof(msg), "fan at x+32: sides %d %d %d %d, diagonal %d, inside %d %d", near_lit(48, 4), near_lit(60, 16),
                         near_lit(48, 28), near_lit(36, 16), near_lit(48, 16), lit(54, 10), lit(42, 22));
            }
            break;
        case C_FAN:
            /* The square's four sides and the diagonal from the centre vertex (4,4) to (28,28). */
            if (!near_lit(16, 4) || !near_lit(28, 16) || !near_lit(16, 28) || !near_lit(4, 16) || !near_lit(16, 16)) {
                ok = 0;
                snprintf(msg, sizeof(msg), "sides/diagonal: top %d right %d bottom %d left %d diagonal %d", near_lit(16, 4),
                         near_lit(28, 16), near_lit(16, 28), near_lit(4, 16), near_lit(16, 16));
            } else if (lit(22, 10) || lit(10, 22)) {
                ok = 0;
                snprintf(msg, sizeof(msg), "inside lit: (22,10) %d (10,22) %d", lit(22, 10), lit(10, 22));
            }
            break;
        case C_POINT: {
            int corners = near_lit(4, 4) + near_lit(28, 4) + near_lit(4, 28), total = count_lit(0, 0, W, H);
            ok = corners == 3 && !lit(16, 4) && !lit(4, 16) && !lit(16, 16) && total <= 12;
            snprintf(msg, sizeof(msg), "%d of 3 corners lit, edge middles %d %d %d, %d pixels lit", corners, lit(16, 4), lit(4, 16),
                     lit(16, 16), total);
            break;
        }
        case C_RESTORE:
            ok = wire_a(0, msg, sizeof(msg));
            if (ok && (!lit(10, 42) || count_lit(0, 32, 32, 64) < 200)) {
                ok = 0;
                snprintf(msg, sizeof(msg), "the solid draw after it: inside (10,42) %d, %d pixels (wanted about 300)", lit(10, 42),
                         count_lit(0, 32, 32, 64));
            }
            break;
        case C_RESUBMIT:
            ok = wire_a(0, msg, sizeof(msg));
            if (ok) {
                /* Triangle B instead of A, through the same recorded commands. */
                ib32[0] = 8, ib32[1] = 9, ib32[2] = 10;
                memset(pixels, 0x55, W * H * 4);
                if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) { check(0, what, "second submit"); goto out; }
                if (count_lit(0, 0, 32, 32) || !near_lit(16, 36) || !near_lit(4, 48) || !near_lit(16, 48) || lit(10, 42)) {
                    ok = 0;
                    snprintf(msg, sizeof(msg), "second submit: %d pixels left where A was; B's edges (16,36) %d (4,48) %d (16,48) %d, "
                             "inside %d", count_lit(0, 0, 32, 32), near_lit(16, 36), near_lit(4, 48), near_lit(16, 48), lit(10, 42));
                }
                memcpy(ib32, i32, sizeof(i32));
            }
            break;
        case C_FILL:
            ok = lit(10, 10) && count_lit(0, 0, 32, 32) > 200;
            snprintf(msg, sizeof(msg), "inside %d, %d pixels lit", lit(10, 10), count_lit(0, 0, 32, 32));
            break;
        }
        check(ok, what, msg);
    }
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
out:
    vkDestroyInstance(inst, NULL);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
