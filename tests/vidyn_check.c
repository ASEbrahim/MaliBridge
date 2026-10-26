/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * vidyn_check: VK_EXT_vertex_input_dynamic_state as Zink uses it (server_vidyn.c). As engine
 * "mesa zink" (argv[1] names another engine), one pipeline with VK_DYNAMIC_STATE_VERTEX_INPUT_EXT
 * draws points into an 8x1 image, one render pass, a vkCmdSetVertexInputEXT before each draw:
 *   px 0-1  layout A: binding 0 stride 12 (pos RG32F at 0, colour RGBA8 at 8)
 *   px 2-3  layout B: binding 0 stride 20 (pos at 0, an unused uv RG32F at 8, colour at 16)
 *   px 4    layout C: stride 12, colour B10G11R11_UFLOAT (emulated where the driver lacks it)
 *   px 5    layout D: stride 12, colour A2R10G10B10_SNORM (emulated where the driver lacks it)
 *   px 6    layout A again
 *   px 7    layout B, in a secondary command buffer (its own render pass instance)
 * Every layout has binding 1 with stride 0, a constant tint of 1 (as Zink binds constant
 * attributes), read at location 2. Zink's way of binding is kept: vkCmdBindVertexBuffers2 without
 * strides. The pixels are read back. Before Mesa 167df22a, Zink without the extension drew layout B
 * with layout A's pipeline (Balatro's missing text); with the extension, a driver or an emulation
 * that kept the old vertex input would show here as wrong colours.
 * "SKIP" when the device does not offer the extension (or its feature).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t spv_vert[] = {
#include "vidyn_vert.inc"
};
static const uint32_t spv_frag[] = {
#include "vidyn_frag.inc"
};

#define W 8

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkDestroyDevice);
I(vkEnumerateDeviceExtensionProperties); I(vkGetPhysicalDeviceFeatures2);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory);
I(vkCreateImageView); I(vkCreateRenderPass); I(vkCreateFramebuffer); I(vkCreateShaderModule); I(vkDestroyShaderModule);
I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkDestroyPipeline); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass);
I(vkCmdBindPipeline); I(vkCmdSetVertexInputEXT); I(vkCmdBindVertexBuffers2); I(vkCmdDraw); I(vkCmdCopyImageToBuffer);
I(vkCmdPipelineBarrier); I(vkCmdExecuteCommands); I(vkDeviceWaitIdle);
#undef I

static int check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
    return ok;
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

static float px_x(int px) { return 2.0f * (px + 0.5f) / W - 1.0f; }

/* The layouts: binding 0's stride and its attributes (colour format, colour offset, uv or not) */
typedef struct layout {
    const char *name;
    uint32_t stride;
    VkFormat colour;
    uint32_t colour_off;
    int uv;
} layout;

static const layout LA = {"A (stride 12, RGBA8)", 12, VK_FORMAT_R8G8B8A8_UNORM, 8, 0};
static const layout LB = {"B (stride 20, uv, RGBA8)", 20, VK_FORMAT_R8G8B8A8_UNORM, 16, 1};
static const layout LC = {"C (stride 12, B10G11R11_UFLOAT)", 12, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 8, 0};
static const layout LD = {"D (stride 12, A2R10G10B10_SNORM)", 12, VK_FORMAT_A2R10G10B10_SNORM_PACK32, 8, 0};

static void set_layout(VkCommandBuffer cb, const layout *l)
{
    VkVertexInputBindingDescription2EXT bd[2] = {
        {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, 0, l->stride, VK_VERTEX_INPUT_RATE_VERTEX, 1},
        {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, 1, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1}};
    VkVertexInputAttributeDescription2EXT ad[4] = {
        {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, 0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
        {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, 1, 0, l->colour, l->colour_off},
        {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, 3, 0, VK_FORMAT_R32G32_SFLOAT, 8}};
    vkCmdSetVertexInputEXT(cb, 2, bd, l->uv ? 4 : 3, ad);
}

/* Vertices of layout l from byte o of the vertex buffer: one per pixel, colours packed as given */
static uint32_t put(uint8_t *vb, uint32_t o, const layout *l, int px0, int n, const uint32_t *colours)
{
    for (int i = 0; i < n; i++) {
        uint8_t *v = vb + o + (uint32_t)i * l->stride;
        float xy[2] = {px_x(px0 + i), 0.0f}, uv[2] = {123.0f, -7.0f};
        memcpy(v, xy, 8);
        if (l->uv) memcpy(v + 8, uv, 8);
        memcpy(v + l->colour_off, &colours[i], 4);
    }
    return o + (uint32_t)n * l->stride;
}

static void bind(VkCommandBuffer cb, VkBuffer vb, VkDeviceSize off, VkBuffer tint)
{
    const VkBuffer bufs[2] = {vb, tint};
    const VkDeviceSize offs[2] = {off, 0};
    vkCmdBindVertexBuffers2(cb, 0, 2, bufs, offs, NULL, NULL); /* as Zink: no strides */
}

#define RGBA8(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)

int main(int argc, char **argv)
{
    const char *engine = argc > 1 ? argv[1] : "mesa zink";
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vidyn_check", 1, engine, 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceQueueFamilyProperties);
    L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr); L(vkDestroyInstance);
    L(vkEnumerateDeviceExtensionProperties); L(vkGetPhysicalDeviceFeatures2);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("GPU: %s, Vulkan %u.%u, engine \"%s\"\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), engine);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    int have = 0, gpl = 0;
    for (uint32_t i = 0; i < ne; i++) {
        have |= !strcmp(ep[i].extensionName, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
        gpl |= !strcmp(ep[i].extensionName, "VK_EXT_graphics_pipeline_library");
    }
    free(ep);
    printf("extensions: %s %s, %s VK_EXT_graphics_pipeline_library\n", have ? "with" : "without",
           VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, gpl ? "with" : "without");
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &vf};
    if (have) vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!have || !vf.vertexInputDynamicState) {
        printf("SKIP %s\n0 checks, 0 failed\n", have ? "vertexInputDynamicState not offered" : "the extension is not offered");
        vkDestroyInstance(inst, NULL);
        return 0;
    }
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    const char *dexts[] = {VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME};
    vf.vertexInputDynamicState = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &vf, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    char msg[512];
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    if (!check(r == VK_SUCCESS, "vkCreateDevice with the extension", msg)) goto out;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule);
    D(vkDestroyShaderModule); D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkDestroyPipeline); D(vkCmdBeginRenderPass);
    D(vkCmdEndRenderPass); D(vkCmdBindPipeline); D(vkCmdSetVertexInputEXT); D(vkCmdBindVertexBuffers2); D(vkCmdDraw);
    D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier); D(vkCmdExecuteCommands); D(vkDeviceWaitIdle);
#undef D
    if (!check(vkCmdSetVertexInputEXT != NULL, "vkCmdSetVertexInputEXT", NULL)) goto out;
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, 1, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
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
    /* two render passes: the first clears, the second (the secondary's) keeps what is there */
    VkAttachmentDescription att = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0}};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 2, dep};
    VkRenderPass rp, rp_load;
    vkCreateRenderPass(dev, &rpci, NULL, &rp);
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCreateRenderPass(dev, &rpci, NULL, &rp_load);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, 1, 1};
    VkFramebuffer fb;
    vkCreateFramebuffer(dev, &fci, NULL, &fb);
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);

    uint8_t *vb;
    VkBuffer vbuf = host_buffer(4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&vb);
    float *tint;
    VkBuffer tbuf = host_buffer(16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&tint);
    uint8_t *rb;
    VkBuffer rbuf = host_buffer(W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&rb);
    if (!vbuf || !tbuf || !rbuf) { check(0, "setup", "buffers"); goto out; }
    memset(vb, 0, 4096);
    for (int i = 0; i < 4; i++) tint[i] = 1.0f;
    /* B10G11R11: (0.75, 0.25, 0.5); A2R10G10B10 SNORM: (1.0, 0, 204/511, 1) */
    const uint32_t uf11 = 928u | 832u << 11 | 448u << 22;
    const uint32_t bgr10 = 204u | 0u << 10 | 511u << 20 | 1u << 30;
    const uint32_t ca[2] = {RGBA8(255, 0, 0, 255), RGBA8(0, 255, 0, 255)}, cb_[2] = {RGBA8(0, 0, 255, 255), RGBA8(255, 255, 0, 255)};
    const uint32_t cc = uf11, cd = bgr10, ce = RGBA8(255, 0, 255, 255), cf = RGBA8(0, 255, 255, 255);
    uint32_t oA = 0, oB = put(vb, oA, &LA, 0, 2, ca);
    uint32_t oC = put(vb, oB, &LB, 2, 2, cb_);
    uint32_t oD = put(vb, oC, &LC, 4, 1, &cc);
    uint32_t oE = put(vb, oD, &LD, 5, 1, &cd);
    uint32_t oF = put(vb, oE, &LA, 6, 1, &ce);
    put(vb, oF, &LB, 7, 1, &cf);

    VkShaderModule vs = shader(spv_vert, sizeof(spv_vert)), fs = shader(spv_frag, sizeof(spv_frag));
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0};
    VkViewport vp = {0, 0, W, 1, 0, 1};
    VkRect2D sc = {{0, 0}, {W, 1}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkDynamicState dyn = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 1, &dyn};
    /* no pVertexInputState, as Zink */
    VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, NULL, &ia, NULL, &vps, &rs, &ms,
                                         NULL, &cbs, &ds, pl, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline pipe;
    r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    if (!check(r == VK_SUCCESS, "pipeline with the vertex input dynamic", msg)) goto out;
    /* the modules go before the first draw, as DXVK's do */
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkCommandPool cpool;
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);

    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb, sec;
    vkAllocateCommandBuffers(dev, &cbai, &cb);
    cbai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    vkAllocateCommandBuffers(dev, &cbai, &sec);
    /* the secondary: layout B at pixel 7 */
    VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, rp_load, 0, fb, VK_FALSE, 0, 0};
    VkCommandBufferBeginInfo sbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &inh};
    vkBeginCommandBuffer(sec, &sbi);
    vkCmdBindPipeline(sec, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    set_layout(sec, &LB);
    bind(sec, vbuf, oF, tbuf);
    vkCmdDraw(sec, 1, 1, 0, 0);
    vkEndCommandBuffer(sec);

    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbbi);
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, 1}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    set_layout(cb, &LA);
    bind(cb, vbuf, oA, tbuf);
    vkCmdDraw(cb, 2, 1, 0, 0);
    set_layout(cb, &LB); /* only the vertex input changes: the case Zink got wrong */
    bind(cb, vbuf, oB, tbuf);
    vkCmdDraw(cb, 2, 1, 0, 0);
    set_layout(cb, &LC);
    bind(cb, vbuf, oC, tbuf);
    vkCmdDraw(cb, 1, 1, 0, 0);
    set_layout(cb, &LD);
    bind(cb, vbuf, oD, tbuf);
    vkCmdDraw(cb, 1, 1, 0, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); /* bound again, then layout A again */
    set_layout(cb, &LA);
    bind(cb, vbuf, oE, tbuf);
    vkCmdDraw(cb, 1, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    rbi.renderPass = rp_load;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    vkCmdExecuteCommands(cb, 1, &sec);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, 1, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    memset(rb, 0x55, W * 4);
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) { check(0, "submit", NULL); goto out; }

    static const struct {
        const char *what;
        uint8_t want[4];
        int tol;
    } px[W] = {
        {"layout A, pixel 0", {255, 0, 0, 255}, 0},       {"layout A, pixel 1", {0, 255, 0, 255}, 0},
        {"layout B after A, pixel 2", {0, 0, 255, 255}, 0}, {"layout B after A, pixel 3", {255, 255, 0, 255}, 0},
        {"layout C (B10G11R11_UFLOAT colour)", {191, 64, 128, 255}, 1},
        {"layout D (A2R10G10B10_SNORM colour)", {255, 0, 102, 255}, 2},
        {"layout A again after a bind", {255, 0, 255, 255}, 0},
        {"layout B in a secondary command buffer", {0, 255, 255, 255}, 0},
    };
    for (int i = 0; i < W; i++) {
        const uint8_t *p = rb + i * 4;
        int ok = 1;
        for (int k = 0; k < 4; k++) ok &= abs((int)p[k] - (int)px[i].want[k]) <= px[i].tol;
        snprintf(msg, sizeof(msg), "(%u, %u, %u, %u), wanted (%u, %u, %u, %u)", p[0], p[1], p[2], p[3], px[i].want[0], px[i].want[1],
                 px[i].want[2], px[i].want[3]);
        check(ok, px[i].what, msg);
    }
    vkDeviceWaitIdle(dev);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyDevice(dev, NULL);
out:
    vkDestroyInstance(inst, NULL);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
