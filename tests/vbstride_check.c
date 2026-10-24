/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * vbstride_check: vkCmdBindVertexBuffers2's pStrides must reach the bindings they are given for
 * (MK-05, Arm erratum 3786496: Mali drivers before r48 apply them by array index). One pipeline,
 * strides dynamic (created 4), three instance-rate R32_UINT attributes from bindings 0, 2 and 3
 * (binding 1 bound but unused), one point per instance; every buffer word holds its own index, so
 * the read-back attributes say which offset each instance read. Three ways to bind the same
 * strides 16 / 32 / 8 for bindings 0 / 2 / 3:
 *   1  one call from binding 0, four bindings (strides 16, 4, 32, 8): array index = binding number
 *   2  binding 0 alone, then one call from binding 1, three bindings (4, 32, 8): array index i is
 *      binding 1 + i - wrong where the stride goes by index
 *   3  single-binding calls only, binding 2 first with stride 12 then again alone with 32
 * The last line says which shape the strides have: right; wrong in multi-binding calls only (the
 * split into single calls, option A, fixes it); or wrong in single calls too (option B: strides
 * baked into pipeline variants). The startup probe vb_strides is the same test.
 *
 * Host imitation (server): VKBRIDGE_ERRATA_IMITATE=vb_split (multi-binding calls),
 * VKBRIDGE_ERRATA_IMITATE_VB=single (single calls too).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The divisor check's shaders (divisor_check.c): uvec4(a0, a1, a2, gl_InstanceIndex) at pixel
 * gl_InstanceIndex of pc.n. */
static const uint32_t spv_vert[] = {
#include "divisor_vert.inc"
};
static const uint32_t spv_frag[] = {
#include "divisor_frag.inc"
};

#define N 4 /* pixels, one per instance */

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkDestroyDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory);
I(vkCreateImageView); I(vkCreateRenderPass); I(vkCreateFramebuffer); I(vkCreateShaderModule); I(vkCreatePipelineLayout);
I(vkCreateGraphicsPipelines); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass); I(vkCmdBindPipeline); I(vkCmdPushConstants);
I(vkCmdBindVertexBuffers2); I(vkCmdDraw); I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier); I(vkDeviceWaitIdle);
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

/* Where each binding's data starts (bytes) and the strides the cases bind */
static const VkDeviceSize OFF[4] = {0, 1024, 2048, 3072};
static const VkDeviceSize STRIDE[4] = {16, 4, 32, 8};

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vbstride_check", 1, "dxvk", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceQueueFamilyProperties);
    L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr); L(vkDestroyInstance);
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
    char msg[256];
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    if (!check(r == VK_SUCCESS, "vkCreateDevice", msg)) goto out;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule);
    D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkCmdBeginRenderPass); D(vkCmdEndRenderPass); D(vkCmdBindPipeline);
    D(vkCmdPushConstants); D(vkCmdBindVertexBuffers2); D(vkCmdDraw); D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier);
    D(vkDeviceWaitIdle);
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    VkFormat fmt = VK_FORMAT_R32G32B32A32_UINT;
    VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {N, 1, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
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
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, N, 1, 1};
    VkFramebuffer fb;
    vkCreateFramebuffer(dev, &fci, NULL, &fb);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);

    uint32_t *words;
    VkBuffer vbuf = host_buffer(4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&words);
    uint32_t *rb;
    VkBuffer rbuf = host_buffer(N * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&rb);
    if (!vbuf || !rbuf) { check(0, "setup", "buffers"); goto out; }
    for (uint32_t w = 0; w < 1024; w++) words[w] = w;

    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, shader(spv_vert, sizeof(spv_vert)), "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader(spv_frag, sizeof(spv_frag)), "main", NULL}};
    /* bindings 0, 2, 3 -> locations 0, 1, 2; binding 1 is not in the pipeline */
    VkVertexInputBindingDescription bd[3] = {{0, 4, VK_VERTEX_INPUT_RATE_INSTANCE}, {2, 4, VK_VERTEX_INPUT_RATE_INSTANCE},
                                             {3, 4, VK_VERTEX_INPUT_RATE_INSTANCE}};
    VkVertexInputAttributeDescription ad[3] = {{0, 0, VK_FORMAT_R32_UINT, 0}, {1, 2, VK_FORMAT_R32_UINT, 0}, {2, 3, VK_FORMAT_R32_UINT, 0}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 3, bd, 3, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0};
    VkViewport vp = {0, 0, N, 1, 0, 1};
    VkRect2D sc = {{0, 0}, {N, 1}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkDynamicState dyn = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 1, &dyn};
    VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms,
                                         NULL, &cbs, &ds, pl, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline pipe;
    r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    if (!check(r == VK_SUCCESS, "pipeline: bindings 0, 2, 3, dynamic strides", msg)) goto out;
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkCommandPool cpool;
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);

    static const char *const what[3] = {"one call from binding 0, 4 bindings (strides 16, 4, 32, 8)",
                                        "binding 0 alone, then one call from binding 1, 3 bindings (strides 4, 32, 8)",
                                        "single-binding calls (binding 2: stride 12, then 32 alone)"};
    int ok[3];
    const VkBuffer bufs[4] = {vbuf, vbuf, vbuf, vbuf};
    for (int k = 0; k < 3; k++) {
        VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cb;
        vkAllocateCommandBuffers(dev, &cbai, &cb);
        VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &cbbi);
        VkClearValue clear = {.color = {.uint32 = {0, 0, 0, 0}}};
        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {N, 1}}, 1, &clear};
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        uint32_t pc = N;
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &pc);
        if (k == 0) {
            vkCmdBindVertexBuffers2(cb, 0, 4, bufs, OFF, NULL, STRIDE);
        } else if (k == 1) {
            vkCmdBindVertexBuffers2(cb, 0, 1, bufs, &OFF[0], NULL, &STRIDE[0]);
            vkCmdBindVertexBuffers2(cb, 1, 3, bufs, &OFF[1], NULL, &STRIDE[1]);
        } else {
            const VkDeviceSize twelve = 12;
            vkCmdBindVertexBuffers2(cb, 0, 1, bufs, &OFF[0], NULL, &STRIDE[0]);
            vkCmdBindVertexBuffers2(cb, 2, 1, bufs, &OFF[2], NULL, &twelve);
            vkCmdBindVertexBuffers2(cb, 3, 1, bufs, &OFF[3], NULL, &STRIDE[3]);
            vkCmdBindVertexBuffers2(cb, 2, 1, bufs, &OFF[2], NULL, &STRIDE[2]);
        }
        vkCmdDraw(cb, 1, N, 0, 0);
        vkCmdEndRenderPass(cb);
        VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {N, 1, 1}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        memset(rb, 0xff, N * 16);
        if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) { check(0, what[k], "submit"); goto out; }
        int bad = 0;
        msg[0] = 0;
        for (uint32_t i = 0; i < N; i++) {
            const uint32_t *p = rb + i * 4;
            /* the word index each binding's attribute reads at instance i */
            uint32_t want[4] = {(uint32_t)(OFF[0] + i * STRIDE[0]) / 4, (uint32_t)(OFF[2] + i * STRIDE[2]) / 4,
                                (uint32_t)(OFF[3] + i * STRIDE[3]) / 4, i};
            if (memcmp(p, want, 16)) bad++;
        }
        /* the stride shows from instance 1 on: that instance is reported */
        if (bad) {
            const uint32_t *p = rb + 4;
            snprintf(msg, sizeof(msg), "instance 1 read words (%u %u %u), wanted (%u %u %u): strides seen %d / %d / %d, wanted 16 / 32 / 8",
                     p[0], p[1], p[2], (uint32_t)(OFF[0] + STRIDE[0]) / 4, (uint32_t)(OFF[2] + STRIDE[2]) / 4, (uint32_t)(OFF[3] + STRIDE[3]) / 4,
                     (int)(p[0] * 4 - (uint32_t)OFF[0]), (int)(p[1] * 4 - (uint32_t)OFF[2]), (int)(p[2] * 4 - (uint32_t)OFF[3]));
        }
        ok[k] = check(!bad, what[k], msg);
    }
    if (ok[0] && ok[1] && ok[2])
        printf("shape: strides right\n");
    else if (ok[2])
        printf("shape: strides wrong in multi-binding calls only - one call per binding (option A) fixes it\n");
    else
        printf("shape: strides wrong in single-binding calls too - the split cannot fix it (option B: strides baked into pipeline variants)\n");
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
out:
    vkDestroyInstance(inst, NULL);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
