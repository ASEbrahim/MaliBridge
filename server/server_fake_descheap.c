/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * VKBRIDGE_FAKE_DESCHEAP=<bytes> (host tests): beneath everything the server does with descriptors,
 * an update-after-bind descriptor pool takes <bytes> of the server's heap for every descriptor it is
 * made for (written to, so it is resident), until the pool is destroyed. That is how PowerVR's DXT
 * 25.1 behaved on the Pixel 10 Pro XL (2026-11-10): RE3's vkd3d-proton heap of six
 * 1,000,000-descriptor sets took the server's heap from 313 MB to 6,472 MB in 10 s and the phone's
 * low-memory killer ended the app (gamescope then aborted: "IWaitable hung up").
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdlib.h>
#include <string.h>

#define FD_POOLS 1024
static pthread_mutex_t fd_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { VkDescriptorPool pool; void *mem; } fd_pools[FD_POOLS];
static PFN_vkCreateDescriptorPool fd_create;
static PFN_vkDestroyDescriptorPool fd_destroy;
static size_t fd_bytes;

static VKAPI_ATTR VkResult VKAPI_CALL fd_CreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *ci,
                                                              const VkAllocationCallbacks *a, VkDescriptorPool *out)
{
    VkResult r = fd_create(device, ci, a, out);
    if (r != VK_SUCCESS || !(ci->flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT)) return r;
    size_t n = 0;
    for (uint32_t i = 0; i < ci->poolSizeCount; i++) n += ci->pPoolSizes[i].descriptorCount;
    void *mem = n ? malloc(n * fd_bytes) : NULL;
    if (mem) memset(mem, 0x5a, n * fd_bytes);
    pthread_mutex_lock(&fd_lock);
    for (int i = 0; i < FD_POOLS && mem; i++)
        if (!fd_pools[i].pool) {
            fd_pools[i].pool = *out;
            fd_pools[i].mem = mem;
            mem = NULL;
        }
    pthread_mutex_unlock(&fd_lock);
    free(mem); /* no slot: not kept */
    return r;
}

static VKAPI_ATTR void VKAPI_CALL fd_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks *a)
{
    void *mem = NULL;
    pthread_mutex_lock(&fd_lock);
    for (int i = 0; i < FD_POOLS && pool; i++)
        if (fd_pools[i].pool == pool) {
            mem = fd_pools[i].mem;
            fd_pools[i].pool = VK_NULL_HANDLE;
            fd_pools[i].mem = NULL;
            break;
        }
    pthread_mutex_unlock(&fd_lock);
    free(mem);
    fd_destroy(device, pool, a);
}

void vkb_fake_descheap_install(vkb_srv_table *dev)
{
    const char *v = vkb_srv_env("VKBRIDGE_FAKE_DESCHEAP");
    size_t bytes = v && *v ? (size_t)strtoul(v, NULL, 0) : 0;
    if (!bytes || !dev->dt.vkCreateDescriptorPool || !dev->dt.vkDestroyDescriptorPool) return;
    pthread_mutex_lock(&fd_lock);
    if (!fd_create) { /* host tests run one driver: the first device's entry points serve all */
        fd_create = dev->dt.vkCreateDescriptorPool;
        fd_destroy = dev->dt.vkDestroyDescriptorPool;
        fd_bytes = bytes;
    }
    pthread_mutex_unlock(&fd_lock);
    dev->dt.vkCreateDescriptorPool = fd_CreateDescriptorPool;
    dev->dt.vkDestroyDescriptorPool = fd_DestroyDescriptorPool;
    VKB_INFO("device %u: imitating a driver whose update-after-bind descriptor pools cost %zu bytes of host memory per descriptor "
             "(VKBRIDGE_FAKE_DESCHEAP)", dev->id, bytes);
}
