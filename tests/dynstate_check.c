/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dynstate_check: dynamic state set before a vkCmdBindPipeline must hold after it (MK-04, Arm errata
 * 3809538 / 3893905 / 3893903: Mali drivers lose it, or let a static pipeline pick it up). Every
 * case draws a full-target triangle into a 4x4 RGBA8 target with a D/S attachment, after binding
 * pipeline A, setting a state dynamically and binding pipeline B (both with that state dynamic,
 * created with the opposite value, which the driver must ignore), without setting it again:
 *   depth write     write OFF: red z 0.5 (A), green z 0.7 (B), blue z 0.8 (B): blue; green = B wrote
 *   depth test      OFF (created ON, compare NEVER): green drawn
 *   depth compare   ALWAYS (created NEVER, test ON): green drawn
 *   cull mode       NONE (created FRONT_AND_BACK): green drawn
 *   stencil write   write mask 0 (created 0xff), REPLACE ref 1; then a static "stencil == 0" pass
 *                   draws blue: blue
 *   stencil op/ref  ALWAYS/REPLACE ref 5 mask 0xff (created NEVER, 0, 0); then "stencil == 5": blue
 *   vertex stride   16 (created 4): the colour's attribute at instance 1 is at offset 16: green
 *   sample mask     (EDS3, if exposed) mask 0 (created 1): nothing drawn; then static pipeline C
 *                   (sample mask all) after it: blue drawn (3893903: C must not pick up the 0)
 * Host imitation (server): VKBRIDGE_ERRATA_IMITATE=dyn_reemit puts the states back to the
 * pipeline's own values at each bind, and gives a static sample mask the last dynamic one.
 *
 * Shaders: tests/shaders/dynstate/ (glslc -O --target-env=vulkan1.1 -mfmt=num -> dynstate_*.inc).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t spv_vert[] = {
#include "dynstate_vert.inc"
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
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceFeatures2);
I(vkEnumerateDeviceExtensionProperties); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceFormatProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkDestroyDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkFreeCommandBuffers); I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements);
I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements);
I(vkBindImageMemory); I(vkCreateImageView); I(vkCreateRenderPass); I(vkCreateFramebuffer); I(vkCreateShaderModule);
I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass); I(vkCmdBindPipeline);
I(vkCmdPushConstants); I(vkCmdBindVertexBuffers); I(vkCmdBindVertexBuffers2); I(vkCmdDraw); I(vkCmdCopyImageToBuffer);
I(vkCmdPipelineBarrier); I(vkDeviceWaitIdle); I(vkCmdSetDepthWriteEnable); I(vkCmdSetDepthTestEnable); I(vkCmdSetDepthCompareOp);
I(vkCmdSetCullMode); I(vkCmdSetStencilWriteMask); I(vkCmdSetStencilOp); I(vkCmdSetStencilReference); I(vkCmdSetStencilCompareMask);
I(vkCmdSetSampleMaskEXT);
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

static VkImage device_image(VkFormat fmt, VkImageUsageFlags usage)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, W, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &ci, NULL, &img)) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    uint32_t t = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_type(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    VkDeviceMemory m;
    if (vkAllocateMemory(dev, &ai, NULL, &m) || vkBindImageMemory(dev, img, m, 0)) return VK_NULL_HANDLE;
    return img;
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

static VkRenderPass rp;
static VkPipelineLayout layout;
static VkPipelineShaderStageCreateInfo stages[2];

/* A pipeline: the dynamic states listed, the rest as the create info below says */
typedef struct {
    const VkDynamicState *dyn;
    uint32_t ndyn;
    VkBool32 dtest, dwrite, stest;
    VkCompareOp dcmp;
    VkStencilOpState st;
    VkCullModeFlags cull;
    uint32_t stride;
    const VkSampleMask *smask;
} pdesc;

static VkPipeline pipeline(const pdesc *d)
{
    VkVertexInputBindingDescription bd = {0, d->stride ? d->stride : 4, VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription ad = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &bd, 1, &ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkViewport vp = {0, 0, W, W, 0, 1};
    VkRect2D sc = {{0, 0}, {W, W}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = d->cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    ms.pSampleMask = d->smask;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d->dtest, ds.depthWriteEnable = d->dwrite, ds.depthCompareOp = d->dcmp;
    ds.stencilTestEnable = d->stest, ds.front = ds.back = d->st;
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, d->ndyn, d->dyn};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms,
                                       &ds, &cbs, d->ndyn ? &dsi : NULL, layout, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (r) printf("     vkCreateGraphicsPipelines: %d\n", r);
    return p;
}

static VkCommandPool cpool;
static VkFramebuffer fb;
static VkImage color;
static VkBuffer vbuf, rbuf;
static uint8_t *rb;

static VkCommandBuffer begin_case(void)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clear[2] = {{.color = {.float32 = {0, 0, 0, 0}}}, {.depthStencil = {1.0f, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, W}}, 2, clear};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
    return cb;
}

static void draw(VkCommandBuffer cb, float r, float g, float b, float z, uint32_t first_instance)
{
    float pc[8] = {r, g, b, 1, z, 0, 0, 0};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cb, 3, 1, 0, first_instance);
}

/* Ends the case and checks that every pixel is want (RGBA8) */
static void end_case(VkCommandBuffer cb, const char *what, const uint8_t want[4])
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
    int bad = 0;
    for (int i = 0; i < W * W; i++)
        if (memcmp(rb + i * 4, want, 4) && !bad++)
            snprintf(msg, sizeof(msg), "pixel %d: (%u %u %u %u), wanted (%u %u %u %u)", i, rb[i * 4], rb[i * 4 + 1], rb[i * 4 + 2],
                     rb[i * 4 + 3], want[0], want[1], want[2], want[3]);
    if (bad) snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "; %d of %d pixels", bad, W * W);
    check(!bad, what, msg);
}

static const uint8_t RED[4] = {255, 0, 0, 255}, GREEN[4] = {0, 255, 0, 255}, BLUE[4] = {0, 0, 255, 255}, NONE[4] = {0, 0, 0, 0};

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dynstate_check", 1, "dxvk", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceFeatures2);
    L(vkEnumerateDeviceExtensionProperties); L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceMemoryProperties);
    L(vkGetPhysicalDeviceFormatProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr); L(vkDestroyInstance);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("GPU: %s, Vulkan %u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
    if (props.apiVersion < VK_API_VERSION_1_3) { printf("SKIP a Vulkan 1.3 device is needed\n0 checks, 0 failed\n"); return 0; }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    int eds3 = 0;
    for (uint32_t i = 0; i < ne; i++) eds3 |= !strcmp(ext[i].extensionName, "VK_EXT_extended_dynamic_state3");
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT f3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f3};
    if (eds3) vkGetPhysicalDeviceFeatures2(pd, &f2);
    int smask = eds3 && f3.extendedDynamicState3SampleMask;

    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT f3en = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    f3en.extendedDynamicState3SampleMask = VK_TRUE;
    const char *names[] = {"VK_EXT_extended_dynamic_state3"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, smask ? &f3en : NULL, 0, 1, &qci, 0, NULL, smask ? 1 : 0, names, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    char msg[160];
    snprintf(msg, sizeof(msg), "VkResult %d%s", r, smask ? ", with the EDS3 sample mask" : ", no EDS3 sample mask");
    check(r == VK_SUCCESS, "vkCreateDevice", msg);
    if (r) goto out;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkFreeCommandBuffers); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer);
    D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage);
    D(vkGetImageMemoryRequirements); D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer);
    D(vkCreateShaderModule); D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkCmdBeginRenderPass); D(vkCmdEndRenderPass);
    D(vkCmdBindPipeline); D(vkCmdPushConstants); D(vkCmdBindVertexBuffers); D(vkCmdBindVertexBuffers2); D(vkCmdDraw);
    D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier); D(vkDeviceWaitIdle); D(vkCmdSetDepthWriteEnable); D(vkCmdSetDepthTestEnable);
    D(vkCmdSetDepthCompareOp); D(vkCmdSetCullMode); D(vkCmdSetStencilWriteMask); D(vkCmdSetStencilOp); D(vkCmdSetStencilReference);
    D(vkCmdSetStencilCompareMask); D(vkCmdSetSampleMaskEXT);
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* ---- target: RGBA8 + a depth/stencil format the GPU has */
    VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM, dfmt = VK_FORMAT_UNDEFINED;
    const VkFormat dcand[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT};
    for (int i = 0; i < 3 && !dfmt; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, dcand[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) dfmt = dcand[i];
    }
    color = device_image(cfmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImage depth = dfmt ? device_image(dfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) : VK_NULL_HANDLE;
    if (!color || !depth) { check(0, "setup", "images"); goto out; }
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, color, VK_IMAGE_VIEW_TYPE_2D, cfmt, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView views[2];
    vkCreateImageView(dev, &vci, NULL, &views[0]);
    vci.image = depth, vci.format = dfmt, vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    vkCreateImageView(dev, &vci, NULL, &views[1]);
    VkAttachmentDescription att[2] = {
        {0, cfmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {0, dfmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_CLEAR,
         VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &cref, NULL, &dref, 0, NULL};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 2, att, 1, &sub, 1, &dep};
    vkCreateRenderPass(dev, &rpci, NULL, &rp);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 2, views, W, W, 1};
    vkCreateFramebuffer(dev, &fci, NULL, &fb);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    vkCreatePipelineLayout(dev, &plci, NULL, &layout);
    stages[0] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,
                                                  shader(spv_vert, sizeof(spv_vert)), "main", NULL};
    stages[1] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                  shader(spv_frag, sizeof(spv_frag)), "main", NULL};
    /* The colour attribute: zero at offset 0 (instance 0) and 4; (0, 1, 0, 0) at offset 16, which
     * instance 1 reads with stride 16 and not with 4. */
    float *vb;
    vbuf = host_buffer(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&vb);
    rbuf = host_buffer(W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&rb);
    if (!vbuf || !rbuf) { check(0, "setup", "buffers"); goto out; }
    memset(vb, 0, 256);
    vb[5] = 1.0f;
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);

    const VkStencilOpState s_never = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_NEVER, 0, 0, 0};
    const VkStencilOpState s_replace1 = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 1};
    const VkStencilOpState s_eq0 = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xff, 0, 0};
    const VkStencilOpState s_eq5 = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xff, 0, 5};
    VkCommandBuffer cb;

    /* depth write */
    {
        const VkDynamicState d[] = {VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
        pdesc pdw = {d, 1, VK_TRUE, VK_TRUE, VK_FALSE, VK_COMPARE_OP_LESS, s_never, VK_CULL_MODE_NONE, 0, NULL};
        VkPipeline a = pipeline(&pdw), b = pipeline(&pdw);
        cb = begin_case();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a);
        vkCmdSetDepthWriteEnable(cb, VK_FALSE);
        draw(cb, 1, 0, 0, 0.5f, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b);
        draw(cb, 0, 1, 0, 0.7f, 0);
        draw(cb, 0, 0, 1, 0.8f, 0);
        end_case(cb, "depth write OFF across a bind (green: the second pipeline wrote depth)", BLUE);
    }
    /* depth test enable, depth compare op: the other half static (test ON, NEVER) */
    for (int k = 0; k < 2; k++) {
        const VkDynamicState d[] = {k ? VK_DYNAMIC_STATE_DEPTH_COMPARE_OP : VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE};
        pdesc p = {d, 1, VK_TRUE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_NEVER, s_never, VK_CULL_MODE_NONE, 0, NULL};
        VkPipeline a = pipeline(&p), b = pipeline(&p);
        cb = begin_case();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a);
        if (k)
            vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
        else
            vkCmdSetDepthTestEnable(cb, VK_FALSE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b);
        draw(cb, 0, 1, 0, 0.5f, 0);
        end_case(cb, k ? "depth compare ALWAYS across a bind (created NEVER)" : "depth test OFF across a bind (created ON, compare NEVER)", GREEN);
    }
    /* cull mode */
    {
        const VkDynamicState d[] = {VK_DYNAMIC_STATE_CULL_MODE};
        pdesc p = {d, 1, VK_FALSE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS, s_never, VK_CULL_MODE_FRONT_AND_BACK, 0, NULL};
        VkPipeline a = pipeline(&p), b = pipeline(&p);
        cb = begin_case();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a);
        vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b);
        draw(cb, 0, 1, 0, 0.5f, 0);
        end_case(cb, "cull mode NONE across a bind (created FRONT_AND_BACK)", GREEN);
    }
    /* stencil write mask */
    {
        const VkDynamicState d[] = {VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
        pdesc p = {d, 1, VK_FALSE, VK_FALSE, VK_TRUE, VK_COMPARE_OP_ALWAYS, s_replace1, VK_CULL_MODE_NONE, 0, NULL};
        pdesc pc = {NULL, 0, VK_FALSE, VK_FALSE, VK_TRUE, VK_COMPARE_OP_ALWAYS, s_eq0, VK_CULL_MODE_NONE, 0, NULL};
        VkPipeline a = pipeline(&p), b = pipeline(&p), c = pipeline(&pc);
        cb = begin_case();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b);
        draw(cb, 0, 1, 0, 0.5f, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c);
        draw(cb, 0, 0, 1, 0.5f, 0);
        end_case(cb, "stencil write mask 0 across a bind (green: the stencil was written)", BLUE);
    }
    /* stencil op, reference, compare mask */
    {
        const VkDynamicState d[] = {VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK};
        VkStencilOpState s = s_never;
        s.writeMask = 0xff;
        pdesc p = {d, 3, VK_FALSE, VK_FALSE, VK_TRUE, VK_COMPARE_OP_ALWAYS, s, VK_CULL_MODE_NONE, 0, NULL};
        pdesc pc = {NULL, 0, VK_FALSE, VK_FALSE, VK_TRUE, VK_COMPARE_OP_ALWAYS, s_eq5, VK_CULL_MODE_NONE, 0, NULL};
        VkPipeline a = pipeline(&p), b = pipeline(&p), c = pipeline(&pc);
        cb = begin_case();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 5);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b);
        draw(cb, 0, 1, 0, 0.5f, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c);
        draw(cb, 0, 0, 1, 0.5f, 0);
        end_case(cb, "stencil op REPLACE, reference 5, compare mask 0xff across a bind (created NEVER, 0, 0)", BLUE);
    }
    /* vertex stride */
    {
        const VkDynamicState d[] = {VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
        pdesc p = {d, 1, VK_FALSE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS, s_never, VK_CULL_MODE_NONE, 4, NULL};
        VkPipeline a = pipeline(&p), b = pipeline(&p);
        cb = begin_case();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a);
        VkDeviceSize off = 0, stride = 16;
        vkCmdBindVertexBuffers2(cb, 0, 1, &vbuf, &off, NULL, &stride);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b);
        draw(cb, 0, 0, 0, 0.5f, 1);
        end_case(cb, "vertex stride 16 across a bind (created 4)", GREEN);
    }
    /* sample mask (EDS3) */
    if (smask) {
        const VkDynamicState d[] = {VK_DYNAMIC_STATE_SAMPLE_MASK_EXT};
        const VkSampleMask one = 1;
        pdesc p = {d, 1, VK_FALSE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS, s_never, VK_CULL_MODE_NONE, 0, &one};
        pdesc pc = {NULL, 0, VK_FALSE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS, s_never, VK_CULL_MODE_NONE, 0, NULL};
        VkPipeline a = pipeline(&p), b = pipeline(&p), c = pipeline(&pc);
        const VkSampleMask zero = 0;
        cb = begin_case();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a);
        vkCmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &zero);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, b);
        draw(cb, 0, 1, 0, 0.5f, 0);
        end_case(cb, "sample mask 0 across a bind (created 1; green: written)", NONE);
        cb = begin_case();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a);
        vkCmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &zero);
        draw(cb, 0, 1, 0, 0.5f, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c);
        draw(cb, 0, 0, 1, 0.5f, 0);
        end_case(cb, "a static sample mask (all) after a dynamic 0 (3893903; nothing: the static pipeline took the 0)", BLUE);
    } else
        printf("SKIP sample mask: no extendedDynamicState3SampleMask\n");
    (void)RED;
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
out:
    vkDestroyInstance(inst, NULL);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
