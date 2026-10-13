/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VK_KHR_push_descriptor on a driver without it (Mali-G720 r44p1). vkd3d-proton refuses to start
 * without it ("Push descriptors are not supported by this implementation"), so no D3D12 game ran.
 *
 * A pushed set becomes an ordinary one: allocated from the command buffer's scratch pools
 * (server_emu_cmdstate.c, reset when the command buffer is begun again), written, and bound in its
 * place. The layouts made for pushing lose the push flag; templates made for pushing become set
 * templates of the same entries on the layout the pipeline layout names for that set. Writes and
 * template updates go through the whole dispatch table (null descriptors, set names), binds through
 * the command-state tracker the other emulations restore from. Exact, at the cost of a descriptor
 * set per push.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdlib.h>
#include <string.h>

#define MAP_BUCKETS 512

typedef struct layout_rec {
    VkPipelineLayout layout;
    uint32_t nsets;
    VkDescriptorSetLayout *sets;
    struct layout_rec *next;
} layout_rec;

typedef struct tmpl_rec {
    VkDescriptorUpdateTemplate tmpl;
    VkPipelineBindPoint bind_point;
    struct tmpl_rec *next;
} tmpl_rec;

typedef struct push_state {
    pthread_mutex_t lock;
    layout_rec *layouts[MAP_BUCKETS];
    tmpl_rec *tmpls[MAP_BUCKETS];
    PFN_vkCreateDescriptorSetLayout create_dsl;
    PFN_vkCreatePipelineLayout create_pl;
    PFN_vkDestroyPipelineLayout destroy_pl;
    PFN_vkCreateDescriptorUpdateTemplate create_tmpl;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_tmpl;
    PFN_vkCmdBindDescriptorSets bind; /* the command-state tracker's */
} push_state;

static push_state *states[4096];

static push_state *st(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < 4096 ? states[t->id] : NULL;
}

static unsigned hb(uint64_t k)
{
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    return (unsigned)(k >> 55) & (MAP_BUCKETS - 1);
}

static VkDescriptorSetLayout set_layout(push_state *s, VkPipelineLayout layout, uint32_t set)
{
    VkDescriptorSetLayout r = VK_NULL_HANDLE;
    pthread_mutex_lock(&s->lock);
    for (layout_rec *l = s->layouts[hb((uint64_t)(uintptr_t)layout)]; l; l = l->next)
        if (l->layout == layout) {
            if (set < l->nsets) r = l->sets[set];
            break;
        }
    pthread_mutex_unlock(&s->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
    push_state *s = st();
    ((VkDescriptorSetLayoutCreateInfo *)ci)->flags &= ~(VkDescriptorSetLayoutCreateFlags)VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
    return s->create_dsl(device, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci,
                                                             const VkAllocationCallbacks *a, VkPipelineLayout *out)
{
    push_state *s = st();
    VkResult r = s->create_pl(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    layout_rec *l = calloc(1, sizeof(*l));
    if (!l) return r;
    l->layout = *out;
    l->nsets = ci->setLayoutCount;
    l->sets = malloc((ci->setLayoutCount ? ci->setLayoutCount : 1) * sizeof(*l->sets));
    if (!l->sets) {
        free(l);
        return r;
    }
    if (ci->setLayoutCount) memcpy(l->sets, ci->pSetLayouts, ci->setLayoutCount * sizeof(*l->sets));
    pthread_mutex_lock(&s->lock);
    unsigned b = hb((uint64_t)(uintptr_t)*out);
    l->next = s->layouts[b];
    s->layouts[b] = l;
    pthread_mutex_unlock(&s->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyPipelineLayout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks *a)
{
    push_state *s = st();
    pthread_mutex_lock(&s->lock);
    for (layout_rec **pp = &s->layouts[hb((uint64_t)(uintptr_t)layout)]; *pp; pp = &(*pp)->next)
        if ((*pp)->layout == layout) {
            layout_rec *l = *pp;
            *pp = l->next;
            free(l->sets);
            free(l);
            break;
        }
    pthread_mutex_unlock(&s->lock);
    s->destroy_pl(device, layout, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL p_CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                                       const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out)
{
    push_state *s = st();
    VkPipelineBindPoint bp = VK_PIPELINE_BIND_POINT_GRAPHICS;
    int push = ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS;
    if (push) {
        /* The same entries on the set layout the pipeline layout names: an ordinary set template.
         * The create info is the server's copy (changed in place, as outer layers expect). */
        VkDescriptorUpdateTemplateCreateInfo *w = (VkDescriptorUpdateTemplateCreateInfo *)ci;
        VkDescriptorSetLayout dsl = set_layout(s, ci->pipelineLayout, ci->set);
        if (!dsl) {
            VKB_ERR("push descriptor emulation: template for set %u of an unknown pipeline layout", ci->set);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        bp = ci->pipelineBindPoint;
        w->templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        w->descriptorSetLayout = dsl;
    }
    VkResult r = s->create_tmpl(device, ci, a, out);
    if (r != VK_SUCCESS || !push) return r;
    tmpl_rec *t = calloc(1, sizeof(*t));
    if (!t) return r;
    t->tmpl = *out;
    t->bind_point = bp;
    pthread_mutex_lock(&s->lock);
    unsigned b = hb((uint64_t)(uintptr_t)*out);
    t->next = s->tmpls[b];
    s->tmpls[b] = t;
    pthread_mutex_unlock(&s->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL p_DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate tmpl,
                                                                    const VkAllocationCallbacks *a)
{
    push_state *s = st();
    pthread_mutex_lock(&s->lock);
    for (tmpl_rec **pp = &s->tmpls[hb((uint64_t)(uintptr_t)tmpl)]; *pp; pp = &(*pp)->next)
        if ((*pp)->tmpl == tmpl) {
            tmpl_rec *t = *pp;
            *pp = t->next;
            free(t);
            break;
        }
    pthread_mutex_unlock(&s->lock);
    s->destroy_tmpl(device, tmpl, a);
}

static VkDescriptorSet scratch(push_state *s, VkCommandBuffer cb, VkPipelineLayout layout, uint32_t set)
{
    VkDescriptorSetLayout dsl = set_layout(s, layout, set);
    VkDescriptorSet ds = dsl ? vkb_emu_cb_scratch_set(vkb_emu_cur(), cb, dsl) : VK_NULL_HANDLE;
    if (!ds) {
        static int told;
        if (!__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
            VKB_ERR("push descriptor emulation: no set for set %u of pipeline layout %p; push dropped (logged once)", set, (void *)layout);
    }
    return ds;
}

static VKAPI_ATTR void VKAPI_CALL p_CmdPushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                                                         uint32_t n, const VkWriteDescriptorSet *w)
{
    push_state *s = st();
    VkDescriptorSet ds = scratch(s, cb, layout, set);
    if (!ds) return;
    VkWriteDescriptorSet local[16], *ws = n <= 16 ? local : malloc(n * sizeof(*ws));
    if (!ws) return;
    for (uint32_t i = 0; i < n; i++) {
        ws[i] = w[i];
        ws[i].dstSet = ds;
    }
    vkb_srv_table *t = vkb_srv_current_table();
    t->dt.vkUpdateDescriptorSets(t->device, n, ws, 0, NULL);
    if (ws != local) free(ws);
    s->bind(cb, bp, layout, set, 1, &ds, 0, NULL);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdPushDescriptorSetWithTemplate(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout,
                                                                     uint32_t set, const void *data)
{
    push_state *s = st();
    VkPipelineBindPoint bp = VK_PIPELINE_BIND_POINT_GRAPHICS;
    pthread_mutex_lock(&s->lock);
    for (tmpl_rec *r = s->tmpls[hb((uint64_t)(uintptr_t)tmpl)]; r; r = r->next)
        if (r->tmpl == tmpl) {
            bp = r->bind_point;
            break;
        }
    pthread_mutex_unlock(&s->lock);
    VkDescriptorSet ds = scratch(s, cb, layout, set);
    if (!ds) return;
    vkb_srv_table *t = vkb_srv_current_table();
    t->dt.vkUpdateDescriptorSetWithTemplate(t->device, ds, tmpl, data);
    s->bind(cb, bp, layout, set, 1, &ds, 0, NULL);
}

static VKAPI_ATTR void VKAPI_CALL p_CmdPushDescriptorSet2(VkCommandBuffer cb, const VkPushDescriptorSetInfo *i)
{
    if (i->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        p_CmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, i->layout, i->set, i->descriptorWriteCount, i->pDescriptorWrites);
    if (i->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        p_CmdPushDescriptorSet(cb, VK_PIPELINE_BIND_POINT_COMPUTE, i->layout, i->set, i->descriptorWriteCount, i->pDescriptorWrites);
}

/* vkCmdPushDescriptorSetWithTemplate2 never reaches the server: the client maps it (descriptor.c). */

void vkb_emu_push_install(vkb_srv_table *dev)
{
    if (!(dev->emu->flags & VKB_EMU_PUSH_DESC) || dev->id >= 4096) return;
    push_state *s = calloc(1, sizeof(*s));
    if (!s) return;
    pthread_mutex_init(&s->lock, NULL);
    vkb_dispatch *dt = &dev->dt;
    s->create_dsl = dt->vkCreateDescriptorSetLayout;
    s->create_pl = dt->vkCreatePipelineLayout;
    s->destroy_pl = dt->vkDestroyPipelineLayout;
    s->create_tmpl = dt->vkCreateDescriptorUpdateTemplate;
    s->destroy_tmpl = dt->vkDestroyDescriptorUpdateTemplate;
    s->bind = dt->vkCmdBindDescriptorSets;
    if (!s->create_dsl || !s->create_pl || !s->destroy_pl || !s->create_tmpl || !s->destroy_tmpl || !s->bind) {
        free(s);
        return;
    }
    dt->vkCreateDescriptorSetLayout = p_CreateDescriptorSetLayout;
    dt->vkCreatePipelineLayout = p_CreatePipelineLayout;
    dt->vkDestroyPipelineLayout = p_DestroyPipelineLayout;
    dt->vkCreateDescriptorUpdateTemplate = p_CreateDescriptorUpdateTemplate;
    dt->vkDestroyDescriptorUpdateTemplate = p_DestroyDescriptorUpdateTemplate;
    dt->vkCmdPushDescriptorSet = p_CmdPushDescriptorSet;
    dt->vkCmdPushDescriptorSetWithTemplate = p_CmdPushDescriptorSetWithTemplate;
    dt->vkCmdPushDescriptorSet2 = p_CmdPushDescriptorSet2;
    states[dev->id] = s;
}

void vkb_emu_push_uninstall(vkb_srv_table *dev)
{
    if (dev->id >= 4096 || !states[dev->id]) return;
    push_state *s = states[dev->id];
    vkb_dispatch *dt = &dev->dt;
    dt->vkCreateDescriptorSetLayout = s->create_dsl;
    dt->vkCreatePipelineLayout = s->create_pl;
    dt->vkDestroyPipelineLayout = s->destroy_pl;
    dt->vkCreateDescriptorUpdateTemplate = s->create_tmpl;
    dt->vkDestroyDescriptorUpdateTemplate = s->destroy_tmpl;
    dt->vkCmdPushDescriptorSet = dev->real.vkCmdPushDescriptorSet;
    dt->vkCmdPushDescriptorSetWithTemplate = dev->real.vkCmdPushDescriptorSetWithTemplate;
    dt->vkCmdPushDescriptorSet2 = dev->real.vkCmdPushDescriptorSet2;
    for (int i = 0; i < MAP_BUCKETS; i++) {
        while (s->layouts[i]) {
            layout_rec *l = s->layouts[i];
            s->layouts[i] = l->next;
            free(l->sets);
            free(l);
        }
        while (s->tmpls[i]) {
            tmpl_rec *t = s->tmpls[i];
            s->tmpls[i] = t->next;
            free(t);
        }
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
    states[dev->id] = NULL;
}
