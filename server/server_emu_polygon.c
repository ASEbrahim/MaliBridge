/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Polygon mode LINE and POINT (wireframe, D3D's FILL_WIREFRAME / D3DFILL_POINT) on a GPU without
 * fillModeNonSolid (Mali-G720 on r44). DXVK 3 will not start without the feature, so it is
 * reported (VKB_EMU_FILL_MODE); this file draws what such a pipeline asks for instead of the solid
 * triangles server_emu_compat.c leaves it with.
 *
 * How: a pipeline with a non-solid polygon mode gets a variant built from the same create info
 * with a line-list (LINE) or point-list (POINT) topology and solid fill. The app's handle stays the
 * solid pipeline - the fallback for any draw the emulation cannot take. A draw with it bound binds
 * the variant, draws, and puts the app's pipeline and index buffer back:
 *  - POINT: the same draw on the point list: every vertex the triangles use is a point (a strip's
 *    or fan's shared vertices once, not once per triangle: the same pixels unless blended).
 *  - LINE, vkCmdDraw: the vertices go through a shared index buffer that lists each triangle's
 *    three edges - list (3t,3t+1)(3t+1,3t+2)(3t+2,3t), strip (t,t+1)(t+1,t+2)(t+2,t), fan
 *    (0,t+1)(t+1,t+2)(t+2,0) - drawn indexed with vertexOffset = firstVertex, so gl_VertexIndex is
 *    what the app's draw gives it. Made once on the CPU and grown as draws need; no GPU work.
 *  - LINE, vkCmdDrawIndexed: the edges come from the app's indices, which only the GPU has at the
 *    time the draw runs. A compute shader (shaders/polygon/lines.comp) writes them, one invocation
 *    per triangle, into the command buffer's scratch; it runs in a command buffer of the
 *    emulation's own (from the app's pool) that is submitted right before the app's, so the draw
 *    stays where it is in the render pass (a render pass cannot hold a dispatch, and ending and
 *    starting it again costs a tile store and load per draw on Mali). It reads the indices as they
 *    are when the app's command buffer starts: a draw whose index buffer an earlier command of the
 *    same command buffer writes (copy, update, fill) is drawn solid instead (logged once).
 *    Strip restarts are honoured; a fan with restart enabled is drawn solid.
 *    In a secondary command buffer (DXVK 3 records every render pass into one on a tiler such as
 *    Mali: dxvk.tilerMode) the dispatches go into a primary of the emulation's own as well, from
 *    the secondary's pool; vkCmdExecuteCommands hands it to the executing primary, and it is
 *    submitted before that primary with the primary's own. The indices are read as they are when
 *    the executing primary starts: an index buffer the primary writes before it executes the
 *    secondary shows its old edges (logged once; DXVK uploads into its separate init command buffer).
 * The approach - turn the triangles' index stream into their edges and give the line rasterizer
 * that, never a geometry shader (Mali runs those very slowly) - is the one Mesa's panvk and
 * taowen's libhybris compatibility layer (arlinux branch, VK_LAYER_HYBRIS_compat polygon_*.c,
 * Apache-2.0) use; this implementation is the bridge's own.
 *
 * Differences from a GPU's own polygon mode (logged once where they can happen): back-face culling
 * does not apply to the lines and points (every triangle's edges are drawn: cull mode is a
 * property of triangles, which the variant no longer has), nor does depth bias (Vulkan applies it
 * to polygons only). Dynamic topology and restart (vkd3d-proton sets them at record time) are
 * taken from the command buffer at each draw; the variant gets a line- or point-list topology set
 * around its draw. Drawn solid, as before (logged once each): indirect draws, pipelines with
 * dynamic polygon mode, geometry or tessellation stages, pipeline libraries.
 *
 * VKBRIDGE_POLYGON_EMU=0 turns it off: the feature is still reported and such pipelines draw solid.
 *
 * Cost: none until the first non-solid pipeline is made - the wrappers below pass straight through
 * on one flag. (The command-state tracker beneath records every command buffer's pool and level
 * from the start, so command buffers allocated before that pipeline get their index pass too.) After it, a lookup per pipeline bind and draw. A non-solid pipeline compiles twice;
 * its draws bind two pipelines and an index buffer more, and indexed ones add a dispatch of
 * ceil(triangles / 64) workgroups, 24 bytes of scratch per triangle and one full barrier per
 * command buffer that has any. Every index buffer is made with STORAGE_BUFFER usage too (while the
 * emulation is on), for the shader to read it.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "polygon_spv.h"

#include <stdlib.h>
#include <string.h>

enum { TOPO_LIST = 0, TOPO_STRIP = 1, TOPO_FAN = 2 };

struct poly_info {
    int want;                 /* make the variant */
    VkPolygonMode mode;       /* LINE or POINT */
    uint32_t topo;            /* TOPO_* (static topology) */
    VkBool32 restart;         /* primitiveRestartEnable (static) */
    int dyn_topo, dyn_restart; /* set at record time instead (vkCmdSetPrimitiveTopology / RestartEnable) */
};

typedef struct poly_rec {
    VkPipeline p, variant;
    VkPolygonMode mode;
    uint32_t topo;
    VkBool32 restart;
    int dyn_topo, dyn_restart;
    struct poly_rec *next;
} poly_rec;

typedef struct pattern {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t tris;
} pattern;

typedef struct retired {
    VkBuffer buf;
    VkDeviceMemory mem;
    struct retired *next;
} retired;

typedef struct poly_dev {
    vkb_emu_device *e;
    pthread_mutex_t lock;
    int live;                 /* a variant exists: the wrappers look */
    poly_rec *b[256];
    VkPhysicalDeviceLimits limits;
    VkPhysicalDeviceMemoryProperties mp;
    /* the index shader (made at the first indexed draw; -1: could not be) */
    int comp_state;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline comp;
    pattern pat[3];           /* edge lists of non-indexed draws, per topology */
    retired *old;             /* outgrown ones (command buffers may still use them) */
    /* what was installed beneath */
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCmdBindPipeline bind;
    PFN_vkCmdBindIndexBuffer bind_ib;
    PFN_vkCmdBindIndexBuffer2 bind_ib2;
    PFN_vkCmdDraw draw;
    PFN_vkCmdDrawIndexed draw_idx;
    PFN_vkCmdDrawIndirect draw_ind;
    PFN_vkCmdDrawIndexedIndirect draw_idx_ind;
    PFN_vkCmdDrawIndirectCount draw_ind_cnt;
    PFN_vkCmdDrawIndexedIndirectCount draw_idx_ind_cnt;
    PFN_vkCmdCopyBuffer copy;
    PFN_vkCmdCopyBuffer2 copy2;
    PFN_vkCmdUpdateBuffer update;
    PFN_vkCmdFillBuffer fill;
    PFN_vkCmdSetPrimitiveTopology set_topo;
    PFN_vkCmdSetPrimitiveRestartEnable set_restart;
    PFN_vkCmdSetPolygonModeEXT set_polygon;
    PFN_vkEndCommandBuffer end;
    PFN_vkCmdExecuteCommands exec;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
    /* totals, logged at vkDestroyDevice */
    uint64_t n_variants, n_lines, n_lines_idx, n_points, n_solid;
} poly_dev;

#define MAX_WRITTEN 32

typedef struct chunk {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size, used;
} chunk;

typedef struct poly_cb {
    int complete;             /* seen since its begin: the index buffer binding is known */
    /* bound graphics pipeline, when it is a non-solid one */
    VkPipeline app, variant;
    VkPolygonMode mode;
    uint32_t topo;
    VkBool32 restart;
    int dyn_topo, dyn_restart;
    /* the app's dynamic topology and restart (when set since the begin) */
    int topo_known, restart_known;
    VkPrimitiveTopology app_topo;
    VkBool32 app_restart;
    /* the app's index buffer */
    int ib_known, ib_two;
    VkBuffer ib;
    VkDeviceSize ib_off, ib_size;
    VkIndexType ib_type;
    /* buffers written by transfer commands so far (an index buffer among them: drawn solid) */
    VkBuffer written[MAX_WRITTEN];
    uint32_t nwritten;
    int written_over;
    /* the command buffer submitted before this one, with the index shader's dispatches */
    VkCommandBuffer pre;
    VkCommandPool pre_pool;
    int pre_open, secondary;
    chunk *chunks;
    uint32_t nchunks;
    /* index buffers the dispatches read (a secondary's: checked against its primary's writes) */
    VkBuffer read[MAX_WRITTEN];
    uint32_t nread;
    int read_over;
    /* a primary: the index command buffers of the secondaries it executes (kept across begins) */
    VkCommandBuffer *xpre;
    uint32_t nxpre, capxpre;
} poly_cb;

static poly_dev *pdev(void)
{
    vkb_emu_device *e = vkb_emu_cur();
    return e ? (poly_dev *)e->polygon : NULL;
}

static int live(poly_dev *s) { return s && __atomic_load_n(&s->live, __ATOMIC_ACQUIRE); }

static unsigned ph(VkPipeline p)
{
    uint64_t k = (uint64_t)p;
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    return (unsigned)(k >> 56);
}

static const char *mode_name(VkPolygonMode m) { return m == VK_POLYGON_MODE_POINT ? "POINT" : "LINE"; }

/* ------------------------------------------------------------------ pipelines */

size_t vkb_emu_polygon_info_size(void) { return sizeof(poly_info); }

static int has_dynamic(const VkGraphicsPipelineCreateInfo *ci, VkDynamicState d)
{
    if (!ci->pDynamicState) return 0;
    for (uint32_t i = 0; i < ci->pDynamicState->dynamicStateCount; i++)
        if (ci->pDynamicState->pDynamicStates[i] == d) return 1;
    return 0;
}

static void solid(VkPolygonMode m, const char *why)
{
    /* One line per reason. */
    static const char *told[24];
    static int n;
    int have = __atomic_load_n(&n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < have && i < 24; i++)
        if (told[i] == why) return;
    int k = __atomic_fetch_add(&n, 1, __ATOMIC_ACQ_REL);
    if (k < 24) told[k] = why;
    VKB_WARN("emulation: polygon mode %s drawn solid (%s; no fillModeNonSolid; logged once)", mode_name(m), why);
}

void vkb_emu_polygon_pipeline(vkb_emu_device *e, const VkGraphicsPipelineCreateInfo *ci, poly_info *out)
{
    memset(out, 0, sizeof(*out));
    if (!(e->flags & VKB_EMU_FILL_MODE) || !e->polygon) return;
    const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
    if (!rs || rs->rasterizerDiscardEnable) return;
    VkPolygonMode m = rs->polygonMode;
    if (has_dynamic(ci, VK_DYNAMIC_STATE_POLYGON_MODE_EXT)) return; /* p_CmdSetPolygonModeEXT */
    if (m != VK_POLYGON_MODE_LINE && m != VK_POLYGON_MODE_POINT) return;
    if (ci->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) {
        solid(m, "a pipeline library");
        return;
    }
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) {
            solid(m, "a pipeline linked from libraries");
            return;
        }
    for (uint32_t i = 0; i < ci->stageCount; i++)
        if (ci->pStages[i].stage & (VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_MESH_BIT_EXT)) {
            solid(m, "a geometry, tessellation or mesh stage");
            return;
        }
    const VkPipelineInputAssemblyStateCreateInfo *ia = ci->pInputAssemblyState;
    if (!ia) return;
    switch (ia->topology) {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: out->topo = TOPO_LIST; break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: out->topo = TOPO_STRIP; break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: out->topo = TOPO_FAN; break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: solid(m, "triangles with adjacency"); return;
    default: return; /* lines and points: polygon mode does not apply */
    }
    out->want = 1;
    out->mode = m;
    out->restart = ia->primitiveRestartEnable;
    /* vkd3d-proton sets both at record time (VK_EXT_extended_dynamic_state): the draw decides. */
    out->dyn_topo = has_dynamic(ci, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
    out->dyn_restart = has_dynamic(ci, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE);
    if (rs->cullMode != VK_CULL_MODE_NONE)
        VKB_ONCE(VKB_LOG_INFO, "emulation: polygon mode %s with back-face culling: the emulation draws every triangle's edges "
                               "(logged once)", mode_name(m));
    if (rs->depthBiasEnable)
        VKB_ONCE(VKB_LOG_INFO, "emulation: polygon mode %s with depth bias: not applied to the emulation's lines (logged once)",
                 mode_name(m));
}

void vkb_emu_polygon_created(vkb_emu_device *e, VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *ci,
                             VkPipeline p, poly_info *info)
{
    poly_dev *s = e->polygon;
    if (!info->want || !p || !s) return;
    VkGraphicsPipelineCreateInfo v = *ci;
    VkPipelineInputAssemblyStateCreateInfo ia = *ci->pInputAssemblyState;
    ia.topology = info->mode == VK_POLYGON_MODE_POINT ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    ia.primitiveRestartEnable = VK_FALSE;
    v.pInputAssemblyState = &ia;
    /* compat has made it solid; the variant is complete on its own, whatever the batch held. */
    v.flags &= ~(VkPipelineCreateFlags)(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
                                        VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
    v.basePipelineHandle = VK_NULL_HANDLE;
    v.basePipelineIndex = -1;
    VkPipeline variant = VK_NULL_HANDLE;
    VkResult r = vkb_emu_real()->vkCreateGraphicsPipelines(device, cache, 1, &v, NULL, &variant);
    if (r != VK_SUCCESS || !variant) {
        VKB_WARN("emulation: polygon mode %s: the %s variant of pipeline %p failed (%d); drawn solid", mode_name(info->mode),
                 info->mode == VK_POLYGON_MODE_POINT ? "point-list" : "line-list", (void *)p, r);
        return;
    }
    poly_rec *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        vkb_emu_real()->vkDestroyPipeline(device, variant, NULL);
        return;
    }
    rec->p = p;
    rec->variant = variant;
    rec->mode = info->mode;
    rec->topo = info->topo;
    rec->restart = info->restart;
    rec->dyn_topo = info->dyn_topo;
    rec->dyn_restart = info->dyn_restart;
    pthread_mutex_lock(&s->lock);
    rec->next = s->b[ph(p)];
    s->b[ph(p)] = rec;
    uint64_t n = ++s->n_variants;
    pthread_mutex_unlock(&s->lock);
    __atomic_store_n(&s->live, 1, __ATOMIC_RELEASE);
    if (n <= 4)
        VKB_INFO("emulation: polygon mode %s: pipeline %p draws through a %s variant (topology %u, restart %u)", mode_name(info->mode),
                 (void *)p, info->mode == VK_POLYGON_MODE_POINT ? "point-list" : "line-list", info->topo, info->restart);
}

static int find_rec(poly_dev *s, VkPipeline p, poly_rec *out)
{
    pthread_mutex_lock(&s->lock);
    poly_rec *r = s->b[ph(p)];
    while (r && r->p != p) r = r->next;
    if (r) *out = *r;
    pthread_mutex_unlock(&s->lock);
    return r != NULL;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyPipeline(VkDevice device, VkPipeline p, const VkAllocationCallbacks *pAllocator)
{
    poly_dev *s = pdev();
    if (p && live(s)) {
        pthread_mutex_lock(&s->lock);
        poly_rec **pp = &s->b[ph(p)];
        while (*pp && (*pp)->p != p) pp = &(*pp)->next;
        poly_rec *r = *pp;
        if (r) *pp = r->next;
        pthread_mutex_unlock(&s->lock);
        if (r) vkb_emu_real()->vkDestroyPipeline(device, r->variant, NULL);
        free(r);
    }
    s->destroy_pipeline(device, p, pAllocator);
}

void vkb_emu_polygon_buffer_usage(vkb_emu_device *e, VkBufferCreateInfo *ci)
{
    /* The index shader reads index buffers as storage buffers. */
    if (!(e->flags & VKB_EMU_FILL_MODE) || !e->polygon) return;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO) {
            VkBufferUsageFlags2CreateInfo *u = (VkBufferUsageFlags2CreateInfo *)b;
            if (u->usage & VK_BUFFER_USAGE_2_INDEX_BUFFER_BIT) u->usage |= VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT;
            return;
        }
    if (ci->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ci->usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
}

/* ------------------------------------------------------------------ resources */

static uint32_t mem_type(poly_dev *s, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < s->mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = s->mp.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT)))
            return i;
    }
    return UINT32_MAX;
}

static int make_buffer(poly_dev *s, VkDeviceSize size, VkBufferUsageFlags usage, int host, VkBuffer *buf, VkDeviceMemory *mem)
{
    const vkb_dispatch *r = &s->e->dev->real;
    VkDevice d = s->e->dev->device;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    *buf = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    if (r->vkCreateBuffer(d, &bci, NULL, buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    r->vkGetBufferMemoryRequirements(d, *buf, &req);
    uint32_t type = host ? mem_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                         : mem_type(s, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX && !host) type = mem_type(s, req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == UINT32_MAX || r->vkAllocateMemory(d, &ai, NULL, mem) != VK_SUCCESS || r->vkBindBufferMemory(d, *buf, *mem, 0) != VK_SUCCESS) {
        r->vkDestroyBuffer(d, *buf, NULL);
        if (*mem) r->vkFreeMemory(d, *mem, NULL);
        *buf = VK_NULL_HANDLE;
        *mem = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

static uint32_t tris_of(uint32_t topo, uint32_t count)
{
    if (topo == TOPO_LIST) return count / 3;
    return count >= 3 ? count - 2 : 0;
}

/* The edge list of a non-indexed draw of `tris` triangles; VK_NULL_HANDLE if it cannot be had. */
static VkBuffer pattern_for(poly_dev *s, uint32_t topo, uint32_t tris)
{
    pthread_mutex_lock(&s->lock);
    pattern *p = &s->pat[topo];
    if (p->tris < tris) {
        uint64_t want = (uint64_t)p->tris * 2;
        if (want < tris) want = tris;
        if (want < 1024) want = 1024;
        VkDeviceSize bytes = (VkDeviceSize)want * 6 * 4;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint32_t *map = NULL;
        if (want > UINT32_MAX / 6 || !make_buffer(s, bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 1, &buf, &mem) ||
            s->e->dev->real.vkMapMemory(s->e->dev->device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map) != VK_SUCCESS) {
            if (map == NULL && want <= UINT32_MAX / 6 && buf) {
                s->e->dev->real.vkDestroyBuffer(s->e->dev->device, buf, NULL);
                s->e->dev->real.vkFreeMemory(s->e->dev->device, mem, NULL);
            }
            pthread_mutex_unlock(&s->lock);
            return VK_NULL_HANDLE;
        }
        for (uint32_t t = 0; t < want; t++) {
            uint32_t a, b, c, *o = map + (size_t)t * 6;
            if (topo == TOPO_LIST) a = 3 * t, b = 3 * t + 1, c = 3 * t + 2;
            else if (topo == TOPO_STRIP) a = t, b = t + 1, c = t + 2;
            else a = 0, b = t + 1, c = t + 2;
            o[0] = a, o[1] = b, o[2] = b, o[3] = c, o[4] = c, o[5] = a;
        }
        s->e->dev->real.vkUnmapMemory(s->e->dev->device, mem);
        if (p->buf) {
            retired *old = calloc(1, sizeof(*old));
            if (old) {
                old->buf = p->buf;
                old->mem = p->mem;
                old->next = s->old;
                s->old = old;
            }
        }
        p->buf = buf;
        p->mem = mem;
        p->tris = (uint32_t)want;
    }
    VkBuffer b = p->buf;
    pthread_mutex_unlock(&s->lock);
    return b;
}

static int compute_ready(poly_dev *s)
{
    pthread_mutex_lock(&s->lock);
    if (!s->comp_state) {
        const vkb_dispatch *r = &s->e->dev->real;
        VkDevice d = s->e->dev->device;
        VkDescriptorSetLayoutBinding bind[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, bind};
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 6 * 4};
        VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_polygon_lines), spv_polygon_lines};
        VkShaderModule mod = VK_NULL_HANDLE;
        VkResult res = r->vkCreateDescriptorSetLayout(d, &dci, NULL, &s->dsl);
        if (res == VK_SUCCESS) {
            VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &s->dsl, 1, &pcr};
            res = r->vkCreatePipelineLayout(d, &plci, NULL, &s->pl);
        }
        if (res == VK_SUCCESS) res = r->vkCreateShaderModule(d, &smci, NULL, &mod);
        if (res == VK_SUCCESS) {
            VkComputePipelineCreateInfo cci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod,
                                                "main", NULL},
                                               s->pl, VK_NULL_HANDLE, -1};
            res = r->vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cci, NULL, &s->comp);
        }
        if (mod) r->vkDestroyShaderModule(d, mod, NULL);
        s->comp_state = res == VK_SUCCESS ? 1 : -1;
        if (res != VK_SUCCESS) VKB_WARN("emulation: polygon mode LINE: the index shader failed (%d); indexed draws drawn solid", res);
    }
    int ok = s->comp_state > 0;
    pthread_mutex_unlock(&s->lock);
    return ok;
}

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

/* `size` bytes of the command buffer's scratch (index output), aligned for a storage buffer. */
static int scratch(poly_dev *s, poly_cb *c, VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off)
{
    VkDeviceSize al = s->limits.minStorageBufferOffsetAlignment > 4 ? s->limits.minStorageBufferOffsetAlignment : 4;
    for (uint32_t i = 0; i < c->nchunks; i++) {
        chunk *k = &c->chunks[i];
        VkDeviceSize o = align_up(k->used, al);
        if (o + size <= k->size) {
            k->used = o + size;
            *buf = k->buf;
            *off = o;
            return 1;
        }
    }
    chunk *nc = realloc(c->chunks, (c->nchunks + 1) * sizeof(*nc));
    if (!nc) return 0;
    c->chunks = nc;
    chunk *k = &c->chunks[c->nchunks];
    memset(k, 0, sizeof(*k));
    k->size = align_up(size > (1u << 20) ? size : (1u << 20), 1u << 16);
    if (!make_buffer(s, k->size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 0, &k->buf, &k->mem)) return 0;
    c->nchunks++;
    k->used = size;
    *buf = k->buf;
    *off = 0;
    return 1;
}

/* ------------------------------------------------------------------ command buffers */

void vkb_emu_polygon_cb_reset(vkb_emu_device *e, VkCommandBuffer cb, VkCommandPool pool, void **poly)
{
    poly_dev *s = e->polygon;
    poly_cb *c = *poly;
    if (!c && live(s)) c = *poly = calloc(1, sizeof(poly_cb));
    if (!c) return;
    (void)cb;
    if (c->pre) e->dev->real.vkFreeCommandBuffers(e->dev->device, c->pre_pool ? c->pre_pool : pool, 1, &c->pre);
    chunk *chunks = c->chunks;
    uint32_t n = c->nchunks, capx = c->capxpre;
    VkCommandBuffer *xpre = c->xpre;
    memset(c, 0, sizeof(*c));
    c->chunks = chunks;
    c->nchunks = n;
    c->xpre = xpre;
    c->capxpre = capx;
    for (uint32_t i = 0; i < n; i++) chunks[i].used = 0;
    c->complete = 1;
}

void vkb_emu_polygon_cb_free(vkb_emu_device *e, void *poly)
{
    poly_cb *c = poly;
    if (!c) return;
    for (uint32_t i = 0; i < c->nchunks; i++) {
        e->dev->real.vkDestroyBuffer(e->dev->device, c->chunks[i].buf, NULL);
        e->dev->real.vkFreeMemory(e->dev->device, c->chunks[i].mem, NULL);
    }
    free(c->chunks);
    free(c->xpre);
    free(c);
}

static poly_cb *cbs(vkb_emu_device *e, VkCommandBuffer cb) { return vkb_emu_cb_poly(e, cb, sizeof(poly_cb)); }

static VKAPI_ATTR void VKAPI_CALL p_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    poly_dev *s = pdev();
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS && live(s)) {
        poly_cb *c = cbs(s->e, cb);
        poly_rec r;
        if (c && find_rec(s, p, &r)) {
            c->app = p;
            c->variant = r.variant;
            c->mode = r.mode;
            c->topo = r.topo;
            c->restart = r.restart;
            c->dyn_topo = r.dyn_topo;
            c->dyn_restart = r.dyn_restart;
        } else if (c) {
            c->app = c->variant = VK_NULL_HANDLE;
        }
    }
    s->bind(cb, bp, p);
}

static void record_ib(poly_dev *s, VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, VkIndexType t, int two)
{
    poly_cb *c = cbs(s->e, cb);
    if (!c) return;
    c->ib_known = 1;
    c->ib_two = two;
    c->ib = b;
    c->ib_off = off;
    c->ib_size = size;
    c->ib_type = t;
}

static VKAPI_ATTR void VKAPI_CALL p_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t)
{
    poly_dev *s = pdev();
    if (live(s)) record_ib(s, cb, b, off, VK_WHOLE_SIZE, t, 0);
    s->bind_ib(cb, b, off, t);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdBindIndexBuffer2(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, VkIndexType t)
{
    poly_dev *s = pdev();
    if (live(s)) record_ib(s, cb, b, off, size, t, 1);
    s->bind_ib2(cb, b, off, size, t);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology t)
{
    poly_dev *s = pdev();
    poly_cb *c = live(s) ? cbs(s->e, cb) : NULL;
    if (c) {
        c->topo_known = 1;
        c->app_topo = t;
    }
    s->set_topo(cb, t);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 on)
{
    poly_dev *s = pdev();
    poly_cb *c = live(s) ? cbs(s->e, cb) : NULL;
    if (c) {
        c->restart_known = 1;
        c->app_restart = on;
    }
    s->set_restart(cb, on);
}

/* VK_EXT_extended_dynamic_state3 on a driver without fillModeNonSolid: the driver must not see a
 * non-solid mode; drawn solid (not emulated: the mode can change between any two draws). */
static VKAPI_ATTR void VKAPI_CALL p_CmdSetPolygonModeEXT(VkCommandBuffer cb, VkPolygonMode m)
{
    poly_dev *s = pdev();
    if (m == VK_POLYGON_MODE_LINE || m == VK_POLYGON_MODE_POINT) {
        solid(m, "dynamic polygon mode");
        m = VK_POLYGON_MODE_FILL;
    }
    s->set_polygon(cb, m);
}

static int topo_of(VkPrimitiveTopology t, uint32_t *out)
{
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: *out = TOPO_LIST; return 1;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: *out = TOPO_STRIP; return 1;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: *out = TOPO_FAN; return 1;
    default: return 0;
    }
}

/* The draw's topology and restart, from the pipeline or the dynamic state. 0: the emulation does not
 * apply (not triangles: polygon mode means nothing), -1: not known (drawn solid). */
static int draw_shape(poly_cb *c, uint32_t *topo, VkBool32 *restart)
{
    *topo = c->topo;
    *restart = c->restart;
    if (c->dyn_restart) *restart = c->restart_known ? c->app_restart : VK_TRUE;
    if (!c->dyn_topo) return 1;
    if (!c->topo_known) return -1;
    return topo_of(c->app_topo, topo);
}

/* The variant bound for the draw, with the dynamic state it needs. */
static void use_variant(poly_dev *s, VkCommandBuffer cb, poly_cb *c)
{
    s->bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->variant);
    if (c->dyn_topo && s->set_topo)
        s->set_topo(cb, c->mode == VK_POLYGON_MODE_POINT ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    /* A list with restart on needs primitiveTopologyListRestart; the edges never need it. */
    if (c->dyn_restart && c->restart_known && c->app_restart && s->set_restart) s->set_restart(cb, VK_FALSE);
}

static void restore(poly_dev *s, VkCommandBuffer cb, poly_cb *c, int ib)
{
    s->bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->app);
    if (c->dyn_topo && s->set_topo) s->set_topo(cb, c->app_topo);
    if (c->dyn_restart && c->restart_known && c->app_restart && s->set_restart) s->set_restart(cb, VK_TRUE);
    if (!ib || !c->ib_known) return; /* none bound since the begin: ours may stay */
    if (c->ib_two && s->bind_ib2) s->bind_ib2(cb, c->ib, c->ib_off, c->ib_size, c->ib_type);
    else s->bind_ib(cb, c->ib, c->ib_off, c->ib_type);
}

static void tell_first(poly_dev *s, VkPolygonMode m, int indexed)
{
    (void)s;
    if (m == VK_POLYGON_MODE_POINT) VKB_ONCE(VKB_LOG_INFO, "emulation: polygon mode POINT drawn as points (first draw; logged once)");
    else if (indexed) VKB_ONCE(VKB_LOG_INFO, "emulation: polygon mode LINE drawn as lines, indexed (first draw; logged once)");
    else VKB_ONCE(VKB_LOG_INFO, "emulation: polygon mode LINE drawn as lines (first draw; logged once)");
}

static void count(poly_dev *s, uint64_t *n)
{
    __atomic_add_fetch(n, 1, __ATOMIC_RELAXED);
    (void)s;
}

static VKAPI_ATTR void VKAPI_CALL p_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    poly_dev *s = pdev();
    poly_cb *c = live(s) ? cbs(s->e, cb) : NULL;
    uint32_t topo = 0;
    VkBool32 restart = 0;
    int shape = c && c->variant ? draw_shape(c, &topo, &restart) : 0;
    if (shape < 0) {
        solid(c->mode, "dynamic topology set before the first non-solid pipeline");
        count(s, &s->n_solid);
    }
    if (shape <= 0) {
        s->draw(cb, vc, ic, fv, fi);
        return;
    }
    (void)restart;
    if (c->mode == VK_POLYGON_MODE_POINT) {
        tell_first(s, c->mode, 0);
        count(s, &s->n_points);
        use_variant(s, cb, c);
        s->draw(cb, vc, ic, fv, fi);
        restore(s, cb, c, 0);
        return;
    }
    uint32_t tris = tris_of(topo, vc);
    VkBuffer pat = VK_NULL_HANDLE;
    if (!tris || !ic) return; /* nothing to draw, as with the GPU's own polygon mode */
    if (!c->complete && !c->ib_known) {
        solid(c->mode, "a command buffer begun before the first non-solid pipeline");
    } else if (fv > (uint32_t)INT32_MAX) {
        solid(c->mode, "firstVertex past INT32_MAX");
    } else if (!(pat = pattern_for(s, topo, tris))) {
        solid(c->mode, "no memory for the edge list");
    }
    if (!pat) {
        count(s, &s->n_solid);
        s->draw(cb, vc, ic, fv, fi);
        return;
    }
    tell_first(s, c->mode, 0);
    count(s, &s->n_lines);
    use_variant(s, cb, c);
    s->bind_ib(cb, pat, 0, VK_INDEX_TYPE_UINT32);
    s->draw_idx(cb, tris * 6, ic, 0, (int32_t)fv, fi);
    restore(s, cb, c, 1);
}

static int written(const poly_cb *c, VkBuffer b)
{
    if (c->written_over) return 1;
    for (uint32_t i = 0; i < c->nwritten; i++)
        if (c->written[i] == b) return 1;
    return 0;
}

/* The command buffer submitted before the app's (a secondary's: before the primary that executes
 * it), recording; VK_NULL_HANDLE and *why if there can be none. */
static VkCommandBuffer pre_cb(poly_dev *s, poly_cb *c, VkCommandBuffer cb, const char **why)
{
    if (c->pre_open) return c->pre;
    if (c->pre) {
        *why = "an indexed draw after its command buffer's index pass was closed (vkEndCommandBuffer not followed by a begin)";
        return VK_NULL_HANDLE;
    }
    int secondary = 0;
    uint32_t flags = 0;
    VkCommandPool pool = vkb_emu_cb_origin(s->e, cb, &secondary, &flags);
    if (!pool) {
        *why = "a command buffer whose allocation the bridge did not see (no pool for the index pass)";
        return VK_NULL_HANDLE;
    }
    const vkb_dispatch *r = &s->e->dev->real;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer pre = VK_NULL_HANDLE;
    if (r->vkAllocateCommandBuffers(s->e->dev->device, &ai, &pre) != VK_SUCCESS) {
        *why = "the index pass's command buffer could not be allocated from the app's pool";
        return VK_NULL_HANDLE;
    }
    /* A secondary can be executed by several primaries of one submission or of pending ones. */
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   secondary ? VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT : flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
                                   NULL};
    if (r->vkBeginCommandBuffer(pre, &bi) != VK_SUCCESS) {
        r->vkFreeCommandBuffers(s->e->dev->device, pool, 1, &pre);
        *why = "the index pass's command buffer could not be begun";
        return VK_NULL_HANDLE;
    }
    /* Whatever wrote the indices before (earlier submissions, the app's own barriers name the
     * index input, not a compute read), and whatever still reads this scratch. */
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    r->vkCmdPipelineBarrier(pre, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    r->vkCmdBindPipeline(pre, VK_PIPELINE_BIND_POINT_COMPUTE, s->comp);
    c->pre = pre;
    c->pre_pool = pool;
    c->pre_open = 1;
    c->secondary = secondary;
    return pre;
}

static uint32_t isize_of(VkIndexType t)
{
    switch (t) {
    case VK_INDEX_TYPE_UINT16: return 2;
    case VK_INDEX_TYPE_UINT32: return 4;
    case VK_INDEX_TYPE_UINT8_KHR: return 1;
    default: return 0;
    }
}

static VKAPI_ATTR void VKAPI_CALL p_CmdDrawIndexed(VkCommandBuffer cb, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi)
{
    poly_dev *s = pdev();
    poly_cb *c = live(s) ? cbs(s->e, cb) : NULL;
    uint32_t topo = 0;
    VkBool32 restart = 0;
    int shape = c && c->variant ? draw_shape(c, &topo, &restart) : 0;
    if (shape < 0) {
        solid(c->mode, "dynamic topology set before the first non-solid pipeline");
        count(s, &s->n_solid);
    }
    if (shape <= 0) {
        s->draw_idx(cb, xc, ic, fx, vo, fi);
        return;
    }
    if (c->mode == VK_POLYGON_MODE_POINT) {
        tell_first(s, c->mode, 1);
        count(s, &s->n_points);
        use_variant(s, cb, c);
        s->draw_idx(cb, xc, ic, fx, vo, fi);
        restore(s, cb, c, 0);
        return;
    }
    uint32_t tris = tris_of(topo, xc), isz = c->ib_known ? isize_of(c->ib_type) : 0;
    if (!tris || !ic) return;
    const char *why = NULL;
    VkDeviceSize first = 0, bind_off = 0, range = 0, out_size = (VkDeviceSize)tris * 24, out_off = 0;
    VkBuffer out = VK_NULL_HANDLE;
    VkCommandBuffer pre = VK_NULL_HANDLE;
    uint64_t groups = ((uint64_t)tris + 63) / 64, gx = groups, gy = 1;
    if (gx > s->limits.maxComputeWorkGroupCount[0]) {
        gx = s->limits.maxComputeWorkGroupCount[0];
        gy = (groups + gx - 1) / gx;
    }
    if (!c->ib_known || !c->ib || !isz) why = "an index buffer bound before the first non-solid pipeline";
    else if (written(c, c->ib)) why = "an index buffer written earlier in the same command buffer";
    else if (topo == TOPO_FAN && restart) why = "an indexed fan with primitive restart";
    else if (out_size > s->limits.maxStorageBufferRange || gy > s->limits.maxComputeWorkGroupCount[1]) why = "a draw too large";
    if (!why) {
        VkDeviceSize al = s->limits.minStorageBufferOffsetAlignment ? s->limits.minStorageBufferOffsetAlignment : 1;
        first = c->ib_off + (VkDeviceSize)fx * isz;
        bind_off = first / al * al;
        range = align_up(first - bind_off + (VkDeviceSize)xc * isz, 4);
        VkDeviceSize size = vkb_emu_buffer_size(s->e, c->ib);
        if (size && bind_off < size && range > size - bind_off) range = size - bind_off;
        if (range > s->limits.maxStorageBufferRange) why = "an index range too large";
    }
    if (!why && !compute_ready(s)) why = "no index shader";
    if (!why) pre = pre_cb(s, c, cb, &why);
    if (!why && !scratch(s, c, out_size, &out, &out_off)) why = "no memory for the edge list";
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!why && !(set = vkb_emu_cb_scratch_set(s->e, cb, s->dsl))) why = "no descriptor set";
    if (why) {
        solid(c->mode, why);
        count(s, &s->n_solid);
        s->draw_idx(cb, xc, ic, fx, vo, fi);
        return;
    }
    const vkb_dispatch *r = &s->e->dev->real;
    VkDescriptorBufferInfo bi[2] = {{c->ib, bind_off, range}, {out, out_off, out_size}};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[0], NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[1], NULL}};
    r->vkUpdateDescriptorSets(s->e->dev->device, 2, w, 0, NULL);
    uint32_t pc[6] = {(uint32_t)(first - bind_off), isz, tris, topo, restart && topo == TOPO_STRIP, (uint32_t)(gx * 64)};
    r->vkCmdBindDescriptorSets(pre, VK_PIPELINE_BIND_POINT_COMPUTE, s->pl, 0, 1, &set, 0, NULL);
    r->vkCmdPushConstants(pre, s->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    r->vkCmdDispatch(pre, (uint32_t)gx, (uint32_t)gy, 1);
    int known = c->read_over;
    for (uint32_t i = 0; i < c->nread && !known; i++) known = c->read[i] == c->ib;
    if (!known) {
        if (c->nread < MAX_WRITTEN) c->read[c->nread++] = c->ib;
        else c->read_over = 1;
    }
    tell_first(s, c->mode, 1);
    if (c->secondary) VKB_ONCE(VKB_LOG_INFO, "emulation: polygon mode LINE drawn as lines, indexed, in a secondary command buffer (first draw; logged once)");
    count(s, &s->n_lines_idx);
    use_variant(s, cb, c);
    s->bind_ib(cb, out, out_off, VK_INDEX_TYPE_UINT32);
    s->draw_idx(cb, tris * 6, ic, 0, vo, fi);
    restore(s, cb, c, 1);
}

static void indirect(poly_dev *s, VkCommandBuffer cb)
{
    poly_cb *c = live(s) ? cbs(s->e, cb) : NULL;
    if (c && c->variant) {
        solid(c->mode, "an indirect draw");
        count(s, &s->n_solid);
    }
}

static VKAPI_ATTR void VKAPI_CALL p_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{
    poly_dev *s = pdev();
    indirect(s, cb);
    s->draw_ind(cb, b, o, n, st);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{
    poly_dev *s = pdev();
    indirect(s, cb);
    s->draw_idx_ind(cb, b, o, n, st);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co,
                                                        uint32_t max, uint32_t st)
{
    poly_dev *s = pdev();
    indirect(s, cb);
    s->draw_ind_cnt(cb, b, o, cbuf, co, max, st);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf,
                                                               VkDeviceSize co, uint32_t max, uint32_t st)
{
    poly_dev *s = pdev();
    indirect(s, cb);
    s->draw_idx_ind_cnt(cb, b, o, cbuf, co, max, st);
}

static void wrote(poly_dev *s, VkCommandBuffer cb, VkBuffer b)
{
    if (!live(s)) return;
    poly_cb *c = cbs(s->e, cb);
    if (!c || written(c, b)) return;
    if (c->nwritten < MAX_WRITTEN) c->written[c->nwritten++] = b;
    else c->written_over = 1;
}

static VKAPI_ATTR void VKAPI_CALL p_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *rg)
{
    poly_dev *s = pdev();
    wrote(s, cb, dst);
    s->copy(cb, src, dst, n, rg);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdCopyBuffer2(VkCommandBuffer cb, const VkCopyBufferInfo2 *info)
{
    poly_dev *s = pdev();
    wrote(s, cb, info->dstBuffer);
    s->copy2(cb, info);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void *data)
{
    poly_dev *s = pdev();
    wrote(s, cb, dst);
    s->update(cb, dst, off, size, data);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdFillBuffer(VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, uint32_t data)
{
    poly_dev *s = pdev();
    wrote(s, cb, dst);
    s->fill(cb, dst, off, size, data);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_EndCommandBuffer(VkCommandBuffer cb)
{
    poly_dev *s = pdev();
    poly_cb *c = live(s) ? vkb_emu_cb_poly(s->e, cb, 0) : NULL;
    if (c && c->pre_open) {
        const vkb_dispatch *r = &s->e->dev->real;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDEX_READ_BIT};
        r->vkCmdPipelineBarrier(c->pre, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        c->pre_open = 0;
        if (r->vkEndCommandBuffer(c->pre) != VK_SUCCESS) {
            /* Its draws then read scratch nobody wrote: zero-length lines at worst. */
            VKB_WARN("emulation: polygon mode LINE: ending the index command buffer failed");
            r->vkFreeCommandBuffers(s->e->dev->device, c->pre_pool, 1, &c->pre);
            c->pre = VK_NULL_HANDLE;
        }
    }
    return s->end(cb);
}

/* A primary executes secondaries: their index passes go before it in its submissions. */
static VKAPI_ATTR void VKAPI_CALL p_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *l)
{
    poly_dev *s = pdev();
    poly_cb *c = live(s) ? cbs(s->e, cb) : NULL;
    for (uint32_t i = 0; c && i < n; i++) {
        poly_cb *x = vkb_emu_cb_poly(s->e, l[i], 0);
        if (!x || !x->pre || x->pre_open) continue;
        int stale = 0;
        for (uint32_t k = 0; k < x->nread && !stale; k++) stale = written(c, x->read[k]);
        if (stale || (x->read_over && (c->nwritten || c->written_over)))
            VKB_ONCE(VKB_LOG_WARN, "emulation: polygon mode LINE: an index buffer written in a primary before it executes a "
                                   "secondary that draws from it: those lines follow the indices as they were before (logged once)");
        uint32_t k = 0;
        while (k < c->nxpre && c->xpre[k] != x->pre) k++;
        if (k < c->nxpre) continue;
        if (c->nxpre == c->capxpre) {
            uint32_t cap = c->capxpre ? c->capxpre * 2 : 4;
            VkCommandBuffer *nx = realloc(c->xpre, cap * sizeof(*nx));
            if (!nx) {
                VKB_ONCE(VKB_LOG_WARN, "emulation: polygon mode LINE: out of memory listing a secondary's index pass; its lines "
                                       "read unwritten scratch (logged once)");
                continue;
            }
            c->xpre = nx;
            c->capxpre = cap;
        }
        c->xpre[c->nxpre++] = x->pre;
    }
    s->exec(cb, n, l);
}

/* The emulation's command buffers that go before cb in a submission (the index passes of the
 * secondaries it executes, then its own); their number, written to out when it is not NULL. */
static uint32_t pres_of(poly_dev *s, VkCommandBuffer cb, VkCommandBuffer *out)
{
    poly_cb *c = vkb_emu_cb_poly(s->e, cb, 0);
    if (!c) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->nxpre; i++, n++)
        if (out) out[n] = c->xpre[i];
    if (c->pre && !c->pre_open) {
        if (out) out[n] = c->pre;
        n++;
    }
    return n;
}

static VKAPI_ATTR VkResult VKAPI_CALL p_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    poly_dev *s = pdev();
    if (!live(s)) return s->submit(q, n, si, f);
    uint32_t extra = 0, total = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < si[i].commandBufferCount; j++, total++) extra += pres_of(s, si[i].pCommandBuffers[j], NULL);
    if (!extra) return s->submit(q, n, si, f);
    VkSubmitInfo *ns = malloc(n * sizeof(*ns));
    VkCommandBuffer *all = malloc((total + extra) * sizeof(*all));
    if (!ns || !all) {
        free(ns);
        free(all);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    /* A device group's per-command-buffer masks follow the command buffers (the pre one gets its
     * app's). The chain is the server's decoded copy of the request: changed in place. */
    uint32_t *masks = malloc((total + extra) * sizeof(*masks));
    if (!masks) {
        free(ns);
        free(all);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)si[i].pNext; b; b = b->pNext) {
            VkDeviceGroupSubmitInfo *g = (VkDeviceGroupSubmitInfo *)b;
            if (b->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO || !g->pCommandBufferDeviceMasks ||
                g->commandBufferCount != si[i].commandBufferCount)
                continue;
            uint32_t m = k;
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++)
                for (uint32_t p = pres_of(s, si[i].pCommandBuffers[j], NULL) + 1; p; p--) masks[m++] = g->pCommandBufferDeviceMasks[j];
            g->pCommandBufferDeviceMasks = masks + k;
            g->commandBufferCount = m - k;
        }
        ns[i] = si[i];
        ns[i].pCommandBuffers = all + k;
        for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
            k += pres_of(s, si[i].pCommandBuffers[j], all + k);
            all[k++] = si[i].pCommandBuffers[j];
        }
        ns[i].commandBufferCount = (uint32_t)(all + k - ns[i].pCommandBuffers);
    }
    VkResult r = s->submit(q, n, ns, f);
    free(ns);
    free(all);
    free(masks);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL p_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f)
{
    poly_dev *s = pdev();
    if (!live(s)) return s->submit2(q, n, si, f);
    uint32_t extra = 0, total = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < si[i].commandBufferInfoCount; j++, total++)
            extra += pres_of(s, si[i].pCommandBufferInfos[j].commandBuffer, NULL);
    if (!extra) return s->submit2(q, n, si, f);
    VkSubmitInfo2 *ns = malloc(n * sizeof(*ns));
    VkCommandBufferSubmitInfo *all = malloc((total + extra) * sizeof(*all));
    if (!ns || !all) {
        free(ns);
        free(all);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        ns[i] = si[i];
        ns[i].pCommandBufferInfos = all + k;
        for (uint32_t j = 0; j < si[i].commandBufferInfoCount; j++) {
            const VkCommandBufferSubmitInfo *app = &si[i].pCommandBufferInfos[j];
            VkCommandBuffer pre[64];
            uint32_t np = pres_of(s, app->commandBuffer, NULL);
            VkCommandBuffer *pl = np <= 64 ? pre : malloc(np * sizeof(*pl));
            if (!pl) {
                free(ns);
                free(all);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            pres_of(s, app->commandBuffer, pl);
            for (uint32_t p = 0; p < np; p++) {
                all[k] = *app;
                all[k].pNext = NULL;
                all[k++].commandBuffer = pl[p];
            }
            if (pl != pre) free(pl);
            all[k++] = *app;
        }
        ns[i].commandBufferInfoCount = (uint32_t)(all + k - ns[i].pCommandBufferInfos);
    }
    VkResult r = s->submit2(q, n, ns, f);
    free(ns);
    free(all);
    return r;
}

/* ------------------------------------------------------------------ install */

void vkb_emu_polygon_install(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    if (!(e->flags & VKB_EMU_FILL_MODE)) return;
    /* VKBRIDGE_POLYGON_EMU=0: the feature still reported, non-solid pipelines drawn solid (as before). */
    const char *off = vkb_srv_env("VKBRIDGE_POLYGON_EMU");
    if (off && !strcmp(off, "0")) {
        VKB_INFO("emulation: polygon mode LINE/POINT drawn solid (VKBRIDGE_POLYGON_EMU=0)");
        return;
    }
    poly_dev *s = calloc(1, sizeof(*s));
    if (!s) return;
    s->e = e;
    pthread_mutex_init(&s->lock, NULL);
    VkPhysicalDeviceProperties p;
    dev->real.vkGetPhysicalDeviceProperties(dev->physical, &p);
    s->limits = p.limits;
    dev->real.vkGetPhysicalDeviceMemoryProperties(dev->physical, &s->mp);
    vkb_dispatch *dt = &dev->dt;
    /* Everything beneath (the command-state tracker, the divisor, compat's split indirect draws). */
#define TAKE(field, fn, slot)            \
    do {                                 \
        s->slot = dt->field;             \
        if (s->slot) dt->field = fn;     \
    } while (0)
    TAKE(vkDestroyPipeline, p_DestroyPipeline, destroy_pipeline);
    TAKE(vkCmdBindPipeline, p_CmdBindPipeline, bind);
    TAKE(vkCmdBindIndexBuffer, p_CmdBindIndexBuffer, bind_ib);
    TAKE(vkCmdBindIndexBuffer2, p_CmdBindIndexBuffer2, bind_ib2);
    TAKE(vkCmdDraw, p_CmdDraw, draw);
    TAKE(vkCmdDrawIndexed, p_CmdDrawIndexed, draw_idx);
    TAKE(vkCmdDrawIndirect, p_CmdDrawIndirect, draw_ind);
    TAKE(vkCmdDrawIndexedIndirect, p_CmdDrawIndexedIndirect, draw_idx_ind);
    TAKE(vkCmdDrawIndirectCount, p_CmdDrawIndirectCount, draw_ind_cnt);
    TAKE(vkCmdDrawIndexedIndirectCount, p_CmdDrawIndexedIndirectCount, draw_idx_ind_cnt);
    TAKE(vkCmdCopyBuffer, p_CmdCopyBuffer, copy);
    TAKE(vkCmdCopyBuffer2, p_CmdCopyBuffer2, copy2);
    TAKE(vkCmdUpdateBuffer, p_CmdUpdateBuffer, update);
    TAKE(vkCmdFillBuffer, p_CmdFillBuffer, fill);
    TAKE(vkCmdSetPrimitiveTopology, p_CmdSetPrimitiveTopology, set_topo);
    TAKE(vkCmdSetPrimitiveRestartEnable, p_CmdSetPrimitiveRestartEnable, set_restart);
    TAKE(vkCmdSetPolygonModeEXT, p_CmdSetPolygonModeEXT, set_polygon);
    TAKE(vkEndCommandBuffer, p_EndCommandBuffer, end);
    TAKE(vkCmdExecuteCommands, p_CmdExecuteCommands, exec);
    TAKE(vkQueueSubmit, p_QueueSubmit, submit);
    TAKE(vkQueueSubmit2, p_QueueSubmit2, submit2);
#undef TAKE
    e->polygon = s;
}

void vkb_emu_polygon_uninstall(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    poly_dev *s = e ? e->polygon : NULL;
    if (!s) return;
    vkb_dispatch *dt = &dev->dt;
#define GIVE(field, fn, slot) \
    do {                      \
        if (dt->field == fn) dt->field = s->slot; \
    } while (0)
    GIVE(vkDestroyPipeline, p_DestroyPipeline, destroy_pipeline);
    GIVE(vkCmdBindPipeline, p_CmdBindPipeline, bind);
    GIVE(vkCmdBindIndexBuffer, p_CmdBindIndexBuffer, bind_ib);
    GIVE(vkCmdBindIndexBuffer2, p_CmdBindIndexBuffer2, bind_ib2);
    GIVE(vkCmdDraw, p_CmdDraw, draw);
    GIVE(vkCmdDrawIndexed, p_CmdDrawIndexed, draw_idx);
    GIVE(vkCmdDrawIndirect, p_CmdDrawIndirect, draw_ind);
    GIVE(vkCmdDrawIndexedIndirect, p_CmdDrawIndexedIndirect, draw_idx_ind);
    GIVE(vkCmdDrawIndirectCount, p_CmdDrawIndirectCount, draw_ind_cnt);
    GIVE(vkCmdDrawIndexedIndirectCount, p_CmdDrawIndexedIndirectCount, draw_idx_ind_cnt);
    GIVE(vkCmdCopyBuffer, p_CmdCopyBuffer, copy);
    GIVE(vkCmdCopyBuffer2, p_CmdCopyBuffer2, copy2);
    GIVE(vkCmdUpdateBuffer, p_CmdUpdateBuffer, update);
    GIVE(vkCmdFillBuffer, p_CmdFillBuffer, fill);
    GIVE(vkCmdSetPrimitiveTopology, p_CmdSetPrimitiveTopology, set_topo);
    GIVE(vkCmdSetPrimitiveRestartEnable, p_CmdSetPrimitiveRestartEnable, set_restart);
    GIVE(vkCmdSetPolygonModeEXT, p_CmdSetPolygonModeEXT, set_polygon);
    GIVE(vkEndCommandBuffer, p_EndCommandBuffer, end);
    GIVE(vkCmdExecuteCommands, p_CmdExecuteCommands, exec);
    GIVE(vkQueueSubmit, p_QueueSubmit, submit);
    GIVE(vkQueueSubmit2, p_QueueSubmit2, submit2);
#undef GIVE
    const vkb_dispatch *r = &dev->real;
    VkDevice d = dev->device;
    if (s->n_variants)
        VKB_INFO("emulation: polygon mode: %llu pipeline variant(s); %llu line draws, %llu indexed; %llu point draws; %llu drawn solid",
                 (unsigned long long)s->n_variants, (unsigned long long)s->n_lines, (unsigned long long)s->n_lines_idx,
                 (unsigned long long)s->n_points, (unsigned long long)s->n_solid);
    for (int i = 0; i < 256; i++)
        while (s->b[i]) {
            poly_rec *rec = s->b[i];
            s->b[i] = rec->next;
            r->vkDestroyPipeline(d, rec->variant, NULL);
            free(rec);
        }
    for (int t = 0; t < 3; t++)
        if (s->pat[t].buf) {
            r->vkDestroyBuffer(d, s->pat[t].buf, NULL);
            r->vkFreeMemory(d, s->pat[t].mem, NULL);
        }
    while (s->old) {
        retired *o = s->old;
        s->old = o->next;
        r->vkDestroyBuffer(d, o->buf, NULL);
        r->vkFreeMemory(d, o->mem, NULL);
        free(o);
    }
    if (s->comp) r->vkDestroyPipeline(d, s->comp, NULL);
    if (s->pl) r->vkDestroyPipelineLayout(d, s->pl, NULL);
    if (s->dsl) r->vkDestroyDescriptorSetLayout(d, s->dsl, NULL);
    pthread_mutex_destroy(&s->lock);
    free(s);
    e->polygon = NULL;
}
