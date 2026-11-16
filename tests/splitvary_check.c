/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * splitvary_check: a fragment input split by Component decorations at one Location (vec2 at component 0
 * + float at component 3), fed by ONE vec4 vertex output at that Location. This is the interface Zink's
 * fixed-function texturing makes (GoldSrc / Half-Life menu text, a player log): legal Vulkan, but
 * suspected to be mis-linked by Mali drivers (texcoords not interpolated -> glyphs read from wrong rows).
 * A quad over a 32x32 RGBA8 target; every case must write (u, v, (1+u)/2, v) at every pixel:
 *   vec4-split   VS vec4 @1      -> FS vec2 @1 + float @1 c3   (Zink fixed-function; the suspect)
 *   vec4-merged  VS vec4 @1      -> FS vec4 @1                 (merged interface)
 *   split-split  VS vec2 + float -> FS vec2 @1 + float @1 c3   (Zink linked GLSL program)
 *   vec4-st      VS vec4 @1      -> FS vec2 @1 only
 *   vec4-q       VS vec4 @1      -> FS float @1 c3 only
 *   zink-split   vec4-split written like Zink's own SPIR-V (1.6, Vulkan memory model, plain Position;
 *                shaders/splitvary/zink.*.spvasm, spirv-as --target-env vulkan1.3)
 * Shaders: tests/shaders/splitvary/ (glslc -O --target-env=vulkan1.1 -mfmt=num -> splitvary_*.inc).
 * SPLITVARY_CASE=<name> runs one case only.
 */
#define VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t spv_vec4_vert[] = {
#include "splitvary_vec4_vert.inc"
};
static const uint32_t spv_split_vert[] = {
#include "splitvary_split_vert.inc"
};
static const uint32_t spv_split_frag[] = {
#include "splitvary_split_frag.inc"
};
static const uint32_t spv_merged_frag[] = {
#include "splitvary_merged_frag.inc"
};
static const uint32_t spv_st_frag[] = {
#include "splitvary_st_frag.inc"
};
static const uint32_t spv_q_frag[] = {
#include "splitvary_q_frag.inc"
};
static const uint32_t spv_zink_vert[] = {
#include "splitvary_zink_vert.inc"
};
static const uint32_t spv_zink_frag[] = {
#include "splitvary_zink_frag.inc"
};

#define W 32

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
I(vkCmdEndRenderPass); I(vkCmdBindPipeline); I(vkCmdDraw); I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier);
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

static VkPipeline pipeline(const uint32_t *vsc, size_t vsz, const uint32_t *fsc, size_t fsz)
{
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, vsz, vsc};
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smci, NULL, &vs)) return VK_NULL_HANDLE;
    smci.codeSize = fsz, smci.pCode = fsc;
    if (vkCreateShaderModule(dev, &smci, NULL, &fs)) return VK_NULL_HANDLE;
    stages[0] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL};
    stages[1] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
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
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia, NULL, &vps, &rs, &ms,
                                       NULL, &cbs, NULL, layout, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    if (r) printf("     vkCreateGraphicsPipelines: %d\n", r);
    return p;
}

/* Draws the quad with p, reads the target back, compares every pixel with (u, v, (1+u)/2, v) */
static void run_case(const char *what, VkPipeline p)
{
    if (!p) { check(0, what, "pipeline"); return; }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, W}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdDraw(cb, 6, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
    vkCmdCopyImageToBuffer(cb, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    memset(rb, 0x55, W * W * 4);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) { check(0, what, "submit"); return; }
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
    int bad = 0, maxd = 0, fx = -1, fy = -1;
    uint8_t got[4] = {0}, want[4] = {0};
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            float u = (x + 0.5f) / W, v = (y + 0.5f) / W, e[4] = {u, v, (1 + u) * 0.5f, v};
            const uint8_t *px = rb + (y * W + x) * 4;
            int pbad = 0;
            for (int c = 0; c < 4; c++) {
                int d = abs((int)px[c] - (int)(e[c] * 255.0f + 0.5f));
                if (d > maxd) maxd = d;
                if (d > 2) pbad = 1;
            }
            if (pbad && !bad++) {
                fx = x, fy = y;
                memcpy(got, px, 4);
                for (int c = 0; c < 4; c++) want[c] = (uint8_t)(e[c] * 255.0f + 0.5f);
            }
        }
    const uint8_t *mid = rb + ((W / 2) * W + W / 2) * 4;
    char msg[256];
    snprintf(msg, sizeof(msg), "%d of %d px off by > 2 (max %d); centre (%u %u %u %u)", bad, W * W, maxd, mid[0], mid[1], mid[2], mid[3]);
    if (bad)
        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "; first (%d, %d) = (%u %u %u %u), wanted (%u %u %u %u)", fx, fy, got[0], got[1],
                 got[2], got[3], want[0], want[1], want[2], want[3]);
    check(!bad, what, msg);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW); /* Android */
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "splitvary_check", 1, "dxvk", 1, VK_API_VERSION_1_3};
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
    D(vkCmdDraw); D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier);
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
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    vkCreatePipelineLayout(dev, &plci, NULL, &layout);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);
    static const struct {
        const char *name;
        const uint32_t *vs;
        size_t vsz;
        const uint32_t *fs;
        size_t fsz;
    } cs[] = {
#define C(n, v, f) {n, spv_##v##_vert, sizeof(spv_##v##_vert), spv_##f##_frag, sizeof(spv_##f##_frag)}
        C("vec4-split", vec4, split), C("vec4-merged", vec4, merged), C("split-split", split, split), C("vec4-st", vec4, st),
        C("vec4-q", vec4, q), C("zink-split", zink, zink),
#undef C
    };
    const char *only = getenv("SPLITVARY_CASE");
    for (unsigned i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
        if (only && *only && strcmp(only, cs[i].name)) continue;
        if (cs[i].vs == spv_zink_vert && props.apiVersion < VK_API_VERSION_1_3) { printf("SKIP %s: SPIR-V 1.6 needs Vulkan 1.3\n", cs[i].name); continue; }
        run_case(cs[i].name, pipeline(cs[i].vs, cs[i].vsz, cs[i].fs, cs[i].fsz));
    }
out:
    if (failures)
        printf("FAIL: %d of %d checks failed\n", failures, checks);
    else
        printf("OK: %d of %d checks passed\n", checks, checks);
    return failures ? 1 : 0;
}
