/*
 * Cached upload memory (VKBRIDGE_CACHED_UPLOAD=1, opt-in; per game in <cache dir>/presets/<appid>.env).
 *
 * Where the self-test needs uncached AHB pages (no coherent type takes a cached AHB: Mali-G77 r32p1
 * on the Retroid Pocket 4 Pro, the Kirin 985), every host-visible allocation an app maps is
 * uncached for the CPU. Plain stores to it are fine, but FEX's TSO emulation turns each guest store
 * into a store-release, and a store-release to uncached memory waits for the memory: Ys I & II
 * Chronicles+ writing its 640x480 paletted picture byte by byte took 163-176 ms a frame (0.74 ms
 * into cached pages; probes/cacheprobe.c). CPU reads of uncached pages run at ~570 MB/s too.
 *
 * With the switch on, an app's shared allocations get cached AHB pages instead (up to
 * VKBRIDGE_CACHED_UPLOAD_MB, default 64, at a time), and the client still sees the coherent type
 * it saw before - DXVK never calls vkFlushMappedMemoryRanges, so the server does the cache
 * maintenance itself, from user space on its own mapping (SCTLR_EL1.UCI; a cache maintenance by
 * VA reaches every cached copy of the page):
 *   before each vkQueueSubmit/2:  dc cvac over every such allocation (the CPU's writes reach memory);
 *   after a wait that completes newer submits than the last such pass (fences, timeline semaphores,
 *   queue/device idle):  dc civac over every such allocation (the CPU's lines are written back and
 *   dropped, so it reads what the GPU wrote). Clean and invalidate, never invalidate alone: a line
 *   the game wrote after the submit's clean is written back, not lost.
 * Measured on the RP4 Pro (probes/gpuprobe.c, 16 MB imported into Mali): without maintenance the
 * GPU and the CPU each saw stale bytes; with it none, at 0.5-0.7 ms per 16 MB per pass.
 * What it cannot cover: a CPU read of GPU results without a wait the server sees (an app that
 * polls memory), and work from another process. Hence opt-in.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t cu_lock = PTHREAD_MUTEX_INITIALIZER;
static vkb_srv_mem **cu_v;
static uint32_t cu_n, cu_cap;
static uint64_t cu_bytes;           /* reserved, the allocations' sizes */
static uint64_t gen_submit;         /* submits seen (each cleaned before it) */
static uint64_t gen_clean;          /* submits whose GPU writes the last clean+invalidate covered */
static struct { uint64_t passes, ns, bytes; } st_flush, st_inval;

static int cu_on(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_CACHED_UPLOAD");
    return v && *v && strcmp(v, "0") != 0;
}

static uint64_t cu_budget(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_CACHED_UPLOAD_MB");
    long mb = v && *v ? atol(v) : 64;
    return (uint64_t)(mb > 0 ? mb : 0) << 20;
}

int vkb_cu_reserve(uint64_t size)
{
#if defined(__ANDROID__) && defined(__aarch64__)
    if (!size || !cu_on()) return 0;
    uint64_t budget = cu_budget();
    pthread_mutex_lock(&cu_lock);
    int ok = cu_bytes + size <= budget;
    if (ok) cu_bytes += size;
    pthread_mutex_unlock(&cu_lock);
    if (!ok) VKB_ONCE(VKB_LOG_INFO, "cached upload: budget of %llu MB used up; further allocations uncached (VKBRIDGE_CACHED_UPLOAD_MB)",
                      (unsigned long long)(budget >> 20));
    return ok;
#else
    (void)size;
    return 0;
#endif
}

void vkb_cu_unreserve(uint64_t size)
{
    pthread_mutex_lock(&cu_lock);
    cu_bytes = cu_bytes > size ? cu_bytes - size : 0;
    pthread_mutex_unlock(&cu_lock);
}

void vkb_cu_add(vkb_srv_mem *m)
{
    int fd = m->sync_fd1 - 1;
    off_t len = lseek(fd, 0, SEEK_END);
    void *p = len > 0 ? mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (p == MAP_FAILED) {
        /* Cached pages nobody can clean would be wrong: the allocation keeps them, so say it loudly. */
        VKB_ERR("cached upload: the server cannot map its pages (%llu bytes); this allocation is not kept coherent",
                (unsigned long long)m->size);
        return;
    }
    pthread_mutex_lock(&cu_lock);
    if (cu_n == cu_cap) {
        uint32_t cap = cu_cap ? cu_cap * 2 : 64;
        vkb_srv_mem **v = realloc(cu_v, cap * sizeof(*v));
        if (!v) {
            pthread_mutex_unlock(&cu_lock);
            munmap(p, (size_t)len);
            VKB_ERR("cached upload: out of memory for the list; this allocation is not kept coherent");
            return;
        }
        cu_v = v, cu_cap = cap;
    }
    m->cu_map = p;
    m->cu_len = (size_t)len;
    cu_v[cu_n++] = m;
    uint32_t n = cu_n;
    uint64_t bytes = cu_bytes;
    pthread_mutex_unlock(&cu_lock);
    if (n == 1 || (n & (n - 1)) == 0)
        VKB_INFO("cached upload: %u allocation(s) with cached pages, %llu MB (this one %llu bytes)", n,
                 (unsigned long long)(bytes >> 20), (unsigned long long)m->size);
}

void vkb_cu_remove(vkb_srv_mem *m)
{
    pthread_mutex_lock(&cu_lock);
    for (uint32_t i = 0; i < cu_n; i++)
        if (cu_v[i] == m) {
            cu_v[i] = cu_v[--cu_n];
            break;
        }
    cu_bytes = cu_bytes > m->size ? cu_bytes - m->size : 0;
    pthread_mutex_unlock(&cu_lock);
    munmap(m->cu_map, m->cu_len);
    m->cu_map = NULL;
}

/* ------------------------------------------------------------------ cache maintenance */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Every allocation, the lock held. invalidate: clean+invalidate (dc civac), else clean (dc cvac). */
static void maintain_locked(int invalidate)
{
#if defined(__aarch64__)
    uint64_t t0 = now_ns(), bytes = 0;
    for (uint32_t i = 0; i < cu_n; i++) {
        uint8_t *p = cu_v[i]->cu_map, *end = p + cu_v[i]->cu_len;
        bytes += cu_v[i]->cu_len;
        if (invalidate)
            for (; p < end; p += 64) __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
        else
            for (; p < end; p += 64) __asm__ volatile("dc cvac, %0" ::"r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    uint64_t ns = now_ns() - t0;
    __typeof__(st_flush) *s = invalidate ? &st_inval : &st_flush;
    uint64_t n = ++s->passes;
    s->ns += ns, s->bytes += bytes;
    if (n == 1 || (n & (n - 1)) == 0) /* the 1st, 2nd, 4th, 8th, ... */
        VKB_INFO("cached upload: %llu %s passes, %.3f ms on average over %.1f MB (this one %.3f ms over %.1f MB)",
                 (unsigned long long)n, invalidate ? "clean+invalidate" : "clean", s->ns / 1e6 / (double)n,
                 s->bytes / 1048576.0 / (double)n, ns / 1e6, bytes / 1048576.0);
#else
    (void)invalidate;
#endif
}

/* Before a submit: the CPU's writes reach memory. Returns the submit's generation. */
static uint64_t before_submit(void)
{
    pthread_mutex_lock(&cu_lock);
    if (cu_n) maintain_locked(0);
    uint64_t g = ++gen_submit;
    pthread_mutex_unlock(&cu_lock);
    return g;
}

/* After a wait that says submits up to generation g are done. */
static void after_wait(uint64_t g)
{
    if (!g || __atomic_load_n(&gen_clean, __ATOMIC_ACQUIRE) >= g) return;
    pthread_mutex_lock(&cu_lock);
    if (g > gen_clean) {
        if (cu_n) maintain_locked(1);
        gen_clean = g;
    }
    pthread_mutex_unlock(&cu_lock);
}

/* What a fence or a timeline value stands for: the generation of the submit that signals it. */
#define TRACK 1024
static struct { uint64_t h; uint64_t gen; } fences[TRACK];
static struct { uint64_t h, value, gen; } sems[TRACK];
static uint32_t sem_next;
static pthread_mutex_t track_lock = PTHREAD_MUTEX_INITIALIZER;

static void fence_note(VkFence f, uint64_t g)
{
    if (!f) return;
    uint64_t h = (uint64_t)(uintptr_t)f;
    uint32_t i = (uint32_t)((h >> 4) ^ (h >> 16)) % TRACK;
    pthread_mutex_lock(&track_lock);
    fences[i].h = h, fences[i].gen = g;
    pthread_mutex_unlock(&track_lock);
}

/* 0 when unknown (an evicted or never-seen fence): then nothing is invalidated for it. */
static uint64_t fence_gen(VkFence f)
{
    uint64_t h = (uint64_t)(uintptr_t)f;
    uint32_t i = (uint32_t)((h >> 4) ^ (h >> 16)) % TRACK;
    pthread_mutex_lock(&track_lock);
    uint64_t g = fences[i].h == h ? fences[i].gen : 0;
    pthread_mutex_unlock(&track_lock);
    return g;
}

static void sem_note(VkSemaphore s, uint64_t value, uint64_t g)
{
    pthread_mutex_lock(&track_lock);
    uint32_t i = sem_next++ % TRACK;
    sems[i].h = (uint64_t)(uintptr_t)s, sems[i].value = value, sems[i].gen = g;
    pthread_mutex_unlock(&track_lock);
}

/* The newest submit that signals s with a value <= v. */
static uint64_t sem_gen(VkSemaphore s, uint64_t v)
{
    uint64_t h = (uint64_t)(uintptr_t)s, g = 0;
    pthread_mutex_lock(&track_lock);
    for (uint32_t i = 0; i < TRACK; i++)
        if (sems[i].h == h && sems[i].value <= v && sems[i].gen > g) g = sems[i].gen;
    pthread_mutex_unlock(&track_lock);
    return g;
}

static int active(void) { return __atomic_load_n(&cu_n, __ATOMIC_RELAXED) != 0; }

/* ------------------------------------------------------------------ hooks */

static PFN_vkQueueSubmit next_submit[VKB_MAX_TABLES];
static PFN_vkQueueSubmit2 next_submit2[VKB_MAX_TABLES];
static PFN_vkWaitForFences next_wait_fences[VKB_MAX_TABLES];
static PFN_vkGetFenceStatus next_fence_status[VKB_MAX_TABLES];
static PFN_vkWaitSemaphores next_wait_sems[VKB_MAX_TABLES];
static PFN_vkGetSemaphoreCounterValue next_sem_value[VKB_MAX_TABLES];
static PFN_vkQueueWaitIdle next_queue_idle[VKB_MAX_TABLES];
static PFN_vkDeviceWaitIdle next_device_idle[VKB_MAX_TABLES];

#define NEXT(arr) vkb_srv_table *t = vkb_srv_current_table(); __typeof__(arr[0]) next = t ? arr[t->id] : NULL

static VKAPI_ATTR VkResult VKAPI_CALL cu_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence fence)
{
    NEXT(next_submit);
    if (!next) return VK_ERROR_DEVICE_LOST;
    if (!active()) return next(q, n, si, fence);
    uint64_t g = before_submit();
    VkResult r = next(q, n, si, fence);
    if (r == VK_SUCCESS) {
        fence_note(fence, g);
        for (uint32_t i = 0; i < n; i++)
            for (const VkBaseInStructure *b = si[i].pNext; b; b = b->pNext)
                if (b->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    const VkTimelineSemaphoreSubmitInfo *ts = (const VkTimelineSemaphoreSubmitInfo *)b;
                    for (uint32_t k = 0; k < si[i].signalSemaphoreCount && k < ts->signalSemaphoreValueCount; k++)
                        if (ts->pSignalSemaphoreValues) sem_note(si[i].pSignalSemaphores[k], ts->pSignalSemaphoreValues[k], g);
                }
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL cu_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence fence)
{
    NEXT(next_submit2);
    if (!next) return VK_ERROR_DEVICE_LOST;
    if (!active()) return next(q, n, si, fence);
    uint64_t g = before_submit();
    VkResult r = next(q, n, si, fence);
    if (r == VK_SUCCESS) {
        fence_note(fence, g);
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t k = 0; k < si[i].signalSemaphoreInfoCount; k++)
                sem_note(si[i].pSignalSemaphoreInfos[k].semaphore, si[i].pSignalSemaphoreInfos[k].value, g);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL cu_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{
    NEXT(next_wait_fences);
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = next(d, n, f, all, timeout);
    if (r == VK_SUCCESS && active()) {
        uint64_t g = 0;
        for (uint32_t i = 0; i < n; i++) {
            /* waitAny: only a fence that says it is signalled counts */
            if (!all && n > 1 && next_fence_status[t->id] && next_fence_status[t->id](d, f[i]) != VK_SUCCESS) continue;
            uint64_t fg = fence_gen(f[i]);
            if (fg > g) g = fg;
        }
        after_wait(g);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL cu_GetFenceStatus(VkDevice d, VkFence f)
{
    NEXT(next_fence_status);
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = next(d, f);
    if (r == VK_SUCCESS && active()) after_wait(fence_gen(f));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL cu_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    NEXT(next_wait_sems);
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = next(d, wi, timeout);
    if (r == VK_SUCCESS && active() && wi) {
        uint64_t g = 0;
        int any = wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT;
        for (uint32_t i = 0; i < wi->semaphoreCount; i++) {
            uint64_t v = wi->pValues[i];
            if (any && next_sem_value[t->id]) {
                uint64_t now = 0;
                if (next_sem_value[t->id](d, wi->pSemaphores[i], &now) != VK_SUCCESS || now < v) continue;
                v = now;
            }
            uint64_t sg = sem_gen(wi->pSemaphores[i], v);
            if (sg > g) g = sg;
        }
        after_wait(g);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL cu_GetSemaphoreCounterValue(VkDevice d, VkSemaphore s, uint64_t *value)
{
    NEXT(next_sem_value);
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = next(d, s, value);
    if (r == VK_SUCCESS && active() && value) after_wait(sem_gen(s, *value));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL cu_QueueWaitIdle(VkQueue q)
{
    NEXT(next_queue_idle);
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = next(q);
    if (r == VK_SUCCESS && active()) after_wait(__atomic_load_n(&gen_submit, __ATOMIC_ACQUIRE));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL cu_DeviceWaitIdle(VkDevice d)
{
    NEXT(next_device_idle);
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = next(d);
    if (r == VK_SUCCESS && active()) after_wait(__atomic_load_n(&gen_submit, __ATOMIC_ACQUIRE));
    return r;
}

/* Installed for every device: the hooks pass straight through while no allocation has cached
 * upload pages, so a server whose games never turn the switch on behaves as before. */
void vkb_cu_device_init(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES) return;
#define HOOK(fn, arr, ours) do { arr[t->id] = t->dt.fn; if (t->dt.fn) t->dt.fn = ours; } while (0)
    HOOK(vkQueueSubmit, next_submit, cu_QueueSubmit);
    HOOK(vkQueueSubmit2, next_submit2, cu_QueueSubmit2);
    HOOK(vkWaitForFences, next_wait_fences, cu_WaitForFences);
    HOOK(vkGetFenceStatus, next_fence_status, cu_GetFenceStatus);
    HOOK(vkWaitSemaphores, next_wait_sems, cu_WaitSemaphores);
    HOOK(vkGetSemaphoreCounterValue, next_sem_value, cu_GetSemaphoreCounterValue);
    HOOK(vkQueueWaitIdle, next_queue_idle, cu_QueueWaitIdle);
    HOOK(vkDeviceWaitIdle, next_device_idle, cu_DeviceWaitIdle);
#undef HOOK
}
