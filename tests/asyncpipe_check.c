/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * asyncpipe_check: the asynchronous pipeline compile (server_async.c) with a slow compiler
 * imitated on the server (VKBRIDGE_ASYNC_COMPILE_IMITATE_MS, 400 in host/asyncpipe_test.sh). A
 * triangle over a 4x4 target, read back each frame:
 *   async  (VKBRIDGE_ASYNC_COMPILE=1)
 *     1  a colour pipeline with vertex input comes back at once; its draws are skipped (the target
 *        keeps its clear colour) until it is compiled, then drawn (red);
 *     2  a depth-only pipeline compiles synchronously (the call takes the delay) and its first
 *        draw writes depth;
 *     3  a colour pipeline without vertex input (a full-screen pass) compiles synchronously too
 *        (async2, VKBRIDGE_ASYNC_COMPILE=2: in the background, its draws skipped);
 *     4  the shader modules and the layout destroyed right after the create (as DXVK does): the
 *        background compile still has them, the pipeline draws;
 *     5  a pipeline destroyed while it compiles, and one left compiling at vkDestroyDevice;
 *   sync   (VKBRIDGE_ASYNC_COMPILE=0): the colour pipeline's create takes the delay, its first
 *          draw is drawn;
 *   quick  (a compile within VKBRIDGE_ASYNC_COMPILE_WAIT_MS): the real pipeline comes back, the
 *          first draw is drawn.
 * Shaders: cullmode_check's vertex shader and dynstate_check's fragment shader.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const uint32_t spv_vert[] = {
#include "cullmode_vert.inc"
};
static const uint32_t spv_frag[] = {
#include "dynstate_frag.inc"
};

#define W 4
#define DELAY_MS 400 /* host/asyncpipe_test.sh's VKBRIDGE_ASYNC_COMPILE_IMITATE_MS */

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
I(vkCreateFramebuffer); I(vkCreateShaderModule); I(vkDestroyShaderModule); I(vkCreatePipelineLayout); I(vkDestroyPipelineLayout);
I(vkCreateGraphicsPipelines); I(vkDestroyPipeline); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass); I(vkCmdBindPipeline);
I(vkCmdPushConstants); I(vkCmdDraw); I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier); I(vkDestroyDevice);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static VkPhysicalDeviceMemoryProperties mp;

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkRenderPass rp_color, rp_depth;
static VkFramebuffer fb_color, fb_depth;
static VkImage color, depth;
static VkBuffer rbuf;
static uint8_t *rb;
static VkCommandPool cpool;
static VkPipelineLayout push_layout;

typedef struct objs {
    VkShaderModule vs, fs;
    VkPipelineLayout layout;
} objs;

static int make_objs(objs *o)
{
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_vert), spv_vert};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    if (vkCreateShaderModule(dev, &smci, NULL, &o->vs)) return -1;
    smci.codeSize = sizeof(spv_frag), smci.pCode = spv_frag;
    if (vkCreateShaderModule(dev, &smci, NULL, &o->fs)) return -1;
    return vkCreatePipelineLayout(dev, &plci, NULL, &o->layout) ? -1 : 0;
}

static void free_objs(objs *o)
{
    vkDestroyShaderModule(dev, o->vs, NULL);
    vkDestroyShaderModule(dev, o->fs, NULL);
    vkDestroyPipelineLayout(dev, o->layout, NULL);
}

enum { P_COLOR, P_DEPTH, P_NO_VERTEX_INPUT };

/* kind's pipeline; *ms: how long the create took */
static VkPipeline pipeline(const objs *o, int kind, double *ms)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, o->vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, o->fs, "main", NULL},
    };
    /* a binding without attributes: vertex input as a game's draws have it, nothing fetched */
    VkVertexInputBindingDescription vb = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, kind != P_NO_VERTEX_INPUT, &vb, 0,
                                               NULL};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkViewport vp = {0, 0, W, W, 0, 1};
    VkRect2D sc = {{0, 0}, {W, W}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms_ = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, kind != P_DEPTH, &cba, {0}};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_TRUE,
                                                VK_COMPARE_OP_LESS_OR_EQUAL};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms_,
                                       kind == P_DEPTH ? &ds : NULL, &cbs, NULL, o->layout, kind == P_DEPTH ? rp_depth : rp_color, 0,
                                       VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    double t0 = now_ms();
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    *ms = now_ms() - t0;
    if (r) printf("     vkCreateGraphicsPipelines: %d\n", r);
    return r ? VK_NULL_HANDLE : p;
}

/* One frame: the pipeline's triangle (red) into a cleared target; returns pixel (1, 1) as RGBA, or
 * for the depth pass its depth * 1000. */
static long frame(VkPipeline p, int kind)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 0}}};
    if (kind == P_DEPTH) clear.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, kind == P_DEPTH ? rp_depth : rp_color,
                                 kind == P_DEPTH ? fb_depth : fb_color, {{0, 0}, {W, W}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    /* any layout with the range will do for the push (compatible): the pipeline's own may be gone */
    float pc[8] = {1, 0, 0, 1, 0, 0, 0, 0};
    vkCmdPushConstants(cb, push_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy cp = {0, 0, 0, {kind == P_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
    vkCmdCopyImageToBuffer(cb, kind == P_DEPTH ? depth : color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    memset(rb, 0x55, W * W * 4);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) return -1;
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
    const uint8_t *px = rb + (1 * W + 1) * 4;
    if (kind == P_DEPTH) {
        float d;
        memcpy(&d, px, 4);
        return (long)(d * 1000 + 0.5f);
    }
    return (long)px[0] | (long)px[1] << 8 | (long)px[2] << 16 | (long)px[3] << 24;
}

#define RED 0xFF0000FFL

/* Frames every 25 ms until the triangle is there (at most 5 s): the frames that skipped it */
static int frames_until_drawn(VkPipeline p, double *ms)
{
    double t0 = now_ms();
    int skipped = 0;
    while (now_ms() - t0 < 5000) {
        long v = frame(p, P_COLOR);
        if (v == RED) break;
        if (v != 0) return -1; /* neither drawn nor the clear colour */
        skipped++;
        usleep(25000);
    }
    *ms = now_ms() - t0;
    return now_ms() - t0 < 5000 ? skipped : -1;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "async";
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "asyncpipe_check", 1, "dxvk", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceQueueFamilyProperties);
    L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("FAIL vkCreateDevice\n"); return 1; }
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkFreeCommandBuffers); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule);
    D(vkDestroyShaderModule); D(vkCreatePipelineLayout); D(vkDestroyPipelineLayout); D(vkCreateGraphicsPipelines); D(vkDestroyPipeline);
    D(vkCmdBeginRenderPass); D(vkCmdEndRenderPass); D(vkCmdBindPipeline); D(vkCmdPushConstants); D(vkCmdDraw);
    D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier); D(vkDestroyDevice);
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* targets: RGBA8 colour, D32 depth; a readback buffer */
    VkImage *imgs[2] = {&color, &depth};
    VkFormat fmts[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT};
    VkImageUsageFlags use[2] = {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    VkImageView views[2];
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmts[i], {W, W, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_TILING_OPTIMAL, use[i] | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                VK_IMAGE_LAYOUT_UNDEFINED};
        VkMemoryRequirements mr;
        VkDeviceMemory m;
        if (vkCreateImage(dev, &ii, NULL, imgs[i])) { check(0, "setup", "image"); goto out; }
        vkGetImageMemoryRequirements(dev, *imgs[i], &mr);
        uint32_t t = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (t == UINT32_MAX) t = find_type(mr.memoryTypeBits, 0);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
        if (vkAllocateMemory(dev, &mai, NULL, &m) || vkBindImageMemory(dev, *imgs[i], m, 0)) { check(0, "setup", "image memory"); goto out; }
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, *imgs[i], VK_IMAGE_VIEW_TYPE_2D, fmts[i], {0},
                                     {i ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(dev, &vci, NULL, &views[i])) { check(0, "setup", "view"); goto out; }
    }
    {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkMemoryRequirements mr;
        VkDeviceMemory m;
        if (vkCreateBuffer(dev, &bci, NULL, &rbuf)) { check(0, "setup", "buffer"); goto out; }
        vkGetBufferMemoryRequirements(dev, rbuf, &mr);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                    find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &m) || vkBindBufferMemory(dev, rbuf, m, 0) ||
            vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&rb)) {
            check(0, "setup", "readback memory");
            goto out;
        }
    }
    for (int i = 0; i < 2; i++) {
        VkAttachmentDescription att = {0, fmts[i], VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                       VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        VkAttachmentReference ref = {0, i ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, i ? 0 : 1, i ? NULL : &ref, NULL, i ? &ref : NULL, 0, NULL};
        VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL,
                                   i ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   i ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT, 0};
        VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
        VkRenderPass *rp = i ? &rp_depth : &rp_color;
        vkCreateRenderPass(dev, &rpci, NULL, rp);
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, *rp, 1, &views[i], W, W, 1};
        vkCreateFramebuffer(dev, &fci, NULL, i ? &fb_depth : &fb_color);
    }
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);
    objs o;
    if (make_objs(&o)) { check(0, "setup", "modules"); goto out; }
    push_layout = o.layout;
    char msg[200];
    double ms, ready_ms;

    if (!strcmp(mode, "sync") || !strcmp(mode, "quick")) {
        VkPipeline p = pipeline(&o, P_COLOR, &ms);
        long v = p ? frame(p, P_COLOR) : -1;
        int slow = !strcmp(mode, "sync");
        snprintf(msg, sizeof(msg), "create %.0f ms, first frame pixel 0x%08lx", ms, v);
        check(p && v == RED && (slow ? ms >= DELAY_MS * 0.9 : 1),
              slow ? "synchronous: the create takes the compile, the first draw is drawn" : "compiled within the wait: the first draw is drawn", msg);
        if (p) vkDestroyPipeline(dev, p, NULL);
        goto out;
    }

    /* 1 */
    VkPipeline p = pipeline(&o, P_COLOR, &ms);
    int skipped = p ? frames_until_drawn(p, &ready_ms) : -1;
    snprintf(msg, sizeof(msg), "create %.0f ms; %d frame(s) skipped the draw, drawn after %.0f ms", ms, skipped, ready_ms);
    check(p && ms < DELAY_MS / 2 && skipped >= 1 && ready_ms >= DELAY_MS * 0.6, "colour pipeline: back at once, draws skipped, then drawn",
          msg);
    if (p) {
        long v = frame(p, P_COLOR);
        snprintf(msg, sizeof(msg), "pixel 0x%08lx", v);
        check(v == RED, "the same pipeline, the next frame: drawn", msg);
        vkDestroyPipeline(dev, p, NULL);
    }
    /* 2 */
    p = pipeline(&o, P_DEPTH, &ms);
    long d = p ? frame(p, P_DEPTH) : -1;
    snprintf(msg, sizeof(msg), "create %.0f ms, depth at (1, 1) %.3f", ms, d / 1000.0);
    check(p && ms >= DELAY_MS * 0.9 && d == 500, "depth-only pipeline: synchronous, its first draw writes depth", msg);
    if (p) vkDestroyPipeline(dev, p, NULL);
    /* 3 */
    long v;
    p = pipeline(&o, P_NO_VERTEX_INPUT, &ms);
    if (!strcmp(mode, "async2")) {
        skipped = p ? frames_until_drawn(p, &ready_ms) : -1;
        snprintf(msg, sizeof(msg), "create %.0f ms; %d frame(s) skipped the draw, drawn after %.0f ms", ms, skipped, ready_ms);
        check(p && ms < DELAY_MS / 2 && skipped >= 1, "colour pipeline without vertex input, VKBRIDGE_ASYNC_COMPILE=2: in the background too",
              msg);
    } else {
        v = p ? frame(p, P_COLOR) : -1;
        snprintf(msg, sizeof(msg), "create %.0f ms, first frame pixel 0x%08lx", ms, v);
        check(p && ms >= DELAY_MS * 0.9 && v == RED, "colour pipeline without vertex input: synchronous, drawn at once", msg);
    }
    if (p) vkDestroyPipeline(dev, p, NULL);
    /* 4 */
    objs o2;
    if (make_objs(&o2)) { check(0, "setup", "modules"); goto out; }
    p = pipeline(&o2, P_COLOR, &ms);
    free_objs(&o2); /* while it compiles */
    skipped = p ? frames_until_drawn(p, &ready_ms) : -1;
    snprintf(msg, sizeof(msg), "create %.0f ms; %d frame(s) skipped, drawn after %.0f ms", ms, skipped, ready_ms);
    check(p && skipped >= 1, "modules and layout destroyed right after the create: drawn once compiled", msg);
    if (p) vkDestroyPipeline(dev, p, NULL);
    /* 5 */
    p = pipeline(&o, P_COLOR, &ms);
    if (p) vkDestroyPipeline(dev, p, NULL); /* still compiling */
    VkPipeline left = pipeline(&o, P_COLOR, &ms);
    usleep(DELAY_MS * 1500);
    v = frame(left, P_COLOR);
    snprintf(msg, sizeof(msg), "pixel 0x%08lx", v);
    check(left && v == RED, "a pipeline destroyed while it compiled, another one compiled meanwhile and drawn", msg);
    left = pipeline(&o, P_COLOR, &ms); /* left compiling at vkDestroyDevice */
    vkDestroyDevice(dev, NULL);
    dev = VK_NULL_HANDLE;
    check(1, "vkDestroyDevice with a pipeline still compiling", "");
out:
    if (failures)
        printf("FAIL: %d of %d checks failed\n", failures, checks);
    else
        printf("OK: %d of %d checks passed\n", checks, checks);
    return failures ? 1 : 0;
}
