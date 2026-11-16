/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv3d_check: 2D-array and 2D views of a 3D image from slice 1 on, used as framebuffer attachments - PowerVR
 * DXT 25.1 renders them into slices 0..n-1 whatever the base layer (explicit counts too; pv_view3d_base, PV-04,
 * server_errata_view3d.c). A 3D image (8x8x6, two mips) is cleared to one colour, then a pass renders through
 * the view and every texel of both mips is read back: what the view covers (and the render area, and the layers
 * a vkCmdClearAttachments names) must have the second colour, the rest the first. The cases cover a render
 * pass (loadOp CLEAR; loadOp LOAD with a layer-range clear inside the pass; a secondary command buffer inside
 * it), an imageless framebuffer where the device has one, and dynamic rendering (1.3), over views of several
 * slices, REMAINING layers, one slice (2D-array and 2D views), a mip level and a partial render area.
 *
 * Draws: layered draws whose slice the shader picks (gl_Layer from the vertex shader where shaderOutputLayer is
 * there, else from a geometry shader; some layers drawn, one cleared only), a framebuffer (layerCount) of fewer
 * layers than the view, multiview with a hole in the view mask, and the reads of the 3D image a pass may do while
 * it renders it: a feedback loop (VK_EXT_attachment_feedback_loop_layout: draws, a barrier inside the pass, then
 * draws that sample the 3D image at their own texel and write it red and blue swapped) and a later subpass that
 * samples what the first rendered (GENERAL layout) into a buffer. After every case the 3D image's mip is also
 * sampled through a 3D view by a compute shader and must equal what the copy read.
 * (Input attachments cannot name these views: VUID-VkDescriptorImageInfo-imageView-06712 / -descriptorType-06713.)
 *
 *   cc -O2 -Ithird_party/Vulkan-Headers-1.4.341/include -Itests -o host/out/pv3d_check tests/pv3d_check.c -ldl
 * Shaders: tests/shaders/pv3d, glslc --target-env=vulkan1.1 -O -mfmt=num (pv3d_layer.vert: vulkan1.2, for ShaderLayer).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t spv_layer_vert[] = {
#include "pv3d_layer_vert.inc"
};
static const uint32_t spv_plain_vert[] = {
#include "pv3d_plain_vert.inc"
};
static const uint32_t spv_layer_geom[] = {
#include "pv3d_layer_geom.inc"
};
static const uint32_t spv_mv_vert[] = {
#include "pv3d_mv_vert.inc"
};
static const uint32_t spv_col_frag[] = {
#include "pv3d_col_frag.inc"
};
static const uint32_t spv_rd_frag[] = {
#include "pv3d_rd_frag.inc"
};
static const uint32_t spv_rd2a_frag[] = {
#include "pv3d_rd2a_frag.inc"
};
static const uint32_t spv_read_comp[] = {
#include "pv3d_read_comp.inc"
};

#define FN(X)                                                                                                                    \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)          \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)       \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage)              \
    X(vkCmdCopyImageToBuffer) X(vkResetCommandBuffer) X(vkDestroyCommandPool) X(vkDestroyDevice) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCmdBeginRenderPass)          \
    X(vkCmdEndRenderPass) X(vkCmdClearAttachments) X(vkCmdExecuteCommands) X(vkCreateShaderModule) X(vkDestroyShaderModule)       \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines)                 \
    X(vkDestroyPipeline) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)                 \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkResetDescriptorPool) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdDispatch) X(vkCmdSetViewport) X(vkCmdSetScissor)         \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCmdNextSubpass) X(vkCmdFillBuffer)
#define DECL(n) static PFN_##n n;
FN(DECL)
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
static PFN_vkCmdBeginRendering vkCmdBeginRendering;
static PFN_vkCmdEndRendering vkCmdEndRendering;

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb, scb;
static VkPhysicalDeviceMemoryProperties mp;
static int failures, checks, skipped, have_dyn, have_imageless;
static int have_layer, have_gs, have_mv, have_fbloop, have_lr, have_fsa;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout pl;
static VkDescriptorPool dpool;
static VkSampler smp;
static VkPipeline cpipe;
static VkShaderStageFlags pc_stages;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

enum { RP, RP_SEC, RP_IMAGELESS, DYN };
enum { CLEAR, LOAD }; /* LOAD: loadOp LOAD, then a clear of view layer 1 alone inside the pass */
/* draws: none (clears only); layered (gl_Layer: instance i into layer first + i * step); multiview (mask);
 * a feedback loop (all layers drawn, a barrier, the 3D image read back into them swapped); a second subpass reading
 * what the first drew into a buffer */
enum { D_NONE, D_LAYER, D_MV, D_FB, D_SUB };

typedef struct {
    const char *name;
    int api, op;
    VkImageViewType view_type;
    uint32_t mip, base, count; /* count: VK_REMAINING_ARRAY_LAYERS as the app gives it */
    VkRect2D area;             /* {{0,0},{0,0}}: the whole mip */
    int draw;
    uint32_t fb_layers;        /* 0: the view's */
    const char *exp;           /* draws: per view layer, c drawn, b drawn then read back swapped, k cleared, u untouched */
    int first, step, ninst;
    uint32_t mask;             /* multiview */
    int plain;                 /* a plain 2D-array image of D layers instead of the 3D image (pv_fbloop_split, PV-06: feedback loops into any image) */
} pcase;

#define W 8
#define H 8
#define D 6
#define MIPS 2
#define CLEAR0 0x11223344u /* the whole image, before */
#define CLEAR1 0xccbbaa99u /* through the view */

static uint32_t colour_of(uint32_t l) /* pv3d_col.frag */
{
    return (20u + 40u * l) | (200u - 30u * l) << 8 | 85u << 16 | 255u << 24;
}

static uint32_t swap_rb(uint32_t c)
{
    return (c & 0xff00ff00u) | (c & 0xffu) << 16 | (c >> 16 & 0xffu);
}

static VkClearColorValue colour(uint32_t c)
{
    VkClearColorValue v;
    for (int i = 0; i < 4; i++) v.float32[i] = (float)(c >> (8 * i) & 255) / 255.0f;
    return v;
}

static void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss,
                    VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static uint32_t dim(uint32_t v, uint32_t m)
{
    return v >> m ? v >> m : 1;
}

static VkShaderModule module(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

/* A pipeline of the case's draws: rd reads the 3D image (pv3d_rd.frag), ncol colour attachments (0 or 1);
 * render pass rp / subpass, or dynamic rendering (rp NULL, view mask). */
static VkPipeline gfx(VkRenderPass rp, uint32_t subpass, int mv, uint32_t mask, int rd, int fbloop, uint32_t ncol)
/* (rd: 0 colour, 1 reads the 3D image, 2 reads a plain 2D-array image) */
{
    VkShaderModule vs = module(mv ? spv_mv_vert : have_layer ? spv_layer_vert : spv_plain_vert,
                               mv ? sizeof(spv_mv_vert) : have_layer ? sizeof(spv_layer_vert) : sizeof(spv_plain_vert));
    VkShaderModule gs = !mv && !have_layer ? module(spv_layer_geom, sizeof(spv_layer_geom)) : VK_NULL_HANDLE;
    VkShaderModule fs = rd == 2 ? module(spv_rd2a_frag, sizeof(spv_rd2a_frag))
                        : module(rd ? spv_rd_frag : spv_col_frag, rd ? sizeof(spv_rd_frag) : sizeof(spv_col_frag));
    VkPipelineShaderStageCreateInfo st[3] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, ncol, &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, mask, ncol, &fmt, VK_FORMAT_UNDEFINED,
                                        VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, rp ? NULL : &pr,
                                       fbloop ? VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0, gs ? 3 : 2, st, &vi, &ia, NULL, &vp, &rs,
                                       &ms, NULL, &cbs, &ds, pl, rp, subpass, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    if (vs && fs && (gs || mv || have_layer)) vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);
    if (gs) vkDestroyShaderModule(dev, gs, NULL);
    return p;
}

static void draw(VkPipeline p, const pcase *c, VkRect2D area, uint32_t vw, uint32_t vh, int mode, int first, int step, int ninst, VkDescriptorSet set)
{
    VkViewport vp = {0, 0, (float)vw, (float)vh, 0, 1};
    int32_t pc[4] = {first, step, (int32_t)c->base, mode};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdPushConstants(cb, pl, pc_stages, 0, sizeof(pc), pc);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    vkCmdDraw(cb, 3, c->draw == D_MV ? 1 : ninst, 0, 0);
}

static VkDescriptorSet make_set(VkImageView v3, VkImageLayout l, VkBuffer b)
{
    VkDescriptorSet s = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, 1, &dsl};
    if (vkAllocateDescriptorSets(dev, &ai, &s)) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii = {smp, v3, l};
    VkDescriptorBufferInfo bi = {b, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL}};
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    return s;
}

static VkBuffer host_buffer(VkDeviceSize size, VkDeviceMemory *mem, uint32_t **map)
{
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b;
    vkCreateBuffer(dev, &bci, NULL, &b);
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    vkAllocateMemory(dev, &ai, NULL, mem);
    vkBindBufferMemory(dev, b, *mem, 0);
    vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)map);
    return b;
}

static void run(const pcase *c)
{
    char detail[480];
    const char *why = c->api == DYN && !have_dyn ? "no dynamic rendering"
                      : c->api == RP_IMAGELESS && !have_imageless ? "no imageless framebuffers"
                      : c->draw == D_LAYER && !have_layer && !have_gs ? "neither shaderOutputLayer nor geometry shaders"
                      : c->draw == D_MV && !have_mv ? "no multiview"
                      : c->draw == D_FB && !have_fbloop ? "no VK_EXT_attachment_feedback_loop_layout"
                      : c->draw == D_FB && c->api == DYN && !have_lr ? "no dynamicRenderingLocalRead (a barrier inside dynamic rendering)"
                      : c->draw == D_SUB && !have_fsa ? "no fragmentStoresAndAtomics"
                      : (c->draw == D_FB || c->draw == D_SUB) && !have_layer && !have_gs ? "neither shaderOutputLayer nor geometry shaders"
                                                                                       : NULL;
    if (why) {
        printf("SKIP %s: %s\n", c->name, why);
        skipped++;
        return;
    }
    VkImageLayout L = c->draw == D_FB ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                      : c->draw == D_SUB ? VK_IMAGE_LAYOUT_GENERAL
                                         : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const int pl2 = c->plain;
#define DL(m) (pl2 ? (uint32_t)D : dim(D, m))
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, pl2 ? 0u : VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT,
                             pl2 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, {W, H, pl2 ? 1u : D}, MIPS, pl2 ? D : 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT | (c->draw == D_FB ? VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0),
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkDeviceMemory imem, bmem, smem;
    VkBuffer buf, sbuf;
    if (vkCreateImage(dev, &ici, NULL, &img)) {
        check(0, c->name, "vkCreateImage failed");
        return;
    }
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = mem_type(r.memoryTypeBits, 0);
    vkAllocateMemory(dev, &ai, NULL, &imem);
    vkBindImageMemory(dev, img, imem, 0);
    VkDeviceSize offs[MIPS], total = 0;
    for (uint32_t m = 0; m < MIPS; m++) {
        offs[m] = total;
        total += (VkDeviceSize)dim(W, m) * dim(H, m) * DL(m) * 4;
    }
    uint32_t *px, *sx;
    buf = host_buffer(total, &bmem, &px);
    sbuf = host_buffer((VkDeviceSize)W * H * D * 4, &smem, &sx); /* the sampled reads: the subpass's, then the compute's */

    uint32_t vw = dim(W, c->mip), vh = dim(H, c->mip), vd = DL(c->mip);
    uint32_t count = c->count == VK_REMAINING_ARRAY_LAYERS ? vd - c->base : c->count; /* what the view really covers */
    uint32_t fbl = c->draw == D_MV ? 1 : c->fb_layers ? c->fb_layers : count;
    VkRect2D area = c->area.extent.width ? c->area : (VkRect2D){{0, 0}, {vw, vh}};
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, c->view_type, VK_FORMAT_R8G8B8A8_UNORM, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, c->mip, 1, c->base, c->count}};
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (c->draw == D_FB ? VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT : 0)};
    vci.pNext = &vu; /* a 2D-array view of a 3D image is for attachments only (the image is also sampled through its 3D view) */
    VkImageViewCreateInfo v3i = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, pl2 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_3D,
                                 VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, c->mip, 1, 0, pl2 ? D : 1}};
    VkImageView view, v3;
    if (vkCreateImageView(dev, &vci, NULL, &view) || vkCreateImageView(dev, &v3i, NULL, &v3)) {
        check(0, c->name, "vkCreateImageView failed");
        return;
    }
    vkResetDescriptorPool(dev, dpool, 0);
    VkDescriptorSet rset = c->draw == D_FB || c->draw == D_SUB ? make_set(v3, L, sbuf) : VK_NULL_HANDLE;
    VkDescriptorSet cset = make_set(v3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, sbuf);
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipeline p1 = VK_NULL_HANDLE, p2 = VK_NULL_HANDLE;
    VkClearValue cv;
    cv.color = colour(CLEAR1);
    if (c->api != DYN) {
        VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                      c->op == LOAD ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                      VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, L, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        VkAttachmentReference ar = {0, L};
        VkSubpassDescription sd[2] = {{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar, NULL, NULL, 0, NULL},
                                      {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 0, NULL, NULL, NULL, 0, NULL}};
        VkSubpassDependency dep[3] = {
            {c->draw == D_SUB ? 1 : 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
             VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT, 0},
            {0, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT},
            {0, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, 0}};
        VkRenderPassMultiviewCreateInfo mvi = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO, NULL, 1, &c->mask, 0, NULL, 0, NULL};
        VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, c->draw == D_MV ? &mvi : NULL, 0, 1, &ad,
                                      c->draw == D_SUB ? 2 : 1, sd, c->draw == D_FB || c->draw == D_SUB ? 2 : 1, dep};
        if (c->draw == D_SUB) dep[1] = dep[2];
        if (vkCreateRenderPass(dev, &rpi, NULL, &rp)) {
            check(0, c->name, "vkCreateRenderPass failed");
            return;
        }
        VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
        VkFramebufferAttachmentImageInfo ii = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO, NULL, 0,
                                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, vw, vh, count, 1, &fmt};
        VkFramebufferAttachmentsCreateInfo fai = {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO, NULL, 1, &ii};
        VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, c->api == RP_IMAGELESS ? &fai : NULL,
                                      c->api == RP_IMAGELESS ? VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT : 0, rp, 1,
                                      c->api == RP_IMAGELESS ? NULL : &view, vw, vh, fbl};
        if (vkCreateFramebuffer(dev, &fi, NULL, &fb)) {
            check(0, c->name, "vkCreateFramebuffer failed");
            return;
        }
    }
    if (c->draw != D_NONE) {
        p1 = gfx(rp, 0, c->draw == D_MV, c->mask, 0, c->draw == D_FB, 1);
        if (c->draw == D_FB) p2 = gfx(rp, 0, 0, 0, pl2 ? 2 : 1, 1, 1);
        if (c->draw == D_SUB) p2 = gfx(rp, 1, 0, 0, pl2 ? 2 : 1, 0, 0);
        if (!p1 || ((c->draw == D_FB || c->draw == D_SUB) && !p2)) {
            check(0, c->name, "vkCreateGraphicsPipelines failed");
            return;
        }
    }

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, cv};
    VkClearRect cr = {area, 1, 1}; /* view layer 1 alone */
    if (c->api == RP_SEC) {
        VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, rp, 0, fb, VK_FALSE, 0, 0};
        VkCommandBufferBeginInfo sbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &inh};
        vkResetCommandBuffer(scb, 0);
        vkBeginCommandBuffer(scb, &sbi);
        if (c->op == LOAD) vkCmdClearAttachments(scb, 1, &ca, 1, &cr);
        if (c->draw == D_FB) { /* the loop recorded in the secondary command buffer, its barrier too (the one Vulkan offers no way to split at) */
            VkMemoryBarrier sbb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
            int ni = c->ninst ? c->ninst : (int)fbl;
            VkCommandBuffer primary = cb;
            cb = scb; /* (draw records on cb) */
            draw(p1, c, area, vw, vh, 0, c->first, c->step, ni, VK_NULL_HANDLE);
            vkCmdPipelineBarrier(scb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT, 1, &sbb, 0, NULL, 0, NULL);
            draw(p2, c, area, vw, vh, 0, c->first, c->step, ni, rset);
            cb = primary;
        }
        vkEndCommandBuffer(scb);
    }
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &bi);
    vkCmdFillBuffer(cb, sbuf, 0, VK_WHOLE_SIZE, 0);
    VkMemoryBarrier fill = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &fill, 0, NULL, 0, NULL);
    barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue c0 = colour(CLEAR0);
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, MIPS, 0, pl2 ? D : 1};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c0, 1, &all);
    barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, L, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    int ninst = c->ninst ? c->ninst : (int)fbl;
    /* the self-dependency of a feedback loop: what the draws wrote, visible to the reads after */
    VkMemoryBarrier fbb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    if (c->api == DYN) {
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, L, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE,
                                         VK_IMAGE_LAYOUT_UNDEFINED, c->op == LOAD ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         VK_ATTACHMENT_STORE_OP_STORE, cv};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, area, c->draw == D_MV ? 0 : fbl, c->draw == D_MV ? c->mask : 0, 1, &att,
                              NULL, NULL};
        vkCmdBeginRendering(cb, &ri);
        if (c->op == LOAD) vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        if (p1) draw(p1, c, area, vw, vh, 0, c->first, c->step, ninst, VK_NULL_HANDLE);
        if (c->draw == D_FB) {
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT, 1, &fbb, 0, NULL, 0, NULL);
            draw(p2, c, area, vw, vh, 0, c->first, c->step, ninst, rset);
        }
        vkCmdEndRendering(cb);
        barrier(img, L, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    } else {
        VkRenderPassAttachmentBeginInfo abi = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO, NULL, 1, &view};
        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, c->api == RP_IMAGELESS ? &abi : NULL, rp, fb, area, 1, &cv};
        vkCmdBeginRenderPass(cb, &rbi, c->api == RP_SEC ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE);
        if (c->api == RP_SEC) vkCmdExecuteCommands(cb, 1, &scb);
        else if (c->op == LOAD) vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        if (p1 && c->api != RP_SEC) draw(p1, c, area, vw, vh, 0, c->first, c->step, ninst, VK_NULL_HANDLE);
        if (c->draw == D_FB && c->api != RP_SEC) {
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_DEPENDENCY_BY_REGION_BIT | VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT, 1, &fbb, 0, NULL, 0, NULL);
            draw(p2, c, area, vw, vh, 0, c->first, c->step, ninst, rset);
        }
        if (c->draw == D_SUB) {
            vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
            draw(p2, c, area, vw, vh, 1, c->first, c->step, ninst, rset);
        }
        vkCmdEndRenderPass(cb);
    }
    for (uint32_t m = 0; m < MIPS; m++) {
        VkBufferImageCopy cp = {offs[m], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, pl2 ? D : 1}, {0, 0, 0}, {dim(W, m), dim(H, m), pl2 ? 1u : dim(D, m)}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    }
    VkBufferMemoryBarrier hb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, buf, 0, VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    VkResult qr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (!qr) qr = vkQueueWaitIdle(queue);
    if (qr) {
        snprintf(detail, sizeof(detail), "submit/wait %d", qr);
        check(0, c->name, detail);
        exit(1); /* a lost device: nothing after it means anything */
    }
    VkMappedMemoryRange mr[2] = {{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, bmem, 0, VK_WHOLE_SIZE},
                                 {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, smem, 0, VK_WHOLE_SIZE}};
    vkInvalidateMappedMemoryRanges(dev, 2, mr);

    /* a later subpass's reads: what the first drew, in every layer it drew, inside the area */
    char sub[64] = "";
    int subbad = 0;
    if (c->draw == D_SUB) {
        size_t so = 0;
        for (uint32_t l = 0; l < count; l++) {
            uint32_t wrong = 0;
            for (uint32_t y = area.offset.y; y < area.offset.y + area.extent.height; y++)
                for (uint32_t x = area.offset.x; x < area.offset.x + area.extent.width; x++) wrong += sx[(l * H + y) * W + x] != colour_of(l);
            subbad += wrong != 0;
            if (so < sizeof(sub)) so += snprintf(sub + so, sizeof(sub) - so, "%c", wrong ? '!' : 'c');
        }
    }

    /* every texel of every slice of every mip: CLEAR1 exactly in the view's slices inside the area (LOAD: in view layer 1 alone);
     * draws: what exp says per view layer */
    char got[96] = "";
    size_t o = 0;
    int bad = 0;
    for (uint32_t m = 0; m < MIPS; m++) {
        uint32_t w = dim(W, m), h = dim(H, m), d = DL(m);
        if (o < sizeof(got)) o += snprintf(got + o, sizeof(got) - o, "%smip %u ", m ? ", " : "", m);
        for (uint32_t s = 0; s < d; s++) {
            int inv = m == c->mip && s >= c->base && s < c->base + count;
            uint32_t l = s - c->base;
            char e = !inv ? 'u' : c->draw != D_NONE ? c->exp[l] : (c->op == CLEAR || s == c->base + 1) ? 'k' : 'u';
            uint32_t want = e == 'c' ? colour_of(l) : e == 'b' ? swap_rb(colour_of(l)) : e == 'k' ? CLEAR1 : CLEAR0;
            const uint32_t *p = px + offs[m] / 4 + (size_t)s * w * h;
            uint32_t n1 = 0, n0 = 0, wrong = 0;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    int inside = inv && x >= (uint32_t)area.offset.x && x < area.offset.x + area.extent.width && y >= (uint32_t)area.offset.y &&
                                 y < area.offset.y + area.extent.height;
                    uint32_t v = p[y * w + x];
                    wrong += v != (inside ? want : CLEAR0);
                    n1 += v == want && e != 'u';
                    n0 += v == CLEAR0;
                }
            bad += wrong != 0;
            if (o < sizeof(got))
                o += snprintf(got + o, sizeof(got) - o, "%c", wrong ? (n0 == w * h ? '.' : n1 ? '?' : 'X') : e == 'u' ? '-' : e);
        }
    }
    snprintf(detail, sizeof(detail), "%s %s view, mip %u from slice %u, %u slice(s)%s%s: %s%s%s (a letter or - right: c drawn, b read back "
             "swapped, k cleared, - untouched; . missed, X wrong, ? mixed)",
             pl2 ? "plain image's" : "3D image's", c->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? "2D_ARRAY" : "2D", c->mip, c->base, count, c->area.extent.width ? ", partial area" : "",
             c->draw == D_LAYER ? (have_layer ? ", gl_Layer from the vertex shader" : ", gl_Layer from a geometry shader") : "", got,
             c->draw == D_SUB ? "; the later subpass read " : "", sub);

    /* the same mip sampled through a 3D view after the pass: what the copy read */
    if (!bad && !subbad && cpipe && cset && !pl2) {
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        barrier(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &cset, 0, NULL);
        vkCmdDispatch(cb, (vw + 3) / 4, (vh + 3) / 4, vd);
        VkBufferMemoryBarrier sb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, sbuf, 0, VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &sb, 0, NULL);
        vkEndCommandBuffer(cb);
        qr = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (!qr) qr = vkQueueWaitIdle(queue);
        vkInvalidateMappedMemoryRanges(dev, 2, mr);
        uint32_t diff = 0;
        const uint32_t *p = px + offs[c->mip] / 4;
        for (uint32_t i = 0; i < vw * vh * vd; i++) diff += sx[i] != p[i];
        if (qr || diff) {
            bad = 1;
            size_t dl = strlen(detail);
            snprintf(detail + dl, sizeof(detail) - dl, "; sampled through a 3D view after the pass: %u texel(s) differ from the copy%s", diff,
                     qr ? " (submit failed)" : "");
        }
    }
    check(!bad && !subbad, c->name, detail);
    if (p1) vkDestroyPipeline(dev, p1, NULL);
    if (p2) vkDestroyPipeline(dev, p2, NULL);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImageView(dev, v3, NULL);
    if (fb) vkDestroyFramebuffer(dev, fb, NULL);
    if (rp) vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyImage(dev, img, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkDestroyBuffer(dev, sbuf, NULL);
    vkFreeMemory(dev, imem, NULL);
    vkFreeMemory(dev, bmem, NULL);
    vkFreeMemory(dev, smem, NULL);
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-pv3d", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) {
        check(0, "instance", NULL);
        return 1;
    }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props_fn = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties mp_fn = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qf_fn =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceFeatures2 feat_fn = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
    PFN_vkEnumerateDeviceExtensionProperties exts_fn = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    PFN_vkCreateDevice create_dev = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    uint32_t n = 1;
    VkPhysicalDevice pd;
    enumerate(inst, &n, &pd);
    if (!n) {
        check(0, "physical device", "none");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    props_fn(pd, &props);
    mp_fn(pd, &mp);
    if (props.apiVersion < VK_API_VERSION_1_2) {
        printf("SKIP: %s is Vulkan %u.%u, 2D-array views of 3D images need 1.1 and the imageless framebuffer core 1.2\nOK: skipped\n",
               props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
        return 0;
    }
    uint32_t ne = 0;
    exts_fn(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
    exts_fn(pd, NULL, &ne, ext);
    int ext_fb = has_ext(ext, ne, "VK_EXT_attachment_feedback_loop_layout"), ext_lr = has_ext(ext, ne, "VK_KHR_dynamic_rendering_local_read");
    have_dyn = props.apiVersion >= VK_API_VERSION_1_3;
    VkPhysicalDeviceDynamicRenderingLocalReadFeatures slr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT sfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,
                                                                   ext_lr ? &slr : NULL};
    VkPhysicalDeviceVulkan13Features s13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, ext_fb ? (void *)&sfb : ext_lr ? (void *)&slr : NULL};
    VkPhysicalDeviceVulkan12Features s12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, have_dyn ? &s13 : NULL};
    VkPhysicalDeviceVulkan11Features s11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &s12};
    VkPhysicalDeviceFeatures2 sf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s11};
    feat_fn(pd, &sf);
    have_imageless = s12.imagelessFramebuffer;
    have_dyn = have_dyn && s13.dynamicRendering;
    have_layer = s12.shaderOutputLayer;
    have_gs = sf.features.geometryShader;
    have_mv = s11.multiview;
    have_fsa = sf.features.fragmentStoresAndAtomics;
    have_fbloop = ext_fb && have_dyn && sfb.attachmentFeedbackLoopLayout;
    have_lr = ext_lr && have_dyn && slr.dynamicRenderingLocalRead;
    uint32_t nq = 16, qfam;
    VkQueueFamilyProperties qp[16];
    qf_fn(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceDynamicRenderingLocalReadFeatures flr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES, NULL, VK_TRUE};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT ffb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,
                                                                   have_lr ? &flr : NULL, VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            have_fbloop ? (void *)&ffb : have_lr ? (void *)&flr : NULL};
    f13.dynamicRendering = have_dyn;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, have_dyn ? &f13 : NULL};
    f12.imagelessFramebuffer = have_imageless;
    f12.shaderOutputLayer = have_layer;
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    f11.multiview = have_mv;
    VkPhysicalDeviceFeatures base = {0};
    base.geometryShader = have_gs;
    base.fragmentStoresAndAtomics = have_fsa;
    const char *dexts[2];
    uint32_t nde = 0;
    if (have_fbloop) dexts[nde++] = "VK_EXT_attachment_feedback_loop_layout";
    if (have_lr) dexts[nde++] = "VK_KHR_dynamic_rendering_local_read";
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f11, 0, 1, &qci, 0, NULL, nde, dexts, &base};
    if (create_dev(pd, &dci, NULL, &dev)) {
        check(0, "device", NULL);
        return 1;
    }
    char dd[200];
    snprintf(dd, sizeof(dd), "%s (layered draws: %s; multiview %s; feedback loop %s; local read %s)", props.deviceName,
             have_layer ? "gl_Layer from the vertex shader" : have_gs ? "gl_Layer from a geometry shader" : "none", have_mv ? "yes" : "no",
             have_fbloop ? "yes" : "no", have_lr ? "yes" : "no");
    check(1, "device", dd);
#define LOAD_FN(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n);
    FN(LOAD_FN)
    vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(dev, "vkCmdBeginRendering");
    vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(dev, "vkCmdEndRendering");
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev, &cai, &cb);
    cai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    vkAllocateCommandBuffers(dev, &cai, &scb);
    /* the draws' and the compute's: a sampler3D of the 3D image and a buffer; 16 bytes of push constants */
    VkDescriptorSetLayoutBinding lb[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb};
    vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    if (!have_gs) pcr.stageFlags &= ~VK_SHADER_STAGE_GEOMETRY_BIT;
    pc_stages = pcr.stageFlags;
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr};
    vkCreatePipelineLayout(dev, &pli, NULL, &pl);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 4, 2, ps};
    vkCreateDescriptorPool(dev, &dpi, NULL, &dpool);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev, &sci, NULL, &smp);
    VkShaderModule cs = module(spv_read_comp, sizeof(spv_read_comp));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                       {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", NULL},
                                       pl, VK_NULL_HANDLE, -1};
    if (cs) vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &cpipe);
    if (cs) vkDestroyShaderModule(dev, cs, NULL);
    if (!cpipe) check(0, "compute pipeline (the sampled reads after each pass)", "vkCreateComputePipelines failed");

    const uint32_t RL = VK_REMAINING_ARRAY_LAYERS;
    const VkImageViewType A = VK_IMAGE_VIEW_TYPE_2D_ARRAY, V2 = VK_IMAGE_VIEW_TYPE_2D;
    const VkRect2D part = {{2, 1}, {4, 5}};
    const pcase cases[] = {
        /* several slices (the bug on PowerVR DXT 25.1: slices 0..n-1) */
        {"pass: 3 slices from 2, loadOp CLEAR", RP, CLEAR, A, 0, 2, 3, {{0}}},
        {"pass: REMAINING layers from 2, loadOp CLEAR", RP, CLEAR, A, 0, 2, RL, {{0}}},
        {"pass: 3 slices from 2, loadOp LOAD, a clear of view layer 1", RP, LOAD, A, 0, 2, 3, {{0}}},
        {"pass: 4 slices from 1, loadOp LOAD, partial area", RP, LOAD, A, 0, 1, 4, part},
        {"pass: 2 slices of mip 1 from 1, loadOp CLEAR", RP, CLEAR, A, 1, 1, 2, {{0}}},
        {"pass: 3 slices from 3, loadOp CLEAR, partial area", RP, CLEAR, A, 0, 3, 3, part},
        {"pass: 3 slices from 2, loadOp LOAD, in a secondary command buffer", RP_SEC, LOAD, A, 0, 2, 3, {{0}}},
        {"imageless: 3 slices from 2, loadOp CLEAR", RP_IMAGELESS, CLEAR, A, 0, 2, 3, {{0}}},
        {"dynamic: 3 slices from 2, loadOp CLEAR", DYN, CLEAR, A, 0, 2, 3, {{0}}},
        {"dynamic: REMAINING layers from 1, loadOp CLEAR", DYN, CLEAR, A, 0, 1, RL, {{0}}},
        {"dynamic: 3 slices from 2, loadOp LOAD, a clear of view layer 1", DYN, LOAD, A, 0, 2, 3, {{0}}},
        {"dynamic: 4 slices from 1, loadOp LOAD, partial area", DYN, LOAD, A, 0, 1, 4, part},
        /* layered draws: the shader picks the slice (layers 0 and 2 drawn, 1 cleared only) */
        {"pass: layered draws into layers 0 and 2 of 3 slices from 2", RP, CLEAR, A, 0, 2, 3, {{0}}, D_LAYER, 0, "ckc", 0, 2, 2},
        {"dynamic: layered draws into layers 0 and 2 of 3 slices from 2", DYN, CLEAR, A, 0, 2, 3, {{0}}, D_LAYER, 0, "ckc", 0, 2, 2},
        {"pass: layered draws, loadOp LOAD, partial area, 3 slices of 4 from 1", RP, LOAD, A, 0, 1, 4, part, D_LAYER, 0, "ckcu", 0, 2, 2},
        {"pass: a layered draw into layer 1 of a 2-layer framebuffer over 4 slices from 1", RP, CLEAR, A, 0, 1, 4, {{0}}, D_LAYER, 2, "kcuu", 1, 1, 1},
        {"dynamic: a layered draw into layer 1, layerCount 2 over 4 slices from 1", DYN, CLEAR, A, 0, 1, 4, {{0}}, D_LAYER, 2, "kcuu", 1, 1, 1},
        {"pass: layered draws of mip 1, 2 slices from 1", RP, CLEAR, A, 1, 1, 2, {{0}}, D_LAYER, 0, "cc", 0, 1, 2},
        /* multiview: view mask 101 (a hole at view 1, not cleared by the pass) */
        {"pass: multiview, view mask 101 over 3 slices from 2", RP, CLEAR, A, 0, 2, 3, {{0}}, D_MV, 0, "cuc", 0, 0, 0, 5},
        {"dynamic: multiview, view mask 101 over 3 slices from 2", DYN, CLEAR, A, 0, 2, 3, {{0}}, D_MV, 0, "cuc", 0, 0, 0, 5},
        /* reads of the 3D image while the pass renders it */
        {"pass: feedback loop, draws, a barrier, draws sampling the 3D image, 3 slices from 2", RP, CLEAR, A, 0, 2, 3, {{0}}, D_FB, 0, "bbb", 0, 1, 3},
        {"dynamic: feedback loop, draws, a barrier, draws sampling the 3D image, 3 slices from 2", DYN, CLEAR, A, 0, 2, 3, {{0}}, D_FB, 0, "bbb", 0, 1,
         3},
        {"pass: feedback loop, partial area, 2 slices from 3", RP, CLEAR, A, 0, 3, 2, part, D_FB, 0, "bb", 0, 1, 2},
        {"pass: subpass 1 samples what subpass 0 drew into 3 slices from 2 (GENERAL)", RP, CLEAR, A, 0, 2, 3, {{0}}, D_SUB, 0, "ccc", 0, 1, 3},
        /* one slice: 2D-array and 2D views */
        {"pass: one-slice 2D_ARRAY view of slice 3", RP, CLEAR, A, 0, 3, 1, {{0}}},
        {"pass: 2D view of slice 4", RP, CLEAR, V2, 0, 4, 1, {{0}}},
        {"pass: one-slice 2D_ARRAY view of slice 5, partial area", RP, CLEAR, A, 0, 5, 1, part},
        {"dynamic: one-slice 2D_ARRAY view of slice 3", DYN, CLEAR, A, 0, 3, 1, {{0}}},
        {"dynamic: 2D view of slice 4", DYN, CLEAR, V2, 0, 4, 1, {{0}}},
        {"pass: one-slice 2D_ARRAY view of mip 1, slice 2", RP, CLEAR, A, 1, 2, 1, {{0}}},
        {"pass: one-slice 2D_ARRAY view of slice 3, a draw", RP, CLEAR, A, 0, 3, 1, {{0}}, D_LAYER, 0, "c", 0, 1, 1},
        {"pass: one-slice 2D_ARRAY view of slice 3, feedback loop", RP, CLEAR, A, 0, 3, 1, {{0}}, D_FB, 0, "b", 0, 1, 1},
        /* from slice 0: never affected */
        {"pass: 3 slices from 0, loadOp CLEAR", RP, CLEAR, A, 0, 0, 3, {{0}}},
        {"pass: layered draws into 3 slices from 0", RP, CLEAR, A, 0, 0, 3, {{0}}, D_LAYER, 0, "ckc", 0, 2, 2},
        /* plain 2D-array images (pv_fbloop_split, PV-06): the feedback loops read right through the bridge on any driver; every other
         * pass, a barrier in a pass with a layout shaders read (GENERAL) among them, is left as it is */
        {"plain pass: feedback loop, draws, a barrier, draws sampling the image, 3 layers from 1", RP, CLEAR, A, 0, 1, 3, {{0}}, D_FB, 0, "bbb", 0, 1, 3, 0, 1},
        {"plain dynamic: feedback loop, draws, a barrier, draws sampling the image, 3 layers from 1", DYN, CLEAR, A, 0, 1, 3, {{0}}, D_FB, 0, "bbb", 0, 1, 3, 0,
         1},
        {"plain pass: feedback loop, partial area, 2 layers from 3", RP, CLEAR, A, 0, 3, 2, part, D_FB, 0, "bb", 0, 1, 2, 0, 1},
        {"plain pass: feedback loop, 2D view of layer 4", RP, CLEAR, V2, 0, 4, 1, {{0}}, D_FB, 0, "b", 0, 1, 1, 0, 1},
        {"plain pass: feedback loop, mip 1, 2 layers from 1", RP, CLEAR, A, 1, 1, 2, {{0}}, D_FB, 0, "bb", 0, 1, 2, 0, 1},
        {"plain pass: feedback loop, the draws and the barrier in a secondary command buffer, 3 layers from 1", RP_SEC, CLEAR, A, 0, 1, 3, {{0}}, D_FB, 0, "bbb", 0,
         1, 3, 0, 1},
        {"plain pass: 3 layers from 1, loadOp CLEAR", RP, CLEAR, A, 0, 1, 3, {{0}}, D_NONE, 0, NULL, 0, 0, 0, 0, 1},
        {"plain pass: layered draws into layers 0 and 2 of 3 from 1", RP, CLEAR, A, 0, 1, 3, {{0}}, D_LAYER, 0, "ckc", 0, 2, 2, 0, 1},
        {"plain dynamic: layered draws into layers 0 and 2 of 3 from 1", DYN, CLEAR, A, 0, 1, 3, {{0}}, D_LAYER, 0, "ckc", 0, 2, 2, 0, 1},
        {"plain pass: subpass 1 samples what subpass 0 drew into 3 layers from 1 (GENERAL)", RP, CLEAR, A, 0, 1, 3, {{0}}, D_SUB, 0, "ccc", 0, 1, 3, 0, 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run(&cases[i]);
    }
    if (cpipe) vkDestroyPipeline(dev, cpipe, NULL);
    vkDestroySampler(dev, smp, NULL);
    vkDestroyDescriptorPool(dev, dpool, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks%s\n", checks, skipped ? " (some skipped)" : "");
    return failures != 0;
}
