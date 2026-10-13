/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Draws the GPU cannot run, made harmless: a graphics pipeline with a listed shader is created
 * with rasterization discarded (its vertex stage still runs; nothing is drawn). Listed by content
 * hash of the driver-level SPIR-V (the hang detector's h-<hash> names), built in or added with
 * VKBRIDGE_DISCARD_SHADERS=h-<hash>,...
 *
 * Experiment: VKBRIDGE_STUB_SHADERS=h-<hash>,... makes a listed fragment shader's module from a
 * constant-colour shader instead (no texture reads, no buffer reads; writes 0.5 grey to location
 * 0). Separates "sampling what this draw samples" from "drawing into its render target".
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MODS 64

static const uint64_t builtin[] = {
    0, /* placeholder: filled in once confirmed */
};

static PFN_vkCreateShaderModule next_mod; /* the driver's (the same for every device) */
static PFN_vkCreateGraphicsPipelines next_gfx;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t listed[32];
static uint32_t nlisted;
static uint64_t stubbed[32];
static uint32_t nstubbed;

/* glslc -O: layout(location = 0) out vec4 o; void main() { o = vec4(0.5, 0.5, 0.5, 1.0); } */
static const uint32_t stub_fs[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47,
    0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x00030010, 0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0004002b,
    0x00000006, 0x0000000a, 0x3f000000, 0x0004002b, 0x00000006, 0x0000000b, 0x3f800000, 0x0007002c, 0x00000007, 0x0000000c,
    0x0000000a, 0x0000000a, 0x0000000a, 0x0000000b, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0003003e, 0x00000009, 0x0000000c, 0x000100fd, 0x00010038};
static VkShaderModule marked[MODS]; /* modules made from a listed shader (a ring) */
static uint32_t nmarked, next_mark;

static uint64_t spv_hash(const uint32_t *code, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)code;
    for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static VKAPI_ATTR VkResult VKAPI_CALL q_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                          VkShaderModule *out)
{
    uint64_t h = (nlisted || nstubbed) ? spv_hash(ci->pCode, ci->codeSize) : 0;
    for (uint32_t i = 0; i < nstubbed; i++)
        if (stubbed[i] == h) {
            VkShaderModuleCreateInfo c2 = *ci;
            c2.pNext = NULL;
            c2.pCode = stub_fs;
            c2.codeSize = sizeof(stub_fs);
            VkResult r = next_mod(d, &c2, a, out);
            VKB_INFO("quirks: shader h-%016llx replaced by the constant-colour fragment shader (%d)", (unsigned long long)h, r);
            return r;
        }
    VkResult r = next_mod(d, ci, a, out);
    if (r != VK_SUCCESS || !nlisted) return r;
    for (uint32_t i = 0; i < nlisted; i++)
        if (listed[i] == h) {
            pthread_mutex_lock(&lock);
            marked[next_mark] = *out;
            next_mark = (next_mark + 1) % MODS;
            if (nmarked < MODS) nmarked++;
            pthread_mutex_unlock(&lock);
            VKB_INFO("quirks: shader h-%016llx seen; pipelines using it draw nothing", (unsigned long long)h);
        }
    return r;
}

static int is_marked(VkShaderModule m)
{
    int hit = 0;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < nmarked && !hit; i++) hit = marked[i] == m;
    pthread_mutex_unlock(&lock);
    return hit;
}

static VKAPI_ATTR VkResult VKAPI_CALL q_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                               const VkAllocationCallbacks *a, VkPipeline *out)
{
    if (!nmarked) return next_gfx(d, c, n, ci, a, out);
    VkGraphicsPipelineCreateInfo local[8], *mine = NULL;
    VkPipelineRasterizationStateCreateInfo rs[8];
    for (uint32_t i = 0; i < n; i++) {
        int hit = 0;
        for (uint32_t s = 0; s < ci[i].stageCount && !hit; s++) hit = ci[i].pStages[s].module && is_marked(ci[i].pStages[s].module);
        if (!hit || !ci[i].pRasterizationState || i >= 8) continue;
        if (!mine) {
            if (n > 8) break;
            memcpy(local, ci, n * sizeof(*ci));
            mine = local;
        }
        rs[i] = *ci[i].pRasterizationState;
        rs[i].rasterizerDiscardEnable = VK_TRUE;
        local[i].pRasterizationState = &rs[i];
        int dyn = 0;
        for (uint32_t k = 0; ci[i].pDynamicState && k < ci[i].pDynamicState->dynamicStateCount; k++)
            dyn |= ci[i].pDynamicState->pDynamicStates[k] == VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE;
        VKB_INFO("quirks: a pipeline with a listed shader made with rasterization discarded%s",
                 dyn ? " (but the state is dynamic: not applied)" : "");
    }
    return next_gfx(d, c, n, mine ? mine : ci, a, out);
}

static void parse_list(const char *v, uint64_t *out, uint32_t *n)
{
    while (v && *v && *n < 32) {
        while (*v == ',' || *v == ' ' || *v == 'h' || *v == '-') v++;
        char *end;
        unsigned long long h = strtoull(v, &end, 16);
        if (end == v) break;
        out[(*n)++] = h;
        v = end;
    }
}

void vkb_quirks_device_init(vkb_srv_table *t)
{
    if (!t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines) return;
    pthread_mutex_lock(&lock);
    if (!nlisted && !nstubbed) {
        for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++)
            if (builtin[i] && nlisted < 32) listed[nlisted++] = builtin[i];
        parse_list(vkb_srv_env("VKBRIDGE_DISCARD_SHADERS"), listed, &nlisted);
        parse_list(vkb_srv_env("VKBRIDGE_STUB_SHADERS"), stubbed, &nstubbed);
    }
    pthread_mutex_unlock(&lock);
    if (!nlisted && !nstubbed) return;
    if (t->real.vkCreateShaderModule != q_CreateShaderModule) next_mod = t->real.vkCreateShaderModule;
    if (t->real.vkCreateGraphicsPipelines != q_CreateGraphicsPipelines) next_gfx = t->real.vkCreateGraphicsPipelines;
    if (t->dt.vkCreateShaderModule == next_mod) t->dt.vkCreateShaderModule = q_CreateShaderModule;
    if (t->dt.vkCreateGraphicsPipelines == next_gfx) t->dt.vkCreateGraphicsPipelines = q_CreateGraphicsPipelines;
    t->real.vkCreateShaderModule = q_CreateShaderModule;
    t->real.vkCreateGraphicsPipelines = q_CreateGraphicsPipelines;
}
