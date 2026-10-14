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
 * Push descriptor writes are fixed the same way (vkCmdPushDescriptorSet, -2, -WithTemplate), for
 * drivers that have VK_KHR_push_descriptor but not robustness2; emulated push descriptors reach
 * vkUpdateDescriptorSets and are fixed there. Lee Gao's compat_layer (MIT) and the DroidDeck Mali
 * compatibility layer (GPL-3.0) cover push descriptors likewise.
 *
 * Image stand-ins have the shape the shader reads them as: a shader declaring a cube, a 3D, a 1D,
 * an arrayed or an integer image and given the 2D RGBA8 view reads through a view of the wrong
 * type or numeric class, which Vulkan leaves undefined (a cube array read as a 2D view has no
 * whole cube: its size is 0 cubes, even on RADV). Pipelines tell which: each module's SPIR-V is
 * read for the dimension, arrayedness and sampled type of the images at each set and binding,
 * and a pipeline's creation notes them on the set layouts of its pipeline layout. A null image
 * then gets the stand-in of its binding's shape - 1D, 2D, 3D, cube, 1D/2D/cube array; float
 * (RGBA8 UNORM), uint (R32_UINT) or sint (R32_SINT) - made on first need; the 2D RGBA8 one where
 * nothing is known (no pipeline seen yet, multisampled images) or pipelines disagree on a
 * binding. Descriptor sets are mapped to their layouts as they are allocated, so this layer sits
 * beneath the client's set names (server_dsmap.c) and sees the driver's sets. The idea, a binding
 * hint per set layout from SPIR-V reflection of the pipelines' modules, follows Lee Gao's
 * compat_layer (MIT: null_descriptors.cpp, shader_reflector.cpp); VKBRIDGE_NULLDESC_2D=1 gives
 * every null image the 2D stand-in as before.
 *
 * The request's own arrays are patched (they live in the server's receive buffer/arena).
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

typedef struct nd_template {
    VkDescriptorUpdateTemplate tmpl;
    VkDescriptorSetLayout layout; /* the set layout it writes (push templates: of its pipeline layout's set) */
    uint32_t count;
    VkDescriptorUpdateTemplateEntry *entries;
    struct nd_template *next;
} nd_template;

/* Image stand-in shapes: a view type (VkImageViewType, 1D..cube array) and a numeric class.
 * Kinds number them from 1; 0 is "not known", KIND_MIXED "pipelines disagree". */
enum { CLS_FLOAT, CLS_UINT, CLS_SINT, CLASSES };
#define VIEW_TYPES 7
#define KINDS (VIEW_TYPES * CLASSES)
#define KIND(vt, cls) ((uint8_t)(1 + (vt) * CLASSES + (cls)))
#define KIND_MIXED 0xff
static const VkFormat class_format[CLASSES] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
static const char *const class_name[CLASSES] = {"float", "uint", "sint"};
static const char *const view_name[VIEW_TYPES] = {"1D", "2D", "3D", "cube", "1D array", "2D array", "cube array"};

typedef struct nd_image {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    int tried;
} nd_image;

/* Small hash maps of handles: modules, pipeline layouts and set layouts to what is known of
 * them (malloc'd), descriptor sets to their layouts. */
typedef struct nd_ent {
    uint64_t key, val;
    struct nd_ent *next;
} nd_ent;

typedef struct nd_map {
    nd_ent **b;
    uint32_t mask, count;
} nd_map;

/* A module's image bindings and the kind each wants; a set layout's bindings and theirs. */
typedef struct nd_use {
    uint32_t set, binding;
    uint8_t kind;
} nd_use;

typedef struct nd_uses {
    uint32_t n, cap;
    nd_use u[];
} nd_uses;

typedef struct nd_layout {
    uint32_t n;
    VkDescriptorSetLayout set[];
} nd_layout;

/* Sets are not forgotten when freed (a driver handle allocated again is noted again); past this
 * many the map starts over. */
#define MAX_SETS_MAPPED (1u << 20)

typedef struct vkb_nulldesc {
    vkb_srv_table *t;
    pthread_mutex_t lock;
    PFN_vkUpdateDescriptorSets update;
    PFN_vkUpdateDescriptorSetWithTemplate update_template;
    PFN_vkCreateDescriptorUpdateTemplate create_template;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_template;
    PFN_vkCmdBindVertexBuffers bind_vb;
    PFN_vkCmdBindVertexBuffers2 bind_vb2;
    PFN_vkCmdPushDescriptorSet push;
    PFN_vkCmdPushDescriptorSet2 push2;
    PFN_vkCmdPushDescriptorSetWithTemplate push_template;
    /* precise stand-ins */
    int precise;
    PFN_vkCreateShaderModule create_module;
    PFN_vkDestroyShaderModule destroy_module;
    PFN_vkCreatePipelineLayout create_layout;
    PFN_vkDestroyPipelineLayout destroy_layout;
    PFN_vkDestroyDescriptorSetLayout destroy_set_layout;
    PFN_vkCreateGraphicsPipelines create_gfx;
    PFN_vkCreateComputePipelines create_compute;
    PFN_vkAllocateDescriptorSets allocate;
    nd_map modules, layouts, hints, sets;
    unsigned long shaped, mixed;
    nd_template *templates;
    int made, failed;
    nd_image shape[KINDS]; /* KIND(2D, float) is the default stand-in, made with the buffer */
    VkDeviceMemory buffer_mem;
    VkImageView view;      /* the default stand-in's view */
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

static void free_image(vkb_srv_table *t, nd_image *s)
{
    if (s->view) t->real.vkDestroyImageView(t->device, s->view, NULL);
    if (s->image) t->real.vkDestroyImage(t->device, s->image, NULL);
    if (s->mem) t->real.vkFreeMemory(t->device, s->mem, NULL);
    s->view = VK_NULL_HANDLE;
    s->image = VK_NULL_HANDLE;
    s->mem = VK_NULL_HANDLE;
}

/* A 1x1 (cubes: 6 faces) sampled and storage image of a kind, and its view; 0 when it could not
 * be made. Cube arrays need imageCubeArray, which a shader declaring one needs too. */
static int make_image(vkb_nulldesc *n, uint8_t kind, nd_image *s)
{
    vkb_srv_table *t = n->t;
    VkDevice d = t->device;
    const vkb_dispatch *r = &t->real;
    VkImageViewType vt = (VkImageViewType)((kind - 1) / CLASSES);
    VkFormat fmt = class_format[(kind - 1) % CLASSES];
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageType it = vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D
                   : vt == VK_IMAGE_VIEW_TYPE_3D                                     ? VK_IMAGE_TYPE_3D
                                                                                     : VK_IMAGE_TYPE_2D;
    uint32_t layers = cube ? 6 : 1;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, it, fmt, {1, 1, 1},
                             1, layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements req;
    uint32_t type;
    if (r->vkCreateImage(d, &ici, NULL, &s->image) != VK_SUCCESS) goto fail;
    r->vkGetImageMemoryRequirements(d, s->image, &req);
    if ((type = mem_type(t, req.memoryTypeBits, 0)) == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (r->vkAllocateMemory(d, &ai, NULL, &s->mem) != VK_SUCCESS || r->vkBindImageMemory(d, s->image, s->mem, 0) != VK_SUCCESS) goto fail;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, s->image, vt, fmt, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    if (r->vkCreateImageView(d, &vci, NULL, &s->view) != VK_SUCCESS) goto fail;
    return 1;
fail:
    s->view = VK_NULL_HANDLE; /* not made: nothing to destroy */
    free_image(t, s);
    return 0;
}

/* The dummies, made on first need (under the lock). 0 when they could not be. */
static int make_dummies(vkb_nulldesc *n)
{
    if (n->made) return !n->failed;
    n->made = 1;
    vkb_srv_table *t = n->t;
    VkDevice d = t->device;
    const vkb_dispatch *r = &t->real;
    VkMemoryRequirements req;
    uint32_t type;
    nd_image *def = &n->shape[KIND(VK_IMAGE_VIEW_TYPE_2D, CLS_FLOAT) - 1];
    def->tried = 1;
    if (!make_image(n, KIND(VK_IMAGE_VIEW_TYPE_2D, CLS_FLOAT), def)) goto fail;
    n->view = def->view;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
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

/* The stand-in view of a kind, made on first need (under the lock, dummies made); the default
 * one when the kind is not known or its image could not be made. */
static VkImageView standin(vkb_nulldesc *n, uint8_t kind)
{
    if (!kind || kind == KIND_MIXED) return n->view;
    nd_image *s = &n->shape[kind - 1];
    if (!s->tried) {
        s->tried = 1;
        const char *vt = view_name[(kind - 1) / CLASSES], *cls = class_name[(kind - 1) % CLASSES];
        if (make_image(n, kind, s)) VKB_INFO("device %u: null descriptor stand-in made: %s, %s", n->t->id, vt, cls);
        else VKB_WARN("device %u: null descriptor stand-in %s, %s could not be made; the 2D one is used", n->t->id, vt, cls);
    }
    if (!s->view) return n->view;
    n->shaped++;
    return s->view;
}

/* ------------------------------------------------------------------ handle maps */

static unsigned slot(const nd_map *m, uint64_t key)
{
    key *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(key >> 40) & m->mask;
}

static nd_ent *map_find(const nd_map *m, uint64_t key)
{
    if (!m->b) return NULL;
    nd_ent *e = m->b[slot(m, key)];
    while (e && e->key != key) e = e->next;
    return e;
}

/* The entry of key, made (val 0) when missing; NULL out of memory. */
static nd_ent *map_get(nd_map *m, uint64_t key, uint32_t buckets)
{
    if (!m->b) {
        if (!(m->b = calloc(buckets, sizeof(*m->b)))) return NULL;
        m->mask = buckets - 1;
    }
    nd_ent *e = map_find(m, key);
    if (e) return e;
    if (!(e = calloc(1, sizeof(*e)))) return NULL;
    e->key = key;
    unsigned s = slot(m, key);
    e->next = m->b[s];
    m->b[s] = e;
    m->count++;
    return e;
}

/* Removes key; returns its value (0 when missing). */
static uint64_t map_take(nd_map *m, uint64_t key)
{
    if (!m->b) return 0;
    for (nd_ent **pp = &m->b[slot(m, key)]; *pp; pp = &(*pp)->next)
        if ((*pp)->key == key) {
            nd_ent *e = *pp;
            uint64_t v = e->val;
            *pp = e->next;
            free(e);
            m->count--;
            return v;
        }
    return 0;
}

/* Empties the map; vals are freed when they are pointers. */
static void map_clear(nd_map *m, int vals)
{
    for (uint32_t i = 0; m->b && i <= m->mask; i++)
        while (m->b[i]) {
            nd_ent *e = m->b[i];
            m->b[i] = e->next;
            if (vals) free((void *)(uintptr_t)e->val);
            free(e);
        }
    m->count = 0;
}

static void map_free(nd_map *m, int vals)
{
    map_clear(m, vals);
    free(m->b);
    m->b = NULL;
}

/* ------------------------------------------------------------------ SPIR-V */

#define OP_TYPE_IMAGE 25
#define OP_TYPE_SAMPLED_IMAGE 27
#define DECORATION_BINDING 33
#define DECORATION_DESCRIPTOR_SET 34
#define STORAGE_UNIFORM_CONSTANT 0

enum { T_NONE, T_FLOAT, T_INT, T_IMAGE, T_SAMPLED, T_ARRAY, T_PTR };

typedef struct spv_id {
    uint8_t what, a, b; /* T_INT: a signed, b wider than 32; T_IMAGE: a Dim, b arrayed | multisampled << 1 */
    uint32_t ref;       /* sampled type, image type, element type, pointee */
    uint32_t set, binding; /* decorations + 1 (0: none) */
} spv_id;

/* Adds a use (or a binding's kind, set ignored) to a list; a different kind for the same binding
 * makes it KIND_MIXED. Returns the list (moved, or NULL out of memory: the old one stays). */
static nd_uses *add_use(nd_uses *l, uint32_t set, uint32_t binding, uint8_t kind, unsigned long *mixed)
{
    for (uint32_t i = 0; l && i < l->n; i++)
        if (l->u[i].set == set && l->u[i].binding == binding) {
            if (l->u[i].kind != kind && l->u[i].kind != KIND_MIXED) {
                l->u[i].kind = KIND_MIXED;
                if (mixed) (*mixed)++;
            }
            return l;
        }
    if (!l || l->n == l->cap) {
        uint32_t cap = l ? l->cap * 2 : 8;
        nd_uses *g = realloc(l, sizeof(*g) + cap * sizeof(g->u[0]));
        if (!g) return NULL;
        if (!l) g->n = 0;
        g->cap = cap;
        l = g;
    }
    l->u[l->n++] = (nd_use){set, binding, kind};
    return l;
}

/* The kind of a UniformConstant variable of type ty (a pointer to an image, a sampled image or an
 * array of them); 0 when it is none of those or its shape has no stand-in (buffer and subpass
 * images, multisampled ones, 64-bit integers). */
static uint8_t var_kind(const spv_id *id, uint32_t bound, uint32_t ty)
{
    for (int k = 0; k < 8 && ty < bound && id[ty].what != T_IMAGE; k++) ty = id[ty].ref;
    if (ty >= bound || id[ty].what != T_IMAGE || (id[ty].b & 2)) return 0;
    /* Dim: 1D 2D 3D Cube Rect Buffer SubpassData; not arrayed, arrayed */
    static const int8_t view[2][8] = {
        {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_2D, -1, -1, -1},
        {VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, -1, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, -1, -1, -1, -1}};
    int vt = view[id[ty].b & 1][id[ty].a];
    uint32_t st = id[ty].ref;
    if (vt < 0 || st >= bound) return 0;
    if (id[st].what == T_FLOAT) return KIND(vt, CLS_FLOAT);
    if (id[st].what == T_INT && !id[st].b) return KIND(vt, id[st].a ? CLS_SINT : CLS_UINT);
    return 0;
}

/* The images a module declares (with a set and a binding) and the stand-in kind of each; NULL
 * when none (or the code is not SPIR-V). */
static nd_uses *reflect(const uint32_t *w, size_t bytes)
{
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return NULL;
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return NULL;
    spv_id *id = calloc(bound, sizeof(*id));
    if (!id) return NULL;
    nd_uses *l = NULL;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n || op == SpvOpFunction) break; /* declarations all come first */
        const uint32_t *o = &w[i + 1];
        if (len < 3 || o[0] >= bound) continue;
        spv_id *r = &id[o[0]];
        switch (op) {
        case SpvOpDecorate:
            if (len >= 4 && o[1] == DECORATION_DESCRIPTOR_SET) r->set = o[2] + 1;
            if (len >= 4 && o[1] == DECORATION_BINDING) r->binding = o[2] + 1;
            break;
        case SpvOpTypeFloat:
            r->what = T_FLOAT;
            break;
        case SpvOpTypeInt:
            if (len >= 4) *r = (spv_id){T_INT, o[2] != 0, o[1] > 32};
            break;
        case OP_TYPE_IMAGE:
            if (len >= 9) *r = (spv_id){T_IMAGE, (uint8_t)(o[2] < 7 ? o[2] : 7), (uint8_t)((o[4] != 0) | (o[5] != 0) << 1), o[1]};
            break;
        case OP_TYPE_SAMPLED_IMAGE:
            *r = (spv_id){T_SAMPLED, 0, 0, o[1]};
            break;
        case SpvOpTypeArray:
        case SpvOpTypeRuntimeArray:
            *r = (spv_id){T_ARRAY, 0, 0, o[1]};
            break;
        case SpvOpTypePointer:
            if (len >= 4) *r = (spv_id){T_PTR, 0, 0, o[2]};
            break;
        case SpvOpVariable: { /* result type, result, storage class */
            if (len < 4 || o[2] != STORAGE_UNIFORM_CONSTANT || o[1] >= bound) break;
            const spv_id *v = &id[o[1]];
            uint8_t kind = v->set && v->binding ? var_kind(id, bound, o[0]) : 0;
            nd_uses *g = kind ? add_use(l, v->set - 1, v->binding - 1, kind, NULL) : NULL;
            if (g) l = g;
            break;
        }
        }
    }
    free(id);
    return l;
}

/* ------------------------------------------------------------------ what pipelines tell */

/* The set layout a descriptor set was allocated with (the lock is held). */
static VkDescriptorSetLayout set_layout(vkb_nulldesc *n, VkDescriptorSet set)
{
    nd_ent *e = map_find(&n->sets, (uint64_t)(uintptr_t)set);
    return e ? (VkDescriptorSetLayout)(uintptr_t)e->val : VK_NULL_HANDLE;
}

/* Set `set` of a pipeline layout (the lock is held). */
static VkDescriptorSetLayout layout_set(vkb_nulldesc *n, VkPipelineLayout pl, uint32_t set)
{
    nd_ent *e = map_find(&n->layouts, (uint64_t)(uintptr_t)pl);
    nd_layout *l = e ? (nd_layout *)(uintptr_t)e->val : NULL;
    return l && set < l->n ? l->set[set] : VK_NULL_HANDLE;
}

/* The kind a binding of a set layout wants (the lock is held); 0 when not known. */
static uint8_t binding_kind(vkb_nulldesc *n, VkDescriptorSetLayout dsl, uint32_t binding)
{
    if (!n->precise || !dsl) return 0;
    nd_ent *e = map_find(&n->hints, (uint64_t)(uintptr_t)dsl);
    nd_uses *h = e ? (nd_uses *)(uintptr_t)e->val : NULL;
    for (uint32_t i = 0; h && i < h->n; i++)
        if (h->u[i].binding == binding) return h->u[i].kind;
    return 0;
}

/* Notes a pipeline's stages on its layout's set layouts. */
static void learn(vkb_nulldesc *n, VkPipelineLayout pl, uint32_t count, const VkPipelineShaderStageCreateInfo *st)
{
    for (uint32_t s = 0; pl && st && s < count; s++) {
        nd_uses *own = NULL;
        if (!st[s].module)
            for (const VkBaseInStructure *b = st[s].pNext; b; b = b->pNext)
                if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                    const VkShaderModuleCreateInfo *mi = (const void *)b;
                    own = reflect(mi->pCode, mi->codeSize);
                }
        pthread_mutex_lock(&n->lock);
        nd_ent *me = st[s].module ? map_find(&n->modules, (uint64_t)(uintptr_t)st[s].module) : NULL;
        const nd_uses *u = me ? (nd_uses *)(uintptr_t)me->val : own;
        for (uint32_t i = 0; u && i < u->n; i++) {
            VkDescriptorSetLayout dsl = layout_set(n, pl, u->u[i].set);
            nd_ent *he = dsl ? map_get(&n->hints, (uint64_t)(uintptr_t)dsl, 1024) : NULL;
            nd_uses *h = he ? add_use((nd_uses *)(uintptr_t)he->val, 0, u->u[i].binding, u->u[i].kind, &n->mixed) : NULL;
            if (h) he->val = (uint64_t)(uintptr_t)h;
        }
        pthread_mutex_unlock(&n->lock);
        free(own);
    }
}

static VkResult VKAPI_CALL nd_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                 VkShaderModule *out)
{
    vkb_nulldesc *n = cur();
    VkResult r = n->create_module(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    nd_uses *u = reflect(ci->pCode, ci->codeSize);
    if (!u) return r;
    pthread_mutex_lock(&n->lock);
    nd_ent *e = map_get(&n->modules, (uint64_t)(uintptr_t)*out, 1024);
    if (e) {
        free((void *)(uintptr_t)e->val);
        e->val = (uint64_t)(uintptr_t)u;
    } else {
        free(u);
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

static void VKAPI_CALL nd_DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks *a)
{
    vkb_nulldesc *n = cur();
    pthread_mutex_lock(&n->lock);
    free((void *)(uintptr_t)map_take(&n->modules, (uint64_t)(uintptr_t)m));
    pthread_mutex_unlock(&n->lock);
    n->destroy_module(device, m, a);
}

static VkResult VKAPI_CALL nd_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a,
                                                   VkPipelineLayout *out)
{
    vkb_nulldesc *n = cur();
    VkResult r = n->create_layout(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    nd_layout *l = malloc(sizeof(*l) + ci->setLayoutCount * sizeof(l->set[0]));
    if (!l) return r;
    l->n = ci->setLayoutCount;
    if (l->n) memcpy(l->set, ci->pSetLayouts, l->n * sizeof(l->set[0]));
    pthread_mutex_lock(&n->lock);
    nd_ent *e = map_get(&n->layouts, (uint64_t)(uintptr_t)*out, 1024);
    if (e) {
        free((void *)(uintptr_t)e->val);
        e->val = (uint64_t)(uintptr_t)l;
    } else {
        free(l);
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

static void VKAPI_CALL nd_DestroyPipelineLayout(VkDevice device, VkPipelineLayout pl, const VkAllocationCallbacks *a)
{
    vkb_nulldesc *n = cur();
    pthread_mutex_lock(&n->lock);
    free((void *)(uintptr_t)map_take(&n->layouts, (uint64_t)(uintptr_t)pl));
    pthread_mutex_unlock(&n->lock);
    n->destroy_layout(device, pl, a);
}

static void VKAPI_CALL nd_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout dsl, const VkAllocationCallbacks *a)
{
    vkb_nulldesc *n = cur();
    pthread_mutex_lock(&n->lock);
    free((void *)(uintptr_t)map_take(&n->hints, (uint64_t)(uintptr_t)dsl));
    pthread_mutex_unlock(&n->lock);
    n->destroy_set_layout(device, dsl, a);
}

static VkResult VKAPI_CALL nd_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                      const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_nulldesc *n = cur();
    for (uint32_t i = 0; i < count; i++) learn(n, ci[i].layout, ci[i].stageCount, ci[i].pStages);
    return n->create_gfx(device, cache, count, ci, a, out);
}

static VkResult VKAPI_CALL nd_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                     const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_nulldesc *n = cur();
    for (uint32_t i = 0; i < count; i++) learn(n, ci[i].layout, 1, &ci[i].stage);
    return n->create_compute(device, cache, count, ci, a, out);
}

static VkResult VKAPI_CALL nd_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    vkb_nulldesc *n = cur();
    VkResult r = n->allocate(device, ai, out);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_lock(&n->lock);
    if (n->sets.count + ai->descriptorSetCount > MAX_SETS_MAPPED) {
        static int told;
        if (!told++) VKB_INFO("device %u: null descriptors: %u descriptor sets mapped to their layouts; the map starts over (logged once)",
                              n->t->id, n->sets.count);
        map_clear(&n->sets, 0);
    }
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        nd_ent *e = map_get(&n->sets, (uint64_t)(uintptr_t)out[i], 8192);
        if (e) e->val = (uint64_t)(uintptr_t)ai->pSetLayouts[i];
    }
    pthread_mutex_unlock(&n->lock);
    return r;
}

/* ------------------------------------------------------------------ the fixes */

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

/* Fixes one descriptor (of type ty, of a binding wanting kind) at p; 1 when it was null.
 * Dummies must exist; the lock is held. */
static int fix_one(vkb_nulldesc *n, VkDescriptorType ty, uint8_t kind, void *p)
{
    if (image_type(ty)) {
        VkDescriptorImageInfo *ii = p;
        if (ii->imageView) return 0;
        ii->imageView = standin(n, kind);
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

/* Replaces the null descriptors in a write list; how many there were. Pushed writes name no set:
 * push_layout is their set layout (VK_NULL_HANDLE when not known). */
static unsigned fix_writes(vkb_nulldesc *n, uint32_t nw, const VkWriteDescriptorSet *w, int pushed, VkDescriptorSetLayout push_layout)
{
    unsigned k = 0;
    int ready = -1;
    VkDescriptorSet last = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = push_layout;
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
            if (ready < 0) {
                pthread_mutex_lock(&n->lock); /* held to the end: the maps, the stand-ins */
                ready = make_dummies(n);
            }
            if (!ready) continue;
            if (!pushed && image_type(ty) && w[i].dstSet != last) {
                last = w[i].dstSet;
                dsl = set_layout(n, last);
            }
            k += (unsigned)fix_one(n, ty, image_type(ty) ? binding_kind(n, dsl, w[i].dstBinding) : 0, p);
        }
    }
    if (ready >= 0) pthread_mutex_unlock(&n->lock);
    return k;
}

static void VKAPI_CALL nd_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                               const VkCopyDescriptorSet *cp)
{
    vkb_nulldesc *n = cur();
    count(n, fix_writes(n, nw, w, 0, VK_NULL_HANDLE));
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
    t->layout = ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS ? layout_set(n, ci->pipelineLayout, ci->set)
                                                                                        : ci->descriptorSetLayout;
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

/* Replaces the null descriptors in a template's data; how many there were. */
static unsigned fix_template_data(vkb_nulldesc *n, VkDescriptorUpdateTemplate tmpl, const void *data)
{
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
            if (ready) k += (unsigned)fix_one(n, ty, image_type(ty) ? binding_kind(n, t->layout, e->dstBinding) : 0, p);
        }
    }
    pthread_mutex_unlock(&n->lock);
    return k;
}

static void VKAPI_CALL nd_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
                                                          const void *data)
{
    vkb_nulldesc *n = cur();
    count(n, fix_template_data(n, tmpl, data));
    n->update_template(device, set, tmpl, data);
}

/* The set layout of a pushed set. */
static VkDescriptorSetLayout push_layout(vkb_nulldesc *n, VkPipelineLayout pl, uint32_t set)
{
    if (!n->precise) return VK_NULL_HANDLE;
    pthread_mutex_lock(&n->lock);
    VkDescriptorSetLayout dsl = layout_set(n, pl, set);
    pthread_mutex_unlock(&n->lock);
    return dsl;
}

static void count_push(vkb_nulldesc *n, unsigned k)
{
    static int told;
    if (k && !__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
        VKB_INFO("device %u: null descriptors in push descriptor writes replaced by dummies", n->t->id);
    count(n, k);
}

static void VKAPI_CALL nd_CmdPushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                                               uint32_t nw, const VkWriteDescriptorSet *w)
{
    vkb_nulldesc *n = cur();
    count_push(n, fix_writes(n, nw, w, 1, push_layout(n, layout, set)));
    n->push(cb, bp, layout, set, nw, w);
}

static void VKAPI_CALL nd_CmdPushDescriptorSet2(VkCommandBuffer cb, const VkPushDescriptorSetInfo *info)
{
    vkb_nulldesc *n = cur();
    count_push(n, fix_writes(n, info->descriptorWriteCount, info->pDescriptorWrites, 1, push_layout(n, info->layout, info->set)));
    n->push2(cb, info);
}

static void VKAPI_CALL nd_CmdPushDescriptorSetWithTemplate(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout,
                                                           uint32_t set, const void *data)
{
    vkb_nulldesc *n = cur();
    count_push(n, fix_template_data(n, tmpl, data));
    n->push_template(cb, tmpl, layout, set, data);
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
    n->push = dt->vkCmdPushDescriptorSet;
    n->push2 = dt->vkCmdPushDescriptorSet2;
    n->push_template = dt->vkCmdPushDescriptorSetWithTemplate;
    if (n->push) dt->vkCmdPushDescriptorSet = nd_CmdPushDescriptorSet;
    if (n->push2) dt->vkCmdPushDescriptorSet2 = nd_CmdPushDescriptorSet2;
    /* Template data is read through the templates this layer saw created. */
    if (n->push_template && dt->vkCreateDescriptorUpdateTemplate == nd_CreateDescriptorUpdateTemplate)
        dt->vkCmdPushDescriptorSetWithTemplate = nd_CmdPushDescriptorSetWithTemplate;
    /* Precise stand-ins: what pipelines declare, and which layout each set has. */
    const char *two_d = vkb_srv_env("VKBRIDGE_NULLDESC_2D");
    n->create_module = dt->vkCreateShaderModule;
    n->destroy_module = dt->vkDestroyShaderModule;
    n->create_layout = dt->vkCreatePipelineLayout;
    n->destroy_layout = dt->vkDestroyPipelineLayout;
    n->destroy_set_layout = dt->vkDestroyDescriptorSetLayout;
    n->create_gfx = dt->vkCreateGraphicsPipelines;
    n->create_compute = dt->vkCreateComputePipelines;
    n->allocate = dt->vkAllocateDescriptorSets;
    n->precise = !(two_d && *two_d && strcmp(two_d, "0")) && n->create_module && n->destroy_module && n->create_layout &&
                 n->destroy_layout && n->destroy_set_layout && n->create_gfx && n->create_compute && n->allocate;
    if (n->precise) {
        dt->vkCreateShaderModule = nd_CreateShaderModule;
        dt->vkDestroyShaderModule = nd_DestroyShaderModule;
        dt->vkCreatePipelineLayout = nd_CreatePipelineLayout;
        dt->vkDestroyPipelineLayout = nd_DestroyPipelineLayout;
        dt->vkDestroyDescriptorSetLayout = nd_DestroyDescriptorSetLayout;
        dt->vkCreateGraphicsPipelines = nd_CreateGraphicsPipelines;
        dt->vkCreateComputePipelines = nd_CreateComputePipelines;
        dt->vkAllocateDescriptorSets = nd_AllocateDescriptorSets;
    } else {
        VKB_INFO("device %u: null images all get the 2D stand-in%s", t->id, two_d ? " (VKBRIDGE_NULLDESC_2D)" : "");
    }
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
    if (dt->vkCmdPushDescriptorSet == nd_CmdPushDescriptorSet) dt->vkCmdPushDescriptorSet = n->push;
    if (dt->vkCmdPushDescriptorSet2 == nd_CmdPushDescriptorSet2) dt->vkCmdPushDescriptorSet2 = n->push2;
    if (dt->vkCmdPushDescriptorSetWithTemplate == nd_CmdPushDescriptorSetWithTemplate) dt->vkCmdPushDescriptorSetWithTemplate = n->push_template;
    if (n->precise) {
        dt->vkCreateShaderModule = n->create_module;
        dt->vkDestroyShaderModule = n->destroy_module;
        dt->vkCreatePipelineLayout = n->create_layout;
        dt->vkDestroyPipelineLayout = n->destroy_layout;
        dt->vkDestroyDescriptorSetLayout = n->destroy_set_layout;
        dt->vkCreateGraphicsPipelines = n->create_gfx;
        dt->vkCreateComputePipelines = n->create_compute;
        dt->vkAllocateDescriptorSets = n->allocate;
    }
    const vkb_dispatch *r = &t->real;
    VkDevice d = t->device;
    if (n->texel) r->vkDestroyBufferView(d, n->texel, NULL);
    if (n->buffer) r->vkDestroyBuffer(d, n->buffer, NULL);
    if (n->buffer_mem) r->vkFreeMemory(d, n->buffer_mem, NULL);
    for (int i = 0; i < KINDS; i++) free_image(t, &n->shape[i]);
    if (n->replaced)
        VKB_INFO("device %u: %lu null descriptors replaced, %lu images by stand-ins of their binding's shape; %lu bindings where pipelines "
                 "disagree on the shape", t->id, n->replaced, n->shaped, n->mixed);
    map_free(&n->modules, 1);
    map_free(&n->layouts, 1);
    map_free(&n->hints, 1);
    map_free(&n->sets, 0);
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
