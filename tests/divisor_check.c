/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * divisor_check: the vertex attribute divisor as vkd3d-proton asks for it. vkd3d-proton 3 refuses a
 * device unless VK_EXT_vertex_attribute_divisor (spec version 3 or later) is listed and both
 * vertexAttributeInstanceRateDivisor and vertexAttributeInstanceRateZeroDivisor are reported; it
 * reads maxVertexAttribDivisor from the EXT properties and enables the EXT name with the features
 * structure. Mali-G925 on Android 16 lists VK_KHR_vertex_attribute_divisor only, without the zero
 * divisor (The Witcher 3 DX12: "Lacking support for VK_EXT_vertex_attribute_divisor").
 *
 * Then instanced points with three instance-rate bindings - divisor 0, 2 and 1 - are drawn, one
 * pixel per instance, at firstInstance 0 and 4, with baked and with dynamic strides; each pixel's
 * fetched values are read back and compared.
 *
 *   divisor_check [--core14]   --core14: Vulkan 1.4 core features instead of the EXT name
 *
 * Host imitation (server environment): VKBRIDGE_FAKE_MISSING=divisorzero,VK_EXT_vertex_attribute_divisor
 * and VKBRIDGE_HIDE_EXTS=VK_EXT_vertex_attribute_divisor (client) look like Mali-G925; without
 * divisorzero, like a driver whose KHR divisor is complete.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* glslc -O --target-env=vulkan1.1 of:
 *   layout(location = 0) in uint a0; layout(location = 1) in uint a1; layout(location = 2) in uint a2;
 *   layout(location = 0) flat out uvec4 o;
 *   layout(push_constant) uniform P { uint n; } pc;
 *   void main() { uint i = uint(gl_InstanceIndex);
 *                 gl_Position = vec4((float(i) + 0.5) / float(pc.n) * 2.0 - 1.0, 0.0, 0.0, 1.0);
 *                 gl_PointSize = 1.0; o = uvec4(a0, a1, a2, i); } */
static const uint32_t spv_vert[] = {
#include "divisor_vert.inc"
};
/* glslc -O --target-env=vulkan1.1 of:
 *   layout(location = 0) flat in uvec4 o; layout(location = 0) out uvec4 c; void main() { c = o; } */
static const uint32_t spv_frag[] = {
#include "divisor_frag.inc"
};

#define N 16         /* pixels, one per instance */
#define FI 4         /* firstInstance of the second draw */

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceProperties2);
I(vkGetPhysicalDeviceFeatures2); I(vkEnumerateDeviceExtensionProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkDestroyDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory);
I(vkCreateImageView); I(vkCreateRenderPass); I(vkCreateFramebuffer); I(vkCreateShaderModule); I(vkCreatePipelineLayout);
I(vkCreateGraphicsPipelines); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass); I(vkCmdBindPipeline); I(vkCmdPushConstants);
I(vkCmdBindVertexBuffers); I(vkCmdBindVertexBuffers2); I(vkCmdDraw); I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier);
I(vkDeviceWaitIdle); I(vkInvalidateMappedMemoryRanges);
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

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name, uint32_t *spec)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) {
            if (spec) *spec = e[i].specVersion;
            return 1;
        }
    return 0;
}

int main(int argc, char **argv)
{
    int core14 = argc > 1 && !strcmp(argv[1], "--core14");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "divisor_check", 1, "vkd3d", 1,
                             core14 ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceProperties2); L(vkGetPhysicalDeviceFeatures2);
    L(vkEnumerateDeviceExtensionProperties); L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceMemoryProperties);
    L(vkCreateDevice); L(vkGetDeviceProcAddr); L(vkDestroyInstance);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("GPU: %s, Vulkan %u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
    if (core14 && props.apiVersion < VK_API_VERSION_1_4) {
        printf("SKIP --core14 on a Vulkan 1.3 device\n0 checks, 0 failed\n");
        return 0;
    }

    /* ---- what vkd3d-proton looks at */
    char msg[256];
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    uint32_t spec = 0;
    int has_ext_name = has_ext(ext, ne, "VK_EXT_vertex_attribute_divisor", &spec);
    int has_khr_name = has_ext(ext, ne, "VK_KHR_vertex_attribute_divisor", NULL);
    snprintf(msg, sizeof(msg), "listed %d, spec version %u (KHR listed %d)", has_ext_name, spec, has_khr_name);
    if (!core14) check(has_ext_name && spec >= 3, "VK_EXT_vertex_attribute_divisor listed, spec version >= 3", msg);
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT df = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan14Features f14 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, core14 ? (void *)&f14 : (void *)&df};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkBool32 rate = core14 ? f14.vertexAttributeInstanceRateDivisor : df.vertexAttributeInstanceRateDivisor;
    VkBool32 zero = core14 ? f14.vertexAttributeInstanceRateZeroDivisor : df.vertexAttributeInstanceRateZeroDivisor;
    snprintf(msg, sizeof(msg), "vertexAttributeInstanceRateDivisor %u, vertexAttributeInstanceRateZeroDivisor %u", rate, zero);
    check(rate && zero, core14 ? "Vulkan 1.4 divisor features" : "divisor features (EXT structure)", msg);
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT dpe = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorProperties dpk = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dpe};
    if (has_khr_name || props.apiVersion >= VK_API_VERSION_1_4) dpe.pNext = &dpk;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    snprintf(msg, sizeof(msg), "EXT maxVertexAttribDivisor %u; KHR %u, supportsNonZeroFirstInstance %u", dpe.maxVertexAttribDivisor,
             dpk.maxVertexAttribDivisor, dpk.supportsNonZeroFirstInstance);
    if (!core14) check(dpe.maxVertexAttribDivisor >= 2, "maxVertexAttribDivisor (EXT properties) >= 2", msg);
    else printf("     %s\n", msg);
    /* Without it, firstInstance must be 0 with a divisor other than 1 (the bridge's emulation reports it). */
    int nonzero_first = dpe.pNext ? dpk.supportsNonZeroFirstInstance : 1;

    /* ---- the device, as vkd3d-proton makes it */
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT den = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, NULL,
                                                             VK_TRUE, VK_TRUE};
    VkPhysicalDeviceVulkan14Features f14en = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    f14en.vertexAttributeInstanceRateDivisor = f14en.vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
    VkPhysicalDeviceFeatures2 fen = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, core14 ? (void *)&f14en : (void *)&den};
    const char *names[] = {"VK_EXT_vertex_attribute_divisor"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &fen, 0, 1, &qci, 0, NULL, core14 ? 0 : 1, names, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    snprintf(msg, sizeof(msg), "VkResult %d", r);
    check(r == VK_SUCCESS, core14 ? "vkCreateDevice with the Vulkan 1.4 divisor features" : "vkCreateDevice with VK_EXT_vertex_attribute_divisor and both features", msg);
    if (r) goto out;
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule);
    D(vkCreatePipelineLayout); D(vkCreateGraphicsPipelines); D(vkCmdBeginRenderPass); D(vkCmdEndRenderPass); D(vkCmdBindPipeline);
    D(vkCmdPushConstants); D(vkCmdBindVertexBuffers); D(vkCmdBindVertexBuffers2); D(vkCmdDraw); D(vkCmdCopyImageToBuffer);
    D(vkCmdPipelineBarrier); D(vkDeviceWaitIdle); D(vkInvalidateMappedMemoryRanges);
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* ---- drawing */
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

    /* Bindings 0, 1, 2: instance rate, divisors 0, 2, 1; elements 100+k, 200+k, 300+k. Binding 1
     * has a wider stride (8) than it reads, so a stride mix-up shows. */
    uint32_t *vb[3];
    VkBuffer vbuf[3];
    const uint32_t stride[3] = {4, 8, 4};
    for (int b = 0; b < 3; b++) {
        vbuf[b] = host_buffer(64 * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, (void **)&vb[b]);
        if (!vbuf[b]) { check(0, "setup", "vertex buffer"); goto out; }
        for (uint32_t k = 0; k < 64; k++) {
            vb[b][k * stride[b] / 4] = (b + 1) * 100 + k;
            if (stride[b] == 8) vb[b][k * 2 + 1] = 0xdead;
        }
    }
    uint32_t *rb;
    VkBuffer rbuf = host_buffer(N * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&rb);
    if (!rbuf) { check(0, "setup", "readback buffer"); goto out; }

    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, shader(spv_vert, sizeof(spv_vert)), "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, shader(spv_frag, sizeof(spv_frag)), "main", NULL}};
    VkVertexInputBindingDescription bd[3];
    VkVertexInputAttributeDescription ad[3];
    for (uint32_t b = 0; b < 3; b++) {
        bd[b] = (VkVertexInputBindingDescription){b, stride[b], VK_VERTEX_INPUT_RATE_INSTANCE};
        ad[b] = (VkVertexInputAttributeDescription){b, b, VK_FORMAT_R32_UINT, 0};
    }
    VkVertexInputBindingDivisorDescriptionEXT divs[3] = {{0, 0}, {1, 2}, {2, 1}};
    VkPipelineVertexInputDivisorStateCreateInfoEXT dsi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, NULL, 3, divs};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, &dsi, 0, 3, bd, 3, ad};
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
    VkPipeline pipe[2];
    for (int d = 0; d < 2; d++) {
        VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 1, &dyn};
        VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms,
                                             NULL, &cbs, d ? &ds : NULL, pl, rp, 0, VK_NULL_HANDLE, -1};
        r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe[d]);
        snprintf(msg, sizeof(msg), "VkResult %d", r);
        check(r == VK_SUCCESS, d ? "pipeline with divisors 0/2/1, dynamic strides" : "pipeline with divisors 0/2/1, baked strides", msg);
        if (r) goto out;
    }
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkCommandPool cpool;
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);

    for (int d = 0; d < 2; d++)
        for (int f = 0; f < 2; f++) {
            uint32_t fi = f ? FI : 0, count = N - fi;
            const char *what = d ? (f ? "dynamic strides, firstInstance 4" : "dynamic strides, firstInstance 0")
                                 : (f ? "baked strides, firstInstance 4" : "baked strides, firstInstance 0");
            if (fi && !nonzero_first) {
                printf("SKIP %s: supportsNonZeroFirstInstance is false\n", what);
                continue;
            }
            VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            VkCommandBuffer cb;
            vkAllocateCommandBuffers(dev, &cbai, &cb);
            VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            vkBeginCommandBuffer(cb, &cbbi);
            VkClearValue clear = {.color = {.uint32 = {0, 0, 0, 0}}};
            VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {N, 1}}, 1, &clear};
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe[d]);
            uint32_t pc = N;
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &pc);
            VkDeviceSize off[3] = {0, 0, 0}, strides[3] = {4, 8, 4};
            if (d) vkCmdBindVertexBuffers2(cb, 0, 3, vbuf, off, NULL, strides);
            else vkCmdBindVertexBuffers(cb, 0, 3, vbuf, off);
            vkCmdDraw(cb, 1, count, 0, fi);
            vkCmdEndRenderPass(cb);
            VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {N, 1, 1}};
            vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            vkEndCommandBuffer(cb);
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
            memset(rb, 0xff, N * 16);
            if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) { check(0, what, "submit"); goto out; }
            /* Pixel x = instance index x (gl_InstanceIndex counts from firstInstance). */
            int bad = 0;
            msg[0] = 0;
            for (uint32_t x = 0; x < N; x++) {
                const uint32_t *p = rb + x * 4;
                uint32_t want[4] = {0, 0, 0, 0};
                if (x >= fi) {
                    uint32_t i = x - fi;
                    want[0] = 100 + fi;               /* divisor 0: element firstInstance */
                    want[1] = 200 + fi + i / 2;       /* divisor 2 */
                    want[2] = 300 + fi + i;           /* divisor 1 */
                    want[3] = x;
                }
                if (memcmp(p, want, 16)) {
                    if (!bad++) snprintf(msg, sizeof(msg), "pixel %u: (%u %u %u %u), wanted (%u %u %u %u)", x, p[0], p[1], p[2], p[3],
                                         want[0], want[1], want[2], want[3]);
                }
            }
            if (bad) snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "; %d of %d pixels wrong", bad, N);
            check(!bad, what, msg);
        }
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
out:
    vkDestroyInstance(inst, NULL);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
