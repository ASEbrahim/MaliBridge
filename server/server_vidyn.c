/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * VK_EXT_vertex_input_dynamic_state for Zink on every Mali: vkCmdSetVertexInputEXT's vertex input
 * made static in pipeline variants.
 *
 * Without the extension Zink (Mesa 26.0 to 26.2.4; the DroidDeck guest has 26.2.3) bakes the vertex
 * input into each pipeline, and when only the vertex elements change between two draws it keeps
 * drawing with the pipeline of the old ones (Mesa 167df22a adds vertex_input_changed to the
 * condition): text in Balatro and other OpenGL games read its vertices with another layout and
 * was not drawn. With the extension Zink calls vkCmdSetVertexInputEXT whenever the elements change,
 * and the draw is right. The G720 r44, G615 r44, G68/G57 r38 and G710 r32 drivers do not have it;
 * the G925 r49 and Dimensity 9500 r54 ones do, but there the bridge hid it from Zink, because the
 * BGRA 10-10-10-2 and B10G11R11 vertex fetch emulations (VKB_EMU_BGR10_VTX, VKB_EMU_UF11_VTX:
 * server_emu_shader.c) fit the vertex shader to the formats at pipeline creation; below r48 the
 * vb_split erratum hides it too (server_errata.c). Now a Zink instance (engine "mesa zink") is
 * given the extension on every driver, in one of two modes (vkb_vidyn_mode):
 *   - native: the driver has the extension and no erratum hides it. Zink's pipelines are made as
 *     asked; vkCmdSetVertexInputEXT goes straight to the driver while the vertex input names no
 *     emulated format. One that does is not passed on, and the draws after it use a variant.
 *   - emulated: the driver does not have it (or an erratum, or VKBRIDGE_FAKE_MISSING=vidyn, takes
 *     it as missing). The extension and its feature are reported by the server itself and taken
 *     off the device's creation; no pipeline with the vertex input dynamic and no
 *     vkCmdSetVertexInputEXT reaches the driver. Every draw uses a variant.
 * Other clients see what they saw before: the extension where the driver has it and no erratum
 * hides it (the formats are then not emulated for them: vkb_emu_bgr10_for), nothing elsewhere.
 * VKBRIDGE_EMU_VIDYN=0 gives Zink the old behaviour too (the extension hidden where formats are
 * emulated or an erratum hides it).
 *
 * The layer sits over every other one that sees pipelines but the asynchronous compile
 * (server_instance.c), so a variant is made through all of them, as an ordinary pipeline is: the
 * pipeline cache, the vertex format and divisor emulations, the errata.
 *   - vkCreateGraphicsPipelines: a complete pipeline with VK_DYNAMIC_STATE_VERTEX_INPUT_EXT (not a
 *     library, none linked from libraries) has its create info kept, encoded, with its shaders'
 *     SPIR-V (server_pipekeep.c, the machinery vb_bake uses too). Native: the pipeline is made as
 *     asked and its handle is the app's. Emulated: nothing is compiled; the app gets a stand-in
 *     handle (the record's address). A create info that cannot be kept (a module this layer did not
 *     see made, one that does not encode) is made as asked where the driver has the extension and
 *     fails with VK_ERROR_OUT_OF_HOST_MEMORY where it does not. A base pipeline that is a
 *     stand-in is no base (the base is only a hint).
 *   - vkCmdSetVertexInputEXT: the bindings (binding, stride, input rate, divisor) and attributes
 *     (location, binding, format, offset) noted per command buffer, as the vertex input of the
 *     draws after it.
 *   - vkCmdBindPipeline of a kept pipeline is held; at each draw after a bind or a new vertex
 *     input, the pipeline that is to draw is bound when the driver does not have it already: the
 *     app's own (native, no emulated format: the fast path), or the variant made with the noted
 *     vertex input static - the bindings and attributes in pVertexInputState, the divisors other
 *     than 1 in a VkPipelineVertexInputDivisorStateCreateInfo, VERTEX_INPUT_EXT and
 *     VERTEX_INPUT_BINDING_STRIDE out of the dynamic list (the strides are the vertex input's),
 *     no pipeline cache (the persistent one beneath takes it), no FAIL_ON_PIPELINE_COMPILE_REQUIRED.
 *     Variants are made on first use and kept per pipeline per vertex input, as long as the
 *     pipeline lives. A variant that fails is remembered: the draws that would use it are dropped.
 *     Native: binding a variant leaves the driver's dynamic vertex input undefined, so the next
 *     draw with the app's own pipeline sends the noted one again first; so does a draw after any
 *     pipeline this layer does not keep is bound.
 *   - vkCmdBindVertexBuffers2: Zink passes no strides when the vertex input is dynamic. Strides
 *     given while a kept pipeline is bound replace those of the noted vertex input (the later call
 *     wins, as for the driver) and are not passed on: a variant's strides are static.
 *   - vkBeginCommandBuffer starts afresh; after vkCmdExecuteCommands the primary's bound pipeline
 *     and vertex input are unknown (the API leaves them undefined); a secondary has its own state.
 *     vkFreeCommandBuffers forgets the buffer.
 *   - vkDestroyPipeline of a kept pipeline destroys its variants (and the app's own pipeline,
 *     native); layouts and render passes a kept create info names live until it is released.
 * The errata beneath see variants as static pipelines: dyn_reemit has no vertex strides to send
 * again, vb_split splits nothing (no strides in Zink's calls), vb_bake bakes nothing (no dynamic
 * strides) - and the G68's MK-05 strides cannot be wrong where every draw has them static.
 * Pipeline libraries: on Mali, Zink makes none (no full EDS3); VK_EXT_graphics_pipeline_library is
 * hidden from Zink where this layer makes variants, so a desktop driver behaves the same.
 *
 * What it costs: one compile per pipeline per vertex input, at the first draw that needs it (as
 * Zink without the extension compiles one pipeline per vertex input at its own draw), and the
 * kept create infos and shader code; the totals are logged at the device's end.
 */
#define _GNU_SOURCE
#include "server_pipekeep.h"
#include "server_emu.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define VIDYN_EXT "VK_EXT_vertex_input_dynamic_state"
#define NB 32  /* vertex bindings and attributes a vertex input may have (Zink: PIPE_MAX_ATTRIBS) */
#define HB 256 /* hash buckets */
#define MANY_VARIANTS 64

static unsigned hk(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    return (unsigned)(k >> 56) & (HB - 1);
}

/* ------------------------------------------------------------------ the decision */

static int env_off(const char *name)
{
    const char *v = vkb_srv_env(name);
    return v && !strcmp(v, "0");
}

int vkb_vidyn_mode(const vkb_srv_table *inst, VkPhysicalDevice pd)
{
    if (inst && inst->is_device) inst = inst->parent;
    if (!inst || !inst->zink || !inst->real.vkEnumerateDeviceExtensionProperties || env_off("VKBRIDGE_EMU_VIDYN")) return VKB_VIDYN_OFF;
    const char *erratum = vkb_errata_hidden_ext(inst, pd);
    if (vkb_emu_driver_vi_dyn(&inst->real, pd) && !vkb_emu_ext_hidden(VIDYN_EXT) && !(erratum && !strcmp(erratum, VIDYN_EXT)))
        return VKB_VIDYN_NATIVE;
    return VKB_VIDYN_EMULATED;
}

/* The vertex formats this instance's devices on pd emulate in the vertex shader */
static uint32_t fmt_emu(const vkb_srv_table *inst, VkPhysicalDevice pd)
{
    const vkb_pd_knowledge *k = vkb_pd_lookup(&inst->real, pd);
    uint32_t f = k ? k->info.emu & (VKB_EMU_BGR10_VTX | VKB_EMU_UF11_VTX) : 0;
    return f && vkb_emu_bgr10_for(inst, pd) ? f : 0;
}

int vkb_vidyn_variants(const vkb_srv_table *inst, VkPhysicalDevice pd)
{
    if (inst && inst->is_device) inst = inst->parent;
    int m = vkb_vidyn_mode(inst, pd);
    return m == VKB_VIDYN_EMULATED || (m == VKB_VIDYN_NATIVE && fmt_emu(inst, pd));
}

/* ------------------------------------------------------------------ the feature query */

static void feature_on(void *chain)
{
    for (VkBaseOutStructure *b = chain; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT)
            ((VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *)b)->vertexInputDynamicState = VK_TRUE;
}

static const vkb_srv_table *query_inst(void)
{
    const vkb_srv_table *t = vkb_srv_current_table();
    return t && t->is_device ? t->parent : t;
}

static VKAPI_ATTR void VKAPI_CALL vi_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->vidyn_f2) return;
    t->vidyn_f2(pd, f);
    if (vkb_vidyn_mode(t, pd) == VKB_VIDYN_EMULATED) feature_on(f->pNext);
}

/* Over the instance's other feature wrappers (server_instance.c: after the promotion's) */
void vkb_vidyn_instance_install(vkb_srv_table *t)
{
    if (!t->zink) return;
    if (t->dt.vkGetPhysicalDeviceFeatures2) {
        t->vidyn_f2 = t->dt.vkGetPhysicalDeviceFeatures2;
        t->dt.vkGetPhysicalDeviceFeatures2 = vi_GetPhysicalDeviceFeatures2;
    }
}

/* ------------------------------------------------------------------ vkCreateDevice */

int vkb_vidyn_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, uint32_t emu, VkDeviceCreateInfo *ci, const char **exts,
                                 uint32_t *n)
{
    int mode = vkb_vidyn_mode(inst, pd);
    if (!mode) return 0;
    int asked = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && !asked; i++) asked = !strcmp(ci->ppEnabledExtensionNames[i], VIDYN_EXT);
    const VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT *f = NULL;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT) f = (const void *)b;
    if (mode == VKB_VIDYN_EMULATED) {
        /* the driver gets neither the name nor the feature (the chain lives in the request's arena) */
        uint32_t k = 0;
        for (uint32_t i = 0; i < *n; i++)
            if (strcmp(exts[i], VIDYN_EXT)) exts[k++] = exts[i];
        *n = k;
        vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT);
    }
    if (!asked || (f && !f->vertexInputDynamicState)) return 0;
    if (mode == VKB_VIDYN_NATIVE && !(emu & (VKB_EMU_BGR10_VTX | VKB_EMU_UF11_VTX))) {
        VKB_INFO("vkCreateDevice: VK_EXT_vertex_input_dynamic_state for Zink: the driver's own");
        return 0;
    }
    VKB_INFO("vkCreateDevice: VK_EXT_vertex_input_dynamic_state for Zink: %s",
             mode == VKB_VIDYN_EMULATED ? "emulated (every draw with a pipeline variant whose vertex input is static)"
                                        : "the driver's, with pipeline variants where the vertex input names an emulated format");
    return mode;
}

/* ------------------------------------------------------------------ records */

typedef struct vkey { /* a vertex input as vkCmdSetVertexInputEXT gives it */
    uint32_t nb, na;
    struct {
        uint32_t binding, stride, rate, divisor;
    } b[NB];
    struct {
        uint32_t location, binding, format, offset;
    } a[NB];
} vkey;

typedef struct variant {
    uint64_t hash;
    vkey *key;
    VkPipeline p; /* VK_NULL_HANDLE: it failed (its draws are dropped) */
    struct variant *next;
} variant;

typedef struct vpipe {
    VkPipeline app;  /* the app's handle: the driver's pipeline (native) or this record's address */
    VkPipeline real; /* native: the pipeline made as asked; emulated: none */
    VkDevice dev;
    vkb_pk_ci *kept;
    variant *vars;
    uint32_t nvars;
    pthread_mutex_t lock;
    struct vpipe *next;
} vpipe;

typedef struct cbst {
    VkCommandBuffer cb;
    VkPipeline bound; /* the app's graphics pipeline when it is a kept one (held), else VK_NULL_HANDLE */
    VkPipeline drv;   /* what the driver has bound (graphics), VK_NULL_HANDLE: unknown */
    int dirty;        /* a bind or a vertex input since the last draw */
    int skip;         /* the last resolve failed: draws dropped until the next bind or vertex input */
    int have_vi;
    int vi_emu;       /* the vertex input names an emulated format */
    int drv_vi_ok;    /* native: the driver's dynamic vertex input is the noted one */
    uint64_t hash;
    vkey vi;
    struct cbst *next;
} cbst;

typedef struct vkb_vidyn {
    vkb_dispatch next;
    vkb_srv_table *t;
    int mode;
    uint32_t fmt; /* VKB_EMU_BGR10_VTX / VKB_EMU_UF11_VTX on this device */
    pthread_mutex_t lock;
    vkb_pk *pk;
    vpipe *pipes[HB];
    cbst *cbs[HB];
    uint64_t kept, unkept, variants, failed, swaps, passed, held, resent, dropped, max_vars;
    uint64_t bytes, peak_bytes; /* kept create infos and shader code (lock) */
    struct vkb_vidyn *gnext;
} vkb_vidyn;

/* Every device's state: a call without a table (an async compile worker) is found by its device */
static pthread_mutex_t glock = PTHREAD_MUTEX_INITIALIZER;
static vkb_vidyn *all;

static vkb_vidyn *V(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (t && t->vidyn) return t->vidyn;
    pthread_mutex_lock(&glock);
    vkb_vidyn *v = all;
    pthread_mutex_unlock(&glock);
    return v;
}

static vkb_vidyn *Vdev(VkDevice dev)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (t && t->vidyn && t->device == dev) return t->vidyn;
    pthread_mutex_lock(&glock);
    vkb_vidyn *v = all;
    while (v && v->t->device != dev) v = v->gnext;
    if (!v) v = all;
    pthread_mutex_unlock(&glock);
    return v;
}

static uint64_t key_hash(const vkey *k)
{
    uint64_t h = 0xcbf29ce484222325ull ^ ((uint64_t)k->nb << 32 | k->na);
    const uint32_t *w = (const uint32_t *)k->b;
    for (uint32_t i = 0; i < k->nb * 4; i++) h = (h ^ w[i]) * 0x100000001b3ull;
    w = (const uint32_t *)k->a;
    for (uint32_t i = 0; i < k->na * 4; i++) h = (h ^ w[i]) * 0x100000001b3ull;
    return h;
}

static int key_eq(const vkey *x, const vkey *y)
{
    return x->nb == y->nb && x->na == y->na && !memcmp(x->b, y->b, x->nb * sizeof(x->b[0])) && !memcmp(x->a, y->a, x->na * sizeof(x->a[0]));
}

static size_t key_size(void) { return sizeof(vkey); }

static int fmt_emulated(uint32_t fmt, VkFormat f)
{
    return ((fmt & VKB_EMU_BGR10_VTX) && vkb_bgr10_rgba(f) != VK_FORMAT_UNDEFINED) ||
           ((fmt & VKB_EMU_UF11_VTX) && f == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
}

/* ---- per command buffer state (lock held) */

static cbst *cb_get(vkb_vidyn *v, VkCommandBuffer cb, int make)
{
    cbst **pp = &v->cbs[hk((uint64_t)(uintptr_t)cb)];
    for (cbst *s = *pp; s; s = s->next)
        if (s->cb == cb) return s;
    if (!make) return NULL;
    cbst *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cb = cb;
    s->next = *pp;
    *pp = s;
    return s;
}

static void cb_clear(cbst *s)
{
    s->bound = s->drv = VK_NULL_HANDLE;
    s->dirty = s->skip = s->have_vi = s->vi_emu = s->drv_vi_ok = 0;
}

static vpipe *pipe_find(vkb_vidyn *v, VkPipeline p)
{
    for (vpipe *r = v->pipes[hk((uint64_t)(uintptr_t)p)]; p && r; r = r->next)
        if (r->app == p) return r;
    return NULL;
}

/* ------------------------------------------------------------------ pipelines */

static int has_dyn(const VkPipelineDynamicStateCreateInfo *d, VkDynamicState s)
{
    for (uint32_t i = 0; d && i < d->dynamicStateCount; i++)
        if (d->pDynamicStates[i] == s) return 1;
    return 0;
}

/* A complete pipeline whose vertex input is dynamic (libraries, and pipelines linked from them,
 * are left as they are) */
static int wants(const VkGraphicsPipelineCreateInfo *ci)
{
    if (!has_dyn(ci->pDynamicState, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) return 0;
    if (ci->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) return 0;
    for (const VkBaseInStructure *n = ci->pNext; n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR || n->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)
            return 0;
        if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR &&
            (((const VkPipelineCreateFlags2CreateInfoKHR *)n)->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR))
            return 0;
    }
    return 1;
}

static vpipe *keep(vkb_vidyn *v, VkDevice dev, const VkGraphicsPipelineCreateInfo *ci)
{
    vpipe *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (!(r->kept = vkb_pk_keep(v->pk, dev, ci))) {
        free(r);
        __atomic_add_fetch(&v->unkept, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    r->dev = dev;
    pthread_mutex_init(&r->lock, NULL);
    return r;
}

static void add_pipe(vkb_vidyn *v, vpipe *r) /* lock held */
{
    unsigned h = hk((uint64_t)(uintptr_t)r->app);
    r->next = v->pipes[h];
    v->pipes[h] = r;
    v->kept++;
    v->bytes += vkb_pk_size(r->kept);
    if (v->bytes > v->peak_bytes) v->peak_bytes = v->bytes;
}

static void pipe_free(vkb_vidyn *v, vpipe *r, int destroy)
{
    pthread_mutex_lock(&v->lock);
    v->bytes -= vkb_pk_size(r->kept);
    pthread_mutex_unlock(&v->lock);
    for (variant *x = r->vars, *nx; x; x = nx) {
        nx = x->next;
        if (destroy && x->p) v->next.vkDestroyPipeline(r->dev, x->p, NULL);
        free(x->key);
        free(x);
    }
    vkb_pk_release(v->pk, r->kept);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static VKAPI_ATTR VkResult VKAPI_CALL vi_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                                const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                                VkPipeline *out)
{
    vkb_vidyn *v = Vdev(dev);
    uint32_t want = 0, base = 0;
    for (uint32_t i = 0; i < n; i++) want += wants(&ci[i]), base |= ci[i].basePipelineHandle != VK_NULL_HANDLE;
    if (!want && (v->mode == VKB_VIDYN_NATIVE || !base)) return v->next.vkCreateGraphicsPipelines(dev, cache, n, ci, a, out);
    if (v->mode == VKB_VIDYN_NATIVE) {
        VkResult r = v->next.vkCreateGraphicsPipelines(dev, cache, n, ci, a, out);
        for (uint32_t i = 0; i < n; i++) {
            if (!out[i] || !wants(&ci[i])) continue;
            vpipe *p = keep(v, dev, &ci[i]);
            if (!p) {
                VKB_ONCE(VKB_LOG_WARN, "vertex input: a pipeline could not be kept for variants (a module made elsewhere, or a create info "
                                       "that does not encode): drawn as made, emulated vertex formats not fitted");
                continue;
            }
            p->app = p->real = out[i];
            pthread_mutex_lock(&v->lock);
            add_pipe(v, p);
            pthread_mutex_unlock(&v->lock);
        }
        return r;
    }
    /* emulated: the kept ones are stand-ins, the others are made as asked in one call */
    VkGraphicsPipelineCreateInfo *c = malloc((n ? n : 1) * sizeof(*c));
    uint32_t *idx = malloc((n ? n : 1) * sizeof(*idx));
    VkPipeline *made = malloc((n ? n : 1) * sizeof(*made));
    if (!c || !idx || !made) {
        free(c), free(idx), free(made);
        for (uint32_t i = 0; i < n; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkResult res = VK_SUCCESS;
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = VK_NULL_HANDLE;
        if (wants(&ci[i])) {
            vpipe *p = keep(v, dev, &ci[i]);
            if (!p) {
                VKB_ONCE(VKB_LOG_WARN, "vertex input: a pipeline could not be kept for variants (a module made elsewhere, or a create "
                                       "info that does not encode); no dynamic vertex input in the driver: refused");
                if (res == VK_SUCCESS) res = VK_ERROR_OUT_OF_HOST_MEMORY;
                continue;
            }
            p->app = (VkPipeline)(uintptr_t)p;
            pthread_mutex_lock(&v->lock);
            add_pipe(v, p);
            pthread_mutex_unlock(&v->lock);
            out[i] = p->app;
            continue;
        }
        /* a base within the batch or a stand-in is no base (only a hint) */
        c[m] = ci[i];
        pthread_mutex_lock(&v->lock);
        int standin = pipe_find(v, c[m].basePipelineHandle) != NULL;
        pthread_mutex_unlock(&v->lock);
        if (standin || c[m].basePipelineIndex >= 0) {
            c[m].basePipelineHandle = standin ? VK_NULL_HANDLE : c[m].basePipelineHandle;
            c[m].basePipelineIndex = -1;
            if (!c[m].basePipelineHandle) c[m].flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        }
        idx[m++] = i;
    }
    if (m) {
        VkResult r = v->next.vkCreateGraphicsPipelines(dev, cache, m, c, a, made);
        for (uint32_t j = 0; j < m; j++) out[idx[j]] = made[j];
        if (r != VK_SUCCESS && (res == VK_SUCCESS || r < 0)) res = r;
    }
    free(c), free(idx), free(made);
    return res;
}

static VKAPI_ATTR void VKAPI_CALL vi_DestroyPipeline(VkDevice dev, VkPipeline p, const VkAllocationCallbacks *a)
{
    vkb_vidyn *v = Vdev(dev);
    vpipe *r = NULL;
    pthread_mutex_lock(&v->lock);
    for (vpipe **pp = &v->pipes[hk((uint64_t)(uintptr_t)p)]; p && *pp; pp = &(*pp)->next)
        if ((*pp)->app == p) {
            r = *pp;
            *pp = r->next;
            break;
        }
    pthread_mutex_unlock(&v->lock);
    if (!r) {
        v->next.vkDestroyPipeline(dev, p, a);
        return;
    }
    VkPipeline real = r->real;
    pipe_free(v, r, 1);
    if (real) v->next.vkDestroyPipeline(dev, real, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL vi_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                           VkShaderModule *out)
{
    vkb_vidyn *v = Vdev(dev);
    VkResult r = v->next.vkCreateShaderModule(dev, ci, a, out);
    if (r == VK_SUCCESS) vkb_pk_module_created(v->pk, *out, ci);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL vi_DestroyShaderModule(VkDevice dev, VkShaderModule m, const VkAllocationCallbacks *a)
{
    vkb_vidyn *v = Vdev(dev);
    vkb_pk_module_destroyed(v->pk, m);
    v->next.vkDestroyShaderModule(dev, m, a);
}

static VKAPI_ATTR void VKAPI_CALL vi_DestroyPipelineLayout(VkDevice dev, VkPipelineLayout l, const VkAllocationCallbacks *a)
{
    vkb_vidyn *v = Vdev(dev);
    if (!vkb_pk_layout_destroy(v->pk, l)) v->next.vkDestroyPipelineLayout(dev, l, a);
}

static VKAPI_ATTR void VKAPI_CALL vi_DestroyRenderPass(VkDevice dev, VkRenderPass rp, const VkAllocationCallbacks *a)
{
    vkb_vidyn *v = Vdev(dev);
    if (!vkb_pk_render_pass_destroy(v->pk, rp)) v->next.vkDestroyRenderPass(dev, rp, a);
}

/* ---- variants */

/* The variant's create info: the vertex input of the key static */
static VkResult vi_edit(VkGraphicsPipelineCreateInfo *ci, vkb_arena *arena, void *ctx)
{
    const vkey *k = ctx;
    VkPipelineVertexInputStateCreateInfo *vi = vkb_arena_alloc(arena, sizeof(*vi));
    VkVertexInputBindingDescription *bd = vkb_arena_alloc(arena, (k->nb ? k->nb : 1) * sizeof(*bd));
    VkVertexInputAttributeDescription *ad = vkb_arena_alloc(arena, (k->na ? k->na : 1) * sizeof(*ad));
    VkVertexInputBindingDivisorDescription *dd = vkb_arena_alloc(arena, (k->nb ? k->nb : 1) * sizeof(*dd));
    VkPipelineVertexInputDivisorStateCreateInfo *ds = vkb_arena_alloc(arena, sizeof(*ds));
    VkDynamicState *list = vkb_arena_alloc(arena, ((ci->pDynamicState ? ci->pDynamicState->dynamicStateCount : 0) + 1) * sizeof(*list));
    VkPipelineDynamicStateCreateInfo *dyn = vkb_arena_alloc(arena, sizeof(*dyn));
    if (!vi || !bd || !ad || !dd || !ds || !list || !dyn) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t nd = 0;
    for (uint32_t i = 0; i < k->nb; i++) {
        bd[i].binding = k->b[i].binding;
        bd[i].stride = k->b[i].stride;
        bd[i].inputRate = (VkVertexInputRate)k->b[i].rate;
        if (k->b[i].rate == VK_VERTEX_INPUT_RATE_INSTANCE && k->b[i].divisor != 1) {
            dd[nd].binding = k->b[i].binding;
            dd[nd].divisor = k->b[i].divisor;
            nd++;
        }
    }
    for (uint32_t i = 0; i < k->na; i++) {
        ad[i].location = k->a[i].location;
        ad[i].binding = k->a[i].binding;
        ad[i].format = (VkFormat)k->a[i].format;
        ad[i].offset = k->a[i].offset;
    }
    memset(ds, 0, sizeof(*ds));
    ds->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO;
    ds->vertexBindingDivisorCount = nd;
    ds->pVertexBindingDivisors = dd;
    memset(vi, 0, sizeof(*vi));
    vi->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi->pNext = nd ? ds : NULL;
    vi->vertexBindingDescriptionCount = k->nb;
    vi->pVertexBindingDescriptions = bd;
    vi->vertexAttributeDescriptionCount = k->na;
    vi->pVertexAttributeDescriptions = ad;
    ci->pVertexInputState = vi;
    /* the vertex input and its strides static */
    uint32_t nl = 0;
    for (uint32_t i = 0; ci->pDynamicState && i < ci->pDynamicState->dynamicStateCount; i++) {
        VkDynamicState s = ci->pDynamicState->pDynamicStates[i];
        if (s != VK_DYNAMIC_STATE_VERTEX_INPUT_EXT && s != VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE) list[nl++] = s;
    }
    if (nl) {
        *dyn = *ci->pDynamicState;
        dyn->dynamicStateCount = nl;
        dyn->pDynamicStates = list;
        ci->pDynamicState = dyn;
    } else {
        ci->pDynamicState = NULL;
    }
    /* made when it is needed: never "compile required" */
    ci->flags &= ~(VkPipelineCreateFlags)(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT | VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)
            ((VkPipelineCreateFlags2CreateInfoKHR *)b)->flags &=
                ~(VkPipelineCreateFlags2KHR)(VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR |
                                             VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR);
    return VK_SUCCESS;
}

/* The variant of r for this vertex input: found, or made from the kept create info
 * (VK_NULL_HANDLE: it failed, now or before) */
static VkPipeline variant_for(vkb_vidyn *v, vpipe *r, const vkey *key, uint64_t hash)
{
    pthread_mutex_lock(&r->lock);
    for (variant *x = r->vars; x; x = x->next)
        if (x->hash == hash && key_eq(x->key, key)) {
            VkPipeline p = x->p;
            pthread_mutex_unlock(&r->lock);
            return p;
        }
    VkPipeline p = VK_NULL_HANDLE;
    VkResult res = vkb_pk_make(v->pk, r->kept, vi_edit, (void *)key, &p);
    variant *x = calloc(1, sizeof(*x));
    vkey *kc = x ? malloc(key_size()) : NULL;
    if (!x || !kc) { /* not remembered: tried again at the next draw */
        free(x);
        pthread_mutex_unlock(&r->lock);
        if (p) v->next.vkDestroyPipeline(r->dev, p, NULL);
        return VK_NULL_HANDLE;
    }
    memcpy(kc, key, key_size());
    x->hash = hash, x->key = kc, x->p = res == VK_SUCCESS ? p : VK_NULL_HANDLE;
    x->next = r->vars;
    r->vars = x;
    uint32_t nv = ++r->nvars;
    pthread_mutex_unlock(&r->lock);
    if (x->p) {
        __atomic_add_fetch(&v->variants, 1, __ATOMIC_RELAXED);
        VKB_ONCE(VKB_LOG_INFO, "vertex input: first pipeline variant with vkCmdSetVertexInputEXT's vertex input static (%u binding(s), "
                               "%u attribute(s))", key->nb, key->na);
    } else {
        __atomic_add_fetch(&v->failed, 1, __ATOMIC_RELAXED);
        VKB_WARN("vertex input: a pipeline variant failed (%d; %u binding(s), %u attribute(s)); its draws are dropped", res, key->nb, key->na);
    }
    uint64_t mx = __atomic_load_n(&v->max_vars, __ATOMIC_RELAXED);
    while (nv > mx && !__atomic_compare_exchange_n(&v->max_vars, &mx, nv, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (nv == MANY_VARIANTS)
        VKB_ONCE(VKB_LOG_WARN, "vertex input: a pipeline has %u variants (one per vertex input it is drawn with); compiles go on", nv);
    return x->p;
}

/* The noted vertex input, as vkCmdSetVertexInputEXT takes it */
static void key_to_api(const vkey *k, VkVertexInputBindingDescription2EXT *bd, VkVertexInputAttributeDescription2EXT *ad)
{
    for (uint32_t i = 0; i < k->nb; i++)
        bd[i] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, k->b[i].binding,
                                                      k->b[i].stride, (VkVertexInputRate)k->b[i].rate, k->b[i].divisor};
    for (uint32_t i = 0; i < k->na; i++)
        ad[i] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, k->a[i].location,
                                                        k->a[i].binding, (VkFormat)k->a[i].format, k->a[i].offset};
}

static void send_vi(vkb_vidyn *v, VkCommandBuffer cb, const vkey *k)
{
    VkVertexInputBindingDescription2EXT bd[NB];
    VkVertexInputAttributeDescription2EXT ad[NB];
    key_to_api(k, bd, ad);
    v->next.vkCmdSetVertexInputEXT(cb, k->nb, bd, k->na, ad);
}

/* Before a draw: the pipeline that is to draw bound (0: the draw is dropped) */
static int prepare(vkb_vidyn *v, VkCommandBuffer cb)
{
    pthread_mutex_lock(&v->lock);
    cbst *s = cb_get(v, cb, 0);
    if (!s || !s->dirty) {
        int ok = !s || !s->skip;
        pthread_mutex_unlock(&v->lock);
        if (!ok) __atomic_add_fetch(&v->dropped, 1, __ATOMIC_RELAXED);
        return ok;
    }
    s->dirty = s->skip = 0;
    int native = v->mode == VKB_VIDYN_NATIVE;
    vpipe *r = s->bound ? pipe_find(v, s->bound) : NULL;
    int resend = 0, use_variant = 0;
    VkPipeline target = VK_NULL_HANDLE;
    vkey *key = NULL;
    uint64_t hash = 0;
    if (!s->bound) {
        /* a pipeline this layer does not keep: the noted vertex input for it, if it takes one */
        resend = native && s->have_vi && !s->vi_emu && !s->drv_vi_ok;
    } else if (!r) {
        s->skip = 1; /* destroyed while bound */
    } else if (native && !s->vi_emu) {
        target = r->real;
        resend = s->have_vi && !s->drv_vi_ok;
    } else {
        use_variant = 1;
        hash = s->hash;
    }
    if (resend || use_variant) {
        key = malloc(key_size());
        if (!key) s->skip = 1, resend = use_variant = 0;
        else if (s->have_vi) memcpy(key, &s->vi, key_size());
        else memset(key, 0, key_size()), hash = key_hash(key); /* no vertex input given: none (the app's mistake) */
    }
    VkPipeline drv = s->drv;
    int skip = s->skip;
    pthread_mutex_unlock(&v->lock);
    if (skip) {
        free(key);
        __atomic_add_fetch(&v->dropped, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (use_variant && !(target = variant_for(v, r, key, hash))) {
        free(key);
        pthread_mutex_lock(&v->lock);
        if ((s = cb_get(v, cb, 0))) s->skip = 1;
        pthread_mutex_unlock(&v->lock);
        __atomic_add_fetch(&v->dropped, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (target && target != drv) {
        v->next.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, target);
        __atomic_add_fetch(&v->swaps, 1, __ATOMIC_RELAXED);
    }
    if (resend) {
        send_vi(v, cb, key);
        __atomic_add_fetch(&v->resent, 1, __ATOMIC_RELAXED);
    }
    free(key);
    pthread_mutex_lock(&v->lock);
    if ((s = cb_get(v, cb, 0))) {
        if (target) s->drv = target;
        if (use_variant && target != drv) s->drv_vi_ok = 0; /* a static vertex input bound: the dynamic one is undefined */
        if (resend) s->drv_vi_ok = 1;
    }
    pthread_mutex_unlock(&v->lock);
    return 1;
}

/* ------------------------------------------------------------------ command buffers */

static VKAPI_ATTR void VKAPI_CALL vi_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_vidyn *v = V();
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) {
        v->next.vkCmdBindPipeline(cb, bp, p);
        return;
    }
    pthread_mutex_lock(&v->lock);
    int kept = pipe_find(v, p) != NULL;
    cbst *s = cb_get(v, cb, 1);
    if (s) {
        s->bound = kept ? p : VK_NULL_HANDLE;
        if (!kept) s->drv = p, s->drv_vi_ok = 0; /* its vertex input may be static */
        s->dirty = 1;
    }
    pthread_mutex_unlock(&v->lock);
    /* a kept one is bound at the draw (without a record to hold it: the app's own, native; a
     * stand-in never reaches the driver) */
    if (!kept || (!s && v->mode == VKB_VIDYN_NATIVE)) v->next.vkCmdBindPipeline(cb, bp, p);
}

static VKAPI_ATTR void VKAPI_CALL vi_CmdSetVertexInputEXT(VkCommandBuffer cb, uint32_t nb, const VkVertexInputBindingDescription2EXT *bd,
                                                         uint32_t na, const VkVertexInputAttributeDescription2EXT *ad)
{
    vkb_vidyn *v = V();
    if (nb > NB || na > NB)
        VKB_ONCE(VKB_LOG_WARN, "vertex input: vkCmdSetVertexInputEXT with %u bindings and %u attributes; the first %u of each are kept", nb, na, NB);
    pthread_mutex_lock(&v->lock);
    cbst *s = cb_get(v, cb, 1);
    int pass = v->mode == VKB_VIDYN_NATIVE;
    if (s) {
        vkey *k = &s->vi;
        memset(k, 0, sizeof(*k));
        k->nb = nb > NB ? NB : nb;
        k->na = na > NB ? NB : na;
        for (uint32_t i = 0; i < k->nb; i++) {
            k->b[i].binding = bd[i].binding;
            k->b[i].stride = bd[i].stride;
            k->b[i].rate = bd[i].inputRate;
            k->b[i].divisor = bd[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? bd[i].divisor : 1;
        }
        s->vi_emu = 0;
        for (uint32_t i = 0; i < k->na; i++) {
            k->a[i].location = ad[i].location;
            k->a[i].binding = ad[i].binding;
            k->a[i].format = ad[i].format;
            k->a[i].offset = ad[i].offset;
            s->vi_emu |= fmt_emulated(v->fmt, ad[i].format);
        }
        s->hash = key_hash(k);
        s->have_vi = 1;
        s->dirty = 1;
        pass = pass && !s->vi_emu;
        s->drv_vi_ok = pass;
    }
    pthread_mutex_unlock(&v->lock);
    if (pass) {
        v->next.vkCmdSetVertexInputEXT(cb, nb, bd, na, ad);
        __atomic_add_fetch(&v->passed, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&v->held, 1, __ATOMIC_RELAXED);
    }
}

static VKAPI_ATTR void VKAPI_CALL vi_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs,
                                                          const VkDeviceSize *off, const VkDeviceSize *size, const VkDeviceSize *st)
{
    vkb_vidyn *v = V();
    int strip = 0;
    if (st) {
        pthread_mutex_lock(&v->lock);
        cbst *s = cb_get(v, cb, 0);
        if (s && s->bound) {
            /* strides for the noted vertex input: the later call wins */
            strip = 1;
            for (uint32_t j = 0; j < n; j++)
                for (uint32_t i = 0; s->have_vi && i < s->vi.nb; i++)
                    if (s->vi.b[i].binding == first + j) s->vi.b[i].stride = (uint32_t)st[j];
            if (s->have_vi) s->hash = key_hash(&s->vi);
            s->dirty = 1;
            s->drv_vi_ok = 0;
        }
        pthread_mutex_unlock(&v->lock);
    }
    v->next.vkCmdBindVertexBuffers2(cb, first, n, bufs, off, size, strip ? NULL : st);
}

static VKAPI_ATTR VkResult VKAPI_CALL vi_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    vkb_vidyn *v = V();
    pthread_mutex_lock(&v->lock);
    cbst *s = cb_get(v, cb, 0);
    if (s) cb_clear(s);
    pthread_mutex_unlock(&v->lock);
    return v->next.vkBeginCommandBuffer(cb, bi);
}

static VKAPI_ATTR void VKAPI_CALL vi_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *sec)
{
    vkb_vidyn *v = V();
    v->next.vkCmdExecuteCommands(cb, n, sec);
    pthread_mutex_lock(&v->lock);
    cbst *s = cb_get(v, cb, 0);
    if (s) cb_clear(s); /* the primary's state is undefined after it */
    pthread_mutex_unlock(&v->lock);
}

static VKAPI_ATTR void VKAPI_CALL vi_FreeCommandBuffers(VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    vkb_vidyn *v = Vdev(dev);
    pthread_mutex_lock(&v->lock);
    for (uint32_t i = 0; i < n; i++) {
        if (!cbs[i]) continue;
        for (cbst **pp = &v->cbs[hk((uint64_t)(uintptr_t)cbs[i])]; *pp; pp = &(*pp)->next)
            if ((*pp)->cb == cbs[i]) {
                cbst *s = *pp;
                *pp = s->next;
                free(s);
                break;
            }
    }
    pthread_mutex_unlock(&v->lock);
    v->next.vkFreeCommandBuffers(dev, pool, n, cbs);
}

#define DRAW(name, params, args)                      \
    static VKAPI_ATTR void VKAPI_CALL vi_##name params \
    {                                                  \
        vkb_vidyn *v = V();                            \
        if (prepare(v, cb)) v->next.name args;         \
    }

DRAW(vkCmdDraw, (VkCommandBuffer cb, uint32_t a, uint32_t c, uint32_t d, uint32_t e), (cb, a, c, d, e))
DRAW(vkCmdDrawIndexed, (VkCommandBuffer cb, uint32_t a, uint32_t c, uint32_t d, int32_t e, uint32_t f), (cb, a, c, d, e, f))
DRAW(vkCmdDrawIndirect, (VkCommandBuffer cb, VkBuffer a, VkDeviceSize o, uint32_t n, uint32_t s), (cb, a, o, n, s))
DRAW(vkCmdDrawIndexedIndirect, (VkCommandBuffer cb, VkBuffer a, VkDeviceSize o, uint32_t n, uint32_t s), (cb, a, o, n, s))
DRAW(vkCmdDrawIndirectCount, (VkCommandBuffer cb, VkBuffer a, VkDeviceSize o, VkBuffer c, VkDeviceSize co, uint32_t m, uint32_t s),
     (cb, a, o, c, co, m, s))
DRAW(vkCmdDrawIndexedIndirectCount, (VkCommandBuffer cb, VkBuffer a, VkDeviceSize o, VkBuffer c, VkDeviceSize co, uint32_t m, uint32_t s),
     (cb, a, o, c, co, m, s))
DRAW(vkCmdDrawMultiEXT, (VkCommandBuffer cb, uint32_t n, const VkMultiDrawInfoEXT *i, uint32_t ic, uint32_t fi, uint32_t s),
     (cb, n, i, ic, fi, s))
DRAW(vkCmdDrawMultiIndexedEXT,
     (VkCommandBuffer cb, uint32_t n, const VkMultiDrawIndexedInfoEXT *i, uint32_t ic, uint32_t fi, uint32_t s, const int32_t *vo),
     (cb, n, i, ic, fi, s, vo))
DRAW(vkCmdDrawIndirectByteCountEXT, (VkCommandBuffer cb, uint32_t ic, uint32_t fi, VkBuffer cbuf, VkDeviceSize co, uint32_t off, uint32_t st),
     (cb, ic, fi, cbuf, co, off, st))

/* ------------------------------------------------------------------ install */

#define HOOK(field, fn)                                          \
    do {                                                         \
        if (t->dt.field) t->dt.field = fn;                       \
    } while (0)

void vkb_vidyn_device_init(vkb_srv_table *t, int mode)
{
    if (!mode) return;
    vkb_dispatch *dt = &t->dt;
    if (!dt->vkCreateGraphicsPipelines || !dt->vkDestroyPipeline || !dt->vkCreateShaderModule || !dt->vkDestroyShaderModule ||
        !dt->vkDestroyPipelineLayout || !dt->vkDestroyRenderPass || !dt->vkCmdBindPipeline || !dt->vkBeginCommandBuffer ||
        !dt->vkFreeCommandBuffers || !dt->vkCmdExecuteCommands || (mode == VKB_VIDYN_NATIVE && !dt->vkCmdSetVertexInputEXT)) {
        VKB_WARN("vertex input: device %u lacks an entry point the layer wraps; vkCmdSetVertexInputEXT reaches the driver as it is", t->id);
        return;
    }
    vkb_vidyn *v = calloc(1, sizeof(*v));
    if (!v) return;
    v->next = t->dt;
    v->t = t;
    v->mode = mode;
    v->fmt = t->emu ? t->emu->flags & (VKB_EMU_BGR10_VTX | VKB_EMU_UF11_VTX) : 0;
    pthread_mutex_init(&v->lock, NULL);
    if (!(v->pk = vkb_pk_create(&v->next))) {
        pthread_mutex_destroy(&v->lock);
        free(v);
        return;
    }
    HOOK(vkCreateGraphicsPipelines, vi_CreateGraphicsPipelines);
    HOOK(vkDestroyPipeline, vi_DestroyPipeline);
    HOOK(vkCreateShaderModule, vi_CreateShaderModule);
    HOOK(vkDestroyShaderModule, vi_DestroyShaderModule);
    HOOK(vkDestroyPipelineLayout, vi_DestroyPipelineLayout);
    HOOK(vkDestroyRenderPass, vi_DestroyRenderPass);
    HOOK(vkCmdBindPipeline, vi_CmdBindPipeline);
    HOOK(vkCmdBindVertexBuffers2, vi_CmdBindVertexBuffers2);
    HOOK(vkBeginCommandBuffer, vi_BeginCommandBuffer);
    HOOK(vkCmdExecuteCommands, vi_CmdExecuteCommands);
    HOOK(vkFreeCommandBuffers, vi_FreeCommandBuffers);
    HOOK(vkCmdDraw, vi_vkCmdDraw);
    HOOK(vkCmdDrawIndexed, vi_vkCmdDrawIndexed);
    HOOK(vkCmdDrawIndirect, vi_vkCmdDrawIndirect);
    HOOK(vkCmdDrawIndexedIndirect, vi_vkCmdDrawIndexedIndirect);
    HOOK(vkCmdDrawIndirectCount, vi_vkCmdDrawIndirectCount);
    HOOK(vkCmdDrawIndexedIndirectCount, vi_vkCmdDrawIndexedIndirectCount);
    HOOK(vkCmdDrawMultiEXT, vi_vkCmdDrawMultiEXT);
    HOOK(vkCmdDrawMultiIndexedEXT, vi_vkCmdDrawMultiIndexedEXT);
    HOOK(vkCmdDrawIndirectByteCountEXT, vi_vkCmdDrawIndirectByteCountEXT);
    /* emulated: the client's entry point exists even where the driver has none */
    t->dt.vkCmdSetVertexInputEXT = vi_CmdSetVertexInputEXT;
    t->vidyn = v;
    pthread_mutex_lock(&glock);
    v->gnext = all;
    all = v;
    pthread_mutex_unlock(&glock);
    VKB_INFO("device %u: vertex input dynamic state for Zink %s%s%s", t->id,
             mode == VKB_VIDYN_EMULATED ? "emulated with pipeline variants" : "the driver's, pipeline variants for emulated formats",
             (v->fmt & VKB_EMU_BGR10_VTX) ? " (BGRA 10-10-10-2 vertex fetch emulated)" : "",
             (v->fmt & VKB_EMU_UF11_VTX) ? " (B10G11R11 vertex fetch emulated)" : "");
}

/* At device destroy: the totals, then the records (the device takes the pipelines left) */
void vkb_vidyn_device_destroy(vkb_srv_table *t)
{
    vkb_vidyn *v = t->vidyn;
    if (!v) return;
    pthread_mutex_lock(&glock);
    for (vkb_vidyn **pp = &all; *pp; pp = &(*pp)->gnext)
        if (*pp == v) {
            *pp = v->gnext;
            break;
        }
    pthread_mutex_unlock(&glock);
    if (v->kept || v->unkept || v->passed || v->held)
        VKB_INFO("vertex input: %llu pipeline(s) kept (at most %llu KB of create infos and shaders at once), %llu variant(s) made (at most "
                 "%llu for one pipeline), %llu failed, %llu variant bind(s), %llu draw(s) dropped; vkCmdSetVertexInputEXT: %llu passed "
                 "to the driver, %llu held, %llu sent again; %llu pipeline(s) not kept",
                 (unsigned long long)v->kept, (unsigned long long)((v->peak_bytes + 1023) / 1024), (unsigned long long)v->variants,
                 (unsigned long long)v->max_vars, (unsigned long long)v->failed, (unsigned long long)v->swaps,
                 (unsigned long long)v->dropped, (unsigned long long)v->passed, (unsigned long long)v->held,
                 (unsigned long long)v->resent, (unsigned long long)v->unkept);
    for (unsigned h = 0; h < HB; h++) {
        for (vpipe *r = v->pipes[h], *nx; r; r = nx) nx = r->next, pipe_free(v, r, 0);
        for (cbst *s = v->cbs[h], *nx; s; s = nx) nx = s->next, free(s);
    }
    vkb_pk_destroy(v->pk);
    pthread_mutex_destroy(&v->lock);
    free(v);
    t->vidyn = NULL;
}
