/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * import_serial (MK-48): on a player's POCO X6 Pro (Mali-G615 MC6, driver r44p1) the one server process
 * faulted four times at the same driver pc, libGLES_mali.so+0xa177bc, SEGV_ACCERR at an address in
 * scudo's never-mapped reserve (a wild pointer inside the driver), always in a dma-buf import or its
 * bind, always while another program's device was making or dropping its own imported memory:
 *   a player log RE Village: gamescope's import of Xwayland's 320x240 window buffer, re8 in vkAllocateMemory
 *     at the same moment (its 1920x1080 swapchain export images), then gamescope stuck 31 s on the lock
 *     the fault left held, the memory fault watchdog ended it and the session with it;
 *   a player log: Xwayland's import of re8's window image overlapping re8's own export import;
 *   a player log: gamescope's bind of an import while RE7's window was torn down;
 *   a player log: Xwayland's export-emulation import 60 ms after re8's device came up.
 * An Android app never runs several devices importing and allocating at once in one process; the bridge
 * does (every program's device lives in the server), so the driver's cross-device import bookkeeping is
 * reached concurrently only here; inside one device the driver's own WSI imports beside an app's
 * allocations on other threads every day. The fix: a driver call that touches imported memory - an
 * allocation that imports (dma-buf / AHardwareBuffer / host pointer, the export emulation's own import too),
 * the free of imported memory, a bind onto imported memory - never runs beside another DEVICE's
 * vkAllocateMemory / vkFreeMemory / vkBind*Memory, and those never beside it. Calls of one device never wait
 * for each other, and plain calls of all devices run side by side as before. Waiting calls go in arrival
 * order (a ticket each), so neither imports nor allocations can be held back by a stream of the other kind:
 * a plain call waits at most for the other devices' import calls running or queued before it (each a short
 * driver call), an import for the other devices' memory calls already running.
 * (Before 2026-11-10 a process-wide writer-preferring rwlock: a device's own imports held back its own
 * allocations, and under steady imports plain allocations starved; S10+ stress: 55x fewer.)
 *
 * It sits above the crash guard (server_guard.c), so a contained fault comes back through the wrapper and
 * the lock is let go. A device that already had a contained memory fault, or is lost, skips the lock: its
 * next call may wait on the driver's lock for good and must not hold ours. Every wait for the lock is
 * bounded (2 s, VKBRIDGE_IMPORT_SERIAL_WAIT_MS): after that the call goes on without it (said once) and
 * leaves the queue, so the lock can never add a hang. Cost: an uncontended mutex lock/unlock per
 * allocation, free or bind (made when resources are made, not per frame). The totals line at each device's
 * end gives the waits (count, longest, sum), the cost on the phone it is on.
 *
 * No imitation here: the race is imitated beneath the guard by VKBRIDGE_TEST_CRASH_CMD=import_race
 * (server_crashctx.c), which faults an import that overlaps another device's memory call.
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IMP_TABLES 4096
#define HBUCKETS 256

typedef struct imp_dev {
    VkDevice device;
    vkb_srv_table *t;
    PFN_vkAllocateMemory alloc;
    PFN_vkFreeMemory free_mem;
    PFN_vkBindImageMemory bind_img;
    PFN_vkBindImageMemory2 bind_img2;
    PFN_vkBindBufferMemory bind_buf;
    PFN_vkBindBufferMemory2 bind_buf2;
} imp_dev;

typedef struct imported {
    VkDevice device;
    VkDeviceMemory mem;
    struct imported *next;
} imported;

static imp_dev *devs[IMP_TABLES];
static pthread_once_t rw_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t set_lock = PTHREAD_MUTEX_INITIALIZER;
static imported *set[HBUCKETS];
static int set_count;
static long wait_ms = 2000;
static uint64_t n_alone, n_shared, n_waited, n_timeout, n_skipped, wait_us_max, wait_us_sum;

/* The calls in the driver, by kind and device (table id), and the queue of waiting calls in ticket order:
 * a call enters when every earlier ticket has entered (or left after its wait limit) and nothing of
 * ANOTHER device that it must stay apart from is in the driver. An import ("excl") stays apart from
 * every other device's call, a plain call from other devices' imports. */
#define GONE_RING 1024
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv;
static uint32_t in_all, in_excl, in_dev[IMP_TABLES], excl_dev[IMP_TABLES];
static uint64_t next_ticket, head; /* head: the oldest ticket not yet entered */
static unsigned char gone[GONE_RING]; /* tickets that left after the wait limit, not yet passed by head */

static int fits(uint32_t id, int excl)
{
    return excl ? in_all == in_dev[id] : in_excl == excl_dev[id];
}

static void head_on(void)
{
    head++;
    while (head < next_ticket && gone[head % GONE_RING]) gone[head++ % GONE_RING] = 0;
}

#define ADD(x, n) __atomic_add_fetch(&(x), (n), __ATOMIC_RELAXED)

static void rw_init(void)
{
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&q_cv, &ca);
    pthread_condattr_destroy(&ca);
    const char *e = vkb_srv_env_global("VKBRIDGE_IMPORT_SERIAL_WAIT_MS");
    if (e && *e) wait_ms = atol(e);
    if (wait_ms < 1) wait_ms = 1;
}

static imp_dev *find(VkDevice device)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (t && t->device == device && t->id < IMP_TABLES) {
        imp_dev *d = __atomic_load_n(&devs[t->id], __ATOMIC_ACQUIRE);
        if (d && d->device == device) return d;
    }
    for (int i = 0; i < IMP_TABLES; i++) {
        imp_dev *d = __atomic_load_n(&devs[i], __ATOMIC_ACQUIRE);
        if (d && d->device == device) return d;
    }
    return NULL;
}

static unsigned hsh(VkDevice d, VkDeviceMemory m)
{
    uint64_t k = (uint64_t)(uintptr_t)d ^ ((uint64_t)(uintptr_t)m * 0x9e3779b97f4a7c15ull);
    k ^= k >> 29;
    return (unsigned)(k % HBUCKETS);
}

static void set_add(VkDevice d, VkDeviceMemory m)
{
    imported *x = malloc(sizeof(*x));
    if (!x) return;
    x->device = d, x->mem = m;
    unsigned k = hsh(d, m);
    pthread_mutex_lock(&set_lock);
    x->next = set[k];
    set[k] = x;
    __atomic_add_fetch(&set_count, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&set_lock);
}

/* 1 when m is imported memory of d (take: and forget it) */
static int set_has(VkDevice d, VkDeviceMemory m, int take)
{
    if (!m || !__atomic_load_n(&set_count, __ATOMIC_ACQUIRE)) return 0;
    unsigned k = hsh(d, m);
    int found = 0;
    pthread_mutex_lock(&set_lock);
    for (imported **pp = &set[k]; *pp; pp = &(*pp)->next)
        if ((*pp)->device == d && (*pp)->mem == m) {
            found = 1;
            if (take) {
                imported *x = *pp;
                *pp = x->next;
                free(x);
                __atomic_sub_fetch(&set_count, 1, __ATOMIC_RELEASE);
            }
            break;
        }
    pthread_mutex_unlock(&set_lock);
    return found;
}

static int is_import(const VkMemoryAllocateInfo *ai)
{
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        switch ((int)b->sType) {
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
            if (((const VkImportMemoryFdInfoKHR *)b)->fd >= 0 && ((const VkImportMemoryFdInfoKHR *)b)->handleType) return 1;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
        case VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID:
            return 1;
        default:
            break;
        }
    return 0;
}

/* 0: not held (skipped or timed out), else 1 + id * 2 + excl (what give() takes back) */
static int take(const imp_dev *d, int alone)
{
    pthread_once(&rw_once, rw_init);
    if (vkb_lost_is(d->t) || vkb_guard_mem_faults(d->t)) {
        ADD(n_skipped, 1);
        return 0;
    }
    uint32_t id = d->t->id;
    pthread_mutex_lock(&q_lock);
    if (head == next_ticket && fits(id, alone)) {
        next_ticket++, head++;
    } else {
        uint64_t me = next_ticket++;
        struct timespec t0, dl;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        dl = t0;
        dl.tv_sec += wait_ms / 1000;
        dl.tv_nsec += (wait_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) dl.tv_sec++, dl.tv_nsec -= 1000000000L;
        int timed_out = 0;
        while (!(head == me && fits(id, alone)))
            if (pthread_cond_timedwait(&q_cv, &q_lock, &dl) == ETIMEDOUT && !(head == me && fits(id, alone))) {
                timed_out = 1;
                break;
            }
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint64_t us = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000);
        ADD(n_waited, 1);
        ADD(wait_us_sum, us);
        if (us > wait_us_max) wait_us_max = us; /* under q_lock */
        if (timed_out) {
            /* leave the queue: head passes this ticket now or when it gets here */
            if (head == me) head_on();
            else gone[me % GONE_RING] = 1;
            pthread_cond_broadcast(&q_cv);
            pthread_mutex_unlock(&q_lock);
            if (!__atomic_fetch_add(&n_timeout, 1, __ATOMIC_RELAXED))
                VKB_WARN("errata: import_serial: a memory call of device %u waited %ld ms for the import lock (a call holding it is stuck?); it "
                         "goes on without the lock (said once)", id, wait_ms);
            return 0;
        }
        head_on();
    }
    in_all++, in_dev[id]++;
    if (alone) in_excl++, excl_dev[id]++;
    if (head < next_ticket) pthread_cond_broadcast(&q_cv); /* the next ticket may fit beside this call */
    pthread_mutex_unlock(&q_lock);
    if (alone) ADD(n_alone, 1); else ADD(n_shared, 1);
    return 1 + (int)id * 2 + (alone ? 1 : 0);
}

static void give(int held)
{
    if (!held) return;
    uint32_t id = (uint32_t)(held - 1) / 2;
    int alone = (held - 1) & 1;
    pthread_mutex_lock(&q_lock);
    in_all--, in_dev[id]--;
    if (alone) in_excl--, excl_dev[id]--;
    if (head < next_ticket) pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL s_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a,
                                                       VkDeviceMemory *out)
{
    imp_dev *d = find(device);
    if (!d) return vkb_errata_next(0)->vkAllocateMemory(device, ai, a, out);
    int imp = is_import(ai);
    int held = take(d, imp);
    VkResult r = d->alloc(device, ai, a, out);
    give(held);
    if (imp && r == VK_SUCCESS && *out) set_add(device, *out);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL s_FreeMemory(VkDevice device, VkDeviceMemory mem, const VkAllocationCallbacks *a)
{
    imp_dev *d = find(device);
    if (!d) {
        vkb_errata_next(0)->vkFreeMemory(device, mem, a);
        return;
    }
    int held = take(d, set_has(device, mem, 1));
    d->free_mem(device, mem, a);
    give(held);
}

static VKAPI_ATTR VkResult VKAPI_CALL s_BindImageMemory(VkDevice device, VkImage img, VkDeviceMemory mem, VkDeviceSize off)
{
    imp_dev *d = find(device);
    if (!d) return vkb_errata_next(0)->vkBindImageMemory(device, img, mem, off);
    int held = take(d, set_has(device, mem, 0));
    VkResult r = d->bind_img(device, img, mem, off);
    give(held);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL s_BindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *bi)
{
    imp_dev *d = find(device);
    if (!d) return vkb_errata_next(0)->vkBindImageMemory2(device, n, bi);
    int alone = 0;
    for (uint32_t i = 0; i < n && !alone; i++) alone = set_has(device, bi[i].memory, 0);
    int held = take(d, alone);
    VkResult r = d->bind_img2(device, n, bi);
    give(held);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL s_BindBufferMemory(VkDevice device, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off)
{
    imp_dev *d = find(device);
    if (!d) return vkb_errata_next(0)->vkBindBufferMemory(device, buf, mem, off);
    int held = take(d, set_has(device, mem, 0));
    VkResult r = d->bind_buf(device, buf, mem, off);
    give(held);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL s_BindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo *bi)
{
    imp_dev *d = find(device);
    if (!d) return vkb_errata_next(0)->vkBindBufferMemory2(device, n, bi);
    int alone = 0;
    for (uint32_t i = 0; i < n && !alone; i++) alone = set_has(device, bi[i].memory, 0);
    int held = take(d, alone);
    VkResult r = d->bind_buf2(device, n, bi);
    give(held);
    return r;
}

void vkb_errata_import_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (e->probe || t->id >= IMP_TABLES || !t->real.vkAllocateMemory || !t->real.vkFreeMemory) return;
    imp_dev *d = calloc(1, sizeof(*d));
    if (!d) return;
    d->device = t->device;
    d->t = t;
    d->alloc = t->real.vkAllocateMemory;
    d->free_mem = t->real.vkFreeMemory;
    d->bind_img = t->real.vkBindImageMemory;
    d->bind_img2 = t->real.vkBindImageMemory2;
    d->bind_buf = t->real.vkBindBufferMemory;
    d->bind_buf2 = t->real.vkBindBufferMemory2;
    ERRATA_HOOK(t, vkAllocateMemory, s_AllocateMemory);
    ERRATA_HOOK(t, vkFreeMemory, s_FreeMemory);
    if (d->bind_img) ERRATA_HOOK(t, vkBindImageMemory, s_BindImageMemory);
    if (d->bind_img2) ERRATA_HOOK(t, vkBindImageMemory2, s_BindImageMemory2);
    if (d->bind_buf) ERRATA_HOOK(t, vkBindBufferMemory, s_BindBufferMemory);
    if (d->bind_buf2) ERRATA_HOOK(t, vkBindBufferMemory2, s_BindBufferMemory2);
    __atomic_store_n(&devs[t->id], d, __ATOMIC_RELEASE);
    vkb_errata_info_once("errata: import_serial: the driver calls on imported memory (imports, their binds and frees) never run beside "
                         "another device's memory calls; a device's own calls and plain calls of all devices side by side");
}

void vkb_errata_import_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (t->id >= IMP_TABLES) return;
    imp_dev *d = devs[t->id];
    if (!d || d->t != t) return;
    if (!e->probe)
        VKB_INFO("errata: import_serial: device %u: totals in the process so far: %llu call(s) alone, %llu shared, %llu waited (longest %.3f ms, "
                 "%.3f ms in all), %llu went on without the lock after the wait limit, %llu skipped (a device after a contained fault, or lost)",
                 t->id, (unsigned long long)n_alone, (unsigned long long)n_shared, (unsigned long long)n_waited, wait_us_max / 1000.0,
                 wait_us_sum / 1000.0, (unsigned long long)n_timeout, (unsigned long long)n_skipped);
    /* the device's imported memory left in the set (its teardown frees go to the driver directly) */
    pthread_mutex_lock(&set_lock);
    for (unsigned k = 0; k < HBUCKETS; k++)
        for (imported **pp = &set[k]; *pp;)
            if ((*pp)->device == t->device) {
                imported *x = *pp;
                *pp = x->next;
                free(x);
                __atomic_sub_fetch(&set_count, 1, __ATOMIC_RELEASE);
            } else {
                pp = &(*pp)->next;
            }
    pthread_mutex_unlock(&set_lock);
    /* kept (not freed): a layer above may still call through the wrapper on another thread; the
     * table slot is cleared so a later device with this id installs its own */
    __atomic_store_n(&devs[t->id], NULL, __ATOMIC_RELEASE);
}
