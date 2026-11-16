/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * fragdepth_nodepth (MK-49; player logs of RE Village and The Witcher 3, Mali-G925 r49p1): a draw whose fragment shader
 * writes gl_FragDepth (DepthReplacing) into a rendering with no depth attachment never finishes; the kernel ends the queue group
 * after ~7 s ('GROUP_ERROR_FATAL ... status(0x72)', then TIMEOUT), every frame, until Android kills the app. The draw is
 * vkd3d-proton's layered image copy (vs h-d59637f9efbe652b 'Layer = InstanceIndex', fs h-d509c67fe2a57e2a: texelFetch, writes
 * the value to a colour output AND to FragDepth) copying into an R32_SFLOAT colour target. Shown on the Ultra:
 * dropping that draw (VKBRIDGE_SKIP_SHADERS) ends the hang in both games; the same fragment shader without its FragDepth store
 * and DepthReplacing (VKBRIDGE_HANG_SUBST) ends it too with the draw running (Witcher's pre-menu cutscene at 60 fps); the vertex
 * shader without its Layer write does not.
 *
 * The fix, where the attachment is known - vkCreateGraphicsPipelines with dynamic rendering: when the rendering has no depth
 * format (VkPipelineRenderingCreateInfo depthAttachmentFormat UNDEFINED, or no rendering info at all) and the fragment stage
 * writes FragDepth, that stage gets the module without its depth output (spirv_fragdepth.c), as a temporary module made for the
 * call (an inline module: its code swapped for the call and put back). By the Vulkan spec a fragment depth has nowhere to go
 * without a depth attachment, so the picture is the same. Pipelines with a depth attachment keep the shader as it is (vkd3d-proton
 * uses the same shader to copy INTO depth images). Render-pass pipelines are left alone (vkd3d-proton uses dynamic rendering; a
 * render pass's subpass would have to be tracked). vkCreateShaderModule keeps the code of fragment modules that write FragDepth
 * only (a scan of every module, no allocation for the others), dropped at vkDestroyShaderModule.
 *
 * Gated to the driver it was seen on (G925 r49p1, both modes); the change is exact on any driver, so widening it needs only the
 * evidence. Cost: the scan per module, a pass and a temporary module per such pipeline (a few vkd3d-proton meta pipelines);
 * nothing per frame, no memory. Logged: each changed fragment module once (the first 16 at info level), a module the pass
 * refuses with the reason, totals at vkDestroyDevice.
 *
 * Imitation (host): beneath the fix, such a pipeline fails to be made (VK_ERROR_UNKNOWN), standing for the draw that never ends;
 * with the fix above it the imitation sees no depth output and lets it through.
 */
#define _GNU_SOURCE
#include "server_errata.h"
#include "spirv_fragdepth.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define BUCKETS 64
#define LOG_INFO_MAX 16
#define MAX_STAGES 8
#define CHANGE_MAX 64

typedef struct mod_rec {
    uint64_t h;
    uint32_t *code;
    size_t size;
    struct mod_rec *next;
} mod_rec;

typedef struct table {
    pthread_mutex_t lock;
    mod_rec *b[BUCKETS];
} table;

typedef struct vkb_errata_fragdepth {
    PFN_vkCreateShaderModule fix_mod, imi_mod;
    PFN_vkDestroyShaderModule fix_dmod, imi_dmod;
    PFN_vkCreateGraphicsPipelines fix_gfx, imi_gfx;
    table tab[2]; /* 0: the fix's, 1: the imitation's */
    unsigned long changed, refused, imitated, logged;
} vkb_errata_fragdepth;

static vkb_errata_fragdepth *last_state;

static vkb_errata_fragdepth *state(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->fragdepth ? e->fragdepth : __atomic_load_n(&last_state, __ATOMIC_ACQUIRE);
}

static unsigned bucket(uint64_t h) { return (unsigned)((h * 0x9e3779b97f4a7c15ull) >> 58) % BUCKETS; }

static const void *chain_find(const void *pNext, VkStructureType t)
{
    for (const VkBaseInStructure *b = pNext; b; b = b->pNext)
        if (b->sType == t) return b;
    return NULL;
}

/* ---- modules: the code of fragment modules that write FragDepth ---- */

static void record(vkb_errata_fragdepth *s, int imitate, VkShaderModule m, const uint32_t *code, size_t size)
{
    if (!vkb_spv_fragdepth_writes(code, size)) return;
    mod_rec *r = calloc(1, sizeof(*r));
    if (!r || !(r->code = malloc(size))) {
        free(r);
        VKB_WARN("errata: fragdepth_nodepth: out of memory keeping a fragment module; its pipelines are left as they are");
        return;
    }
    memcpy(r->code, code, size);
    r->size = size;
    r->h = (uint64_t)(uintptr_t)m;
    table *t = &s->tab[imitate];
    pthread_mutex_lock(&t->lock);
    unsigned k = bucket(r->h);
    r->next = t->b[k];
    t->b[k] = r;
    pthread_mutex_unlock(&t->lock);
}

static void forget(vkb_errata_fragdepth *s, int imitate, VkShaderModule m)
{
    table *t = &s->tab[imitate];
    uint64_t h = (uint64_t)(uintptr_t)m;
    pthread_mutex_lock(&t->lock);
    for (mod_rec **p = &t->b[bucket(h)]; *p; p = &(*p)->next)
        if ((*p)->h == h) {
            mod_rec *r = *p;
            *p = r->next;
            free(r->code);
            free(r);
            break;
        }
    pthread_mutex_unlock(&t->lock);
}

/* A copy of a kept module's code (NULL: not one that writes FragDepth) */
static uint32_t *lookup(vkb_errata_fragdepth *s, int imitate, VkShaderModule m, size_t *size)
{
    table *t = &s->tab[imitate];
    uint64_t h = (uint64_t)(uintptr_t)m;
    uint32_t *c = NULL;
    pthread_mutex_lock(&t->lock);
    for (mod_rec *r = t->b[bucket(h)]; r; r = r->next)
        if (r->h == h) {
            if ((c = malloc(r->size))) memcpy(c, r->code, r->size), *size = r->size;
            break;
        }
    pthread_mutex_unlock(&t->lock);
    return c;
}

static VkResult module_call(int imitate, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out)
{
    vkb_errata_fragdepth *s = state();
    VkResult r = (imitate ? s->imi_mod : s->fix_mod)(d, ci, a, out);
    if (r == VK_SUCCESS && ci && ci->pCode) record(s, imitate, *out, ci->pCode, ci->codeSize);
    return r;
}

static void dmodule_call(int imitate, VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    vkb_errata_fragdepth *s = state();
    if (m) forget(s, imitate, m);
    (imitate ? s->imi_dmod : s->fix_dmod)(d, m, a);
}

/* ---- pipelines ---- */

/* 1: a dynamic-rendering pipeline that renders to no depth attachment (the case the fix and the imitation act on) */
static int no_depth(const VkGraphicsPipelineCreateInfo *p)
{
    if (p->renderPass) return 0;
    if ((p->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || chain_find(p->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT) ||
        chain_find(p->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR))
        return 0;
    const VkPipelineRenderingCreateInfo *ri = chain_find(p->pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    return !ri || ri->depthAttachmentFormat == VK_FORMAT_UNDEFINED;
}

/* The fragment stage's code that writes FragDepth (an inline module's, or a kept module's copy in *owned) */
static const uint32_t *frag_code(vkb_errata_fragdepth *s, int imitate, const VkPipelineShaderStageCreateInfo *st, size_t *size,
                                 VkShaderModuleCreateInfo **inl, uint32_t **owned)
{
    *owned = NULL;
    *inl = (VkShaderModuleCreateInfo *)chain_find(st->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    if (*inl) {
        if (!(*inl)->pCode || !vkb_spv_fragdepth_writes((*inl)->pCode, (*inl)->codeSize)) return NULL;
        *size = (*inl)->codeSize;
        return (*inl)->pCode;
    }
    if (!st->module) return NULL;
    *owned = lookup(s, imitate, st->module, size);
    return *owned;
}

typedef struct change {
    uint32_t ci, stage;
    VkShaderModule temp;
    VkShaderModuleCreateInfo *inl;
    const uint32_t *inl_code;
    size_t inl_size;
    uint32_t *code;
    size_t code_size;
} change;

static VkResult gfx_imitate(vkb_errata_fragdepth *s, VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                            const VkAllocationCallbacks *a, VkPipeline *out)
{
    for (uint32_t i = 0; i < n; i++) {
        const VkGraphicsPipelineCreateInfo *p = &ci[i];
        if (!no_depth(p) || !p->pStages) continue;
        for (uint32_t k = 0; k < p->stageCount; k++) {
            if (p->pStages[k].stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            VkShaderModuleCreateInfo *inl;
            uint32_t *owned;
            size_t size = 0;
            const uint32_t *code = frag_code(s, 1, &p->pStages[k], &size, &inl, &owned);
            free(owned);
            if (code) {
                __atomic_add_fetch(&s->imitated, 1, __ATOMIC_RELAXED);
                VKB_WARN("errata: fragdepth_nodepth imitation: a pipeline writes FragDepth with no depth attachment; refused "
                         "(the G925 r49p1's draw never ends)");
                for (uint32_t j = 0; j < n; j++) out[j] = VK_NULL_HANDLE;
                return VK_ERROR_UNKNOWN;
            }
        }
    }
    return s->imi_gfx(d, c, n, ci, a, out);
}

static void log_change(vkb_errata_fragdepth *s, const uint32_t *code, size_t size, size_t out_size)
{
    unsigned long n = __atomic_fetch_add(&s->logged, 1, __ATOMIC_RELAXED);
    if (n < LOG_INFO_MAX || vkb_log_level() >= VKB_LOG_DEBUG)
        vkb_log(n < LOG_INFO_MAX ? VKB_LOG_INFO : VKB_LOG_DEBUG,
                "errata: fragdepth_nodepth: fragment module h-%016llx: its FragDepth output dropped in a pipeline with no depth "
                "attachment, %zu -> %zu bytes%s",
                (unsigned long long)vkb_spv_app_hash(code, size), size, out_size, n == LOG_INFO_MAX - 1 ? " (further modules at debug level)" : "");
}

static VkResult gfx_fix(vkb_errata_fragdepth *s, VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                        const VkAllocationCallbacks *a, VkPipeline *out)
{
    change ch[CHANGE_MAX];
    unsigned nch = 0;
    for (uint32_t i = 0; i < n && nch < CHANGE_MAX; i++) {
        const VkGraphicsPipelineCreateInfo *p = &ci[i];
        if (!no_depth(p) || !p->pStages || p->stageCount > MAX_STAGES) continue;
        for (uint32_t k = 0; k < p->stageCount && nch < CHANGE_MAX; k++) {
            if (p->pStages[k].stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
            VkShaderModuleCreateInfo *inl;
            uint32_t *owned;
            size_t size = 0;
            const uint32_t *code = frag_code(s, 0, &p->pStages[k], &size, &inl, &owned);
            if (!code) continue;
            uint32_t *nc = NULL;
            size_t ns = 0;
            const char *why = "";
            int r = vkb_spv_strip_fragdepth(code, size, &nc, &ns, &why);
            if (r < 0) {
                __atomic_add_fetch(&s->refused, 1, __ATOMIC_RELAXED);
                VKB_WARN("errata: fragdepth_nodepth: fragment module h-%016llx left as it is (%s); its draws may never end on this driver",
                         (unsigned long long)vkb_spv_app_hash(code, size), why);
            }
            if (r != 1) {
                free(owned);
                continue;
            }
            change *x = &ch[nch];
            memset(x, 0, sizeof(*x));
            x->ci = i, x->stage = k, x->code = nc, x->code_size = ns;
            if (inl) {
                x->inl = inl, x->inl_code = inl->pCode, x->inl_size = inl->codeSize;
            } else {
                VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, ns, nc};
                if (s->fix_mod(d, &mci, NULL, &x->temp) != VK_SUCCESS) {
                    VKB_WARN("errata: fragdepth_nodepth: the changed fragment module could not be made; the pipeline is left as it is");
                    free(nc), free(owned);
                    continue;
                }
            }
            __atomic_add_fetch(&s->changed, 1, __ATOMIC_RELAXED);
            log_change(s, code, size, ns);
            free(owned);
            nch++;
        }
    }
    if (!nch) return s->fix_gfx(d, c, n, ci, a, out);
    VkGraphicsPipelineCreateInfo *cc = malloc(n * sizeof(*cc));
    VkPipelineShaderStageCreateInfo *ss = malloc((size_t)nch * MAX_STAGES * sizeof(*ss));
    VkResult res;
    if (!cc || !ss) {
        VKB_WARN("errata: fragdepth_nodepth: out of memory; %u pipeline(s) made as they were", nch);
        res = s->fix_gfx(d, c, n, ci, a, out);
    } else {
        memcpy(cc, ci, n * sizeof(*cc));
        for (unsigned k = 0; k < nch; k++)
            if (ch[k].inl) ch[k].inl->pCode = ch[k].code, ch[k].inl->codeSize = ch[k].code_size;
        for (unsigned k = 0; k < nch; k++) {
            change *x = &ch[k];
            if (x->inl) continue;
            VkPipelineShaderStageCreateInfo *dst = ss + (size_t)k * MAX_STAGES;
            /* one fragment stage per create info: two changes never meet in one */
            memcpy(dst, cc[x->ci].pStages, cc[x->ci].stageCount * sizeof(*dst));
            dst[x->stage].module = x->temp;
            cc[x->ci].pStages = dst;
        }
        res = s->fix_gfx(d, c, n, cc, a, out);
    }
    for (unsigned k = 0; k < nch; k++) {
        change *x = &ch[k];
        if (x->inl) x->inl->pCode = x->inl_code, x->inl->codeSize = x->inl_size;
        if (x->temp) s->fix_dmod(d, x->temp, NULL);
        free(x->code);
    }
    free(cc);
    free(ss);
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL fd_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    return module_call(0, d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL fdi_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                             VkShaderModule *out)
{
    return module_call(1, d, ci, a, out);
}

static VKAPI_ATTR void VKAPI_CALL fd_DestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) { dmodule_call(0, d, m, a); }

static VKAPI_ATTR void VKAPI_CALL fdi_DestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) { dmodule_call(1, d, m, a); }

static VKAPI_ATTR VkResult VKAPI_CALL fd_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_fix(state(), d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL fdi_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_imitate(state(), d, c, n, ci, a, out);
}

static vkb_errata_fragdepth *get_state(vkb_errata_dev *e)
{
    if (!e->fragdepth && (e->fragdepth = calloc(1, sizeof(*e->fragdepth))))
        for (int i = 0; i < 2; i++) pthread_mutex_init(&e->fragdepth->tab[i].lock, NULL);
    return e->fragdepth;
}

static int hookable(vkb_srv_table *t)
{
    return t->real.vkCreateShaderModule && t->real.vkDestroyShaderModule && t->real.vkCreateGraphicsPipelines;
}

void vkb_errata_fragdepth_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_fragdepth *s = get_state(e);
    if (!s || !hookable(t)) return;
    s->fix_mod = t->real.vkCreateShaderModule;
    s->fix_dmod = t->real.vkDestroyShaderModule;
    s->fix_gfx = t->real.vkCreateGraphicsPipelines;
    if (!s->imi_mod) s->imi_mod = s->fix_mod, s->imi_dmod = s->fix_dmod, s->imi_gfx = s->fix_gfx;
    ERRATA_HOOK(t, vkCreateShaderModule, fd_CreateShaderModule);
    ERRATA_HOOK(t, vkDestroyShaderModule, fd_DestroyShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, fd_CreateGraphicsPipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_fragdepth_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_fragdepth *s = get_state(e);
    if (!s || !hookable(t)) return;
    s->imi_mod = t->real.vkCreateShaderModule;
    s->imi_dmod = t->real.vkDestroyShaderModule;
    s->imi_gfx = t->real.vkCreateGraphicsPipelines;
    if (!s->fix_mod) s->fix_mod = s->imi_mod, s->fix_dmod = s->imi_dmod, s->fix_gfx = s->imi_gfx;
    ERRATA_HOOK(t, vkCreateShaderModule, fdi_CreateShaderModule);
    ERRATA_HOOK(t, vkDestroyShaderModule, fdi_DestroyShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, fdi_CreateGraphicsPipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_fragdepth_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_fragdepth *s = e->fragdepth;
    if (!s) return;
    if ((e->on >> VKB_ERRATUM_FRAGDEPTH_NODEPTH & 1) && !e->probe)
        VKB_INFO("errata: fragdepth_nodepth: device %u: %lu pipeline(s) given a fragment shader without its depth output, %lu left as "
                 "they were", t->id, s->changed, s->refused);
    if ((e->imitated >> VKB_ERRATUM_FRAGDEPTH_NODEPTH & 1) && !e->probe)
        VKB_INFO("errata: fragdepth_nodepth: device %u: the imitation refused %lu pipeline(s)", t->id, s->imitated);
    for (int i = 0; i < 2; i++)
        for (unsigned k = 0; k < BUCKETS; k++)
            for (mod_rec *r = s->tab[i].b[k], *nx; r; r = nx) {
                nx = r->next;
                free(r->code);
                free(r);
            }
    memset(s->tab, 0, sizeof(s->tab));
    for (int i = 0; i < 2; i++) pthread_mutex_init(&s->tab[i].lock, NULL);
    e->fragdepth = NULL;
    if (e->probe || __atomic_load_n(&last_state, __ATOMIC_ACQUIRE) != s) free(s);
}
