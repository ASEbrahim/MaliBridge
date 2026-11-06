/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * imageless_check: VK_KHR_imageless_framebuffer, as Zink uses it (every Mesa since 22.x requires it;
 * Mesa 26.0.8, the DroidDeck Ubuntu runtime's, fails "ZINK: VK_KHR_imageless_framebuffer required!").
 * On a driver without it (Mali r18-r26, Vulkan 1.1) the bridge makes a real framebuffer per set of
 * views at vkCmdBeginRenderPass (server_emu_imageless.c); host/imageless_test.sh runs this with the
 * emulation and without it. One render pass (RGBA8, CLEAR/STORE), one imageless framebuffer, two
 * 64x64 targets A and B, a triangle over the upper-left half (white), read back:
 *   1  A cleared red + triangle; 2  B cleared blue + triangle, A unchanged;
 *   3  A again, cleared green (the same views: no new framebuffer);
 *   4  vkCmdBeginRenderPass2 with B (where the device has it), cleared yellow;
 *   5  the draw in a secondary command buffer inheriting the imageless framebuffer, A cleared cyan;
 *   6  B's view destroyed, a new view of B (the driver may hand out the same handle) cleared red + triangle;
 *   7  20 begins alternating A / B in one command buffer (no new framebuffers: host/imageless_test.sh counts);
 *   8  an ordinary framebuffer with A's view next to it (passed through), cleared blue;
 *   9  the imageless framebuffer destroyed and made again, A cleared green.
 * Prints PASS/FAIL per check and "OK: n of n checks passed" / "FAIL: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* glslc -O --target-env=vulkan1.1 (polygon_check's): a triangle from the vertex buffer, white */
static const uint32_t spv_vert[] = {
#include "polygon_vert.inc"
};
static const uint32_t spv_frag[] = {
#include "polygon_frag.inc"
};

#define W 64
#define H 64
#define WHITE 0xffffffffu
#define RED 0xff0000ffu /* as bytes R,G,B,A read big-endian */
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
I(vkDestroyImageView); I(vkCreateShaderModule); I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkCmdBindPipeline);
I(vkCmdBindVertexBuffers); I(vkCmdDraw); I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier); I(vkCmdExecuteCommands);
I(vkCreateRenderPass); I(vkCreateFramebuffer); I(vkDestroyFramebuffer); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass);
I(vkCmdBeginRenderPass2); I(vkCmdEndRenderPass2); I(vkResetCommandBuffer); I(vkDestroyDevice); I(vkDestroyInstance);
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

static const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
static const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

static VkImage make_image(void)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
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

static VkImageView make_view(VkImage img)
{
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(dev, &vci, NULL, &v);
    return v;
}

static void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkCommandPool pool;
static VkCommandBuffer cb, sec;
static VkBuffer rbuf, vbuf;
static uint8_t *pixels;
static VkRenderPass rp;
static VkPipeline pipe_;

static void begin(void)
{
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
}

/* The target (COLOR_ATTACHMENT_OPTIMAL, where the render pass leaves it) read back. */
static void finish_and_read(VkImage img)
{
    barrier(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r);
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
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

static const int pts2[][2] = {{8, 8}, {56, 56}};

static void expect2(const char *what, uint32_t inside, uint32_t outside)
{
    char d[160] = "";
    uint32_t want[2] = {inside, outside};
    int ok = 1;
    for (int i = 0; i < 2 && ok; i++) {
        uint32_t got = px(pts2[i][0], pts2[i][1]);
        if (got != want[i]) {
            ok = 0;
            snprintf(d, sizeof(d), "(%d,%d) is %08x, want %08x", pts2[i][0], pts2[i][1], got, want[i]);
        }
    }
    check(ok, what, d);
}

static VkClearValue clear_of(uint32_t rgba)
{
    VkClearValue c;
    c.color.float32[0] = (float)(rgba >> 24 & 0xff) / 255.0f;
    c.color.float32[1] = (float)(rgba >> 16 & 0xff) / 255.0f;
    c.color.float32[2] = (float)(rgba >> 8 & 0xff) / 255.0f;
    c.color.float32[3] = (float)(rgba & 0xff) / 255.0f;
    return c;
}

static void draw(VkCommandBuffer c)
{
    VkDeviceSize off = 0;
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
    vkCmdBindVertexBuffers(c, 0, 1, &vbuf, &off);
    vkCmdDraw(c, 3, 1, 0, 0);
}

/* A render pass on fb with view (imageless: through VkRenderPassAttachmentBeginInfo), cleared to rgba. */
static void pass(VkFramebuffer fb, int imageless, VkImageView view, uint32_t rgba, int with_draw, VkSubpassContents contents, int v2)
{
    VkRenderPassAttachmentBeginInfo ab = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO, NULL, 1, &view};
    VkClearValue cv = clear_of(rgba);
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, imageless ? &ab : NULL, rp, fb, {{0, 0}, {W, H}}, 1, &cv};
    if (v2) {
        VkSubpassBeginInfo sb = {VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, NULL, contents};
        VkSubpassEndInfo se = {VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
        vkCmdBeginRenderPass2(cb, &bi, &sb);
        if (with_draw) draw(cb);
        vkCmdEndRenderPass2(cb, &se);
        return;
    }
    vkCmdBeginRenderPass(cb, &bi, contents);
    if (contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS) vkCmdExecuteCommands(cb, 1, &sec);
    else if (with_draw) draw(cb);
    vkCmdEndRenderPass(cb);
}

static VkFramebuffer make_imageless(void)
{
    VkFramebufferAttachmentImageInfo ai = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO, NULL, 0, usage, W, H, 1, 1, &fmt};
    VkFramebufferAttachmentsCreateInfo aci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO, NULL, 1, &ai};
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, &aci, VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, rp, 1, NULL, W, H, 1};
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult r = vkCreateFramebuffer(dev, &fci, NULL, &fb);
    char msg[64];
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    check(r == VK_SUCCESS && fb, "vkCreateFramebuffer with VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT", msg);
    return r == VK_SUCCESS ? fb : VK_NULL_HANDLE;
}

static int has(const VkExtensionProperties *ep, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ep[i].extensionName, name)) return 1;
    return 0;
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL: no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "imageless_check", 1, "zink", 1, VK_API_VERSION_1_1};
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
    int core12 = props.apiVersion >= VK_API_VERSION_1_2;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne + 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    int has_il = has(ep, ne, "VK_KHR_imageless_framebuffer"), has_rp2 = has(ep, ne, "VK_KHR_create_renderpass2");
    printf("GPU: %s, Vulkan %u.%u, VK_KHR_imageless_framebuffer %s\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), has_il ? "listed" : "not listed");
    VkPhysicalDeviceImagelessFramebufferFeatures ilf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &ilf};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    char msg[200];
    snprintf(msg, sizeof(msg), "extension %s, feature %u", has_il ? "listed" : "missing", ilf.imagelessFramebuffer);
    check((core12 || has_il) && ilf.imagelessFramebuffer, "imageless framebuffers offered", msg);
    if (!ilf.imagelessFramebuffer || !(core12 || has_il)) goto out;

    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    const char *exts[8];
    uint32_t nx = 0;
    static const char *const want[] = {"VK_KHR_imageless_framebuffer", "VK_KHR_maintenance2", "VK_KHR_image_format_list",
                                       "VK_KHR_create_renderpass2", "VK_KHR_multiview"};
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        if (has(ep, ne, want[i])) exts[nx++] = want[i];
    free(ep);
    VkPhysicalDeviceImagelessFramebufferFeatures en = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, NULL, VK_TRUE};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en, 0, 1, &qci, 0, NULL, nx, exts, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    check(r == VK_SUCCESS, "vkCreateDevice with imagelessFramebuffer", msg);
    if (r) goto out;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory);
    D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkBindImageMemory);
    D(vkCreateImageView); D(vkDestroyImageView); D(vkCreateShaderModule); D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines);
    D(vkCmdBindPipeline); D(vkCmdBindVertexBuffers); D(vkCmdDraw); D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier);
    D(vkCmdExecuteCommands); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkDestroyFramebuffer); D(vkCmdBeginRenderPass);
    D(vkCmdEndRenderPass); D(vkResetCommandBuffer); D(vkDestroyDevice);
#undef D
    if (core12) {
        vkCmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2)vkGetDeviceProcAddr(dev, "vkCmdBeginRenderPass2");
        vkCmdEndRenderPass2 = (PFN_vkCmdEndRenderPass2)vkGetDeviceProcAddr(dev, "vkCmdEndRenderPass2");
    } else if (has_rp2) {
        vkCmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2)vkGetDeviceProcAddr(dev, "vkCmdBeginRenderPass2KHR");
        vkCmdEndRenderPass2 = (PFN_vkCmdEndRenderPass2)vkGetDeviceProcAddr(dev, "vkCmdEndRenderPass2KHR");
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkImage ia = make_image(), ib = make_image();
    VkImageView va = ia ? make_view(ia) : VK_NULL_HANDLE, vb_ = ib ? make_view(ib) : VK_NULL_HANDLE;
    float *vmap;
    vbuf = host_buffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&vmap);
    rbuf = host_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&pixels);
    if (!va || !vb_ || !vbuf || !rbuf) { check(0, "setup", "images / buffers"); goto out; }
    const float tri[] = {-1, -1, 1, -1, -1, 1}; /* the upper-left half (x + y < 64) */
    memcpy(vmap, tri, sizeof(tri));

    VkAttachmentDescription ad = {0, fmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 0, NULL};
    if (vkCreateRenderPass(dev, &rpci, NULL, &rp)) { check(0, "setup", "render pass"); goto out; }

    VkPipelineLayout layout;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    vkCreatePipelineLayout(dev, &plci, NULL, &layout);
    VkShaderModuleCreateInfo smv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_vert), spv_vert};
    VkShaderModuleCreateInfo smf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_frag), spv_frag};
    VkShaderModule mv = VK_NULL_HANDLE, mf = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &smv, NULL, &mv);
    vkCreateShaderModule(dev, &smf, NULL, &mf);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, mv, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mf, "main", NULL}};
    VkVertexInputBindingDescription bd = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vad = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &bd, 1, &vad};
    VkPipelineInputAssemblyStateCreateInfo ias = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY,
                                               1, &cba};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ias, NULL, &vps, &rs, &ms,
                                        NULL, &cbs, NULL, layout, rp, 0, VK_NULL_HANDLE, -1};
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe_) || !pipe_) { check(0, "setup", "pipeline"); goto out; }

    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo pai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBufferAllocateInfo sai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1};
    if (vkAllocateCommandBuffers(dev, &pai, &cb) || vkAllocateCommandBuffers(dev, &sai, &sec)) { check(0, "setup", "command buffers"); goto out; }

    VkFramebuffer fb = make_imageless();
    if (!fb) goto out;

    /* 1, 2: A then B */
    begin();
    barrier(cb, ia, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(cb, ib, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pass(fb, 1, va, RED, 1, VK_SUBPASS_CONTENTS_INLINE, 0);
    finish_and_read(ia);
    expect2("1 A: cleared red, triangle drawn", WHITE, RED);
    begin();
    pass(fb, 1, vb_, BLUE, 1, VK_SUBPASS_CONTENTS_INLINE, 0);
    finish_and_read(ib);
    expect2("2 B: cleared blue, triangle drawn", WHITE, BLUE);
    begin();
    finish_and_read(ia);
    expect2("2 A untouched by B's pass", WHITE, RED);

    /* 3: A again (the same views) */
    begin();
    pass(fb, 1, va, GREEN, 1, VK_SUBPASS_CONTENTS_INLINE, 0);
    finish_and_read(ia);
    expect2("3 A again: cleared green", WHITE, GREEN);

    /* 4: vkCmdBeginRenderPass2 */
    if (vkCmdBeginRenderPass2 && vkCmdEndRenderPass2) {
        begin();
        pass(fb, 1, vb_, YELLOW, 0, VK_SUBPASS_CONTENTS_INLINE, 1);
        finish_and_read(ib);
        expect2("4 vkCmdBeginRenderPass2 with B: cleared yellow", YELLOW, YELLOW);
    } else {
        printf("(no vkCmdBeginRenderPass2 on this device)\n");
    }

    /* 5: a secondary inheriting the imageless framebuffer */
    VkCommandBufferInheritanceInfo ii = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, rp, 0, fb, VK_FALSE, 0, 0};
    VkCommandBufferBeginInfo sbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &ii};
    vkBeginCommandBuffer(sec, &sbi);
    draw(sec);
    vkEndCommandBuffer(sec);
    begin();
    pass(fb, 1, va, CYAN, 0, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS, 0);
    finish_and_read(ia);
    expect2("5 secondary inheriting the imageless framebuffer: A cleared cyan", WHITE, CYAN);

    /* 6: B's view destroyed, a new one made */
    vkDestroyImageView(dev, vb_, NULL);
    VkImageView old = vb_;
    vb_ = make_view(ib);
    printf("(B's new view %s the old handle)\n", vb_ == old ? "reuses" : "does not reuse");
    begin();
    pass(fb, 1, vb_, RED, 1, VK_SUBPASS_CONTENTS_INLINE, 0);
    finish_and_read(ib);
    expect2("6 a new view of B after the old one was destroyed: cleared red", WHITE, RED);

    /* 7: 20 begins alternating A and B; the last on A, cleared blue */
    begin();
    for (int i = 0; i < 20; i++) pass(fb, 1, (i & 1) ? va : vb_, (i & 1) ? BLUE : GREEN, 1, VK_SUBPASS_CONTENTS_INLINE, 0);
    finish_and_read(ia);
    expect2("7 20 alternating begins: A last, cleared blue", WHITE, BLUE);

    /* 8: an ordinary framebuffer with A's view */
    VkFramebufferCreateInfo ofci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &va, W, H, 1};
    VkFramebuffer ofb = VK_NULL_HANDLE;
    r = vkCreateFramebuffer(dev, &ofci, NULL, &ofb);
    if (r == VK_SUCCESS) {
        begin();
        pass(ofb, 0, va, YELLOW, 1, VK_SUBPASS_CONTENTS_INLINE, 0);
        finish_and_read(ia);
        expect2("8 an ordinary framebuffer next to it: A cleared yellow", WHITE, YELLOW);
        vkDestroyFramebuffer(dev, ofb, NULL);
    } else {
        snprintf(msg, sizeof(msg), "VkResult %d", r);
        check(0, "8 an ordinary framebuffer", msg);
    }

    /* 9: destroyed and made again */
    vkDestroyFramebuffer(dev, fb, NULL);
    fb = make_imageless();
    if (fb) {
        begin();
        pass(fb, 1, va, GREEN, 1, VK_SUBPASS_CONTENTS_INLINE, 0);
        finish_and_read(ia);
        expect2("9 the framebuffer made again: A cleared green", WHITE, GREEN);
        vkDestroyFramebuffer(dev, fb, NULL);
    }
    vkDestroyImageView(dev, va, NULL);
    vkDestroyImageView(dev, vb_, NULL);

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
