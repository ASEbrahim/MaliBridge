/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * nan_position (MK-45): the Mali-G77's r32p1 (Retroid Pocket 4 Pro, 2026-10-27) rasterizes a
 * primitive whose clip-space position holds NaN as a sheet over the whole render target; other
 * GPUs drop such primitives. Ys Origin keeps its dead particles in the draw list with NaN world
 * matrices (154 of the 208 sprite draws of one traced frame), which costs nothing on a PC and
 * here painted every room white at 12 fps (tools/ys_replay.py on the apitrace; tools/vkstrip.py).
 *
 * The fix rewrites every vertex shader on its way to the driver (spirv_nanpos.c: before each
 * return of the entry point, a position with a NaN component becomes (2, 2, 2, 1), outside the
 * clip volume): vkCreateShaderModule and the inline modules of vkCreateGraphicsPipelines.
 * Installed driver-near like every erratum, chained with the other module rewrites of the same
 * functions. The new code is a function of the old, so the driver's pipeline cache keeps its
 * entries from one launch to the next; rewritten modules are named by the app's code
 * (vkb_errata_spv_origin), so the compile guard, the hang detector and the discard list see the
 * app's hashes. No imitation (host).
 *
 * Logged: each distinct rewritten module once (the first 64 per device at info level, the rest
 * at debug), a module the pass cannot rewrite with the reason, and a device total at
 * vkDestroyDevice.
 */
#define _GNU_SOURCE
#include "server_errata.h"
#include "spirv_nanpos.h"

#include <stdlib.h>
#include <string.h>

#define LOG_INFO_MAX 64
#define INLINE_MAX 64

typedef struct vkb_errata_nanpos {
    PFN_vkCreateShaderModule fix_mod;
    PFN_vkCreateGraphicsPipelines fix_gfx;
    unsigned long seen, rewritten, guards, failed, logged;
} vkb_errata_nanpos;

static vkb_errata_nanpos *last_state;

static uint64_t fnv1a(const uint32_t *code, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)code;
    for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static vkb_errata_nanpos *state(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->nanpos ? e->nanpos : __atomic_load_n(&last_state, __ATOMIC_ACQUIRE);
}

/* The fix on one module: 1 with *out the new code to use (the caller frees it). */
static int rewrite(vkb_errata_nanpos *s, const uint32_t *code, size_t size, uint32_t **out, size_t *out_size)
{
    if (!code || size < 20) return 0;
    vkb_nanpos_stats st;
    const char *why;
    int r = vkb_spv_nan_position(code, size, out, out_size, &st, &why);
    __atomic_add_fetch(&s->seen, 1, __ATOMIC_RELAXED);
    if (r < 0) {
        __atomic_add_fetch(&s->failed, 1, __ATOMIC_RELAXED);
        VKB_WARN("errata: nan_position: module h-%016llx left as it is (%s); a NaN position may cover the screen",
                 (unsigned long long)vkb_spv_app_hash(code, size), why);
        return 0;
    }
    if (r == 0) return 0;
    __atomic_add_fetch(&s->rewritten, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->guards, st.guards, __ATOMIC_RELAXED);
    uint64_t app = vkb_spv_app_hash(code, size);
    if (vkb_errata_spv_origin(*out, *out_size, code, size)) {
        unsigned long n = __atomic_fetch_add(&s->logged, 1, __ATOMIC_RELAXED);
        if (n < LOG_INFO_MAX || vkb_log_level() >= VKB_LOG_DEBUG)
            vkb_log(n < LOG_INFO_MAX ? VKB_LOG_INFO : VKB_LOG_DEBUG,
                    "errata: nan_position: vertex module h-%016llx: %u return(s) guarded (%s), %zu -> %zu bytes, driver code h-%016llx%s",
                    (unsigned long long)app, st.guards, st.member ? "gl_PerVertex member" : "Position variable", size, *out_size,
                    (unsigned long long)fnv1a(*out, *out_size), n == LOG_INFO_MAX - 1 ? " (further modules at debug level)" : "");
    }
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL np_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    vkb_errata_nanpos *s = state();
    uint32_t *code = NULL;
    size_t size = 0;
    if (!rewrite(s, ci->pCode, ci->codeSize, &code, &size)) return s->fix_mod(d, ci, a, out);
    VkShaderModuleCreateInfo c = *ci;
    c.pCode = code;
    c.codeSize = size;
    VkResult r = s->fix_mod(d, &c, a, out);
    free(code);
    return r;
}

/* Inline modules of a batch's stages, rewritten in place for the call (and put back after: the
 * create infos may be a layer's own). */
typedef struct {
    VkShaderModuleCreateInfo *mi;
    const uint32_t *code;
    size_t size;
    uint32_t *repl;
} inline_repl;

static VKAPI_ATTR VkResult VKAPI_CALL np_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_errata_nanpos *s = state();
    inline_repl r[INLINE_MAX];
    unsigned nr = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < ci[i].stageCount; k++)
            for (const VkBaseInStructure *b = ci[i].pStages[k].pNext; b; b = b->pNext) {
                if (b->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
                VkShaderModuleCreateInfo *mi = (VkShaderModuleCreateInfo *)b;
                uint32_t *code;
                size_t size;
                if (nr == INLINE_MAX) {
                    VKB_WARN("errata: nan_position: more than %d inline modules in one call; the rest left as they are", INLINE_MAX);
                    break;
                }
                if (!rewrite(s, mi->pCode, mi->codeSize, &code, &size)) continue;
                r[nr++] = (inline_repl){mi, mi->pCode, mi->codeSize, code};
                mi->pCode = code;
                mi->codeSize = size;
            }
    VkResult res = s->fix_gfx(d, c, n, ci, a, out);
    for (unsigned k = 0; k < nr; k++) {
        r[k].mi->pCode = r[k].code;
        r[k].mi->codeSize = r[k].size;
        free(r[k].repl);
    }
    return res;
}

void vkb_errata_nanpos_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (!e->nanpos) e->nanpos = calloc(1, sizeof(*e->nanpos));
    vkb_errata_nanpos *s = e->nanpos;
    if (!s || !t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines) return;
    if (!e->probe)
        vkb_errata_info_once("errata: nan_position: vertex shaders guarded - a position with a NaN component is moved outside the clip "
                             "volume before each return - MK-45");
    s->fix_mod = t->real.vkCreateShaderModule;
    s->fix_gfx = t->real.vkCreateGraphicsPipelines;
    ERRATA_HOOK(t, vkCreateShaderModule, np_CreateShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, np_CreateGraphicsPipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_nanpos_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_nanpos *s = e->nanpos;
    if (!s) return;
    if (e->on >> VKB_ERRATUM_NAN_POSITION & 1)
        VKB_INFO("errata: nan_position: device %u: %lu of %lu module(s) guarded (%lu return(s)), %lu left as they were", t->id, s->rewritten,
                 s->seen, s->guards, s->failed);
    e->nanpos = NULL;
    /* the last one made stays: a server thread that finds no table falls back to it */
    if (e->probe || __atomic_load_n(&last_state, __ATOMIC_ACQUIRE) != s) free(s);
}
