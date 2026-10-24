/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Texel buffer view recycling.
 *
 * DXVK creates and destroys texel buffer views constantly, with the same parameters over and
 * over: Deep Rock Galactic (DX11 through DXVK 3.1, tablet, 5 s) made 6,814 vkCreateBufferView
 * round trips and 6,902 vkDestroyBufferView. Views are immutable, so a destroyed view is parked
 * here under its full create info instead, and the next identical create gets it back without
 * a round trip.
 *
 * A view is only parked after the app destroyed it, so a handle is never out twice. Parked views
 * are really destroyed when their buffer is (its handle may come back for another buffer), when
 * the cache is full (oldest first) and with the device. Views made while a buffer lives but
 * destroyed after it are not parked: the buffer's destruction forgets them.
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "gen/vkb_client_gen.h"

#include <string.h>

#define BV_MAX_PARKED 4096
#define BV_HB 4096            /* buckets per table; chains, so more entries than buckets is fine */

typedef struct bv_key {
    VkBuffer buffer;
    VkFormat format;
    VkBufferViewCreateFlags flags;
    VkDeviceSize offset, range;
    VkBufferUsageFlags2 usage; /* VkBufferUsageFlags2CreateInfo, if chained */
    uint32_t has_usage;
} bv_key;

typedef struct bv_entry {
    VkBufferView view;
    bv_key key;
    int parked;
    /* Live: in the by-view table. Parked: in the by-key table and the age list. Both: by buffer. */
    struct bv_entry *hnext, *hprev;
    struct bv_entry *bnext, *bprev;
    struct bv_entry *anext, *aprev;
} bv_entry;

typedef struct vkb_bv_cache {
    pthread_mutex_t lock;
    bv_entry *by_view[BV_HB];
    bv_entry *by_key[BV_HB];
    bv_entry *by_buffer[BV_HB];
    bv_entry *oldest, *newest;
    uint32_t parked, entries;
    uint64_t hits;
} vkb_bv_cache;

static unsigned mix(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return (unsigned)k & (BV_HB - 1);
}

static unsigned key_hash(const bv_key *k)
{
    uint64_t h = (uint64_t)(uintptr_t)k->buffer;
    h = h * 0x100000001b3ull ^ (uint64_t)k->format;
    h = h * 0x100000001b3ull ^ k->offset;
    h = h * 0x100000001b3ull ^ k->range;
    h = h * 0x100000001b3ull ^ ((uint64_t)k->flags << 32 | k->has_usage);
    h = h * 0x100000001b3ull ^ k->usage;
    return mix(h);
}

static int key_eq(const bv_key *a, const bv_key *b)
{
    return a->buffer == b->buffer && a->format == b->format && a->flags == b->flags && a->offset == b->offset &&
           a->range == b->range && a->usage == b->usage && a->has_usage == b->has_usage;
}

/* The key of a create info, or 0 when its chain has something the key cannot hold exactly. */
static int make_key(const VkBufferViewCreateInfo *ci, bv_key *k)
{
    memset(k, 0, sizeof(*k));
    k->buffer = ci->buffer;
    k->format = ci->format;
    k->flags = ci->flags;
    k->offset = ci->offset;
    k->range = ci->range;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO || k->has_usage) return 0;
        k->usage = ((const VkBufferUsageFlags2CreateInfo *)b)->usage;
        k->has_usage = 1;
    }
    return 1;
}

/* Intrusive doubly linked lists: head pointer, next/prev fields named by the caller. */
#define LIST_PUSH(head, e, nx, pv) do { (e)->pv = NULL; (e)->nx = (head); if (head) (head)->pv = (e); (head) = (e); } while (0)
#define LIST_DEL(head, e, nx, pv) \
    do { \
        if ((e)->pv) { \
            (e)->pv->nx = (e)->nx; \
        } else { \
            (head) = (e)->nx; \
        } \
        if ((e)->nx) (e)->nx->pv = (e)->pv; \
        (e)->nx = (e)->pv = NULL; \
    } while (0)

static bv_entry **view_bucket(vkb_bv_cache *c, VkBufferView v) { return &c->by_view[mix((uint64_t)(uintptr_t)v)]; }
static bv_entry **buffer_bucket(vkb_bv_cache *c, VkBuffer b) { return &c->by_buffer[mix((uint64_t)(uintptr_t)b)]; }

static void park(vkb_bv_cache *c, bv_entry *e)
{
    LIST_DEL(*view_bucket(c, e->view), e, hnext, hprev);
    LIST_PUSH(c->by_key[key_hash(&e->key)], e, hnext, hprev);
    e->anext = NULL;
    e->aprev = c->newest;
    if (c->newest) c->newest->anext = e;
    else c->oldest = e;
    c->newest = e;
    e->parked = 1;
    c->parked++;
}

static void unpark(vkb_bv_cache *c, bv_entry *e)
{
    LIST_DEL(c->by_key[key_hash(&e->key)], e, hnext, hprev);
    if (e->aprev) e->aprev->anext = e->anext;
    else c->oldest = e->anext;
    if (e->anext) e->anext->aprev = e->aprev;
    else c->newest = e->aprev;
    e->anext = e->aprev = NULL;
    e->parked = 0;
    c->parked--;
}

/* Takes e out of every table; the caller frees it (and destroys the view if it was parked). */
static void forget(vkb_bv_cache *c, bv_entry *e)
{
    if (e->parked) unpark(c, e);
    else LIST_DEL(*view_bucket(c, e->view), e, hnext, hprev);
    LIST_DEL(*buffer_bucket(c, e->key.buffer), e, bnext, bprev);
    c->entries--;
}

void vkb_bufview_init_device(vkb_device *d)
{
    vkb_bv_cache *c = calloc(1, sizeof(*c));
    if (c) pthread_mutex_init(&c->lock, NULL);
    d->bufviews = c;
}

/* Before the device goes: parked views are destroyed, the books dropped. */
void vkb_bufview_destroy_device(vkb_device *d)
{
    vkb_bv_cache *c = d->bufviews;
    if (!c) return;
    for (unsigned b = 0; b < BV_HB; b++) {
        for (bv_entry *e = c->by_buffer[b], *nx; e; e = nx) {
            nx = e->bnext;
            if (e->parked) vkb_wire_vkDestroyBufferView((VkDevice)d, e->view, NULL);
            free(e);
        }
    }
    if (c->hits) VKB_DBG("buffer views: %llu creates answered from the cache", (unsigned long long)c->hits);
    pthread_mutex_destroy(&c->lock);
    free(c);
    d->bufviews = NULL;
}

/*
 * Creates a view the client names (VKB_BV_TAG | n, sent in *pView) without waiting: DXVK makes a
 * view per slice of a discarded buffer, so most creates are new ones (Subnautica gameplay,
 * 2026-10-12: ~1,000 round trips a second, and as many destroys reached the server). The server
 * maps the name to the driver's view (server_bvmap.c). Only out-of-memory can fail a valid
 * create; the server then writes such a view as a null descriptor.
 */
#define VKB_BV_TAG 0xBBBB000000000000ull /* server_bvmap.c */

static VkResult create_view(VkDevice device, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *alloc, VkBufferView *out)
{
    static uint64_t next_name;
    /* A synchronous create sends *pView too: a stale name in it must not pass for a new one. */
    *out = VK_NULL_HANDLE;
    if (!vkb_async_results()) return vkb_wire_vkCreateBufferView(device, ci, alloc, out);
    VkBufferView name = (VkBufferView)(VKB_BV_TAG | __atomic_add_fetch(&next_name, 1, __ATOMIC_RELAXED));
    vkb_device *dev = vkb_dev(device);
    vkb_call c;
    /* vkb_wire_vkCreateBufferView's request, without awaiting its reply. */
    vkb_call_begin(&c, VKB_CMD_vkCreateBufferView, dev->obj.table);
    vkb_enc_u64(&c.e, dev->obj.remote);
    vkb_enc_u8(&c.e, 1);
    vkb_enc_u32(&c.e, 1);
    vkb_enc_VkBufferViewCreateInfo(&c.e, ci);
    vkb_enc_u8(&c.e, 1);
    vkb_enc_bytes(&c.e, &name, sizeof(name));
    if (!vkb_call_exec_async(&c)) return VK_ERROR_DEVICE_LOST;
    *out = name;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo *pCreateInfo,
                                                         const VkAllocationCallbacks *pAllocator, VkBufferView *pView)
{
    vkb_bv_cache *c = vkb_dev(device)->bufviews;
    bv_key k;
    if (!c || !make_key(pCreateInfo, &k)) return create_view(device, pCreateInfo, pAllocator, pView);
    pthread_mutex_lock(&c->lock);
    for (bv_entry *e = c->by_key[key_hash(&k)]; e; e = e->hnext) {
        if (!key_eq(&e->key, &k)) continue;
        unpark(c, e);
        LIST_PUSH(*view_bucket(c, e->view), e, hnext, hprev);
        c->hits++;
        *pView = e->view;
        pthread_mutex_unlock(&c->lock);
        return VK_SUCCESS;
    }
    pthread_mutex_unlock(&c->lock);
    VkResult r = create_view(device, pCreateInfo, pAllocator, pView);
    if (r != VK_SUCCESS || !*pView) return r;
    bv_entry *e = calloc(1, sizeof(*e));
    if (!e) return r; /* not tracked: its destruction is a real one */
    e->view = *pView;
    e->key = k;
    pthread_mutex_lock(&c->lock);
    LIST_PUSH(*view_bucket(c, e->view), e, hnext, hprev);
    LIST_PUSH(*buffer_bucket(c, k.buffer), e, bnext, bprev);
    c->entries++;
    pthread_mutex_unlock(&c->lock);
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyBufferView(VkDevice device, VkBufferView view, const VkAllocationCallbacks *pAllocator)
{
    if (!view) return;
    vkb_bv_cache *c = vkb_dev(device)->bufviews;
    if (!c) {
        vkb_wire_vkDestroyBufferView(device, view, pAllocator);
        return;
    }
    pthread_mutex_lock(&c->lock);
    bv_entry *e = *view_bucket(c, view);
    while (e && e->view != view) e = e->hnext;
    if (!e || e->parked) {
        /* Not ours (uncacheable, or its buffer is gone); parked twice would be an app bug. */
        pthread_mutex_unlock(&c->lock);
        if (!e) vkb_wire_vkDestroyBufferView(device, view, pAllocator);
        return;
    }
    park(c, e);
    if (c->parked > BV_MAX_PARKED) {
        /* Sent under the lock: a vkDestroyBuffer on another thread must not overtake it. */
        bv_entry *old = c->oldest;
        forget(c, old);
        vkb_wire_vkDestroyBufferView(device, old->view, NULL);
        free(old);
    }
    pthread_mutex_unlock(&c->lock);
}

/* Called by vkDestroyBuffer before the buffer goes: its parked views are destroyed first, and
 * live ones forgotten, so they are destroyed for real when the app gets to them. */
void vkb_bufview_buffer_destroyed(vkb_device *d, VkBuffer buffer)
{
    vkb_bv_cache *c = d->bufviews;
    if (!c) return;
    bv_entry *gone = NULL;
    pthread_mutex_lock(&c->lock);
    for (bv_entry *e = *buffer_bucket(c, buffer), *nx; e; e = nx) {
        nx = e->bnext;
        if (e->key.buffer != buffer) continue;
        int parked = e->parked;
        forget(c, e);
        if (parked) {
            e->anext = gone;
            gone = e;
        } else {
            free(e);
        }
    }
    pthread_mutex_unlock(&c->lock);
    for (bv_entry *e = gone, *nx; e; e = nx) {
        nx = e->anext;
        vkb_wire_vkDestroyBufferView((VkDevice)d, e->view, NULL);
        free(e);
    }
}
