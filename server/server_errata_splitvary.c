/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * split_varying (MK-47; a player log Mali-G68 r38p1 JM, a player log Mali-G925 r49p1 CSF): a fragment input at
 * a Component above 0 of a Location the stage before writes as one whole vector reads wrong (0 on
 * the G68 r38p1). Zink's fixed-function texturing makes that pair - a vec4 texture coordinate out
 * of the vertex shader, (s, t) at component 0 and q at component 3 into the fragment shader - so
 * texture2DProj divides by a wrong q and GoldSrc's menu text (Half-Life) came out as fragments.
 * Shown at the Vulkan level on a Galaxy A35 (Mali-G68 r38p1, tests/splitvary_check.c): the
 * split pair wrong, the same pair with one merged vec4 input right, both right on lavapipe.
 *
 * The fix, where the pair is known - vkCreateGraphicsPipelines: when the fragment stage has such a
 * split Location and the last pre-rasterization stage (geometry, else tessellation evaluation,
 * else vertex) writes it whole, the fragment code goes to the driver with the pieces merged into
 * one input of the whole vector's type (spirv_splitvary.c), as a temporary module made for that
 * call (an inline module: its code swapped for the call and put back). vkCreateShaderModule keeps
 * what the pairing needs: a short scan of every module's interface (no allocation), and the code
 * of fragment modules with a split input only (rare: Zink's fixed-function fragment shaders,
 * vkd3d-proton's packed signatures), dropped at vkDestroyShaderModule; VKBRIDGE_SPLITVARY_KEEP_MB
 * caps that code (32 MB by default; past it a module's pipelines are left as they are, logged).
 * Pipeline libraries are left alone (Mali offers none).
 *
 * Every Arm driver, both modes: the bug is on two GPU families and drivers two years apart with no
 * Arm erratum to bound it, and the merged interface is exact on any driver. Cost: the scan per
 * module, a SPIR-V pass and a temporary module per pipeline with the pattern; nothing per frame.
 * Logged: each distinct merged fragment module once (the first 16 at info level), a module the
 * pass cannot merge with the reason, totals at vkDestroyDevice.
 *
 * Imitation (host): the same hooks beneath the fix, the pass in VKB_SPLITVARY_IMITATE mode - a
 * load of a piece at a Component above 0 of such a Location gives 0, as the G68 r38p1 read Zink's
 * q. The fixed code has no such piece left, so with the fix above it the imitation breaks nothing.
 */
#define _GNU_SOURCE
#include "server_errata.h"
#include "spirv_splitvary.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUCKETS 256
#define LOG_INFO_MAX 16
#define KEEP_MB_DEFAULT 32
#define MAX_STAGES 8
#define INLINE_MAX 64
#define DUMP_MAX 8

typedef struct mod_rec {
    uint64_t h;
    vkb_sv_info info;
    uint32_t *code; /* fragment modules with a split input only */
    size_t size;
    struct mod_rec *next;
} mod_rec;

typedef struct table {
    pthread_mutex_t lock;
    mod_rec *b[BUCKETS];
    size_t kept;
} table;

typedef struct vkb_errata_splitvary {
    PFN_vkCreateShaderModule fix_mod, imi_mod;
    PFN_vkDestroyShaderModule fix_dmod, imi_dmod;
    PFN_vkCreateGraphicsPipelines fix_gfx, imi_gfx;
    size_t keep_max;
    table tab[2]; /* 0: the fix's, 1: the imitation's */
    unsigned long merged, failed, imitated, not_kept, logged;
} vkb_errata_splitvary;

static vkb_errata_splitvary *last_state;

/* A fragment module the pass cannot merge: its SPIR-V once into the session folder's failed-spv/ (shared by
 * "Send logs"), so one player log carries the module; at most DUMP_MAX per server, at pipeline creation only. */
static pthread_mutex_t dump_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t dumped[DUMP_MAX];
static unsigned ndumped;

static const char *dump_refused(uint64_t h, const uint32_t *code, size_t size)
{
    static __thread char path[700];
    char dir[620];
    int fresh = 1;
    pthread_mutex_lock(&dump_lock);
    for (unsigned i = 0; i < ndumped; i++)
        if (dumped[i] == h) fresh = 0;
    if (fresh && ndumped < DUMP_MAX) dumped[ndumped++] = h;
    else fresh = 0;
    pthread_mutex_unlock(&dump_lock);
    if (!fresh || !vkb_report_dir(dir, sizeof(dir)) || !*dir) return NULL;
    snprintf(path, sizeof(path), "%s/failed-spv", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/failed-spv/splitvary-h-%016llx-fs.spv", dir, (unsigned long long)h);
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    int ok = fwrite(code, 1, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        unlink(path);
        return NULL;
    }
    return path;
}

static vkb_errata_splitvary *state(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->splitvary ? e->splitvary : __atomic_load_n(&last_state, __ATOMIC_ACQUIRE);
}

static unsigned bucket(uint64_t h) { return (unsigned)((h * 0x9e3779b97f4a7c15ull) >> 56) % BUCKETS; }

static const void *chain_find(const void *pNext, VkStructureType t)
{
    for (const VkBaseInStructure *b = pNext; b; b = b->pNext)
        if (b->sType == t) return b;
    return NULL;
}

/* ---- modules ---- */

static void record(vkb_errata_splitvary *s, int imitate, VkShaderModule m, const uint32_t *code, size_t size)
{
    vkb_sv_info info;
    if (vkb_spv_sv_scan(code, size, &info) < 0) return;
    int frag = info.model == 4 /* Fragment */ && info.split;
    if (!frag && !info.whole) return; /* nothing a pairing would use */
    mod_rec *r = calloc(1, sizeof(*r));
    if (!r) return;
    r->h = (uint64_t)(uintptr_t)m;
    r->info = info;
    table *t = &s->tab[imitate];
    pthread_mutex_lock(&t->lock);
    if (frag) {
        if (t->kept + size <= s->keep_max && (r->code = malloc(size))) {
            memcpy(r->code, code, size);
            r->size = size;
            t->kept += size;
        } else {
            s->not_kept++;
            if (s->not_kept == 1)
                VKB_WARN("errata: split_varying: past %zu MB of fragment code kept (VKBRIDGE_SPLITVARY_KEEP_MB); pipelines of "
                         "further split-input modules are left as they are", s->keep_max >> 20);
        }
    }
    unsigned k = bucket(r->h);
    r->next = t->b[k];
    t->b[k] = r;
    pthread_mutex_unlock(&t->lock);
}

static void forget(vkb_errata_splitvary *s, int imitate, VkShaderModule m)
{
    table *t = &s->tab[imitate];
    uint64_t h = (uint64_t)(uintptr_t)m;
    pthread_mutex_lock(&t->lock);
    for (mod_rec **p = &t->b[bucket(h)]; *p; p = &(*p)->next)
        if ((*p)->h == h) {
            mod_rec *r = *p;
            *p = r->next;
            t->kept -= r->size;
            free(r->code);
            free(r);
            break;
        }
    pthread_mutex_unlock(&t->lock);
}

/* The record's interface and (fragment) a copy of its code, taken under the lock */
static int lookup(vkb_errata_splitvary *s, int imitate, VkShaderModule m, vkb_sv_info *info, uint32_t **code, size_t *size)
{
    table *t = &s->tab[imitate];
    uint64_t h = (uint64_t)(uintptr_t)m;
    int found = 0;
    pthread_mutex_lock(&t->lock);
    for (mod_rec *r = t->b[bucket(h)]; r; r = r->next)
        if (r->h == h) {
            *info = r->info;
            if (code && r->code && (*code = malloc(r->size))) memcpy(*code, r->code, r->size), *size = r->size;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&t->lock);
    return found;
}

static VkResult module_call(int imitate, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out)
{
    vkb_errata_splitvary *s = state();
    VkResult r = (imitate ? s->imi_mod : s->fix_mod)(d, ci, a, out);
    /* each layer keeps its own records: the fix's temporary modules reach the imitation's */
    if (r == VK_SUCCESS && ci && ci->pCode) record(s, imitate, *out, ci->pCode, ci->codeSize);
    return r;
}

static void dmodule_call(int imitate, VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    vkb_errata_splitvary *s = state();
    if (m) forget(s, imitate, m);
    (imitate ? s->imi_dmod : s->fix_dmod)(d, m, a);
}

/* ---- pipelines ---- */

static int stage_model(VkShaderStageFlagBits st)
{
    switch (st) {
    case VK_SHADER_STAGE_VERTEX_BIT: return 0;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return 2;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return 3;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return 4;
    default: return -1;
    }
}

/* A stage's interface (and its code when want_code): its inline module's, or its module's record */
static int stage_info(vkb_errata_splitvary *s, int imitate, const VkPipelineShaderStageCreateInfo *st, vkb_sv_info *info, uint32_t **code,
                      size_t *size, VkShaderModuleCreateInfo **inl)
{
    *inl = (VkShaderModuleCreateInfo *)chain_find(st->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    if (*inl) {
        if (!(*inl)->pCode || vkb_spv_sv_scan((*inl)->pCode, (*inl)->codeSize, info) < 0) return 0;
        if (code) *code = NULL; /* the caller reads the inline code itself */
        return 1;
    }
    if (!st->module) return 0;
    return lookup(s, imitate, st->module, info, code, size);
}

typedef struct change {
    uint32_t ci, stage;
    VkShaderModule temp;               /* a module made for the call */
    VkShaderModuleCreateInfo *inl;     /* or an inline module's code swapped */
    const uint32_t *inl_code;
    size_t inl_size;
    uint32_t *code;
    size_t code_size;
} change;

static void log_merge(vkb_errata_splitvary *s, const uint32_t *code, size_t size, const uint32_t *out, size_t out_size,
                      const vkb_splitvary_stats *st)
{
    if (!vkb_errata_spv_origin(out, out_size, code, size)) return;
    unsigned long n = __atomic_fetch_add(&s->logged, 1, __ATOMIC_RELAXED);
    if (n < LOG_INFO_MAX || vkb_log_level() >= VKB_LOG_DEBUG)
        vkb_log(n < LOG_INFO_MAX ? VKB_LOG_INFO : VKB_LOG_DEBUG,
                "errata: split_varying: fragment module h-%016llx: location(s) 0x%x merged (%u input(s) into one each, %u load(s)), "
                "%zu -> %zu bytes%s",
                (unsigned long long)vkb_spv_app_hash(code, size), st->locations, st->pieces, st->loads, size, out_size,
                n == LOG_INFO_MAX - 1 ? " (further modules at debug level)" : "");
}

static VkResult gfx_call(int imitate, VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                         const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_errata_splitvary *s = state();
    PFN_vkCreateGraphicsPipelines next = imitate ? s->imi_gfx : s->fix_gfx;
    PFN_vkCreateShaderModule mod = imitate ? s->imi_mod : s->fix_mod;
    PFN_vkDestroyShaderModule dmod = imitate ? s->imi_dmod : s->fix_dmod;
    change ch[INLINE_MAX];
    unsigned nch = 0;
    for (uint32_t i = 0; i < n && nch < INLINE_MAX; i++) {
        const VkGraphicsPipelineCreateInfo *p = &ci[i];
        if ((p->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || chain_find(p->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT) ||
            chain_find(p->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR) || p->stageCount > MAX_STAGES || !p->pStages)
            continue;
        int fs = -1, pre = -1, pre_model = -1;
        for (uint32_t k = 0; k < p->stageCount; k++) {
            int m = stage_model(p->pStages[k].stage);
            if (m == 4) fs = (int)k;
            else if (m >= 0 && m > pre_model) pre = (int)k, pre_model = m;
        }
        if (fs < 0 || pre < 0) continue;
        vkb_sv_info fi, pi;
        VkShaderModuleCreateInfo *finl, *pinl;
        uint32_t *fcode = NULL;
        size_t fsize = 0;
        if (!stage_info(s, imitate, &p->pStages[pre], &pi, NULL, NULL, &pinl) || !pi.whole) continue;
        if (!stage_info(s, imitate, &p->pStages[fs], &fi, &fcode, &fsize, &finl) || !(fi.split & pi.whole)) {
            free(fcode);
            continue;
        }
        const uint32_t *src = finl ? finl->pCode : fcode;
        size_t src_size = finl ? finl->codeSize : fsize;
        if (!src) continue; /* a module whose code was not kept */
        uint32_t *nc = NULL;
        size_t ns = 0;
        vkb_splitvary_stats st;
        const char *why;
        int r = vkb_spv_split_varying(src, src_size, &pi, imitate ? VKB_SPLITVARY_IMITATE : 0, &nc, &ns, &st, &why);
        if (r < 0 && !imitate) {
            __atomic_add_fetch(&s->failed, 1, __ATOMIC_RELAXED);
            uint64_t h = vkb_spv_app_hash(src, src_size);
            const char *saved = dump_refused(h, src, src_size);
            const char *name = saved ? strrchr(saved, '/') : NULL;
            VKB_WARN("errata: split_varying: fragment module h-%016llx left as it is (%s); Mali may read its inputs wrong%s%s",
                     (unsigned long long)h, why, name ? "; its SPIR-V: session folder failed-spv" : "", name ? name : "");
        }
        if (r != 1) {
            free(fcode);
            continue;
        }
        change *x = &ch[nch];
        memset(x, 0, sizeof(*x));
        x->ci = i, x->stage = (uint32_t)fs, x->code = nc, x->code_size = ns;
        if (finl) {
            x->inl = finl, x->inl_code = finl->pCode, x->inl_size = finl->codeSize;
        } else {
            VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, ns, nc};
            if (mod(d, &mci, NULL, &x->temp) != VK_SUCCESS) {
                free(nc), free(fcode);
                continue;
            }
        }
        if (imitate) __atomic_add_fetch(&s->imitated, 1, __ATOMIC_RELAXED);
        else {
            __atomic_add_fetch(&s->merged, 1, __ATOMIC_RELAXED);
            log_merge(s, src, src_size, nc, ns, &st);
        }
        free(fcode);
        nch++;
    }
    if (!nch) return next(d, c, n, ci, a, out);
    /* the create infos and stages the call sees: copies where a stage's module changes */
    VkGraphicsPipelineCreateInfo *cc = malloc(n * sizeof(*cc));
    VkPipelineShaderStageCreateInfo *ss = malloc((size_t)nch * MAX_STAGES * sizeof(*ss));
    VkResult res;
    if (!cc || !ss) {
        res = next(d, c, n, ci, a, out);
    } else {
        memcpy(cc, ci, n * sizeof(*cc));
        for (unsigned k = 0; k < nch; k++) {
            change *x = &ch[k];
            if (x->inl) {
                x->inl->pCode = x->code;
                x->inl->codeSize = x->code_size;
            }
        }
        for (unsigned k = 0; k < nch; k++) {
            change *x = &ch[k];
            if (x->inl) continue;
            VkPipelineShaderStageCreateInfo *dst = ss + (size_t)k * MAX_STAGES;
            const VkPipelineShaderStageCreateInfo *from = cc[x->ci].pStages;
            /* two changes never meet in one create info: one fragment stage each */
            memcpy(dst, from, cc[x->ci].stageCount * sizeof(*dst));
            dst[x->stage].module = x->temp;
            cc[x->ci].pStages = dst;
        }
        res = next(d, c, n, cc, a, out);
    }
    for (unsigned k = 0; k < nch; k++) {
        change *x = &ch[k];
        if (x->inl) x->inl->pCode = x->inl_code, x->inl->codeSize = x->inl_size;
        if (x->temp) dmod(d, x->temp, NULL);
        free(x->code);
    }
    free(cc);
    free(ss);
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL sv_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    return module_call(0, d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL svi_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                             VkShaderModule *out)
{
    return module_call(1, d, ci, a, out);
}

static VKAPI_ATTR void VKAPI_CALL sv_DestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    dmodule_call(0, d, m, a);
}

static VKAPI_ATTR void VKAPI_CALL svi_DestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    dmodule_call(1, d, m, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL sv_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_call(0, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL svi_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_call(1, d, c, n, ci, a, out);
}

static vkb_errata_splitvary *get_state(vkb_errata_dev *e)
{
    if (!e->splitvary && (e->splitvary = calloc(1, sizeof(*e->splitvary)))) {
        for (int i = 0; i < 2; i++) pthread_mutex_init(&e->splitvary->tab[i].lock, NULL);
        const char *v = vkb_srv_env("VKBRIDGE_SPLITVARY_KEEP_MB");
        long mb = v && *v ? strtol(v, NULL, 10) : KEEP_MB_DEFAULT;
        e->splitvary->keep_max = (size_t)(mb < 0 ? 0 : mb) << 20;
    }
    return e->splitvary;
}

static int hookable(vkb_srv_table *t)
{
    return t->real.vkCreateShaderModule && t->real.vkDestroyShaderModule && t->real.vkCreateGraphicsPipelines;
}

void vkb_errata_splitvary_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_splitvary *s = get_state(e);
    if (!s || !hookable(t)) return;
    s->fix_mod = t->real.vkCreateShaderModule;
    s->fix_dmod = t->real.vkDestroyShaderModule;
    s->fix_gfx = t->real.vkCreateGraphicsPipelines;
    if (!s->imi_mod) s->imi_mod = s->fix_mod, s->imi_dmod = s->fix_dmod, s->imi_gfx = s->fix_gfx;
    ERRATA_HOOK(t, vkCreateShaderModule, sv_CreateShaderModule);
    ERRATA_HOOK(t, vkDestroyShaderModule, sv_DestroyShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, sv_CreateGraphicsPipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_splitvary_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_splitvary *s = get_state(e);
    if (!s || !hookable(t)) return;
    s->imi_mod = t->real.vkCreateShaderModule;
    s->imi_dmod = t->real.vkDestroyShaderModule;
    s->imi_gfx = t->real.vkCreateGraphicsPipelines;
    if (!s->fix_mod) s->fix_mod = s->imi_mod, s->fix_dmod = s->imi_dmod, s->fix_gfx = s->imi_gfx;
    ERRATA_HOOK(t, vkCreateShaderModule, svi_CreateShaderModule);
    ERRATA_HOOK(t, vkDestroyShaderModule, svi_DestroyShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, svi_CreateGraphicsPipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_splitvary_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_splitvary *s = e->splitvary;
    if (!s) return;
    if ((e->on >> VKB_ERRATUM_SPLIT_VARYING & 1) && !e->probe)
        VKB_INFO("errata: split_varying: device %u: %lu pipeline(s) given a merged fragment input, %lu left as they were, "
                 "%lu module(s) whose code was not kept",
                 t->id, s->merged, s->failed, s->not_kept);
    if ((e->imitated >> VKB_ERRATUM_SPLIT_VARYING & 1) && !e->probe)
        VKB_INFO("errata: split_varying: device %u: the imitation broke %lu pipeline(s)", t->id, s->imitated);
    for (int i = 0; i < 2; i++)
        for (unsigned k = 0; k < BUCKETS; k++)
            for (mod_rec *r = s->tab[i].b[k], *nx; r; r = nx) {
                nx = r->next;
                free(r->code);
                free(r);
            }
    memset(s->tab, 0, sizeof(s->tab));
    for (int i = 0; i < 2; i++) pthread_mutex_init(&s->tab[i].lock, NULL);
    e->splitvary = NULL;
    /* the last one made stays: a server thread that finds no table falls back to it */
    if (e->probe || __atomic_load_n(&last_state, __ATOMIC_ACQUIRE) != s) free(s);
}
