/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * crash_contain_check: a driver crash inside pipeline compilation fails that call, not the
 * session (server_guard.c; ULTRAKILL's SIGSEGV 0x40 in vkCreateGraphicsPipelines took it down).
 * Needs a server started with VKBRIDGE_TEST_CRASH_PIPELINE=1, which faults on purpose (a write to
 * 0x40) inside the guarded call when a stage carries the marker specialization constant. Checks:
 *   device A: marked graphics + compute pipelines fail with VK_ERROR_INITIALIZATION_FAILED and
 *             null handles; the connection lives; a normal pipeline still draws (two strikes);
 *             the third crash poisons A: a normal pipeline then gets VK_ERROR_DEVICE_LOST
 *   device B: (new) compute dispatch and draw correct
 * Three contained crashes per run: the server's process ceiling (64) allows twenty-one runs per server.
 *
 *   cc -O2 -Wall -Ithird_party/Vulkan-Headers-1.4.341/include -Icommon -Icommon/gen -Itests \
 *      -o host/out/crash_contain_check tests/crash_contain_check.c -ldl
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smoke_spv.h"

#define TEST_CRASH_SPEC_ID 0x7E57C0DEu /* server_guard.c */
#define W 64
#define H 32

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties mp;
static uint32_t qfam;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties);
I(vkGetPhysicalDeviceProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance);
I(vkDestroyDevice); I(vkDeviceWaitIdle); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers);
I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence); I(vkWaitForFences); I(vkCreateBuffer);
I(vkGetBufferMemoryRequirements); I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkInvalidateMappedMemoryRanges);
I(vkCreateShaderModule); I(vkCreateDescriptorSetLayout); I(vkCreatePipelineLayout); I(vkCreateComputePipelines);
I(vkCreateGraphicsPipelines); I(vkCreateDescriptorPool); I(vkAllocateDescriptorSets); I(vkUpdateDescriptorSets);
I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets); I(vkCmdPushConstants); I(vkCmdDispatch); I(vkCmdPipelineBarrier);
I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateImageView); I(vkCreateRenderPass);
I(vkCreateFramebuffer); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass); I(vkCmdDraw); I(vkCmdSetViewport); I(vkCmdSetScissor);
I(vkCmdCopyImageToBuffer); I(vkGetFenceStatus);
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

static int open_device(void)
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return 0;
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkDestroyDevice); G(vkDeviceWaitIdle); G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers);
    G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkQueueSubmit); G(vkCreateFence); G(vkWaitForFences); G(vkCreateBuffer);
    G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkBindBufferMemory); G(vkMapMemory); G(vkInvalidateMappedMemoryRanges);
    G(vkCreateShaderModule); G(vkCreateDescriptorSetLayout); G(vkCreatePipelineLayout); G(vkCreateComputePipelines);
    G(vkCreateGraphicsPipelines); G(vkCreateDescriptorPool); G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets);
    G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets); G(vkCmdPushConstants); G(vkCmdDispatch); G(vkCmdPipelineBarrier);
    G(vkCreateImage); G(vkGetImageMemoryRequirements); G(vkBindImageMemory); G(vkCreateImageView); G(vkCreateRenderPass);
    G(vkCreateFramebuffer); G(vkCmdBeginRenderPass); G(vkCmdEndRenderPass); G(vkCmdDraw); G(vkCmdSetViewport); G(vkCmdSetScissor);
    G(vkCmdCopyImageToBuffer); G(vkGetFenceStatus);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    return vkCreateCommandPool(dev, &pci, NULL, &pool) == VK_SUCCESS;
}

static int make_buffer(VkBuffer *b, void **ptr, VkDeviceMemory *mem, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(dev, &ci, NULL, b)) return 0;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, *b, &r);
    uint32_t t = find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    return t != UINT32_MAX && !vkAllocateMemory(dev, &ai, NULL, mem) && !vkBindBufferMemory(dev, *b, *mem, 0) &&
           !vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, ptr);
}

static void read_back(VkDeviceMemory mem)
{
    VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};
    vkInvalidateMappedMemoryRanges(dev, 1, &r);
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static int submit(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (vkEndCommandBuffer(cb)) return 0;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    vkCreateFence(dev, &fci, NULL, &f);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    return !vkQueueSubmit(queue, 1, &si, f) && !vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull);
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

/* The marker: a specialization constant ID no shader uses (legal, ignored by drivers). */
static const VkSpecializationMapEntry marker_entry = {TEST_CRASH_SPEC_ID, 0, 4};
static const uint32_t marker_value = 1;
static const VkSpecializationInfo marker = {1, &marker_entry, 4, &marker_value};

/* ------------------------------------------------------------------ compute */

static VkDescriptorSetLayout c_dsl;
static VkPipelineLayout c_pl;

static VkResult compute_pipeline(int marked, VkPipeline *pipe)
{
    if (!c_pl) {
        VkDescriptorSetLayoutBinding binds[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                                 {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, binds};
        vkCreateDescriptorSetLayout(dev, &lci, NULL, &c_dsl);
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &c_dsl, 1, &pcr};
        vkCreatePipelineLayout(dev, &plci, NULL, &c_pl);
    }
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                 shader(spv_smoke_comp, sizeof(spv_smoke_comp)), "main", marked ? &marker : NULL};
    ci.layout = c_pl;
    *pipe = (VkPipeline)(uintptr_t)0xBAD;
    return vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, pipe);
}

static int dispatch_ok(VkPipeline pipe)
{
    const uint32_t n = 1024;
    VkBuffer in, out;
    VkDeviceMemory min, mout;
    uint32_t *pin, *pout;
    if (!make_buffer(&in, (void **)&pin, &min, n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !make_buffer(&out, (void **)&pout, &mout, n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        return 0;
    for (uint32_t i = 0; i < n; i++) pin[i] = i;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp;
    vkCreateDescriptorPool(dev, &dpci, NULL, &dp);
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &c_dsl};
    VkDescriptorSet set;
    vkAllocateDescriptorSets(dev, &dsai, &set);
    VkDescriptorBufferInfo bi[2] = {{in, 0, VK_WHOLE_SIZE}, {out, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[0], NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[1], NULL}};
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    VkCommandBuffer cb = begin();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c_pl, 0, 1, &set, 0, NULL);
    uint32_t pc[2] = {5, 11};
    vkCmdPushConstants(cb, c_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
    vkCmdDispatch(cb, n / 64, 1, 1);
    if (!submit(cb)) return 0;
    read_back(mout);
    for (uint32_t i = 0; i < n; i++)
        if (pout[i] != i * 5 + 11) return 0;
    return 1;
}

/* ------------------------------------------------------------------ graphics */

static VkRenderPass g_rp;
static VkPipelineLayout g_pl;

static VkResult graphics_pipeline(int marked, VkPipeline *pipe)
{
    if (!g_pl) {
        VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                       VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
        /* The copy after the pass reads what it stored (see smoke.c's draw check). */
        VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
        VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
        vkCreateRenderPass(dev, &rpci, NULL, &g_rp);
        VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
        vkCreatePipelineLayout(dev, &plci, NULL, &g_pl);
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, shader(spv_smoke_vert, sizeof(spv_smoke_vert)),
         "main", marked ? &marker : NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader(spv_smoke_frag, sizeof(spv_smoke_frag)),
         "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, &ds,
                                       g_pl, g_rp, 0, VK_NULL_HANDLE, -1};
    *pipe = (VkPipeline)(uintptr_t)0xBAD;
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, pipe);
}

static int draw_ok(VkPipeline pipe)
{
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {W, H, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ici, NULL, &img)) return 0;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    uint32_t t = find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_type(r.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    VkDeviceMemory im;
    if (vkAllocateMemory(dev, &ai, NULL, &im) || vkBindImageMemory(dev, img, im, 0)) return 0;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    vkCreateImageView(dev, &vci, NULL, &view);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, g_rp, 1, &view, W, H, 1};
    VkFramebuffer fb;
    vkCreateFramebuffer(dev, &fci, NULL, &fb);
    VkBuffer rb;
    VkDeviceMemory rm;
    uint32_t *p;
    if (!make_buffer(&rb, (void **)&p, &rm, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return 0;
    VkCommandBuffer cb = begin();
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 1}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, g_rp, fb, {{0, 0}, {W, H}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport v = {0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc = {{0, 0}, {W / 2, H}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float color[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, color);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkImageMemoryBarrier done = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &done);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &c);
    if (!submit(cb)) return 0;
    read_back(rm);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            if (p[y * W + x] != (x < W / 2 ? 0xFFFF00FFu : 0xFF000000u)) return 0; /* RGBA: magenta left, black right */
    return 1;
}

/* ------------------------------------------------------------------ */

static void expect_contained(VkResult r, VkPipeline pipe, const char *what)
{
    char d[160];
    snprintf(d, sizeof(d), "result %d, handle %llx (want %d, 0)%s", r, (unsigned long long)(uintptr_t)pipe, VK_ERROR_INITIALIZATION_FAILED,
             r == VK_ERROR_DEVICE_LOST ? " - connection lost or device poisoned too early" :
             r == VK_SUCCESS ? " - server not started with VKBRIDGE_TEST_CRASH_PIPELINE=1?" : "");
    check(r == VK_ERROR_INITIALIZATION_FAILED && pipe == VK_NULL_HANDLE, what, d);
}

static int alive(void)
{
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence f;
    return vkCreateFence(dev, &fci, NULL, &f) == VK_SUCCESS && vkGetFenceStatus(dev, f) == VK_SUCCESS;
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "crash_contain_check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAIL instance\n");
        return 1;
    }
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkGetPhysicalDeviceMemoryProperties);
    G(vkGetPhysicalDeviceProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr); G(vkDestroyInstance);
#undef G
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAIL no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("     %s\n", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t nq = 0;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;

    /* device A */
    if (!open_device()) {
        printf("FAIL device A\n");
        return 1;
    }
    VkPipeline pipe;
    VkResult r = graphics_pipeline(1, &pipe);
    expect_contained(r, pipe, "A: faulting graphics pipeline fails, not the connection");
    check(alive(), "A: connection alive after the contained crash", "");
    r = compute_pipeline(1, &pipe);
    expect_contained(r, pipe, "A: faulting compute pipeline fails");
    r = graphics_pipeline(0, &pipe);
    check(r == VK_SUCCESS && draw_ok(pipe), "A: normal pipeline draws after two contained crashes", r ? "creation failed" : "");
    r = graphics_pipeline(1, &pipe);
    expect_contained(r, pipe, "A: third faulting pipeline fails");
    r = compute_pipeline(0, &pipe);
    char d[96];
    snprintf(d, sizeof(d), "result %d (want %d)", r, VK_ERROR_DEVICE_LOST);
    check(r == VK_ERROR_DEVICE_LOST && pipe == VK_NULL_HANDLE, "A: poisoned after three: pipeline creation reports device lost", d);
    check(alive(), "A: other calls still work on the poisoned device (teardown possible)", "");
    vkDestroyDevice(dev, NULL);

    /* device B */
    c_pl = VK_NULL_HANDLE;
    g_pl = VK_NULL_HANDLE;
    if (!open_device()) {
        check(0, "B: new device", "vkCreateDevice failed");
    } else {
        r = compute_pipeline(0, &pipe);
        check(r == VK_SUCCESS && dispatch_ok(pipe), "B: new device dispatches correctly", r ? "creation failed" : "");
        r = graphics_pipeline(0, &pipe);
        check(r == VK_SUCCESS && draw_ok(pipe), "B: new device draws correctly", r ? "creation failed" : "");
        vkDestroyDevice(dev, NULL);
    }
    vkDestroyInstance(inst, NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
