/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dedicated_import (MK-39): on older Mali drivers an allocation that imports external memory (a
 * dma-buf or opaque fd, an AHardwareBuffer, a host pointer) and also carries a
 * VkMemoryDedicatedAllocateInfo fails, or crashes the driver, unless the dedicated image or buffer
 * was made with that external handle type (VkExternalMemoryImageCreateInfo /
 * VkExternalMemoryBufferCreateInfo). Lee Gao, "WSI woes on Mali" (winlator-internals, 2026-07-28):
 * fixed at some point between r32p1 and r54p1; GameNative's mesa PR #8 unlinks the struct then.
 * The bridge makes such imports itself (the shared-memory strategies import pages the client maps
 * for memory the app allocated for a plain image or buffer), and so do apps.
 *
 * The fix records the external handle types each image and buffer was made with (only those made
 * with some), and on an import whose dedicated resource lacks the imported handle type takes the
 * dedicated struct out of the chain for the driver's call (it is a hint; the chain is put back
 * after). An import into a resource made for its handle type keeps the struct: that combination
 * works, and an AHardwareBuffer image import requires it.
 *
 * Imitation (host): beneath the fix, such an import fails with VK_ERROR_INVALID_EXTERNAL_HANDLE.
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define BUCKETS 1024

typedef struct res {
    uint64_t handle;
    VkExternalMemoryHandleTypeFlags types;
    struct res *next;
} res;

typedef struct vkb_errata_mem {
    pthread_mutex_t lock;
    res *b[BUCKETS];
    int life_on;
    vkb_dispatch life, fix, imi;
    uint64_t imports, stripped, kept, imi_failed;
} vkb_errata_mem;

static vkb_errata_mem *last_mem;

#define ADD(x, n) __atomic_add_fetch(&(x), (n), __ATOMIC_RELAXED)

static vkb_errata_mem *M(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->mem ? e->mem : __atomic_load_n(&last_mem, __ATOMIC_ACQUIRE);
}

static unsigned hsh(uint64_t k)
{
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ull;
    k ^= k >> 27;
    return (unsigned)(k % BUCKETS);
}

/* Images and buffers are distinct handle values on one device: one table holds both. */
static void rec_add(vkb_errata_mem *m, uint64_t h, VkExternalMemoryHandleTypeFlags types)
{
    res *r = malloc(sizeof(*r));
    if (!r) return;
    r->handle = h, r->types = types;
    unsigned k = hsh(h);
    pthread_mutex_lock(&m->lock);
    r->next = m->b[k];
    m->b[k] = r;
    pthread_mutex_unlock(&m->lock);
}

static void rec_del(vkb_errata_mem *m, uint64_t h)
{
    if (!h) return;
    unsigned k = hsh(h);
    pthread_mutex_lock(&m->lock);
    for (res **pp = &m->b[k]; *pp; pp = &(*pp)->next)
        if ((*pp)->handle == h) {
            res *r = *pp;
            *pp = r->next;
            free(r);
            break;
        }
    pthread_mutex_unlock(&m->lock);
}

static VkExternalMemoryHandleTypeFlags rec_types(vkb_errata_mem *m, uint64_t h)
{
    VkExternalMemoryHandleTypeFlags t = 0;
    unsigned k = hsh(h);
    pthread_mutex_lock(&m->lock);
    for (res *r = m->b[k]; r; r = r->next)
        if (r->handle == h) {
            t = r->types;
            break;
        }
    pthread_mutex_unlock(&m->lock);
    return t;
}

static const void *chain_find(const void *pnext, VkStructureType type)
{
    for (const VkBaseInStructure *b = pnext; b; b = b->pNext)
        if (b->sType == type) return b;
    return NULL;
}

/* ---- the record of each resource's external handle types (once, beneath the rest) */

static VKAPI_ATTR VkResult VKAPI_CALL l_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    vkb_errata_mem *m = M();
    VkResult r = m->life.vkCreateImage(d, ci, a, out);
    const VkExternalMemoryImageCreateInfo *x = chain_find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    if (r == VK_SUCCESS && x && x->handleTypes) rec_add(m, (uint64_t)(uintptr_t)*out, x->handleTypes);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL l_DestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks *a)
{
    vkb_errata_mem *m = M();
    rec_del(m, (uint64_t)(uintptr_t)img);
    m->life.vkDestroyImage(d, img, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL l_CreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out)
{
    vkb_errata_mem *m = M();
    VkResult r = m->life.vkCreateBuffer(d, ci, a, out);
    const VkExternalMemoryBufferCreateInfo *x = chain_find(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
    if (r == VK_SUCCESS && x && x->handleTypes) rec_add(m, (uint64_t)(uintptr_t)*out, x->handleTypes);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL l_DestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a)
{
    vkb_errata_mem *m = M();
    rec_del(m, (uint64_t)(uintptr_t)b);
    m->life.vkDestroyBuffer(d, b, a);
}

/* The imported handle type of an allocation (0: not an import) */
static VkExternalMemoryHandleTypeFlags import_type(const VkMemoryAllocateInfo *ai)
{
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        switch ((int)b->sType) {
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
            return ((const VkImportMemoryFdInfoKHR *)b)->handleType;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
            return ((const VkImportMemoryHostPointerInfoEXT *)b)->handleType;
        case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID:
            return VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        default:
            break;
        }
    return 0;
}

/* The allocation's dedicated struct when it is an import into a resource not made for that
 * handle type (the erratum's pattern), else NULL */
static const VkMemoryDedicatedAllocateInfo *pattern(vkb_errata_mem *m, const VkMemoryAllocateInfo *ai, int count)
{
    VkExternalMemoryHandleTypeFlags imp = import_type(ai);
    if (!imp) return NULL;
    const VkMemoryDedicatedAllocateInfo *d = chain_find(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    if (count) ADD(m->imports, 1);
    if (!d || (!d->image && !d->buffer)) return NULL;
    uint64_t h = d->image ? (uint64_t)(uintptr_t)d->image : (uint64_t)(uintptr_t)d->buffer;
    if (rec_types(m, h) & imp) {
        if (count) ADD(m->kept, 1);
        return NULL;
    }
    return d;
}

/* ---- the fix */

static VKAPI_ATTR VkResult VKAPI_CALL f_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a,
                                                       VkDeviceMemory *out)
{
    vkb_errata_mem *m = M();
    const VkMemoryDedicatedAllocateInfo *d = pattern(m, ai, 1);
    if (!d) return m->fix.vkAllocateMemory(dev, ai, a, out);
    /* Unlinked for the call and put back: the chain belongs to the caller. */
    VkBaseInStructure *prev = NULL;
    for (VkBaseInStructure *b = (VkBaseInStructure *)ai->pNext; b && b != (const void *)d; b = (VkBaseInStructure *)b->pNext) prev = b;
    VkMemoryAllocateInfo info = *ai;
    if (prev)
        prev->pNext = d->pNext;
    else
        info.pNext = d->pNext;
    VkResult r = m->fix.vkAllocateMemory(dev, &info, a, out);
    if (prev) prev->pNext = (const VkBaseInStructure *)d;
    if (!__atomic_fetch_add(&m->stripped, 1, __ATOMIC_RELAXED))
        vkb_errata_info_once("errata: dedicated_import: an import's dedicated %s (made without handle type 0x%x) taken off: %d",
                             d->image ? "image" : "buffer", import_type(ai), r);
    return r;
}

/* ---- the imitation */

static VKAPI_ATTR VkResult VKAPI_CALL i_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a,
                                                       VkDeviceMemory *out)
{
    vkb_errata_mem *m = M();
    if (pattern(m, ai, 0)) {
        ADD(m->imi_failed, 1);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    return m->imi.vkAllocateMemory(dev, ai, a, out);
}

/* ---- installs */

static vkb_errata_mem *mem_get(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (e->mem) return e->mem;
    vkb_errata_mem *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    pthread_mutex_init(&m->lock, NULL);
    e->mem = m;
    if (!e->probe) __atomic_store_n(&last_mem, m, __ATOMIC_RELEASE);
    return m;
}

static void life_install(vkb_srv_table *t, vkb_errata_mem *m)
{
    if (m->life_on) return;
    m->life_on = 1;
    m->life = t->real;
    ERRATA_HOOK(t, vkCreateImage, l_CreateImage);
    ERRATA_HOOK(t, vkDestroyImage, l_DestroyImage);
    ERRATA_HOOK(t, vkCreateBuffer, l_CreateBuffer);
    ERRATA_HOOK(t, vkDestroyBuffer, l_DestroyBuffer);
}

void vkb_errata_dedicated_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_mem *m = mem_get(t, e);
    if (!m) return;
    life_install(t, m);
    m->fix = t->real;
    ERRATA_HOOK(t, vkAllocateMemory, f_AllocateMemory);
}

void vkb_errata_dedicated_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_mem *m = mem_get(t, e);
    if (!m) return;
    life_install(t, m);
    m->imi = t->real;
    ERRATA_HOOK(t, vkAllocateMemory, i_AllocateMemory);
}

void vkb_errata_mem_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_mem *m = e->mem;
    if (!m) return;
    if (!e->probe && ((e->on | e->imitated) >> VKB_ERRATUM_DEDICATED_IMPORT & 1) && (m->imports || m->imi_failed))
        VKB_INFO("errata: dedicated_import: %llu import(s), %llu with the dedicated struct taken off, %llu kept (resource made for "
                 "the handle type); imitated: %llu failed",
                 (unsigned long long)m->imports, (unsigned long long)m->stripped, (unsigned long long)m->kept,
                 (unsigned long long)m->imi_failed);
    for (unsigned k = 0; k < BUCKETS; k++)
        for (res *r = m->b[k], *n; r; r = n) n = r->next, free(r);
    pthread_mutex_destroy(&m->lock);
    vkb_errata_mem *exp = m;
    __atomic_compare_exchange_n(&last_mem, &exp, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    free(m);
    e->mem = NULL;
}
