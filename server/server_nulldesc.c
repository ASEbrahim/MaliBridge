/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Null descriptors for GPUs without VK_EXT_robustness2 (Mali r44).
 *
 * The bridge reports robustness2 with nullDescriptor on such GPUs (VKB_EMU_ROBUSTNESS2: DXVK and
 * Zink insist on it), and Zink then writes VK_NULL_HANDLE for unused image views, buffers and
 * texel buffer views, and binds null vertex buffers. Mali dereferences them (tablet run 10: a
 * null read at 0x28 inside its vkUpdateDescriptorSetWithTemplate, as Steam started). Here the
 * nulls become small dummy resources: a 1x1 image view, a buffer, a texel buffer view. The buffer
 * (and the texel view on it) is zero-filled where it can be mapped, so null buffer reads return
 * zeros as the extension promises; reads of the image view return whatever it holds (zeros once
 * cleared, not guaranteed). Nothing crashes. Zero-filling the stand-in buffer follows the Mali
 * compatibility layer of David Cimo and The412Banner (DroidDeck, GPL-3.0) and Lee Gao's
 * compat_layer (MIT).
 *
 * The request's own arrays are patched (they live in the server's receive buffer/arena).
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdlib.h>
#include <string.h>

typedef struct nd_template {
    VkDescriptorUpdateTemplate tmpl;
    uint32_t count;
    VkDescriptorUpdateTemplateEntry *entries;
    struct nd_template *next;
} nd_template;

typedef struct vkb_nulldesc {
    vkb_srv_table *t;
    pthread_mutex_t lock;
    PFN_vkUpdateDescriptorSets update;
    PFN_vkUpdateDescriptorSetWithTemplate update_template;
    PFN_vkCreateDescriptorUpdateTemplate create_template;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_template;
    PFN_vkCmdBindVertexBuffers bind_vb;
    PFN_vkCmdBindVertexBuffers2 bind_vb2;
    nd_template *templates;
    int made, failed;
    VkImage image;
    VkDeviceMemory image_mem, buffer_mem;
    VkImageView view;
    VkBuffer buffer;
    VkBufferView texel;
    unsigned long replaced;
} vkb_nulldesc;

static vkb_nulldesc *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->nulldesc : NULL;
}

/* A memory type the resource accepts, never lazily allocated (Mali's only GPU-only type is):
 * device-local first, and with `host` host-visible first (device-local and host-visible, then
 * host-visible, then device-local). */
static uint32_t mem_type(vkb_srv_table *t, uint32_t bits, int host)
{
    VkPhysicalDeviceMemoryProperties mp;
    t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    static const VkMemoryPropertyFlags want[2][3] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0},
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
    };
    for (int pass = 0; pass < 4; pass++) {
        VkMemoryPropertyFlags w = pass < 3 ? want[host != 0][pass] : 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) continue;
            if ((f & w) != w) continue;
            return i;
        }
    }
    return UINT32_MAX;
}

/* Fills a host-visible allocation with zeros; 0 when the memory cannot be mapped. */
static int zero_fill(vkb_srv_table *t, VkDeviceMemory mem, uint32_t type, VkDeviceSize size)
{
    VkPhysicalDeviceMemoryProperties mp;
    t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    VkMemoryPropertyFlags f = mp.memoryTypes[type].propertyFlags;
    void *p = NULL;
    if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || t->real.vkMapMemory(t->device, mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || !p)
        return 0;
    memset(p, 0, (size_t)size);
    if (!(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};
        t->real.vkFlushMappedMemoryRanges(t->device, 1, &r);
    }
    t->real.vkUnmapMemory(t->device, mem);
    return 1;
}

/* The dummies, made on first need (under the lock). 0 when they could not be. */
static int make_dummies(vkb_nulldesc *n)
{
    if (n->made) return !n->failed;
    n->made = 1;
    vkb_srv_table *t = n->t;
    VkDevice d = t->device;
    const vkb_dispatch *r = &t->real;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements req;
    uint32_t type;
    if (r->vkCreateImage(d, &ici, NULL, &n->image) != VK_SUCCESS) goto fail;
    r->vkGetImageMemoryRequirements(d, n->image, &req);
    if ((type = mem_type(t, req.memoryTypeBits, 0)) == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (r->vkAllocateMemory(d, &ai, NULL, &n->image_mem) != VK_SUCCESS || r->vkBindImageMemory(d, n->image, n->image_mem, 0) != VK_SUCCESS)
        goto fail;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, n->image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                 {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (r->vkCreateImageView(d, &vci, NULL, &n->view) != VK_SUCCESS) goto fail;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (r->vkCreateBuffer(d, &bci, NULL, &n->buffer) != VK_SUCCESS) goto fail;
    r->vkGetBufferMemoryRequirements(d, n->buffer, &req);
    if ((type = mem_type(t, req.memoryTypeBits, 1)) == UINT32_MAX) goto fail;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (r->vkAllocateMemory(d, &ai, NULL, &n->buffer_mem) != VK_SUCCESS || r->vkBindBufferMemory(d, n->buffer, n->buffer_mem, 0) != VK_SUCCESS)
        goto fail;
    int zeroed = zero_fill(t, n->buffer_mem, type, req.size);
    VkBufferViewCreateInfo bv = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, n->buffer, VK_FORMAT_R32_UINT, 0, VK_WHOLE_SIZE};
    if (r->vkCreateBufferView(d, &bv, NULL, &n->texel) != VK_SUCCESS) n->texel = VK_NULL_HANDLE; /* texel nulls stay */
    VKB_INFO("device %u: null descriptors emulated (dummy image view, buffer%s%s)", t->id, n->texel ? ", texel buffer view" : "",
             zeroed ? "; buffer zero-filled" : "; buffer not mappable, contents undefined");
    return 1;
fail:
    n->failed = 1;
    VKB_ERR("device %u: null descriptor dummies could not be made; null descriptors go to the driver as they are", t->id);
    return 0;
}

static int image_type(VkDescriptorType ty)
{
    return ty == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || ty == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           ty == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

static int buffer_type(VkDescriptorType ty)
{
    return ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static int texel_type(VkDescriptorType ty)
{
    return ty == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || ty == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

/* Fixes one descriptor (of type ty) at p; 1 when it was null. Dummies must exist. */
static int fix_one(vkb_nulldesc *n, VkDescriptorType ty, void *p)
{
    if (image_type(ty)) {
        VkDescriptorImageInfo *ii = p;
        if (ii->imageView) return 0;
        ii->imageView = n->view;
        ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        return 1;
    }
    if (buffer_type(ty)) {
        VkDescriptorBufferInfo *bi = p;
        if (bi->buffer) return 0;
        bi->buffer = n->buffer;
        bi->offset = 0;
        bi->range = VK_WHOLE_SIZE;
        return 1;
    }
    if (texel_type(ty) && n->texel) {
        VkBufferView *bv = p;
        if (*bv) return 0;
        *bv = n->texel;
        return 1;
    }
    return 0;
}

static int need_dummies(vkb_nulldesc *n)
{
    pthread_mutex_lock(&n->lock);
    int ok = make_dummies(n);
    pthread_mutex_unlock(&n->lock);
    return ok;
}

static void count(vkb_nulldesc *n, unsigned k)
{
    if (!k) return;
    unsigned long before = __atomic_fetch_add(&n->replaced, k, __ATOMIC_RELAXED);
    if (!before) VKB_INFO("device %u: first null descriptors replaced by dummies", n->t->id);
}

static void VKAPI_CALL nd_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                               const VkCopyDescriptorSet *cp)
{
    vkb_nulldesc *n = cur();
    unsigned k = 0;
    int ready = -1;
    for (uint32_t i = 0; i < nw; i++) {
        VkDescriptorType ty = w[i].descriptorType;
        for (uint32_t j = 0; j < w[i].descriptorCount; j++) {
            void *p = NULL;
            int null = 0;
            if (image_type(ty) && w[i].pImageInfo) {
                p = (void *)&w[i].pImageInfo[j];
                null = !((VkDescriptorImageInfo *)p)->imageView;
            } else if (buffer_type(ty) && w[i].pBufferInfo) {
                p = (void *)&w[i].pBufferInfo[j];
                null = !((VkDescriptorBufferInfo *)p)->buffer;
            } else if (texel_type(ty) && w[i].pTexelBufferView) {
                p = (void *)&w[i].pTexelBufferView[j];
                null = !*(VkBufferView *)p;
            }
            if (!null) continue;
            if (ready < 0) ready = need_dummies(n);
            if (ready) k += (unsigned)fix_one(n, ty, p);
        }
    }
    count(n, k);
    n->update(device, nw, w, nc, cp);
}

static nd_template *find_template(vkb_nulldesc *n, VkDescriptorUpdateTemplate tmpl)
{
    for (nd_template *t = n->templates; t; t = t->next)
        if (t->tmpl == tmpl) return t;
    return NULL;
}

static VkResult VKAPI_CALL nd_CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                             const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out)
{
    vkb_nulldesc *n = cur();
    VkResult r = n->create_template(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    nd_template *t = calloc(1, sizeof(*t));
    if (!t) return r;
    t->entries = malloc((size_t)ci->descriptorUpdateEntryCount * sizeof(*t->entries) + 1);
    if (!t->entries) {
        free(t);
        return r;
    }
    memcpy(t->entries, ci->pDescriptorUpdateEntries, (size_t)ci->descriptorUpdateEntryCount * sizeof(*t->entries));
    t->count = ci->descriptorUpdateEntryCount;
    t->tmpl = *out;
    pthread_mutex_lock(&n->lock);
    t->next = n->templates;
    n->templates = t;
    pthread_mutex_unlock(&n->lock);
    return r;
}

static void VKAPI_CALL nd_DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate tmpl, const VkAllocationCallbacks *a)
{
    vkb_nulldesc *n = cur();
    pthread_mutex_lock(&n->lock);
    for (nd_template **pp = &n->templates; *pp; pp = &(*pp)->next)
        if ((*pp)->tmpl == tmpl) {
            nd_template *t = *pp;
            *pp = t->next;
            free(t->entries);
            free(t);
            break;
        }
    pthread_mutex_unlock(&n->lock);
    n->destroy_template(device, tmpl, a);
}

static void VKAPI_CALL nd_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
                                                          const void *data)
{
    vkb_nulldesc *n = cur();
    unsigned k = 0;
    int ready = -1;
    pthread_mutex_lock(&n->lock);
    nd_template *t = find_template(n, tmpl);
    for (uint32_t i = 0; t && data && i < t->count; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
        VkDescriptorType ty = e->descriptorType;
        if (!image_type(ty) && !buffer_type(ty) && !texel_type(ty)) continue;
        for (uint32_t j = 0; j < e->descriptorCount; j++) {
            uint8_t *p = (uint8_t *)data + e->offset + (size_t)j * e->stride;
            int null = image_type(ty) ? !((VkDescriptorImageInfo *)p)->imageView
                     : buffer_type(ty) ? !((VkDescriptorBufferInfo *)p)->buffer
                                       : !*(VkBufferView *)p;
            if (!null) continue;
            if (ready < 0) ready = make_dummies(n); /* the lock is held */
            if (ready) k += (unsigned)fix_one(n, ty, p);
        }
    }
    pthread_mutex_unlock(&n->lock);
    count(n, k);
    n->update_template(device, set, tmpl, data);
}

static void VKAPI_CALL nd_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count_, const VkBuffer *bufs,
                                               const VkDeviceSize *offs)
{
    vkb_nulldesc *n = cur();
    for (uint32_t i = 0; i < count_; i++)
        if (!bufs[i] && need_dummies(n)) {
            ((VkBuffer *)bufs)[i] = n->buffer;
            ((VkDeviceSize *)offs)[i] = 0;
        }
    n->bind_vb(cb, first, count_, bufs, offs);
}

static void VKAPI_CALL nd_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t count_, const VkBuffer *bufs,
                                                const VkDeviceSize *offs, const VkDeviceSize *sizes, const VkDeviceSize *strides)
{
    vkb_nulldesc *n = cur();
    for (uint32_t i = 0; i < count_; i++)
        if (!bufs[i] && need_dummies(n)) {
            ((VkBuffer *)bufs)[i] = n->buffer;
            ((VkDeviceSize *)offs)[i] = 0;
            if (sizes) ((VkDeviceSize *)sizes)[i] = VK_WHOLE_SIZE;
        }
    n->bind_vb2(cb, first, count_, bufs, offs, sizes, strides);
}

void vkb_nulldesc_device_init(vkb_srv_table *t)
{
    if (!t->emu || !(t->emu->flags & VKB_EMU_ROBUSTNESS2) || vkb_srv_env("VKBRIDGE_NO_NULLDESC")) return;
    vkb_dispatch *dt = &t->dt;
    if (!dt->vkUpdateDescriptorSets) return;
    vkb_nulldesc *n = calloc(1, sizeof(*n));
    if (!n) return;
    n->t = t;
    pthread_mutex_init(&n->lock, NULL);
    n->update = dt->vkUpdateDescriptorSets;
    n->update_template = dt->vkUpdateDescriptorSetWithTemplate;
    n->create_template = dt->vkCreateDescriptorUpdateTemplate;
    n->destroy_template = dt->vkDestroyDescriptorUpdateTemplate;
    n->bind_vb = dt->vkCmdBindVertexBuffers;
    n->bind_vb2 = dt->vkCmdBindVertexBuffers2;
    dt->vkUpdateDescriptorSets = nd_UpdateDescriptorSets;
    if (n->update_template && n->create_template && n->destroy_template) {
        dt->vkUpdateDescriptorSetWithTemplate = nd_UpdateDescriptorSetWithTemplate;
        dt->vkCreateDescriptorUpdateTemplate = nd_CreateDescriptorUpdateTemplate;
        dt->vkDestroyDescriptorUpdateTemplate = nd_DestroyDescriptorUpdateTemplate;
    }
    if (n->bind_vb) dt->vkCmdBindVertexBuffers = nd_CmdBindVertexBuffers;
    if (n->bind_vb2) dt->vkCmdBindVertexBuffers2 = nd_CmdBindVertexBuffers2;
    t->nulldesc = n;
}

void vkb_nulldesc_device_destroy(vkb_srv_table *t)
{
    vkb_nulldesc *n = t->nulldesc;
    if (!n) return;
    vkb_dispatch *dt = &t->dt;
    dt->vkUpdateDescriptorSets = n->update;
    if (dt->vkUpdateDescriptorSetWithTemplate == nd_UpdateDescriptorSetWithTemplate) {
        dt->vkUpdateDescriptorSetWithTemplate = n->update_template;
        dt->vkCreateDescriptorUpdateTemplate = n->create_template;
        dt->vkDestroyDescriptorUpdateTemplate = n->destroy_template;
    }
    if (n->bind_vb) dt->vkCmdBindVertexBuffers = n->bind_vb;
    if (n->bind_vb2) dt->vkCmdBindVertexBuffers2 = n->bind_vb2;
    const vkb_dispatch *r = &t->real;
    VkDevice d = t->device;
    if (n->texel) r->vkDestroyBufferView(d, n->texel, NULL);
    if (n->buffer) r->vkDestroyBuffer(d, n->buffer, NULL);
    if (n->buffer_mem) r->vkFreeMemory(d, n->buffer_mem, NULL);
    if (n->view) r->vkDestroyImageView(d, n->view, NULL);
    if (n->image) r->vkDestroyImage(d, n->image, NULL);
    if (n->image_mem) r->vkFreeMemory(d, n->image_mem, NULL);
    if (n->replaced) VKB_INFO("device %u: %lu null descriptors replaced", t->id, n->replaced);
    while (n->templates) {
        nd_template *x = n->templates;
        n->templates = x->next;
        free(x->entries);
        free(x);
    }
    pthread_mutex_destroy(&n->lock);
    free(n);
    t->nulldesc = NULL;
}
