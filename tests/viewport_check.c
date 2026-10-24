/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * viewport_check: MK-02 (a negative viewport height ignored, Mali-G715 r54p2). Two triangles of
 * opposite winding - upper left and lower right with y up - are drawn into a 32x32 RGBA32UI
 * target under negative-height (and positive) viewports; each fragment writes its facing, its
 * primitive, the sign of dFdy of the app's own y, gl_FragCoord.y and .z, and the app's y there
 * (or gl_PointCoord.y for points) - shaders/viewport/. Every pixel is compared with the CPU's
 * rasterization of the same draws (Vulkan's viewport transform, front face by the sign of the
 * framebuffer area, a pixel within 0.05 px of an edge not compared).
 *
 * Cases: static viewports with and without back-face culling; a positive one; dynamic viewports
 * and scissors, partial ones, a pipeline drawn with both signs in one command buffer (the
 * erratum's variants), vkCmdDrawIndexed and vkCmdDrawIndirect; vkCmdSetViewportWithCount
 * (extended dynamic state); two viewports each flipped in its own half, a negative and a positive
 * one, ViewportIndex from the vertex shader (multiViewport, shaderOutputViewportIndex);
 * vkCmdSetViewport(first = 1) leaving viewport 0 alone; points (gl_PointCoord); a geometry
 * stage; a depth range. The pipelines' render pass, layout and shader modules are destroyed
 * right after the pipelines are made (compatible ones are used to draw), as DXVK does with its
 * modules: a variant built later must not need them.
 *
 * host/viewport_test.sh runs it through the bridge as it is, with the bug imitated beneath
 * (VKBRIDGE_ERRATA_IMITATE=viewport_flip: the negative cases fail), with the fix over the
 * imitation, and with the fix forced on the desktop driver (correct either way).
 *   viewport_check [--no-multi] [--inline] [--bench N]
 *       --no-multi  no two-viewport cases (the bridge's multiViewport emulation, mali mode, draws
 *                   into the first viewport only)
 *       --inline    the shaders chained to the pipelines' stages (maintenance5), no modules
 *       --bench N   N dynamic-viewport pipelines made, each drawn with a negative and then a
 *                   positive viewport; times printed
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const uint32_t spv_vert[] = {
#include "viewport_vert.inc"
};
static const uint32_t spv_vert_mv[] = {
#include "viewport_vert_mv.inc"
};
static const uint32_t spv_geom[] = {
#include "viewport_geom.inc"
};
static const uint32_t spv_frag[] = {
#include "viewport_frag.inc"
};

#define W 32
#define H 32

/* the vertex shader's triangles (vp.vert) */
static const float T[6][2] = {{-0.9f, 0.15f}, {-0.15f, 0.15f}, {-0.9f, 0.85f}, {0.2f, -0.1f}, {0.85f, -0.1f}, {0.85f, -0.85f}};

static VkDevice dev;
static VkQueue queue;
static int failures, checks;
static VkPhysicalDeviceMemoryProperties mp;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceFeatures2);
I(vkEnumerateDeviceExtensionProperties); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkDestroyDevice); I(vkGetDeviceQueue); I(vkCreateCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkCreateBuffer);
I(vkGetBufferMemoryRequirements); I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage);
I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateImageView); I(vkCreateRenderPass); I(vkDestroyRenderPass);
I(vkCreateFramebuffer); I(vkCreateShaderModule); I(vkDestroyShaderModule); I(vkCreatePipelineLayout); I(vkDestroyPipelineLayout);
I(vkCreateGraphicsPipelines); I(vkDestroyPipeline); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass); I(vkCmdBindPipeline);
I(vkCmdPushConstants); I(vkCmdDraw); I(vkCmdDrawIndexed); I(vkCmdDrawIndirect); I(vkCmdBindIndexBuffer); I(vkCmdSetViewport);
I(vkCmdSetScissor); I(vkCmdSetViewportWithCount); I(vkCmdSetScissorWithCount); I(vkCmdCopyImageToBuffer); I(vkCmdPipelineBarrier);
I(vkDeviceWaitIdle); I(vkFreeCommandBuffers);
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

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, NULL, &m);
    return m;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ the CPU's picture */

typedef struct {
    uint32_t v[4];
    int care; /* 0: not compared (a center on an edge) */
} px;

static px want[H][W];

/* fb position of an NDC point under v */
static void to_fb(VkViewport v, float x, float y, double *fx, double *fy)
{
    *fx = v.x + v.width / 2.0 + v.width / 2.0 * x;
    *fy = v.y + v.height / 2.0 + v.height / 2.0 * y;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* One draw's triangles (or points) for one instance, into want[][] */
static void cpu_draw(VkViewport v, VkRect2D sc, int cull_back, int points, float z)
{
    double zf = v.minDepth + (v.maxDepth - v.minDepth) * z;
    for (int t = 0; t < 2; t++) {
        if (points) {
            for (int k = 0; k < 3; k++) {
                int vi = t * 3 + k;
                double cx, cy;
                to_fb(v, T[vi][0], T[vi][1], &cx, &cy);
                for (int py = 0; py < H; py++)
                    for (int x = 0; x < W; x++) {
                        double fx = x + 0.5, fy = py + 0.5;
                        if (x < sc.offset.x || py < sc.offset.y || x >= sc.offset.x + (int)sc.extent.width ||
                            py >= sc.offset.y + (int)sc.extent.height)
                            continue;
                        double dx = fabs(fx - cx), dy = fabs(fy - cy);
                        if (dx > 2.05 || dy > 2.05) continue;
                        px *p = &want[py][x];
                        if (fabs(dx - 2) < 0.05 || fabs(dy - 2) < 0.05) {
                            p->care = 0;
                            continue;
                        }
                        p->care = 1;
                        p->v[0] = 1u | (uint32_t)(vi / 3) << 4 | 3u << 8;
                        p->v[1] = fbits((float)fy);
                        p->v[2] = fbits((float)zf);
                        p->v[3] = fbits((float)((fy - (cy - 2.0)) / 4.0));
                    }
            }
            continue;
        }
        double fx[3], fy[3];
        for (int k = 0; k < 3; k++) to_fb(v, T[t * 3 + k][0], T[t * 3 + k][1], &fx[k], &fy[k]);
        /* Vulkan: a = -1/2 sum(x_i y_i+1 - x_i+1 y_i); counter-clockwise front: a > 0 */
        double a = 0;
        for (int k = 0; k < 3; k++) a += fx[k] * fy[(k + 1) % 3] - fx[(k + 1) % 3] * fy[k];
        a *= -0.5;
        int front = a > 0;
        if (cull_back && !front) continue;
        for (int py = 0; py < H; py++)
            for (int x = 0; x < W; x++) {
                if (x < sc.offset.x || py < sc.offset.y || x >= sc.offset.x + (int)sc.extent.width || py >= sc.offset.y + (int)sc.extent.height)
                    continue;
                double cx = x + 0.5, cy = py + 0.5;
                int inside = 1, edge = 0;
                for (int k = 0; k < 3; k++) {
                    double ex = fx[(k + 1) % 3] - fx[k], ey = fy[(k + 1) % 3] - fy[k];
                    double e = (ex * (cy - fy[k]) - ey * (cx - fx[k])) / sqrt(ex * ex + ey * ey);
                    if (a > 0) e = -e; /* a > 0: clockwise in y-up terms, the inside on the right */
                    if (fabs(e) < 0.05) edge = 1;
                    if (e < 0) inside = 0;
                }
                if (!inside && !edge) continue;
                px *p = &want[py][x];
                if (edge) {
                    p->care = 0;
                    continue;
                }
                p->care = 1;
                p->v[0] = (front ? 1u : 2u) | (uint32_t)t << 4 | (v.height > 0 ? 1u : 2u) << 8;
                p->v[1] = fbits((float)cy);
                p->v[2] = fbits((float)zf);
                p->v[3] = fbits((float)((cy - (v.y + v.height / 2.0)) / (v.height / 2.0)));
            }
    }
}

static float f_of(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* the picture against want[][]: 1 when it matches */
static int compare(const uint32_t *got, char *msg, size_t cap, int points)
{
    int bad = 0, lit = 0, care = 0;
    msg[0] = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint32_t *g = got + (y * W + x) * 4;
            const px *p = &want[y][x];
            if (!p->care) continue;
            care++;
            if (p->v[0]) lit++;
            int ok = g[0] == p->v[0];
            if (ok && p->v[0]) {
                ok = g[1] == p->v[1] && fabsf(f_of(g[2]) - f_of(p->v[2])) < 1e-5f &&
                     fabsf(f_of(g[3]) - f_of(p->v[3])) < (points ? 1e-3f : 2e-3f);
            }
            if (!ok && !bad++)
                snprintf(msg, cap, "pixel (%d, %d): r 0x%x y %.2f z %.4f a %.4f, wanted r 0x%x y %.2f z %.4f a %.4f", x, y, g[0], f_of(g[1]),
                         f_of(g[2]), f_of(g[3]), p->v[0], f_of(p->v[1]), f_of(p->v[2]), f_of(p->v[3]));
        }
    size_t n = strlen(msg);
    snprintf(msg + n, cap - n, "%s%d of %d pixels wrong (%d lit)", n ? "; " : "", bad, care, lit);
    return !bad && lit;
}

/* rows of the picture, '#' lit (front), 'b' back, '.' clear: for a failing case's detail */
static void picture(const uint32_t *got)
{
    for (int y = 0; y < H; y += 2) {
        char row[W + 1];
        for (int x = 0; x < W; x++) {
            uint32_t r = got[(y * W + x) * 4];
            row[x] = !r ? '.' : (r & 0xf) == 1 ? ((r >> 4 & 0xf) ? 'B' : 'A') : ((r >> 4 & 0xf) ? 'b' : 'a');
        }
        row[W] = 0;
        printf("     %s\n", row);
    }
}

/* ------------------------------------------------------------------ the GPU's */

static VkRenderPass rp;
static VkFramebuffer fb;
static VkPipelineLayout pl;
static VkImage img;
static VkBuffer rbuf, ibuf, indbuf;
static uint32_t *rb;
static VkCommandPool cpool;

typedef struct {
    VkPipeline p;
    int dyn;       /* 0 static, 1 VIEWPORT + SCISSOR, 2 WITH_COUNT */
    int cull_back;
    int points;
    int multi;     /* ViewportIndex from the vertex shader */
    VkViewport vp[2];
    VkRect2D sc[2];
    uint32_t nvp;
} pipe;

static VkViewport VP(float x, float y, float w, float h) { return (VkViewport){x, y, w, h, 0.0f, 1.0f}; }
static VkRect2D SC(int x, int y, uint32_t w, uint32_t h) { return (VkRect2D){{x, y}, {w, h}}; }
static const VkRect2D FULL = {{0, 0}, {W, H}};

static VkShaderModule m_vert, m_vert_mv, m_geom, m_frag;
static int inline_mods;

/* --inline: a stage's code chained to it, its module VK_NULL_HANDLE */
static VkShaderModuleCreateInfo mci_of(VkShaderModule m)
{
    VkShaderModuleCreateInfo c = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    if (m == m_vert) c.codeSize = sizeof(spv_vert), c.pCode = spv_vert;
    if (m == m_vert_mv) c.codeSize = sizeof(spv_vert_mv), c.pCode = spv_vert_mv;
    if (m == m_geom) c.codeSize = sizeof(spv_geom), c.pCode = spv_geom;
    if (m == m_frag) c.codeSize = sizeof(spv_frag), c.pCode = spv_frag;
    return c;
}

static void inline_stages(VkPipelineShaderStageCreateInfo *st, VkShaderModuleCreateInfo *mci, uint32_t n)
{
    for (uint32_t i = 0; inline_mods && i < n; i++) {
        mci[i] = mci_of(st[i].module);
        st[i].pNext = &mci[i];
        st[i].module = VK_NULL_HANDLE;
    }
}

static int make_pipe(pipe *p, VkRenderPass prp, VkPipelineLayout ppl, int gs)
{
    VkPipelineShaderStageCreateInfo st[3] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, p->multi ? m_vert_mv : m_vert, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, m_frag, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, m_geom, "main", NULL}};
    VkShaderModuleCreateInfo mci[3];
    inline_stages(st, mci, 3);
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 p->points ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, p->nvp, p->vp, p->nvp, p->sc};
    if (p->dyn == 2) vps.viewportCount = vps.scissorCount = 0;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = p->cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkDynamicState dyn1[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkDynamicState dyn2[2] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, p->dyn == 2 ? dyn2 : dyn1};
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, gs ? 3 : 2, st, &vi, &ia, NULL, &vps, &rs, &ms,
                                       NULL, &cbs, p->dyn ? &ds : NULL, ppl, prp, 0, VK_NULL_HANDLE, -1};
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &p->p);
}

/* The same pipeline as four graphics pipeline libraries, linked; the libraries go at once. */
static int make_gpl(pipe *p, VkRenderPass prp, VkPipelineLayout ppl)
{
    VkPipelineShaderStageCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, m_vert, "main", NULL};
    VkPipelineShaderStageCreateInfo fs = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, m_frag, "main", NULL};
    VkShaderModuleCreateInfo mci[2];
    inline_stages(&vs, &mci[0], 1);
    inline_stages(&fs, &mci[1], 1);
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = p->cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    const VkGraphicsPipelineLibraryFlagsEXT parts[4] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    VkPipeline lib[4];
    for (int i = 0; i < 4; i++) {
        VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, NULL, parts[i]};
        VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &gl, VK_PIPELINE_CREATE_LIBRARY_BIT_KHR};
        gp.basePipelineIndex = -1;
        if (i == 0) gp.pVertexInputState = &vi, gp.pInputAssemblyState = &ia;
        if (i == 1) gp.stageCount = 1, gp.pStages = &vs, gp.pViewportState = &vps, gp.pRasterizationState = &rs, gp.pDynamicState = &ds;
        if (i == 2) gp.stageCount = 1, gp.pStages = &fs, gp.pMultisampleState = &ms;
        if (i == 3) gp.pColorBlendState = &cbs, gp.pMultisampleState = &ms;
        if (i == 1 || i == 2) gp.layout = ppl;
        if (i >= 1) gp.renderPass = prp;
        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &lib[i]);
        if (r) return r;
    }
    VkPipelineLibraryCreateInfoKHR li = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, NULL, 4, lib};
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &li};
    gp.layout = ppl;
    gp.basePipelineIndex = -1;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &p->p);
    for (int i = 0; i < 4; i++) vkDestroyPipeline(dev, lib[i], NULL);
    return r;
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clear = {.color = {.uint32 = {0, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, H}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindIndexBuffer(cb, ibuf, 0, VK_INDEX_TYPE_UINT32);
    memset(want, 0, sizeof(want));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) want[y][x].care = 1;
    return cb;
}

/* the draw kinds */
enum { D_DRAW, D_INDEXED, D_INDIRECT };

/* One draw: dynamic state from vp/sc (n of them; first for vkCmdSetViewport), then the CPU's. */
static void draw(VkCommandBuffer cb, const pipe *p, uint32_t n, const VkViewport *vp, const VkRect2D *sc, float z, int kind)
{
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p);
    if (p->dyn == 1) {
        vkCmdSetViewport(cb, 0, n, vp);
        vkCmdSetScissor(cb, 0, n, sc);
    } else if (p->dyn == 2) {
        vkCmdSetViewportWithCount(cb, n, vp);
        vkCmdSetScissorWithCount(cb, n, sc);
    } else {
        n = p->nvp, vp = p->vp, sc = p->sc;
    }
    struct { int mode; float zz; } pc = {p->points, z};
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, &pc);
    uint32_t inst = p->multi ? n : 1;
    if (kind == D_INDEXED) vkCmdDrawIndexed(cb, 6, inst, 0, 0, 0);
    else if (kind == D_INDIRECT) vkCmdDrawIndirect(cb, indbuf, inst == 1 ? 0 : 16, 1, 16);
    else vkCmdDraw(cb, 6, inst, 0, 0);
    for (uint32_t i = 0; i < inst; i++) cpu_draw(vp[p->multi ? i : 0], sc[p->multi ? i : 0], p->cull_back, p->points, z);
}

static void finish(VkCommandBuffer cb, const char *what, int points)
{
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    memset(rb, 0xee, W * H * 16);
    char msg[512];
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(queue)) {
        check(0, what, "submit");
        return;
    }
    int ok = compare(rb, msg, sizeof(msg), points);
    check(ok, what, msg);
    if (!ok) picture(rb);
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
}

int main(int argc, char **argv)
{
    int bench = 0, no_multi = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-multi")) no_multi = 1;
        if (!strcmp(argv[i], "--inline")) inline_mods = 1;
        if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL no libvulkan\n");
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "viewport_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAIL vkCreateInstance\n");
        return 1;
    }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceFeatures2); L(vkEnumerateDeviceExtensionProperties);
    L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
    L(vkDestroyInstance);
#undef L
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAIL no GPU\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &feds};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    int core13 = props.apiVersion >= VK_API_VERSION_1_3;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    int has_eds = 0;
    for (uint32_t i = 0; i < ne; i++) has_eds |= !strcmp(ext[i].extensionName, "VK_EXT_extended_dynamic_state");
    int eds = core13 || (has_eds && feds.extendedDynamicState);
    int multi = f2.features.multiViewport && f12.shaderOutputViewportIndex && !no_multi;
    int has_gpl = 0, has_pl = 0;
    for (uint32_t i = 0; i < ne; i++) {
        has_gpl |= !strcmp(ext[i].extensionName, "VK_EXT_graphics_pipeline_library");
        has_pl |= !strcmp(ext[i].extensionName, "VK_KHR_pipeline_library");
    }
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (has_gpl && has_pl) {
        VkPhysicalDeviceFeatures2 q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fgpl};
        vkGetPhysicalDeviceFeatures2(pd, &q);
    }
    int gpl = has_gpl && has_pl && fgpl.graphicsPipelineLibrary;
    int has_m5 = 0;
    for (uint32_t i = 0; i < ne; i++) has_m5 |= !strcmp(ext[i].extensionName, "VK_KHR_maintenance5");
    if (inline_mods && !has_m5 && props.apiVersion < VK_API_VERSION_1_4) {
        printf("SKIP --inline: no maintenance5\n0 checks, 0 failed\n");
        return 0;
    }
    int geom = f2.features.geometryShader;
    printf("GPU: %s, Vulkan %u.%u; extended dynamic state %d, multi-viewport with ViewportIndex %d, geometry %d, pipeline libraries %d, "
           "maxViewports %u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), eds, multi,
           geom, gpl, props.limits.maxViewports);

    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.shaderOutputViewportIndex = multi;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT egpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, NULL, VK_TRUE};
    if (gpl) egpl.pNext = e12.pNext, e12.pNext = &egpl;
    VkPhysicalDeviceMaintenance5Features em5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES, NULL, VK_TRUE};
    if (inline_mods) em5.pNext = e12.pNext, e12.pNext = &em5;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eeds = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, &e12, VK_TRUE};
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, !core13 && eds ? (void *)&eeds : (void *)&e12};
    ef.features.multiViewport = multi;
    ef.features.geometryShader = geom;
    const char *names[4];
    uint32_t nnames = 0;
    if (!core13 && eds) names[nnames++] = "VK_EXT_extended_dynamic_state";
    if (gpl) names[nnames++] = "VK_KHR_pipeline_library", names[nnames++] = "VK_EXT_graphics_pipeline_library";
    if (inline_mods && props.apiVersion < VK_API_VERSION_1_4) names[nnames++] = "VK_KHR_maintenance5";
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, NULL, nnames, names, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) {
        printf("FAIL vkCreateDevice %d\n", r);
        return 1;
    }
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkDestroyDevice); D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory);
    D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkBindImageMemory); D(vkCreateImageView);
    D(vkCreateRenderPass); D(vkDestroyRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule); D(vkDestroyShaderModule);
    D(vkCreatePipelineLayout); D(vkDestroyPipelineLayout); D(vkCreateGraphicsPipelines); D(vkDestroyPipeline); D(vkCmdBeginRenderPass);
    D(vkCmdEndRenderPass); D(vkCmdBindPipeline); D(vkCmdPushConstants); D(vkCmdDraw); D(vkCmdDrawIndexed); D(vkCmdDrawIndirect);
    D(vkCmdBindIndexBuffer); D(vkCmdSetViewport); D(vkCmdSetScissor); D(vkCmdCopyImageToBuffer); D(vkCmdPipelineBarrier);
    D(vkDeviceWaitIdle); D(vkFreeCommandBuffers);
#undef D
    if (core13) {
        vkCmdSetViewportWithCount = (PFN_vkCmdSetViewportWithCount)vkGetDeviceProcAddr(dev, "vkCmdSetViewportWithCount");
        vkCmdSetScissorWithCount = (PFN_vkCmdSetScissorWithCount)vkGetDeviceProcAddr(dev, "vkCmdSetScissorWithCount");
    } else if (eds) {
        vkCmdSetViewportWithCount = (PFN_vkCmdSetViewportWithCount)vkGetDeviceProcAddr(dev, "vkCmdSetViewportWithCountEXT");
        vkCmdSetScissorWithCount = (PFN_vkCmdSetScissorWithCount)vkGetDeviceProcAddr(dev, "vkCmdSetScissorWithCountEXT");
    }
    eds = eds && vkCmdSetViewportWithCount && vkCmdSetScissorWithCount;
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* the target */
    VkFormat fmt = VK_FORMAT_R32G32B32A32_UINT;
    VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements mr;
    VkDeviceMemory imem;
    if (vkCreateImage(dev, &imci, NULL, &img)) {
        printf("FAIL setup: image\n");
        return 1;
    }
    vkGetImageMemoryRequirements(dev, img, &mr);
    uint32_t t = find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_type(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    if (vkAllocateMemory(dev, &mai, NULL, &imem) || vkBindImageMemory(dev, img, imem, 0)) {
        printf("FAIL setup: image memory\n");
        return 1;
    }
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
    VkRenderPass rp_pipe;
    VkPipelineLayout pl_pipe;
    vkCreateRenderPass(dev, &rpci, NULL, &rp);
    vkCreateRenderPass(dev, &rpci, NULL, &rp_pipe); /* compatible: the pipelines are made with this one, which goes */
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 1};
    vkCreateFramebuffer(dev, &fci, NULL, &fb);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    vkCreatePipelineLayout(dev, &plci, NULL, &pl_pipe);
    rbuf = host_buffer(W * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, (void **)&rb);
    uint32_t *ib, *ind;
    ibuf = host_buffer(64, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, (void **)&ib);
    indbuf = host_buffer(64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, (void **)&ind);
    if (!rbuf || !ibuf || !indbuf) {
        printf("FAIL setup: buffers\n");
        return 1;
    }
    for (int i = 0; i < 6; i++) ib[i] = (uint32_t)i;
    uint32_t indv[8] = {6, 1, 0, 0, 6, 2, 0, 0};
    memcpy(ind, indv, sizeof(indv));
    m_vert = shader(spv_vert, sizeof(spv_vert));
    m_frag = shader(spv_frag, sizeof(spv_frag));
    m_vert_mv = multi ? shader(spv_vert_mv, sizeof(spv_vert_mv)) : VK_NULL_HANDLE;
    m_geom = geom ? shader(spv_geom, sizeof(spv_geom)) : VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);
    char msg[256];

    if (bench) {
        /* N dynamic-viewport pipelines (one module pair, as a driver cache would see N compiles
         * only with its cache off), each drawn negative then positive */
        VkPipeline *ps = calloc(bench, sizeof(*ps));
        pipe p = {0};
        p.dyn = 1, p.nvp = 1, p.vp[0] = VP(0, H, W, -H), p.sc[0] = FULL;
        double t0 = now_ms();
        for (int i = 0; i < bench; i++) {
            if (make_pipe(&p, rp_pipe, pl_pipe, 0)) {
                printf("FAIL bench: pipeline %d\n", i);
                return 1;
            }
            ps[i] = p.p;
        }
        double t1 = now_ms();
        VkCommandBuffer cb = begin();
        VkViewport neg = VP(0, H, W, -H), pos = VP(0, 0, W, H);
        VkRect2D sc = FULL;
        for (int i = 0; i < bench; i++) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps[i]);
            vkCmdSetViewport(cb, 0, 1, &neg);
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(cb);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        double t2 = now_ms();
        cb = begin();
        for (int i = 0; i < bench; i++) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ps[i]);
            vkCmdSetViewport(cb, 0, 1, &pos);
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(cb);
        vkEndCommandBuffer(cb);
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        double t3 = now_ms();
        printf("bench: %d pipelines made in %.1f ms (%.2f ms each); drawn negative in %.1f ms, then positive in %.1f ms\n", bench, t1 - t0,
               (t1 - t0) / bench, t2 - t1, t3 - t2);
        for (int i = 0; i < bench; i++) vkDestroyPipeline(dev, ps[i], NULL);
        vkDeviceWaitIdle(dev);
        vkDestroyDevice(dev, NULL);
        vkDestroyInstance(inst, NULL);
        return 0;
    }

    /* the pipelines */
    enum { S_NEG, S_NEG_CULL, S_POS, DYN, DYN_NOCULL, WC, MV_S, MV_MIX, MV_DYN, POINTS, GS, DEPTH, GPL, NP };
    pipe P[NP];
    memset(P, 0, sizeof(P));
    for (int i = 0; i < NP; i++) P[i].nvp = 1, P[i].vp[0] = VP(0, H, W, -H), P[i].sc[0] = FULL;
    P[S_NEG_CULL].cull_back = 1;
    P[S_POS].cull_back = 1, P[S_POS].vp[0] = VP(0, 0, W, H);
    P[DYN].dyn = 1, P[DYN].cull_back = 1;
    P[DYN_NOCULL].dyn = 1;
    P[WC].dyn = 2, P[WC].cull_back = 1, P[WC].multi = multi;
    P[MV_S].multi = 1, P[MV_S].nvp = 2, P[MV_S].vp[0] = VP(0, H, 16, -H), P[MV_S].vp[1] = VP(16, H, 16, -H), P[MV_S].sc[1] = FULL;
    P[MV_MIX] = P[MV_S], P[MV_MIX].vp[1] = VP(16, 0, 16, H);
    P[MV_DYN] = P[MV_S], P[MV_DYN].dyn = 1, P[MV_DYN].cull_back = 1;
    P[POINTS].points = 1, P[POINTS].dyn = 1;
    P[GS].cull_back = 1;
    P[DEPTH].vp[0].minDepth = 0.2f, P[DEPTH].vp[0].maxDepth = 0.6f;
    P[GPL].dyn = 1, P[GPL].cull_back = 1;
    int have[NP];
    for (int i = 0; i < NP; i++) {
        int needs_mv = (i == MV_S || i == MV_MIX || i == MV_DYN);
        if ((needs_mv && !multi) || (i == WC && !eds) || (i == GS && !geom) || (i == GPL && !gpl)) {
            have[i] = 0;
            continue;
        }
        r = i == GPL ? make_gpl(&P[i], rp_pipe, pl_pipe) : make_pipe(&P[i], rp_pipe, pl_pipe, i == GS);
        snprintf(msg, sizeof(msg), "pipeline %d: VkResult %d", i, r);
        if (r) check(0, "setup", msg);
        have[i] = !r;
    }
    /* what the pipelines were made with goes now */
    vkDestroyRenderPass(dev, rp_pipe, NULL);
    vkDestroyPipelineLayout(dev, pl_pipe, NULL);
    vkDestroyShaderModule(dev, m_vert, NULL);
    vkDestroyShaderModule(dev, m_frag, NULL);
    if (m_vert_mv) vkDestroyShaderModule(dev, m_vert_mv, NULL);
    if (m_geom) vkDestroyShaderModule(dev, m_geom, NULL);

    VkCommandBuffer cb;
    VkViewport v[2];
    VkRect2D s[2];
#define CASE(name) (printf("---- %s\n", name), name)
    if (have[S_NEG]) {
        cb = begin();
        draw(cb, &P[S_NEG], 1, NULL, NULL, 0.3f, D_DRAW);
        finish(cb, CASE("static viewport (0, 32, 32, -32), no culling: both triangles, facing"), 0);
    }
    if (have[S_NEG_CULL]) {
        cb = begin();
        draw(cb, &P[S_NEG_CULL], 1, NULL, NULL, 0.3f, D_DRAW);
        finish(cb, CASE("static viewport (0, 32, 32, -32), back faces culled"), 0);
    }
    if (have[S_POS]) {
        cb = begin();
        draw(cb, &P[S_POS], 1, NULL, NULL, 0.3f, D_DRAW);
        finish(cb, CASE("static viewport (0, 0, 32, 32), back faces culled"), 0);
    }
    if (have[DYN]) {
        cb = begin();
        v[0] = VP(4, 30, 24, -26), s[0] = SC(2, 6, 28, 20);
        draw(cb, &P[DYN], 1, v, s, 0.3f, D_INDIRECT);
        finish(cb, CASE("dynamic viewport (4, 30, 24, -26), dynamic scissor (2, 6, 28, 20), vkCmdDrawIndirect"), 0);
        cb = begin();
        v[0] = VP(4, 2, 24, 26), s[0] = FULL;
        draw(cb, &P[DYN], 1, v, s, 0.3f, D_DRAW);
        finish(cb, CASE("the same pipeline, dynamic viewport (4, 2, 24, 26)"), 0);
        cb = begin();
        v[0] = VP(0, 32, 16, -32), s[0] = SC(0, 0, 16, 32);
        draw(cb, &P[DYN], 1, v, s, 0.3f, D_DRAW);
        v[0] = VP(16, 0, 16, 32), s[0] = SC(16, 0, 16, 32);
        draw(cb, &P[DYN], 1, v, s, 0.3f, D_DRAW);
        v[0] = VP(8, 20, 16, -12), s[0] = SC(8, 8, 16, 12);
        draw(cb, &P[DYN], 1, v, s, 0.3f, D_INDEXED);
        finish(cb, CASE("one command buffer, one pipeline: negative, positive, negative (vkCmdDrawIndexed)"), 0);
    }
    if (have[DYN_NOCULL]) {
        cb = begin();
        v[0] = VP(2, 31, 28, -29), s[0] = SC(3, 1, 26, 30);
        draw(cb, &P[DYN_NOCULL], 1, v, s, 0.3f, D_INDEXED);
        finish(cb, CASE("dynamic viewport (2, 31, 28, -29), no culling, vkCmdDrawIndexed"), 0);
    }
    if (have[WC]) {
        cb = begin();
        uint32_t nv = multi ? 2 : 1;
        v[0] = VP(0, 32, 16, -32), v[1] = VP(16, 32, 16, -32), s[0] = s[1] = FULL;
        if (!multi) v[0] = VP(0, 32, 32, -32);
        draw(cb, &P[WC], nv, v, s, 0.3f, D_DRAW);
        finish(cb, CASE(multi ? "vkCmdSetViewportWithCount: two viewports, each flipped in its half" : "vkCmdSetViewportWithCount: one viewport"), 0);
        cb = begin();
        draw(cb, &P[WC], 1, (VkViewport[]){VP(0, 32, 32, -32)}, s, 0.3f, D_DRAW);
        finish(cb, CASE("vkCmdSetViewportWithCount: count 1 on the same pipeline"), 0);
    } else {
        printf("SKIP vkCmdSetViewportWithCount: no extended dynamic state\n");
    }
    if (have[MV_S]) {
        cb = begin();
        draw(cb, &P[MV_S], 2, NULL, NULL, 0.3f, D_DRAW);
        finish(cb, CASE("two static viewports, each flipped in its half, ViewportIndex from the vertex shader"), 0);
        cb = begin();
        draw(cb, &P[MV_MIX], 2, NULL, NULL, 0.3f, D_DRAW);
        finish(cb, CASE("two static viewports, negative and positive"), 0);
        cb = begin();
        v[0] = VP(0, 32, 16, -32), v[1] = VP(16, 0, 16, 32), s[0] = SC(0, 0, 16, 32), s[1] = SC(16, 0, 16, 32);
        draw(cb, &P[MV_DYN], 2, v, s, 0.3f, D_DRAW);
        finish(cb, CASE("two dynamic viewports, negative and positive"), 0);
        cb = begin();
        v[0] = VP(0, 32, 16, -32), v[1] = VP(16, 0, 16, 32);
        draw(cb, &P[MV_DYN], 2, v, s, 0.3f, D_DRAW);
        /* viewport 1 only: viewport 0 keeps what it had */
        VkViewport v1 = VP(16, 32, 16, -32);
        vkCmdSetViewport(cb, 1, 1, &v1);
        vkCmdDraw(cb, 6, 2, 0, 0);
        v[1] = v1;
        for (int i = 0; i < 2; i++) cpu_draw(v[i], s[i], 1, 0, 0.3f);
        finish(cb, CASE("vkCmdSetViewport(first = 1) leaves viewport 0 as it was"), 0);
    } else {
        printf("SKIP multi-viewport: no multiViewport / shaderOutputViewportIndex\n");
    }
    if (have[POINTS]) {
        cb = begin();
        v[0] = VP(0, 32, 32, -32), s[0] = FULL;
        draw(cb, &P[POINTS], 1, v, s, 0.3f, D_DRAW);
        finish(cb, CASE("points of size 4, negative viewport: gl_PointCoord"), 1);
    }
    if (have[GS]) {
        cb = begin();
        draw(cb, &P[GS], 1, NULL, NULL, 0.3f, D_DRAW);
        finish(cb, CASE("a geometry stage, negative viewport, back faces culled"), 0);
    } else {
        printf("SKIP geometry stage: no geometryShader\n");
    }
    if (have[DEPTH]) {
        cb = begin();
        draw(cb, &P[DEPTH], 1, NULL, NULL, 0.3f, D_DRAW);
        finish(cb, CASE("depth range 0.2-0.6, negative viewport: gl_FragCoord.z"), 0);
    }
    if (have[GPL]) {
        cb = begin();
        v[0] = VP(0, 32, 32, -32), s[0] = FULL;
        draw(cb, &P[GPL], 1, v, s, 0.3f, D_DRAW);
        finish(cb, CASE("graphics pipeline library, linked (the libraries gone): negative viewport"), 0);
        cb = begin();
        v[0] = VP(0, 0, 16, 32), s[0] = SC(0, 0, 16, 32);
        draw(cb, &P[GPL], 1, v, s, 0.3f, D_DRAW);
        v[0] = VP(16, 32, 16, -32), s[0] = SC(16, 0, 16, 32);
        draw(cb, &P[GPL], 1, v, s, 0.3f, D_DRAW);
        finish(cb, CASE("graphics pipeline library, linked: positive, then negative"), 0);
    } else {
        printf("SKIP graphics pipeline library: not there\n");
    }
    for (int i = 0; i < NP; i++)
        if (have[i]) vkDestroyPipeline(dev, P[i].p, NULL);
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
