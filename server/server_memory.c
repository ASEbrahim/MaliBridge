/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Host-visible memory shared with the client.
 *
 * A Linux program maps Vulkan memory and writes to it directly; those writes must land in the
 * pages the GPU reads. The server makes such memory shareable in one of three ways, chosen per
 * GPU by a self-test at startup that checks the GPU and both CPU mappings really see the same
 * bytes:
 *   hostptr  the client creates a memfd, the server maps it and imports it
 *            (VK_EXT_external_memory_host);
 *   dmabuf   the server allocates exportable memory and hands the client its dma-buf
 *            (VK_EXT_external_memory_dma_buf), which the client maps;
 *   ahb      the server allocates a BLOB AHardwareBuffer, imports it, and hands the client the
 *            dma-buf behind it (Android only);
 *   dmaheap  the server allocates from /dev/dma_heap (system-uncached, else system), imports that
 *            dma-buf into the driver and hands it to the client - what GameNative's wrapper does on
 *            Mali when the driver will not export one (Android; root-only on desktops).
 * VKBRIDGE_MEMORY=hostptr|dmabuf|dmaheap|ahb forces one.
 *
 * Cached readback (the dma-buf strategies): the pages above are uncached for the CPU on Mali, so
 * the client reads GPU results back at ~400 MB/s. Where the self-test passes it, an allocation the
 * client asks for as VKB_ALLOC_SHARED_CACHED gets cached pages instead (the dma-heap's "system"
 * heap; the dma-buf strategies' own dma-buf otherwise), and the server keeps its dma-buf so that
 * vkFlushMappedMemoryRanges / vkInvalidateMappedMemoryRanges do the CPU cache maintenance with
 * DMA_BUF_IOCTL_SYNC. Mali here does not snoop the CPU's caches, so the cached pages are offered
 * only as memory the client flushes and invalidates: the client's own frame readback (the copy
 * present path), and with VKBRIDGE_CACHED_READBACK=apps a HOST_CACHED type without HOST_COHERENT
 * for apps. After David Cimo's Venus work on the Mali-G715 (droiddeck-mali, GPL-3.0: WSI's
 * readback buffer in Mali's cached type, allocated from /dev/dma_heap/system, flush and invalidate
 * as DMA_BUF_IOCTL_SYNC; the frame memcpy went from 7-10 ms to ~1.3 ms).
 */
#define _GNU_SOURCE
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include "vkb_server.h"
#include "server_emu.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
/* libnativewindow, opened at run time (getNativeHandle is not in the NDK headers). */
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} vkb_ahb_desc;
typedef struct {
    int version, numFds, numInts;
    int data[0];
} vkb_native_handle;
static int (*p_AHardwareBuffer_allocate)(const vkb_ahb_desc *, struct AHardwareBuffer **);
static void (*p_AHardwareBuffer_release)(struct AHardwareBuffer *);
static const vkb_native_handle *(*p_AHardwareBuffer_getNativeHandle)(const struct AHardwareBuffer *);
#define AHB_FORMAT_BLOB 0x21
#define AHB_USAGE_CPU_READ_OFTEN 3ull
#define AHB_USAGE_CPU_WRITE_OFTEN (3ull << 4)
#define AHB_USAGE_GPU_DATA_BUFFER (1ull << 24)
#endif

/* AHB pages uncached (CPU usage "rarely"): -1 until decided, VKBRIDGE_AHB_RARELY forcing it. The
 * memory self-test turns it on when cached AHBs share no coherent type: Huawei's Kirin 985
 * (Mali-G77 r34p0, MaliBridge#10) lets a cached AHB into its cached type 1 only, and that phone has
 * no dma-heaps, so no method worked and Linux programs got no host-visible memory. */
static int ahb_rarely = -1;
static int ahb_uncached(void)
{
    if (ahb_rarely < 0) ahb_rarely = vkb_srv_env_global("VKBRIDGE_AHB_RARELY") != NULL;
    return ahb_rarely;
}
#ifdef __ANDROID__

static int ahb_load(void)
{
    static int state;
    if (state) return state > 0;
    void *h = dlopen("libnativewindow.so", RTLD_NOW);
    if (!h) h = dlopen("libandroid.so", RTLD_NOW);
    if (h) {
        p_AHardwareBuffer_allocate = dlsym(h, "AHardwareBuffer_allocate");
        p_AHardwareBuffer_release = dlsym(h, "AHardwareBuffer_release");
        p_AHardwareBuffer_getNativeHandle = dlsym(h, "AHardwareBuffer_getNativeHandle");
    }
    state = (p_AHardwareBuffer_allocate && p_AHardwareBuffer_release && p_AHardwareBuffer_getNativeHandle) ? 1 : -1;
    if (state < 0) VKB_WARN("AHardwareBuffer functions unavailable");
    return state > 0;
}

/* The dma-buf behind a BLOB AHardwareBuffer's pages (the handle's own fd, not a dup), -1 if none.
 * Mali's gralloc can put more than one fd in the handle (metadata beside the pages): the memory
 * is the one at least as large as the allocation. */
static int ahb_pages_fd(const vkb_native_handle *nh, uint64_t sz)
{
    int pick = -1;
    for (int i = 0; nh && i < nh->numFds && pick < 0; i++) {
        off_t end = lseek(nh->data[i], 0, SEEK_END);
        if (end >= (off_t)sz) pick = nh->data[i];
        if (nh->numFds > 1) VKB_INFO("ahb: handle fd %d of %d is %lld bytes", i + 1, nh->numFds, (long long)end);
    }
    return pick;
}
#endif

static const char *const ext_hostptr[] = {"VK_KHR_external_memory", "VK_EXT_external_memory_host"};
static const char *const ext_dmabuf[] = {"VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
#ifdef __ANDROID__
static const char *const ext_ahb[] = {"VK_KHR_external_memory", "VK_ANDROID_external_memory_android_hardware_buffer",
                                      "VK_EXT_queue_family_foreign", "VK_KHR_sampler_ycbcr_conversion",
                                      "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2"};
#endif

const char *const *vkb_mem_required_extensions(uint32_t strategy, uint32_t *count)
{
    switch (strategy) {
    case VKB_MEM_HOSTPTR: *count = 2; return ext_hostptr;
    case VKB_MEM_DMABUF: *count = 3; return ext_dmabuf;
    case VKB_MEM_DMAHEAP: *count = 3; return ext_dmabuf;
#ifdef __ANDROID__
    case VKB_MEM_AHB: *count = 6; return ext_ahb;
#endif
    default: *count = 0; return NULL;
    }
}

static uint32_t strategy_handle_type(uint32_t s)
{
    switch (s) {
    case VKB_MEM_HOSTPTR: return VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    case VKB_MEM_DMABUF: return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    case VKB_MEM_DMAHEAP: return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
#ifdef __ANDROID__
    case VKB_MEM_AHB: return VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
#endif
    default: return 0;
    }
}

int vkb_memfd(const char *name, size_t size)
{
    int fd = (int)syscall(SYS_memfd_create, name, 1u /* MFD_CLOEXEC */);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ tracking */

static unsigned mem_hash(VkDeviceMemory m)
{
    uint64_t v = (uint64_t)(uintptr_t)m;
    v ^= v >> 17;
    v *= 0x9E3779B97F4A7C15ull;
    return (unsigned)(v >> 56);
}

/* ------------------------------------------------------------------ accounting */

/*
 * What the server holds, for finding what fills memory: Subnautica loading a save took the server
 * from 1.7 to 5.6 GB in 30 s and the tablet ran out (2026-10-12), with decode scratch capped. On
 * Mali every page the GPU owns counts in the owner's RSS, so the server's RSS is all the clients'
 * GPU memory plus its own heap. One line every ~5 s while anything moved; VKBRIDGE_MEM_STATS=0
 * turns it off. Counts are process-wide (every client), under one lock taken per allocation.
 * "buffers" overlaps the rest: DXVK spans each 256 MiB chunk with a buffer.
 *
 * What it showed (2026-10-12, the same load): no leak - tracked allocations follow RSS, 1.1 GB at the
 * menu, 5.8 GB at the end, all of it DXVK's 256 MiB device-local chunks (type 0, no frees) holding
 * BC-emulated images: 4.7 GB decoded from 0.97 GB of BC (BC3 sRGB 2.5 GB, BC1 sRGB 1.3 GB at 8x,
 * BC3 0.6 GB), the driver within 2% of the decoded texels. The heap (~2.2 GB) stayed flat.
 *
 * Titanfall 2 on a 12 GB XRing O3 (MaliBridge#18): the system closed the server within 3-5 minutes,
 * 4 GB of RSS, 2.4 GB of images, and the heap from 0.8 to 1.9 GB in the last minute - which part of
 * the heap the line could not say. So it also has the memory free and the swap free, the
 * pipelines made since the last line and what the persistent pipeline caches hold in the driver
 * (the driver's own allocations, its compiler's among them, count in the heap figure too), and the
 * descriptor overflow pools (server_pools.c).
 *
 * Low memory (vkb_mem_low): MemAvailable under an eighth of RAM, until it is over a sixth again.
 * Entering it is said with what the server holds (the last lines in the log of a server the system
 * then closes); while it lasts, the allocator's free pages go back to the system every 30 s and
 * descriptor pool resets free their idle overflow pools. Neither changes what the app has.
 */
enum { CLS_PLAIN, CLS_HOSTPTR, CLS_DMABUF, CLS_AHB, CLS_DMAHEAP, CLS_EXPORT, CLS_N };
static const char *const cls_name[CLS_N] = {"plain", "hostptr", "dmabuf", "ahb", "dmaheap", "export"};
#define ST_TYPES 32
/* Processes whose memory is counted (the report's GPU memory per process). A Steam session keeps
 * eight Vulkan processes holding memory before any game (gamescope, two Xwaylands, two steam, two
 * steamwebhelper, mangoapp): with 8 slots a game found none and its report read 0 MB. As many as
 * the report follows (RP_PROCS); a slot is taken again once its process holds nothing. */
#define ST_PIDS 64
#define ST_TOP 4
typedef struct { int64_t bytes, n; } st_pair;
typedef struct { int64_t n, src, dec, drv; } st_bc; /* per app BC format: compressed, decoded texels, driver's size */
static struct {
    pthread_mutex_t lock;
    st_pair cls[CLS_N], type[ST_TYPES], ded[3];
    st_pair img, img_bc, img_bc_src, buf;
    st_bc bc[VK_FORMAT_BC7_SRGB_BLOCK - VK_FORMAT_BC1_RGB_UNORM_BLOCK + 1];
    struct { int pid; int64_t bytes; } pid[ST_PIDS];
    uint64_t allocs, frees;           /* cumulative */
    int64_t alloc_bytes, free_bytes;  /* cumulative */
    struct { int64_t size; uint8_t cls, ded; uint32_t type; int pid; } top[ST_TOP]; /* largest since the last line */
} st = {PTHREAD_MUTEX_INITIALIZER};
static pthread_once_t st_once = PTHREAD_ONCE_INIT;

static void st_pid_add(int pid, int64_t d)
{
    int free_slot = -1;
    for (int i = 0; i < ST_PIDS; i++) {
        if (st.pid[i].pid == pid) {
            st.pid[i].bytes += d;
            return;
        }
        if (free_slot < 0 && (!st.pid[i].pid || st.pid[i].bytes <= 0)) free_slot = i;
    }
    if (free_slot >= 0) st.pid[free_slot].pid = pid, st.pid[free_slot].bytes = d;
}

static void st_account(const vkb_srv_mem *m, int sign)
{
    int64_t d = sign * (int64_t)m->size;
    pthread_mutex_lock(&st.lock);
    st.cls[m->stat_cls].bytes += d, st.cls[m->stat_cls].n += sign;
    if (m->type < ST_TYPES) st.type[m->type].bytes += d, st.type[m->type].n += sign;
    st.ded[m->stat_ded].bytes += d, st.ded[m->stat_ded].n += sign;
    st_pid_add(m->stat_pid, d);
    if (sign > 0) {
        st.allocs++, st.alloc_bytes += d;
        int lo = 0;
        for (int i = 1; i < ST_TOP; i++)
            if (st.top[i].size < st.top[lo].size) lo = i;
        if ((int64_t)m->size > st.top[lo].size) {
            st.top[lo].size = (int64_t)m->size;
            st.top[lo].cls = m->stat_cls, st.top[lo].ded = m->stat_ded, st.top[lo].type = m->type, st.top[lo].pid = m->stat_pid;
        }
    } else {
        st.frees++, st.free_bytes -= d;
    }
    pthread_mutex_unlock(&st.lock);
}

uint8_t vkb_memstat_dedicated(const VkMemoryAllocateInfo *ai)
{
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            const VkMemoryDedicatedAllocateInfo *d = (const void *)b;
            return d->image ? 1 : d->buffer ? 2 : 0;
        }
    return 0;
}

void vkb_memstat_image(VkFormat bc, int64_t bytes, int64_t bc_src_bytes, int64_t bc_decoded_bytes)
{
    int sign = bytes < 0 || bc_src_bytes < 0 ? -1 : 1;
    pthread_mutex_lock(&st.lock);
    st.img.bytes += bytes, st.img.n += sign;
    if (bc >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && bc <= VK_FORMAT_BC7_SRGB_BLOCK) {
        st_bc *b = &st.bc[bc - VK_FORMAT_BC1_RGB_UNORM_BLOCK];
        b->n += sign, b->src += bc_src_bytes, b->dec += bc_decoded_bytes, b->drv += bytes;
        st.img_bc.bytes += bytes, st.img_bc.n += sign, st.img_bc_src.bytes += bc_src_bytes;
    }
    pthread_mutex_unlock(&st.lock);
}

void vkb_memstat_buffer(int64_t bytes)
{
    pthread_mutex_lock(&st.lock);
    st.buf.bytes += bytes, st.buf.n += bytes < 0 ? -1 : 1;
    pthread_mutex_unlock(&st.lock);
}

/* For the session report: one process's tracked memory, and the process-wide image totals. */
void vkb_memstat_query(int pid, int64_t *pid_bytes, int64_t *total, int64_t *images, int64_t *bc_images, int64_t *bc_src)
{
    pthread_mutex_lock(&st.lock);
    *pid_bytes = 0;
    for (int i = 0; i < ST_PIDS; i++)
        if (st.pid[i].pid == pid) *pid_bytes = st.pid[i].bytes;
    *total = 0;
    for (int i = 0; i < CLS_N; i++) *total += st.cls[i].bytes;
    *images = st.img.bytes;
    *bc_images = st.img_bc.bytes;
    *bc_src = st.img_bc_src.bytes;
    pthread_mutex_unlock(&st.lock);
}

#define MB(x) ((long long)((x) >> 20))

static long status_kb(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    return p ? strtol(p + strlen(key), NULL, 10) : 0;
}

static int mem_low_now; /* vkb_mem_low (atomic) */

int vkb_mem_low(void) { return __atomic_load_n(&mem_low_now, __ATOMIC_RELAXED); }

/* SwapFree in KB, 0 when unknown or none. */
static uint64_t swapfree_kb(void)
{
    char buf[4096];
    int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t k = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[k > 0 ? k : 0] = 0;
    long v = status_kb(buf, "SwapFree:");
    return v > 0 ? (uint64_t)v : 0;
}

/* The allocator's free pages back to the system (what the heap figure does not count). */
static void heap_purge(void)
{
#ifdef __ANDROID__
#ifdef M_PURGE_ALL
    if (mallopt(M_PURGE_ALL, 0) == 1) return; /* Android 14 and later */
#endif
    mallopt(M_PURGE, 0);
#else
    malloc_trim(0);
#endif
}

static void *st_thread(void *arg)
{
    const int lines = arg != NULL;
    uint64_t last_sig = 0, last_allocs = 0, last_frees = 0, last_made = 0;
    int64_t last_ab = 0, last_fb = 0;
    unsigned lows = 0;
    time_t last_purge = 0;
    for (;;) {
        sleep(5);
        uint64_t avail = vkb_memavail_kb(), total = vkb_memtotal_mb() << 10, swap = swapfree_kb();
        int low = vkb_mem_low(), entered = 0;
        if (avail && total) {
            if (!low && avail < total / 8) low = entered = 1;
            else if (low && avail > total / 6) low = 0;
        }
        if (low != vkb_mem_low()) {
            __atomic_store_n(&mem_low_now, low, __ATOMIC_RELAXED);
            if (!low) VKB_INFO("memory: %llu MB free of %llu MB again; idle descriptor pools are kept again", (unsigned long long)(avail >> 10),
                               (unsigned long long)(total >> 10));
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (low && (entered || now.tv_sec - last_purge >= 30)) {
            heap_purge();
            last_purge = now.tv_sec;
        }
        if (!lines && !entered) continue;
        char sbuf[4096] = "";
        int sfd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
        if (sfd >= 0) {
            ssize_t k = read(sfd, sbuf, sizeof(sbuf) - 1);
            sbuf[k > 0 ? k : 0] = 0;
            close(sfd);
        }
        long rss = status_kb(sbuf, "VmRSS:"), anon = status_kb(sbuf, "RssAnon:"), file = status_kb(sbuf, "RssFile:"),
             shm = status_kb(sbuf, "RssShmem:");
        int nfd = 0;
        DIR *dd = opendir("/proc/self/fd");
        if (dd) {
            while (readdir(dd)) nfd++;
            closedir(dd);
        }
        uint64_t made = 0, pc_bytes = 0, pools = vkb_pools_overflow_live();
        vkb_pcache_stats(&made, &pc_bytes);
        pthread_mutex_lock(&st.lock);
        uint64_t sig = (uint64_t)(rss >> 14) ^ (st.allocs * 0x9E3779B97F4A7C15ull) ^ (st.frees << 32) ^ (uint64_t)st.img.n * 31 ^ (uint64_t)st.buf.n ^
                       made * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)low << 63;
        if (sig == last_sig && !entered) {
            pthread_mutex_unlock(&st.lock);
            continue;
        }
        last_sig = sig;
        char line[3072];
        int n = 0;
#define ADD(...) n += snprintf(line + n, n < (int)sizeof(line) ? sizeof(line) - (size_t)n : 0, __VA_ARGS__)
        int64_t live = 0, nlive = 0;
        for (int i = 0; i < CLS_N; i++) live += st.cls[i].bytes, nlive += st.cls[i].n;
        struct mallinfo mi = mallinfo();
        long long heap = MB((int64_t)(size_t)mi.uordblks);
        ADD("mem: rss %ldM (anon %ldM file %ldM shmem %ldM) heap %lldM fds %d | free %lluM swap %lluM%s | live %lldM/%lld:", rss >> 10,
            anon >> 10, file >> 10, shm >> 10, heap, nfd - 2, (unsigned long long)(avail >> 10), (unsigned long long)(swap >> 10),
            low ? " (low)" : "", MB(live), (long long)nlive);
        for (int i = 0; i < CLS_N; i++)
            if (st.cls[i].n) ADD(" %s %lldM/%lld", cls_name[i], MB(st.cls[i].bytes), (long long)st.cls[i].n);
        ADD(" | types");
        for (int i = 0; i < ST_TYPES; i++)
            if (st.type[i].n) ADD(" %d:%lldM/%lld", i, MB(st.type[i].bytes), (long long)st.type[i].n);
        ADD(" | dedicated img %lldM/%lld buf %lldM/%lld", MB(st.ded[1].bytes), (long long)st.ded[1].n, MB(st.ded[2].bytes), (long long)st.ded[2].n);
        ADD(" | images %lldM/%lld (BC %lldM/%lld from %lldM compressed) buffers %lldM/%lld", MB(st.img.bytes), (long long)st.img.n,
            MB(st.img_bc.bytes), (long long)st.img_bc.n, MB(st.img_bc_src.bytes), MB(st.buf.bytes), (long long)st.buf.n);
        for (int i = 0; i <= VK_FORMAT_BC7_SRGB_BLOCK - VK_FORMAT_BC1_RGB_UNORM_BLOCK; i++)
            if (st.bc[i].n)
                ADD(" fmt%d %lld:%lldM>%lldM>%lldM", VK_FORMAT_BC1_RGB_UNORM_BLOCK + i, (long long)st.bc[i].n, MB(st.bc[i].src),
                    MB(st.bc[i].dec), MB(st.bc[i].drv));
        ADD(" | +%llu allocs %lldM, -%llu frees %lldM", (unsigned long long)(st.allocs - last_allocs), MB(st.alloc_bytes - last_ab),
            (unsigned long long)(st.frees - last_frees), MB(st.free_bytes - last_fb));
        last_allocs = st.allocs, last_frees = st.frees, last_ab = st.alloc_bytes, last_fb = st.free_bytes;
        ADD(" | largest");
        for (int i = 0; i < ST_TOP; i++)
            if (st.top[i].size)
                ADD(" %lldK(%s t%u%s pid %d)", (long long)(st.top[i].size >> 10), cls_name[st.top[i].cls], st.top[i].type,
                    st.top[i].ded == 1 ? " img" : st.top[i].ded == 2 ? " buf" : "", st.top[i].pid);
        memset(st.top, 0, sizeof(st.top));
        ADD(" | pids");
        for (int i = 0; i < ST_PIDS; i++)
            if (st.pid[i].pid && st.pid[i].bytes > 0) ADD(" %d:%lldM", st.pid[i].pid, MB(st.pid[i].bytes));
        ADD(" | pipelines +%llu (%llu), pipeline caches %lluK, desc overflow pools %llu", (unsigned long long)(made - last_made),
            (unsigned long long)made, (unsigned long long)(pc_bytes >> 10), (unsigned long long)pools);
        last_made = made;
        int64_t gpu = live;
#undef ADD
        pthread_mutex_unlock(&st.lock);
        if (lines) VKB_INFO("%s | bc scratch %lldM", line, MB(vkb_emu_bcn_scratch_live()));
        if (entered && ++lows <= 4)
            VKB_WARN("memory low: %llu MB free of %llu MB (swap free %llu MB); the server holds %ld MB (GPU memory %lld MB, heap %lld MB, "
                     "pipeline caches %llu MB): free heap pages go back to the system and idle descriptor pools are freed; the system "
                     "may close apps, and the bridge with the game%s",
                     (unsigned long long)(avail >> 10), (unsigned long long)(total >> 10), (unsigned long long)(swap >> 10), rss >> 10, MB(gpu),
                     heap, (unsigned long long)(pc_bytes >> 20), lows == 4 ? " (said 4 times, not again)" : "");
    }
    return NULL;
}

/* The thread watches for low memory with the lines off too. */
static void st_start(void)
{
    const char *e = vkb_srv_env_global("VKBRIDGE_MEM_STATS");
    pthread_t th;
    if (pthread_create(&th, NULL, st_thread, e && *e == '0' ? NULL : (void *)1) == 0) pthread_detach(th);
}

void vkb_mem_track(vkb_srv_table *dev, vkb_srv_mem *m)
{
    pthread_once(&st_once, st_start);
    m->stat_cls = m->strategy && m->strategy < CLS_EXPORT ? (uint8_t)m->strategy : m->export_fd1 ? CLS_EXPORT : CLS_PLAIN;
    m->stat_pid = dev->proc ? dev->proc->pid : 0;
    st_account(m, 1);
    pthread_mutex_lock(&dev->lock);
    unsigned h = mem_hash(m->mem);
    m->next = dev->mems[h];
    dev->mems[h] = m;
    pthread_mutex_unlock(&dev->lock);
}

vkb_srv_mem *vkb_mem_find(vkb_srv_table *dev, VkDeviceMemory mem, int remove)
{
    pthread_mutex_lock(&dev->lock);
    vkb_srv_mem **pp = &dev->mems[mem_hash(mem)];
    while (*pp && (*pp)->mem != mem) pp = &(*pp)->next;
    vkb_srv_mem *m = *pp;
    if (m && remove) *pp = m->next;
    pthread_mutex_unlock(&dev->lock);
    return m;
}

/* Frees the server-side resources of an allocation (the VkDeviceMemory must already be freed). */
static void mem_release_backing(vkb_srv_mem *m)
{
    if (m->host_ptr) munmap(m->host_ptr, m->host_len);
#ifdef __ANDROID__
    if (m->ahb) p_AHardwareBuffer_release(m->ahb);
#endif
    if (m->export_fd1) close(m->export_fd1 - 1);
    if (m->sync_fd1) close(m->sync_fd1 - 1);
    free(m);
}

void vkb_mem_release(vkb_srv_table *dev, vkb_srv_mem *m)
{
    st_account(m, -1);
    if (m->mapped) dev->real.vkUnmapMemory(dev->device, m->mem);
    dev->real.vkFreeMemory(dev->device, m->mem, NULL);
    mem_release_backing(m);
}

void vkb_mem_free_all(vkb_srv_table *dev)
{
    for (int i = 0; i < 256; i++) {
        vkb_srv_mem *m = dev->mems[i];
        dev->mems[i] = NULL;
        while (m) {
            vkb_srv_mem *n = m->next;
            vkb_mem_release(dev, m);
            m = n;
        }
    }
}

/* The dma-heap the dmaheap strategy allocates from, opened once: system-uncached first (the GPU
 * and the client both write through it without cache maintenance), else the cached system heap.
 * VKBRIDGE_FAKE_MISSING=dmaheap (device tests): none, as on the phones without /dev/dma_heap
 * (Retroid Pocket 4 Pro, Kirin 985; MaliBridge#10). Read once per server start. */
static int dmaheap_fd(void)
{
    static int fd = -2;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (fd == -2) {
        const char *names[] = {"/dev/dma_heap/system-uncached", "/dev/dma_heap/system"};
        if (vkb_srv_env_global("VKBRIDGE_DMAHEAP_CACHED")) names[0] = "/dev/dma_heap/system";
        fd = -1;
        if (vkb_emu_faked("dmaheap")) VKB_INFO("dmaheap: none, as on a phone without /dev/dma_heap (VKBRIDGE_FAKE_MISSING=dmaheap)");
        for (int i = 0; i < 2 && fd < 0 && !vkb_emu_faked("dmaheap"); i++) {
            fd = open(names[i], O_RDONLY | O_CLOEXEC);
            if (fd >= 0) VKB_INFO("dmaheap: allocating from %s", names[i]);
            else VKB_INFO("dmaheap: %s: %s", names[i], strerror(errno));
        }
    }
    int r = fd;
    pthread_mutex_unlock(&lock);
    return r;
}

int vkb_mem_dmaheap_available(void) { return dmaheap_fd() >= 0; }

/* The cached system heap, for cached readback (opened once; -1 when there is none). */
static int dmaheap_cached_fd(void)
{
    static int fd = -2;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (fd == -2) {
        fd = vkb_emu_faked("dmaheap") ? (errno = ENOENT, -1) : open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) VKB_INFO("dmaheap: cached readback allocates from /dev/dma_heap/system");
        else VKB_INFO("dmaheap: /dev/dma_heap/system: %s (no cached readback)", strerror(errno));
    }
    int r = fd;
    pthread_mutex_unlock(&lock);
    return r;
}

/* ------------------------------------------------------------------ cached readback: cache maintenance */

/* What the CPU cache maintenance did, process-wide (the host suite reads it from the log). */
static struct {
    uint64_t flushes, invalidates, failures;
} sync_st;

/*
 * One DMA_BUF_IOCTL_SYNC bracket on a cached shared dma-buf: START then END with the same
 * direction, as Cimo's vtest renderer does. Cache maintenance acts on the pages, not on a mapping,
 * so the server's call covers the client's mapping too. What the kernel does (arm64, a non-snooping
 * GPU; the system heap syncs each attachment the driver has mapped, and an attachment mapped only
 * while the GPU uses it is synced on map and unmap):
 *   write (flush):      START is a no-op, END cleans the CPU's dirty lines to memory;
 *   read (invalidate):  START invalidates the CPU's lines, END invalidates them again.
 */
static int dmabuf_sync(vkb_srv_mem *m, int write)
{
    if (!m->sync_fd1) return 0;
    uint64_t dir = write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ;
    struct dma_buf_sync sync = {DMA_BUF_SYNC_START | dir};
    int r;
    do r = ioctl(m->sync_fd1 - 1, DMA_BUF_IOCTL_SYNC, &sync);
    while (r < 0 && (errno == EINTR || errno == EAGAIN));
    if (r == 0) {
        sync.flags = DMA_BUF_SYNC_END | dir;
        do r = ioctl(m->sync_fd1 - 1, DMA_BUF_IOCTL_SYNC, &sync);
        while (r < 0 && (errno == EINTR || errno == EAGAIN));
    }
    if (r < 0) {
        if (__atomic_fetch_add(&sync_st.failures, 1, __ATOMIC_RELAXED) < 8)
            VKB_WARN("cached readback: DMA_BUF_IOCTL_SYNC (%s) failed: %s", write ? "flush" : "invalidate", strerror(errno));
        return -1;
    }
    uint64_t n = __atomic_add_fetch(write ? &sync_st.flushes : &sync_st.invalidates, 1, __ATOMIC_RELAXED);
    if (n == 1 || (n & (n - 1)) == 0) /* the 1st, 2nd, 4th, 8th, ... */
        VKB_INFO("cached readback: %llu %s so far (DMA_BUF_IOCTL_SYNC; %llu bytes this one)", (unsigned long long)n,
                 write ? "flushes" : "invalidates", (unsigned long long)m->size);
    return 0;
}

/* Unlinks every structure of type t from the chain starting at *head. */
static void chain_drop(const void **head, VkStructureType t)
{
    const VkBaseInStructure **pp = (const VkBaseInStructure **)head;
    while (*pp) {
        if ((*pp)->sType == t) *pp = (*pp)->pNext;
        else pp = (const VkBaseInStructure **)&((VkBaseInStructure *)*pp)->pNext;
    }
}

/* The driver's vkGetDeviceProcAddr (for entry points the dispatch table does not carry). */
PFN_vkGetDeviceProcAddr vkb_mem_gdpa;

/* ------------------------------------------------------------------ allocation */

/*
 * Allocates shared memory with strategy `s`. `ai` is the app's (or the self-test's) allocate
 * info; for hostptr, `client_fd` is the client's memfd (consumed). On success *out_fd is the fd
 * to send to the client (-1 for hostptr, where the client already has its pages).
 */
static VkResult alloc_shared(const vkb_dispatch *dt, VkDevice dev, uint32_t s, const VkMemoryAllocateInfo *ai,
                             int client_fd, int cached, vkb_srv_mem **out, int *out_fd)
{
    *out_fd = -1;
    *out = NULL;
    vkb_srv_mem *m = calloc(1, sizeof(*m));
    if (!m) {
        if (client_fd >= 0) close(client_fd);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    m->size = ai->allocationSize;
    m->type = ai->memoryTypeIndex;
    m->strategy = s;
    VkMemoryAllocateInfo info = *ai;
    VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    switch (s) {
    case VKB_MEM_HOSTPTR: {
        /* No dma-buf to sync through: never cached readback (the self-test does not offer it). */
        if (client_fd < 0 || cached) break;
        void *p = mmap(NULL, (size_t)ai->allocationSize, PROT_READ | PROT_WRITE, MAP_SHARED, client_fd, 0);
        close(client_fd);
        client_fd = -1;
        if (p == MAP_FAILED) {
            VKB_ERR("hostptr: mmap of the client's memory failed: %s", strerror(errno));
            r = VK_ERROR_OUT_OF_HOST_MEMORY;
            break;
        }
        m->host_ptr = p;
        m->host_len = (size_t)ai->allocationSize;
        VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        if (!dt->vkGetMemoryHostPointerPropertiesEXT ||
            dt->vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &hp) != VK_SUCCESS ||
            !(hp.memoryTypeBits & (1u << ai->memoryTypeIndex))) {
            /* Said, so a log shows which types the driver takes host pointers in (Mali: cached
             * ones only, it seems - a Pixel 9 refused type 0, its uncached coherent type). */
            VKB_INFO("hostptr: host pointer properties %s, memory types 0x%x (want type %u)",
                     dt->vkGetMemoryHostPointerPropertiesEXT ? "read" : "not loaded", hp.memoryTypeBits, ai->memoryTypeIndex);
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            break;
        }
        VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, info.pNext,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p};
        info.pNext = &imp;
        r = dt->vkAllocateMemory(dev, &info, NULL, &m->mem);
        /* The driver took the pointer's types but not the pages: a memfd is a shared file
         * mapping, and some drivers import only anonymous private memory (RADV on the host
         * refuses it so; whether Mali does is open). */
        if (r != VK_SUCCESS)
            VKB_INFO("hostptr: import of %llu bytes of the client's memfd (type %u) failed: %d", (unsigned long long)info.allocationSize,
                     info.memoryTypeIndex, r);
        break;
    }
    case VKB_MEM_DMABUF: {
        VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, info.pNext,
                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        /* An export the app asked for itself is merged rather than duplicated. */
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)info.pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
                ((VkExportMemoryAllocateInfo *)b)->handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                exp.sType = VK_STRUCTURE_TYPE_MAX_ENUM;
            }
        }
        if (exp.sType != VK_STRUCTURE_TYPE_MAX_ENUM) info.pNext = &exp;
        r = dt->vkAllocateMemory(dev, &info, NULL, &m->mem);
        if (r != VK_SUCCESS) {
            VKB_INFO("dmabuf: exportable allocation of %llu bytes (type %u) failed: %d", (unsigned long long)info.allocationSize,
                    info.memoryTypeIndex, r);
            break;
        }
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, m->mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        int fd = -1;
        VkResult fr = dt->vkGetMemoryFdKHR ? dt->vkGetMemoryFdKHR(dev, &gi, &fd) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (fr != VK_SUCCESS || fd < 0) {
            VKB_INFO("dmabuf: vkGetMemoryFdKHR %s: %d (fd %d)", dt->vkGetMemoryFdKHR ? "failed" : "not loaded", fr, fd);
            dt->vkFreeMemory(dev, m->mem, NULL);
            m->mem = VK_NULL_HANDLE;
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            break;
        }
        if (cached) m->sync_fd1 = fcntl(fd, F_DUPFD_CLOEXEC, 0) + 1;
        *out_fd = fd;
        break;
    }
    case VKB_MEM_DMAHEAP: {
        int hfd = cached ? dmaheap_cached_fd() : dmaheap_fd();
        if (hfd < 0) break;
        uint64_t len = (ai->allocationSize + 4095) & ~4095ull;
        struct dma_heap_allocation_data hd = {len, 0, O_RDWR | O_CLOEXEC, 0};
        if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &hd) < 0) {
            VKB_INFO("dmaheap: allocating %llu bytes failed: %s", (unsigned long long)len, strerror(errno));
            r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            break;
        }
        int fd = (int)hd.fd;
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        VkResult pr = dt->vkGetMemoryFdPropertiesKHR
                          ? dt->vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp)
                          : VK_ERROR_EXTENSION_NOT_PRESENT;
        /* VKBRIDGE_DMAHEAP_ANY_TYPE=1 (an experiment): imported even into a type the driver's fd
         * properties leave out, as virglrenderer's Android path does (Cimo's droiddeck-mali imports
         * dma-heap pages into the Mali-G715's uncached type 0 with no such check, and it ran there).
         * Out of the spec; the self-test still checks the bytes both ways before a type is used. */
        static int any_type = -1;
        if (any_type < 0) any_type = vkb_srv_env_global("VKBRIDGE_DMAHEAP_ANY_TYPE") && *vkb_srv_env_global("VKBRIDGE_DMAHEAP_ANY_TYPE") == '1';
        if (pr != VK_SUCCESS || !(fp.memoryTypeBits & (1u << ai->memoryTypeIndex))) {
            VKB_INFO("dmaheap: fd properties %d, memory types 0x%x (want type %u)%s", pr, fp.memoryTypeBits, ai->memoryTypeIndex,
                     any_type ? "; importing anyway (VKBRIDGE_DMAHEAP_ANY_TYPE)" : "");
            if (!any_type) {
                close(fd);
                r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                break;
            }
        }
        /* The import takes its own fd; ours goes to the client. A dedicated-allocation struct
         * with an imported dma-buf crashes some Mali drivers (GameNative mesa PR #8): dropped. */
        int imp_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, info.pNext,
                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, imp_fd};
        chain_drop(&imp.pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
        chain_drop(&imp.pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
        info.pNext = &imp;
        info.allocationSize = len;
        r = imp_fd >= 0 ? dt->vkAllocateMemory(dev, &info, NULL, &m->mem) : VK_ERROR_TOO_MANY_OBJECTS;
        if (r != VK_SUCCESS) {
            VKB_INFO("dmaheap: import of %llu bytes failed: %d", (unsigned long long)len, r);
            if (imp_fd >= 0) close(imp_fd); /* a failed import leaves the fd with us */
            close(fd);
            break;
        }
        if (cached) m->sync_fd1 = fcntl(fd, F_DUPFD_CLOEXEC, 0) + 1;
        *out_fd = fd;
        break;
    }
#ifdef __ANDROID__
    case VKB_MEM_AHB: {
        if (!ahb_load()) break;
        /* Through the device's own vkGetDeviceProcAddr: Android's loader answers NULL for it from
         * vkGetInstanceProcAddr(NULL, ...), which desktop loaders allow (the first device run). */
        PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_props = NULL;
        if (vkb_mem_gdpa)
            get_props = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkb_mem_gdpa(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
        if (!get_props) {
            VKB_INFO("ahb: vkGetAndroidHardwareBufferPropertiesANDROID not found");
            break;
        }
        uint64_t sz = ai->allocationSize;
        if (sz > 0xFFFFFFFFull) {
            r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            break;
        }
        /* CPU usage decides how gralloc caches the pages: uncached where the self-test needed it
         * (ahb_rarely) or VKBRIDGE_AHB_RARELY=1 asks. */
        uint64_t cpu = ahb_uncached() ? (2ull | (2ull << 4)) : (AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN);
        vkb_ahb_desc d = {(uint32_t)sz, 1, 1, AHB_FORMAT_BLOB, cpu | AHB_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
        struct AHardwareBuffer *ahb = NULL;
        int ar = p_AHardwareBuffer_allocate(&d, &ahb);
        if (ar != 0 || !ahb) {
            VKB_INFO("ahb: AHardwareBuffer_allocate(%u bytes) failed: %d", (unsigned)sz, ar);
            r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            break;
        }
        m->ahb = ahb;
        VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        VkResult pr = get_props(dev, ahb, &props);
        if (pr != VK_SUCCESS || !(props.memoryTypeBits & (1u << ai->memoryTypeIndex))) {
            VKB_INFO("ahb: properties %d, memory types 0x%x (want type %u)", pr, props.memoryTypeBits, ai->memoryTypeIndex);
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            break;
        }
        VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, info.pNext, ahb};
        info.pNext = &imp;
        info.allocationSize = props.allocationSize;
        r = dt->vkAllocateMemory(dev, &info, NULL, &m->mem);
        if (r != VK_SUCCESS) {
            VKB_INFO("ahb: import of %llu bytes failed: %d", (unsigned long long)info.allocationSize, r);
            break;
        }
        const vkb_native_handle *nh = p_AHardwareBuffer_getNativeHandle(ahb);
        if (!nh || nh->numFds < 1) {
            VKB_INFO("ahb: no fd in the native handle (%d)", nh ? nh->numFds : -1);
            dt->vkFreeMemory(dev, m->mem, NULL);
            m->mem = VK_NULL_HANDLE;
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            break;
        }
        int pick = ahb_pages_fd(nh, sz);
        if (pick < 0) {
            VKB_INFO("ahb: no fd in the handle covers %llu bytes", (unsigned long long)sz);
            dt->vkFreeMemory(dev, m->mem, NULL);
            m->mem = VK_NULL_HANDLE;
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            break;
        }
        *out_fd = fcntl(pick, F_DUPFD_CLOEXEC, 0);
        if (cached && *out_fd >= 0) m->sync_fd1 = fcntl(pick, F_DUPFD_CLOEXEC, 0) + 1;
        break;
    }
#endif
    default:
        break;
    }
    if (client_fd >= 0) close(client_fd);
    if (r == VK_SUCCESS && cached && !m->sync_fd1) {
        /* Cached pages with no dma-buf to sync through would be wrong: not made. */
        VKB_WARN("cached readback: no dma-buf kept for the cache maintenance; allocation refused");
        dt->vkFreeMemory(dev, m->mem, NULL);
        m->mem = VK_NULL_HANDLE;
        if (*out_fd >= 0) close(*out_fd);
        *out_fd = -1;
        r = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (r != VK_SUCCESS) {
        mem_release_backing(m);
        return r;
    }
    *out = m;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ dma-buf export of images */

/*
 * Mali imports dma-bufs for images (every modifier) but exports none; gamescope's nested backend
 * hands its output images to the compositor as dma-bufs and gives up when no modifier is
 * exportable. Emulated (VKB_EMU_DMABUF_EXPORT): an allocation that asks to export a dma-buf gets
 * pages from the dma-heap, imported into the driver; vkGetMemoryFdKHR hands out those pages. The
 * driver keeps choosing the image's layout; the importer learns it from the modifier as usual.
 *
 * Phones without /dev/dma_heap (Retroid Pocket 4 Pro's Dimensity 1100 and Huawei's Kirin 985,
 * Mali-G77 r32p1/r34p0; MaliBridge#10: gamescope stopped at "no exportable modifier"): the pages
 * come from a BLOB AHardwareBuffer instead, its dma-buf imported as any other. The buffer is kept
 * with the memory (m->ahb) and released after it. Offered only once a real one has been allocated,
 * imported and mapped at start (ahb_export_check): a gralloc that refuses any of it leaves the
 * export unclaimed, as before, instead of promising gamescope modifiers it cannot have.
 */

/* The AHardwareBuffer path, checked once at start by the memory self-test (ahb_export_check)
 * before gamescope is told it can export: -1 not checked, 0 failed, 1 passed; the CPU usage that
 * passed (the export emulation allocates with it); what the check saw, for the probe and the report. */
static char ahb_export_text[360];
#ifdef __ANDROID__
static int ahb_export_ok = -1;
static uint64_t ahb_export_cpu = 2ull | (2ull << 4);
#endif

/* Host tests only (VKBRIDGE_TEST_EXPORT_PAGES=driver): the desktop's dma-heap is root's, so the
 * emulation's pages are a dma-buf the driver itself exports from a plain allocation (made, exported,
 * freed: the fd keeps the pages). It lets the export emulation run on the host with
 * VKBRIDGE_FAKE_MISSING=dmabufexport; never on Android. */
static int test_driver_pages(void)
{
#ifdef __ANDROID__
    return 0;
#else
    static int on = -1;
    if (on < 0) {
        const char *v = vkb_srv_env_global("VKBRIDGE_TEST_EXPORT_PAGES");
        on = v && !strcmp(v, "driver");
    }
    return on;
#endif
}

/* Where the emulation's pages come from: 1 the dma-heap, 2 AHardwareBuffers (Android, where the
 * start's check passed), 3 the driver's own dma-bufs (host tests), 0 nowhere. */
int vkb_mem_export_backing(void)
{
    if (vkb_mem_dmaheap_available()) return 1;
#ifdef __ANDROID__
    if (ahb_export_ok == 1 && ahb_load()) return 2;
#endif
    if (test_driver_pages()) return 3;
    return 0;
}

const char *vkb_mem_ahb_export_result(void)
{
    return ahb_export_text;
}

const char *vkb_mem_export_backing_name(void)
{
    int b = vkb_mem_export_backing();
    return b == 1 ? "dma-heap" : b == 2 ? "AHardwareBuffer" : b == 3 ? "driver's own dma-bufs (host test)" : "none";
}

/* VKBRIDGE_TEST_EXPORT_PAGES=driver: len bytes the driver allocates in memory type type and exports. */
static int driver_pages(vkb_srv_table *t, uint64_t len, uint32_t type)
{
    const vkb_dispatch *dt = &t->real;
    if (!dt->vkGetMemoryFdKHR) return -1;
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, len, type};
    VkDeviceMemory mem;
    if (dt->vkAllocateMemory(t->device, &ai, NULL, &mem) != VK_SUCCESS) return -1;
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    if (dt->vkGetMemoryFdKHR(t->device, &gi, &fd) != VK_SUCCESS) fd = -1;
    dt->vkFreeMemory(t->device, mem, NULL);
    VKB_ONCE(VKB_LOG_INFO, "export emulation: pages from the driver's own dma-bufs (VKBRIDGE_TEST_EXPORT_PAGES=driver, host test)");
    return fd;
}

/* A dma-buf of len bytes for the emulation (the caller owns the fd), -1 on failure. */
static int export_pages(vkb_srv_table *t, uint64_t len, uint32_t type, vkb_srv_mem *m)
{
    if (vkb_mem_export_backing() == 3) return driver_pages(t, len, type);
    (void)t;
    (void)type;
    int hfd = dmaheap_fd();
    if (hfd >= 0) {
        struct dma_heap_allocation_data hd = {len, 0, O_RDWR | O_CLOEXEC, 0};
        if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &hd) < 0) {
            VKB_ERR("export emulation: dma-heap allocation of %llu bytes failed: %s", (unsigned long long)len, strerror(errno));
            return -1;
        }
        return (int)hd.fd;
    }
#ifdef __ANDROID__
    if (!ahb_load() || len > 0xFFFFFFFFull) return -1;
    /* Uncached for the CPU, as the system-uncached heap's pages: the importer reads them by the GPU,
     * and a CPU read (the dump-exports evidence) needs no cache maintenance. Cached where the start's
     * check found the gralloc refusing "rarely" (ahb_export_check). */
    vkb_ahb_desc d = {(uint32_t)len, 1, 1, AHB_FORMAT_BLOB, ahb_export_cpu | AHB_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    struct AHardwareBuffer *ahb = NULL;
    int ar = p_AHardwareBuffer_allocate(&d, &ahb);
    if (ar != 0 || !ahb) {
        VKB_ERR("export emulation: AHardwareBuffer_allocate(%llu bytes) failed: %d", (unsigned long long)len, ar);
        return -1;
    }
    const vkb_native_handle *nh = p_AHardwareBuffer_getNativeHandle(ahb);
    VKB_ONCE(VKB_LOG_INFO, "export emulation: first AHardwareBuffer: %llu bytes, format 0x%x, usage 0x%llx; %d fd(s), %d int(s) in its handle",
             (unsigned long long)len, d.format, (unsigned long long)d.usage, nh ? nh->numFds : -1, nh ? nh->numInts : -1);
    int pick = ahb_pages_fd(nh, len);
    int fd = pick >= 0 ? fcntl(pick, F_DUPFD_CLOEXEC, 0) : -1;
    if (fd < 0) {
        VKB_ERR("export emulation: no dma-buf of %llu bytes in the AHardwareBuffer's handle", (unsigned long long)len);
        p_AHardwareBuffer_release(ahb);
        return -1;
    }
    m->ahb = ahb;
    return fd;
#else
    (void)m;
    return -1;
#endif
}

/* Undoes export_pages after a failed import. */
static void export_pages_drop(int fd, vkb_srv_mem *m)
{
    close(fd);
#ifdef __ANDROID__
    if (m->ahb) p_AHardwareBuffer_release(m->ahb);
#endif
    m->ahb = NULL;
}
static int wants_dmabuf_export(const VkMemoryAllocateInfo *ai)
{
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO &&
            (((const VkExportMemoryAllocateInfo *)b)->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT))
            return 1;
    return 0;
}

static VkResult alloc_exportable(vkb_srv_table *t, VkMemoryAllocateInfo *ai, vkb_srv_mem *m)
{
    const vkb_dispatch *dt = &t->real;
    uint64_t len = (ai->allocationSize + 4095) & ~4095ull;
    int fd = export_pages(t, len, ai->memoryTypeIndex, m);
    if (fd < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (!dt->vkGetMemoryFdPropertiesKHR ||
        dt->vkGetMemoryFdPropertiesKHR(t->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) != VK_SUCCESS)
        fp.memoryTypeBits = 0;
    /* A type both the dma-buf and the resource accept (the dedicated image or buffer says which). */
    uint32_t want = UINT32_MAX;
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) continue;
        const VkMemoryDedicatedAllocateInfo *d = (const void *)b;
        VkMemoryRequirements req;
        if (d->image) {
            dt->vkGetImageMemoryRequirements(t->device, d->image, &req);
            want = req.memoryTypeBits;
        } else if (d->buffer) {
            dt->vkGetBufferMemoryRequirements(t->device, d->buffer, &req);
            want = req.memoryTypeBits;
        }
    }
    /* Never a lazily allocated or protected type: ordinary memory cannot live there (Mali's only
     * device-local type without host access is lazily allocated; allocations in it fail). */
    VkPhysicalDeviceMemoryProperties mp;
    t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    uint32_t ordinary = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (!(mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT))) ordinary |= 1u << i;
    uint32_t bits = fp.memoryTypeBits & want & ordinary;
    if (!bits) {
        VKB_ERR("export emulation: no ordinary memory type takes both the dma-buf (0x%x) and the resource (0x%x)", fp.memoryTypeBits, want);
        export_pages_drop(fd, m);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (!(bits & (1u << ai->memoryTypeIndex))) {
        uint32_t to = (uint32_t)__builtin_ctz(bits);
        VKB_INFO("export emulation: type %u does not take the dma-buf (types 0x%x); type %u (flags 0x%x) instead", ai->memoryTypeIndex,
                 fp.memoryTypeBits, to, mp.memoryTypes[to].propertyFlags);
        ai->memoryTypeIndex = to;
    }
    int imp_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, ai->pNext, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, imp_fd};
    chain_drop(&imp.pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    VkMemoryAllocateInfo info = *ai;
    info.pNext = &imp;
    info.allocationSize = len;
    VkResult r = imp_fd >= 0 ? dt->vkAllocateMemory(t->device, &info, NULL, &m->mem) : VK_ERROR_TOO_MANY_OBJECTS;
    if (r != VK_SUCCESS && imp_fd >= 0) {
        /* Some Mali drivers reject (or crash on) a dedicated struct with a dma-buf import. */
        chain_drop(&imp.pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
        r = dt->vkAllocateMemory(t->device, &info, NULL, &m->mem);
    }
    if (r != VK_SUCCESS) {
        VKB_ERR("export emulation: importing %llu bytes (type %u) failed: %d", (unsigned long long)len, info.memoryTypeIndex, r);
        if (imp_fd >= 0) close(imp_fd);
        export_pages_drop(fd, m);
        return r;
    }
    m->export_fd1 = fd + 1;
    m->type = info.memoryTypeIndex;
    {
        VkImage ded_img = VK_NULL_HANDLE;
        for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) ded_img = ((const VkMemoryDedicatedAllocateInfo *)b)->image;
        int still_dedicated = 0;
        for (const VkBaseInStructure *b = imp.pNext; b; b = b->pNext) still_dedicated |= b->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        struct stat fst;
        VKB_INFO("export emulation: memory %llx (%llu bytes, pid %d, dma-buf ino %lu) for image %llx%s", (unsigned long long)(uintptr_t)m->mem,
                 (unsigned long long)len, t->proc ? t->proc->pid : 0, fstat(fd, &fst) == 0 ? (unsigned long)fst.st_ino : 0UL,
                 (unsigned long long)(uintptr_t)ded_img,
                 still_dedicated ? "" : " (imported without the dedicated struct)");
    }
    VKB_DBG("export emulation: %llu bytes from the %s, type %u", (unsigned long long)len, m->ahb ? "AHardwareBuffer" : "dma-heap",
            info.memoryTypeIndex);
    return VK_SUCCESS;
}

/* Evidence: with <cache dir>/dump-exports present, every exported allocation of 1 MB or more is
 * remembered (a dup of its dma-buf), and touching <cache dir>/dump-exports/now writes what all of
 * them hold at that moment, read through the dma-buf from the CPU (tablet run 13: gamescope's
 * frames reached the screen black; this shows at which buffer the picture is lost). */
#define EXPORT_SLOTS 96
typedef struct {
    int fd;
    size_t len;
    int pid;
} export_slot;
static export_slot export_slots[EXPORT_SLOTS];
static unsigned export_count;
static pthread_mutex_t export_lock = PTHREAD_MUTEX_INITIALIZER;

static void *export_watch_thread(void *arg)
{
    (void)arg;
    char trigger[600];
    snprintf(trigger, sizeof(trigger), "%s/dump-exports/now", vkb_pcache_dir());
    for (unsigned round = 0;; ) {
        sleep(1);
        if (access(trigger, F_OK) != 0) continue;
        unlink(trigger);
        pthread_mutex_lock(&export_lock);
        unsigned n = export_count < EXPORT_SLOTS ? export_count : EXPORT_SLOTS;
        for (unsigned i = 0; i < n; i++) {
            export_slot *e = &export_slots[i];
            void *p = mmap(NULL, e->len, PROT_READ, MAP_SHARED, e->fd, 0);
            if (p == MAP_FAILED) continue;
            char path[700];
            snprintf(path, sizeof(path), "%s/dump-exports/r%u-%02u-pid%d-%zu.raw", vkb_pcache_dir(), round, i, e->pid, e->len);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(p, 1, e->len, f);
                fclose(f);
            }
            munmap(p, e->len);
        }
        pthread_mutex_unlock(&export_lock);
        VKB_INFO("export dump round %u: %u allocations written", round, n);
        round++;
    }
    return NULL;
}

static void maybe_dump_export(int fd, size_t len, int pid)
{
    static int enabled = -1;
    if (enabled < 0) {
        char path[600];
        const char *dir = vkb_pcache_dir();
        snprintf(path, sizeof(path), "%s/dump-exports", dir ? dir : "");
        enabled = dir && access(path, F_OK) == 0;
        pthread_t th;
        if (enabled && pthread_create(&th, NULL, export_watch_thread, NULL) == 0) pthread_detach(th);
    }
    if (!enabled || len < (1u << 20)) return;
    pthread_mutex_lock(&export_lock);
    for (unsigned i = 0; i < export_count && i < EXPORT_SLOTS; i++)
        if (export_slots[i].len == len && export_slots[i].pid == pid) {
            struct stat a, b;
            if (fstat(export_slots[i].fd, &a) == 0 && fstat(fd, &b) == 0 && a.st_ino == b.st_ino) {
                pthread_mutex_unlock(&export_lock);
                return; /* the same buffer exported again */
            }
        }
    if (export_count < EXPORT_SLOTS) {
        int d = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (d >= 0) export_slots[export_count++] = (export_slot){d, len, pid};
    }
    pthread_mutex_unlock(&export_lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL exp_GetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *gi, int *pFd)
{
    vkb_srv_table *t = vkb_srv_current_table();
    vkb_srv_mem *m = t && gi ? vkb_mem_find(t, gi->memory, 0) : NULL;
    VkMemoryGetFdInfoKHR as_dmabuf;
    if (gi) gi = vkb_opaque_get_fd_info(t, gi, &as_dmabuf); /* OPAQUE_FD: the dma-buf (server_opaque.c) */
    if (m && m->export_fd1 && gi->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
        *pFd = fcntl(m->export_fd1 - 1, F_DUPFD_CLOEXEC, 0);
        if (*pFd >= 0) maybe_dump_export(m->export_fd1 - 1, (size_t)((m->size + 4095) & ~4095ull), t->proc ? t->proc->pid : 0);
        return *pFd >= 0 ? VK_SUCCESS : VK_ERROR_TOO_MANY_OBJECTS;
    }
    VkResult r = t->real.vkGetMemoryFdKHR(device, gi, pFd);
    static int told;
    if (r != VK_SUCCESS && __atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 5)
        VKB_ERR("vkGetMemoryFdKHR (handle type 0x%x) failed: %d", gi ? gi->handleType : 0, r);
    return r;
}

void vkb_mem_export_install(vkb_srv_table *t)
{
    if (t->pd && (t->pd->info.emu & (VKB_EMU_DMABUF_EXPORT | VKB_EMU_OPAQUE_FD)) && t->real.vkGetMemoryFdKHR)
        t->dt.vkGetMemoryFdKHR = exp_GetMemoryFdKHR;
}

/* ------------------------------------------------------------------ Mali's memory types, imitated */

/*
 * VKBRIDGE_FAKE_MISSING=...,lazymem (host tests): the memory types Mali reports. Its only
 * device-local type without host access is LAZILY_ALLOCATED (G720 and G715: flags 0x11), and an
 * ordinary resource allocated there fails with VK_ERROR_OUT_OF_DEVICE_MEMORY. A client that took
 * that type for the device's ordinary memory had nowhere to put images (tablet run 3, and a Pixel 9
 * on an older build, 2026-10-13: the self-test's image checks failed at "setup", gamescope got
 * vkAllocateMemory -2). Here the desktop driver's device-local types without host access say
 * LAZILY_ALLOCATED to the client, and the client's allocations in them are refused so.
 */
static int lazy_imitated(VkMemoryPropertyFlags f)
{
    return (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(f & VK_MEMORY_PROPERTY_PROTECTED_BIT);
}

static int fake_lazy(void)
{
    static int v = -1;
    if (v < 0) v = vkb_emu_faked("lazymem") || vkb_emu_faked("xringmem") || vkb_emu_faked("g720mem");
    return v;
}

static void fake_lazy_props(VkPhysicalDeviceMemoryProperties *mp)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (lazy_imitated(mp->memoryTypes[i].propertyFlags)) mp->memoryTypes[i].propertyFlags |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
}

static VKAPI_ATTR void VKAPI_CALL lazy_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *mp)
{
    vkb_srv_current_table()->real.vkGetPhysicalDeviceMemoryProperties(pd, mp);
    fake_lazy_props(mp);
}

static VKAPI_ATTR void VKAPI_CALL lazy_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *mp)
{
    vkb_srv_current_table()->real.vkGetPhysicalDeviceMemoryProperties2(pd, mp);
    fake_lazy_props(&mp->memoryProperties);
}

/* ------------------------------------------------------------------ linear images under the AHB strategy */

/*
 * Under the AHardwareBuffer memory strategy (no dma-heap: Retroid Pocket 4 Pro, kernel 4.19) the
 * client makes its linear images with the AHB handle type so the app can map them, and the server
 * binds them to BLOB AHardwareBuffers. Mali r32p1 cannot address such an image: it reports a
 * rowPitch of 0 for it and reads every row as row 0 (tools/vklinear.py: a 320x240 image written
 * through its mapping copied out as 240 copies of its first row), so a Zink staging texture mapped
 * this way - wined3d's system-memory surfaces, Ys Origin's 320x240 DirectDraw scratch - came out
 * as one repeated row, white on screen. Zink takes linear tiling only where the format's
 * linearTilingFeatures allow it; with none reported it stages through buffers, whose uploads and
 * read-backs are right here. So under this strategy no format offers linear tiling.
 * VKBRIDGE_AHB_LINEAR=1 keeps the driver's answer (to see what a newer driver does).
 */
static int ahb_linear_hidden(VkPhysicalDevice pd)
{
    static int keep = -1;
    if (keep < 0) {
        const char *v = vkb_srv_env("VKBRIDGE_AHB_LINEAR");
        keep = v && !strcmp(v, "1");
    }
    if (keep) return 0;
    vkb_srv_table *t = vkb_srv_current_table();
    const vkb_pd_knowledge *k = t ? vkb_pd_lookup(&t->real, pd) : NULL;
    return k && k->info.strategy == VKB_MEM_AHB;
}

static void hide_linear(VkPhysicalDevice pd, VkFormatProperties *p, VkFormatProperties3 *p3)
{
    if (!ahb_linear_hidden(pd)) return;
    static int told;
    if (p->linearTilingFeatures && !__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
        VKB_INFO("linear tiling hidden from clients under the AHardwareBuffer memory strategy: an image bound to a BLOB AHardwareBuffer "
                 "reads every row as its first (Mali r32p1); Zink stages its mapped textures through buffers instead (VKBRIDGE_AHB_LINEAR=1 keeps it)");
    p->linearTilingFeatures = 0;
    if (p3) p3->linearTilingFeatures = 0;
}

static VKAPI_ATTR void VKAPI_CALL nolinear_GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat format, VkFormatProperties *p)
{
    vkb_srv_current_table()->real.vkGetPhysicalDeviceFormatProperties(pd, format, p);
    hide_linear(pd, p, NULL);
}

static VKAPI_ATTR void VKAPI_CALL nolinear_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *p)
{
    vkb_srv_current_table()->real.vkGetPhysicalDeviceFormatProperties2(pd, format, p);
    VkFormatProperties3 *p3 = NULL;
    for (VkBaseOutStructure *b = p->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) p3 = (VkFormatProperties3 *)b;
    hide_linear(pd, &p->formatProperties, p3);
}

void vkb_mem_instance_install(vkb_srv_table *t)
{
    if (t->real.vkGetPhysicalDeviceFormatProperties) t->dt.vkGetPhysicalDeviceFormatProperties = nolinear_GetPhysicalDeviceFormatProperties;
    if (t->real.vkGetPhysicalDeviceFormatProperties2) t->dt.vkGetPhysicalDeviceFormatProperties2 = nolinear_GetPhysicalDeviceFormatProperties2;
    if (!fake_lazy()) return;
    if (t->real.vkGetPhysicalDeviceMemoryProperties) t->dt.vkGetPhysicalDeviceMemoryProperties = lazy_GetPhysicalDeviceMemoryProperties;
    if (t->real.vkGetPhysicalDeviceMemoryProperties2) t->dt.vkGetPhysicalDeviceMemoryProperties2 = lazy_GetPhysicalDeviceMemoryProperties2;
    VKB_INFO("imitating Mali's memory types: device-local types without host access are lazily allocated");
}

/* ------------------------------------------------------------------ another Mali's memory table, imitated */

/*
 * VKBRIDGE_FAKE_MISSING=...,xringmem, o1mem or g720mem (host tests): the driver's memory types become
 * another Mali's, beneath everything the server does (vkb_gipa: the self-test, the probes and
 * the devices all see them). Each imitated type is backed by a desktop type with the same host
 * access; the resources' memoryTypeBits are what that Mali's are, as far as testers' logs show.
 *
 * g720mem: the Mali-G720's and G925's four types (0:0x7 1:0xb 2:0x11 3:0x21/h1). Images and
 * buffers allow 0x3 (their self-tests: "images allow types 0x3, plain buffers 0x3").
 *
 * xringmem: Mali-G2-Ultra on the XRing O3 (driver r56p0, issue #1), 22 types: 0,3..8 0x7; 1,9..14
 * 0xb; 2,15..20 0x11 (lazily allocated); 21 0x21 (heap 1). As measured there (the tester's v0.5.1
 * logs): images allow 0x1040 (types 6 and 12), plain buffers 0x4100 (8 and 14); with the client's
 * handle types (it makes every buffer so) indirect buffers allow 0x1040 like images, staging
 * buffers (transfer source only: gamescope's upload buffer) and texel buffers 0x3, the others
 * 0x4100 still - and so does any buffer that is also a transfer destination (the sharing
 * self-test's buffers are, and it tried types 8 and 14 only: "dmaheap, type 8: allocation
 * failed", "types 0x4000 shared"; dma-bufs go in 1, 9..14). So an indirect or staging buffer's
 * mask has host-visible types (0, 1, 6, 12), none of them shareable: v0.5.1 offered it no type to map ("no memory type for the buffer (types 0x1
 * allowed)", gamescope's "findMemoryType failed"). Unshared staging buffers allow 0x3 too (the
 * self-test's "resource masks 0x4100->8 0x3->0").
 *
 * o1mem: Mali-G925 on the XRing O1 (driver r49p1, Xiaomi Pad 7S Pro, issues #6 and #8), 6 types:
 * 0 0x1 (device-local, no host access, not lazily allocated), 1 0x7, 2 0xb, 3 and 4 0x11 (lazily
 * allocated), 5 0x21 (heap 1). Images and buffers allow 0x6 ("images allow types 0x6, plain buffers
 * 0x6"): never type 0, the one a client takes for its device-local type when a driver has such a type.
 * v0.5.2 gave every image and GPU-only buffer an empty mask (gamescope's "vkAllocateMemory in memory
 * type 4294967295", then vulkan_create_flat_texture's assertion).
 *
 * All: memory is imported (dma-buf, host pointer) or made exportable only in a HOST_CACHED type
 * (the G925's self-test: "dmaheap, type 0: allocation failed (-1000072003)", type 1 shared OK),
 * and fd and host pointer properties list only those.
 */
typedef struct {
    VkMemoryPropertyFlags flags;
    uint32_t heap;
} imit_type;

static const imit_type imit_g720[] = {{0x7, 0}, {0xb, 0}, {0x11, 0}, {0x21, 1}};
static const imit_type imit_o1[] = {{0x1, 0}, {0x7, 0}, {0xb, 0}, {0x11, 0}, {0x11, 0}, {0x21, 1}};
static const imit_type imit_xring[] = {{0x7, 0},  {0xb, 0},  {0x11, 0}, {0x7, 0},  {0x7, 0},  {0x7, 0},  {0x7, 0},  {0x7, 0},
                                       {0x7, 0},  {0xb, 0},  {0xb, 0},  {0xb, 0},  {0xb, 0},  {0xb, 0},  {0xb, 0},  {0x11, 0},
                                       {0x11, 0}, {0x11, 0}, {0x11, 0}, {0x11, 0}, {0x11, 0}, {0x21, 1}};

static struct {
    int on; /* 1 g720mem, 2 xringmem, 3 o1mem */
    const imit_type *types;
    uint32_t count;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkGetPhysicalDeviceMemoryProperties props;
    PFN_vkGetPhysicalDeviceMemoryProperties2 props2;
    PFN_vkCreateDevice create_device;
} imit;

typedef struct {
    VkDevice dev;
    uint32_t map[VK_MAX_MEMORY_TYPES]; /* imitated type -> the driver's */
    PFN_vkDestroyDevice destroy;
    PFN_vkAllocateMemory alloc;
    PFN_vkCreateBuffer create_buffer;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkGetBufferMemoryRequirements buf_req;
    PFN_vkGetBufferMemoryRequirements2 buf_req2;
    PFN_vkGetDeviceBufferMemoryRequirements dev_buf_req;
    PFN_vkGetImageMemoryRequirements img_req;
    PFN_vkGetImageMemoryRequirements2 img_req2;
    PFN_vkGetDeviceImageMemoryRequirements dev_img_req;
    PFN_vkGetMemoryFdPropertiesKHR fd_props;
    PFN_vkGetMemoryHostPointerPropertiesEXT host_props;
} imit_dev;

static imit_dev imit_devs[64];
static pthread_mutex_t imit_lock = PTHREAD_MUTEX_INITIALIZER;

/* Buffers' imitated memoryTypeBits, from their create info (looked up by handle). */
#define IMIT_BUFS 16384
#define IMIT_TOMB ((VkBuffer)(uintptr_t)~0ull)
static struct {
    VkBuffer b;
    uint32_t bits;
} imit_bufs[IMIT_BUFS];

static int imit_on(void)
{
    static int v = -1;
    if (v < 0) v = vkb_emu_faked("xringmem") ? 2 : vkb_emu_faked("o1mem") ? 3 : vkb_emu_faked("g720mem") ? 1 : 0;
    return v;
}

static imit_dev *imit_find(VkDevice d)
{
    for (int i = 0; i < 64; i++)
        if (__atomic_load_n(&imit_devs[i].dev, __ATOMIC_ACQUIRE) == d) return &imit_devs[i];
    VKB_ERR("memory table imitation: unknown device");
    abort();
}

static uint32_t imit_cached_types(void)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < imit.count; i++)
        if (imit.types[i].flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) m |= 1u << i;
    return m;
}

static uint32_t imit_image_bits(void) { return imit.on == 2 ? 0x1040u : imit.on == 3 ? 0x6u : 0x3u; }

static uint32_t imit_buffer_bits(const VkBufferCreateInfo *ci)
{
    if (imit.on == 3) return 0x6u;
    if (imit.on != 2) return 0x3u;
    int external = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO && ((const VkExternalMemoryBufferCreateInfo *)b)->handleTypes)
            external = 1;
    /* staging: a transfer source only (gamescope's upload buffer; also one with storage, which the BCn
     * emulation adds to every transfer source, server_emu_bcn.c - stricter than measured) */
    if ((ci->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) && !(ci->usage & ~(VkBufferUsageFlags)(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)))
        return 0x3u;
    if (!external) return 0x4100u;
    if (ci->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) return 0x4100u;
    if (ci->usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) return 0x1040u;
    if (ci->usage & (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) return 0x3u;
    return 0x4100u;
}

/* The driver's bits -> the imitated types they back, of those `allow` lists. */
static uint32_t imit_bits(const imit_dev *d, uint32_t real, uint32_t allow)
{
    uint32_t bits = 0;
    for (uint32_t i = 0; i < imit.count; i++)
        if ((allow & (1u << i)) && (real & (1u << d->map[i]))) bits |= 1u << i;
    return bits;
}

static void imit_fill(VkPhysicalDeviceMemoryProperties *mp)
{
    /* Both heaps the driver's device-local one. */
    VkMemoryHeap dl = mp->memoryHeaps[0];
    for (uint32_t h = 0; h < mp->memoryHeapCount; h++)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            dl = mp->memoryHeaps[h];
            break;
        }
    memset(mp, 0, sizeof(*mp));
    mp->memoryHeapCount = 2;
    mp->memoryHeaps[0] = mp->memoryHeaps[1] = dl;
    mp->memoryTypeCount = imit.count;
    for (uint32_t i = 0; i < imit.count; i++) {
        mp->memoryTypes[i].propertyFlags = imit.types[i].flags;
        mp->memoryTypes[i].heapIndex = imit.types[i].heap;
    }
}

static VKAPI_ATTR void VKAPI_CALL imit_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *mp)
{
    imit.props(pd, mp);
    imit_fill(mp);
}

static VKAPI_ATTR void VKAPI_CALL imit_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *mp)
{
    imit.props2(pd, mp);
    VkMemoryHeap dl = mp->memoryProperties.memoryHeaps[0];
    uint32_t dlh = 0;
    for (uint32_t h = 0; h < mp->memoryProperties.memoryHeapCount; h++)
        if (mp->memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            dl = mp->memoryProperties.memoryHeaps[h];
            dlh = h;
            break;
        }
    (void)dl;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)mp->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp = (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)b;
            bp->heapBudget[1] = bp->heapBudget[0] = bp->heapBudget[dlh];
            bp->heapUsage[1] = bp->heapUsage[0] = bp->heapUsage[dlh];
        }
    imit_fill(&mp->memoryProperties);
}

/* The driver's type with the host access an imitated type has. */
static uint32_t imit_backing(const VkPhysicalDeviceMemoryProperties *mp, VkMemoryPropertyFlags want)
{
    const VkMemoryPropertyFlags odd = VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD |
                                      VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    VkMemoryPropertyFlags need = want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                     ? want & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t t = 0; t < mp->memoryTypeCount; t++) {
            VkMemoryPropertyFlags f = mp->memoryTypes[t].propertyFlags;
            if ((f & odd) || (f & need) != need) continue;
            if (!(want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            /* the device-local host-visible kind on the device-local heap first */
            if (pass == 0 && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
                !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                continue;
            return t;
        }
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL imit_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a,
                                                        VkDevice *out)
{
    VkResult r = imit.create_device(pd, ci, a, out);
    if (r != VK_SUCCESS) return r;
    VkPhysicalDeviceMemoryProperties mp;
    imit.props(pd, &mp);
    pthread_mutex_lock(&imit_lock);
    imit_dev *d = NULL;
    for (int i = 0; i < 64 && !d; i++)
        if (!imit_devs[i].dev || imit_devs[i].dev == *out) d = &imit_devs[i];
    if (!d) {
        pthread_mutex_unlock(&imit_lock);
        VKB_ERR("memory table imitation: too many devices");
        abort();
    }
    memset(d, 0, sizeof(*d));
    for (uint32_t i = 0; i < imit.count; i++) d->map[i] = imit_backing(&mp, imit.types[i].flags);
#define IMIT_GET(field, name) d->field = (void *)imit.gdpa(*out, name)
    IMIT_GET(destroy, "vkDestroyDevice");
    IMIT_GET(alloc, "vkAllocateMemory");
    IMIT_GET(create_buffer, "vkCreateBuffer");
    IMIT_GET(destroy_buffer, "vkDestroyBuffer");
    IMIT_GET(buf_req, "vkGetBufferMemoryRequirements");
    IMIT_GET(buf_req2, "vkGetBufferMemoryRequirements2");
    IMIT_GET(dev_buf_req, "vkGetDeviceBufferMemoryRequirements");
    IMIT_GET(img_req, "vkGetImageMemoryRequirements");
    IMIT_GET(img_req2, "vkGetImageMemoryRequirements2");
    IMIT_GET(dev_img_req, "vkGetDeviceImageMemoryRequirements");
    IMIT_GET(fd_props, "vkGetMemoryFdPropertiesKHR");
    IMIT_GET(host_props, "vkGetMemoryHostPointerPropertiesEXT");
#undef IMIT_GET
    __atomic_store_n(&d->dev, *out, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&imit_lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL imit_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *a)
{
    if (!dev) return;
    imit_dev *d = imit_find(dev);
    PFN_vkDestroyDevice destroy = d->destroy;
    __atomic_store_n(&d->dev, VK_NULL_HANDLE, __ATOMIC_RELEASE);
    destroy(dev, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL imit_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a,
                                                          VkDeviceMemory *out)
{
    imit_dev *d = imit_find(dev);
    if (ai->memoryTypeIndex >= imit.count) return d->alloc(dev, ai, a, out); /* the driver's own error */
    int import = 0;
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        import |= b->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR || b->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT ||
                  b->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    if (import && !(imit_cached_types() & (1u << ai->memoryTypeIndex))) {
        static int told;
        if (!__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
            VKB_INFO("imitated memory table: an import or export in type %u refused (HOST_CACHED types only; logged once)",
                     ai->memoryTypeIndex);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkMemoryAllocateInfo c = *ai;
    c.memoryTypeIndex = d->map[ai->memoryTypeIndex];
    return d->alloc(dev, &c, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL imit_CreateBuffer(VkDevice dev, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out)
{
    VkResult r = imit_find(dev)->create_buffer(dev, ci, a, out);
    if (r != VK_SUCCESS) return r;
    uint32_t h = (uint32_t)(((uint64_t)*out * 0x9e3779b97f4a7c15ull) >> 50) % IMIT_BUFS;
    pthread_mutex_lock(&imit_lock);
    for (uint32_t n = 0; n < IMIT_BUFS; n++, h = (h + 1) % IMIT_BUFS)
        if (!imit_bufs[h].b || imit_bufs[h].b == IMIT_TOMB) {
            imit_bufs[h].b = *out;
            imit_bufs[h].bits = imit_buffer_bits(ci);
            break;
        }
    pthread_mutex_unlock(&imit_lock);
    return r;
}

static uint32_t imit_buffer_lookup(VkBuffer b, int forget)
{
    uint32_t h = (uint32_t)(((uint64_t)b * 0x9e3779b97f4a7c15ull) >> 50) % IMIT_BUFS, bits = 0x3u;
    pthread_mutex_lock(&imit_lock);
    for (uint32_t n = 0; n < IMIT_BUFS; n++, h = (h + 1) % IMIT_BUFS)
        if (imit_bufs[h].b == b) {
            bits = imit_bufs[h].bits;
            if (forget) imit_bufs[h].b = IMIT_TOMB; /* the chain goes on past it */
            break;
        } else if (!imit_bufs[h].b) {
            break;
        }
    pthread_mutex_unlock(&imit_lock);
    return bits;
}

static VKAPI_ATTR void VKAPI_CALL imit_DestroyBuffer(VkDevice dev, VkBuffer b, const VkAllocationCallbacks *a)
{
    if (b) imit_buffer_lookup(b, 1);
    imit_find(dev)->destroy_buffer(dev, b, a);
}

static VKAPI_ATTR void VKAPI_CALL imit_GetBufferMemoryRequirements(VkDevice dev, VkBuffer b, VkMemoryRequirements *r)
{
    imit_dev *d = imit_find(dev);
    d->buf_req(dev, b, r);
    r->memoryTypeBits = imit_bits(d, r->memoryTypeBits, imit_buffer_lookup(b, 0));
}

static VKAPI_ATTR void VKAPI_CALL imit_GetBufferMemoryRequirements2(VkDevice dev, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r)
{
    imit_dev *d = imit_find(dev);
    d->buf_req2(dev, i, r);
    r->memoryRequirements.memoryTypeBits = imit_bits(d, r->memoryRequirements.memoryTypeBits, imit_buffer_lookup(i->buffer, 0));
}

static VKAPI_ATTR void VKAPI_CALL imit_GetDeviceBufferMemoryRequirements(VkDevice dev, const VkDeviceBufferMemoryRequirements *i,
                                                                         VkMemoryRequirements2 *r)
{
    imit_dev *d = imit_find(dev);
    d->dev_buf_req(dev, i, r);
    r->memoryRequirements.memoryTypeBits = imit_bits(d, r->memoryRequirements.memoryTypeBits, imit_buffer_bits(i->pCreateInfo));
}

static VKAPI_ATTR void VKAPI_CALL imit_GetImageMemoryRequirements(VkDevice dev, VkImage img, VkMemoryRequirements *r)
{
    imit_dev *d = imit_find(dev);
    d->img_req(dev, img, r);
    r->memoryTypeBits = imit_bits(d, r->memoryTypeBits, imit_image_bits());
}

static VKAPI_ATTR void VKAPI_CALL imit_GetImageMemoryRequirements2(VkDevice dev, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r)
{
    imit_dev *d = imit_find(dev);
    d->img_req2(dev, i, r);
    r->memoryRequirements.memoryTypeBits = imit_bits(d, r->memoryRequirements.memoryTypeBits, imit_image_bits());
}

static VKAPI_ATTR void VKAPI_CALL imit_GetDeviceImageMemoryRequirements(VkDevice dev, const VkDeviceImageMemoryRequirements *i,
                                                                        VkMemoryRequirements2 *r)
{
    imit_dev *d = imit_find(dev);
    d->dev_img_req(dev, i, r);
    r->memoryRequirements.memoryTypeBits = imit_bits(d, r->memoryRequirements.memoryTypeBits, imit_image_bits());
}

static VKAPI_ATTR VkResult VKAPI_CALL imit_GetMemoryFdPropertiesKHR(VkDevice dev, VkExternalMemoryHandleTypeFlagBits h, int fd,
                                                                    VkMemoryFdPropertiesKHR *p)
{
    imit_dev *d = imit_find(dev);
    VkResult r = d->fd_props(dev, h, fd, p);
    if (r == VK_SUCCESS) p->memoryTypeBits = imit_bits(d, p->memoryTypeBits, imit_cached_types());
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL imit_GetMemoryHostPointerPropertiesEXT(VkDevice dev, VkExternalMemoryHandleTypeFlagBits h,
                                                                             const void *ptr, VkMemoryHostPointerPropertiesEXT *p)
{
    imit_dev *d = imit_find(dev);
    VkResult r = d->host_props(dev, h, ptr, p);
    if (r == VK_SUCCESS) p->memoryTypeBits = imit_bits(d, p->memoryTypeBits, imit_cached_types());
    return r;
}

/* The device-level ones, for both procaddr functions (NULL stays NULL: an extension not there). */
static PFN_vkVoidFunction imit_device_fn(const char *n)
{
    static const struct {
        const char *name;
        PFN_vkVoidFunction fn;
    } fns[] = {
        {"vkDestroyDevice", (PFN_vkVoidFunction)imit_DestroyDevice},
        {"vkAllocateMemory", (PFN_vkVoidFunction)imit_AllocateMemory},
        {"vkCreateBuffer", (PFN_vkVoidFunction)imit_CreateBuffer},
        {"vkDestroyBuffer", (PFN_vkVoidFunction)imit_DestroyBuffer},
        {"vkGetBufferMemoryRequirements", (PFN_vkVoidFunction)imit_GetBufferMemoryRequirements},
        {"vkGetBufferMemoryRequirements2", (PFN_vkVoidFunction)imit_GetBufferMemoryRequirements2},
        {"vkGetBufferMemoryRequirements2KHR", (PFN_vkVoidFunction)imit_GetBufferMemoryRequirements2},
        {"vkGetDeviceBufferMemoryRequirements", (PFN_vkVoidFunction)imit_GetDeviceBufferMemoryRequirements},
        {"vkGetDeviceBufferMemoryRequirementsKHR", (PFN_vkVoidFunction)imit_GetDeviceBufferMemoryRequirements},
        {"vkGetImageMemoryRequirements", (PFN_vkVoidFunction)imit_GetImageMemoryRequirements},
        {"vkGetImageMemoryRequirements2", (PFN_vkVoidFunction)imit_GetImageMemoryRequirements2},
        {"vkGetImageMemoryRequirements2KHR", (PFN_vkVoidFunction)imit_GetImageMemoryRequirements2},
        {"vkGetDeviceImageMemoryRequirements", (PFN_vkVoidFunction)imit_GetDeviceImageMemoryRequirements},
        {"vkGetDeviceImageMemoryRequirementsKHR", (PFN_vkVoidFunction)imit_GetDeviceImageMemoryRequirements},
        {"vkGetMemoryFdPropertiesKHR", (PFN_vkVoidFunction)imit_GetMemoryFdPropertiesKHR},
        {"vkGetMemoryHostPointerPropertiesEXT", (PFN_vkVoidFunction)imit_GetMemoryHostPointerPropertiesEXT},
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
        if (!strcmp(fns[i].name, n)) return fns[i].fn;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL imit_GetDeviceProcAddr(VkDevice dev, const char *n)
{
    PFN_vkVoidFunction f = imit.gdpa(dev, n);
    PFN_vkVoidFunction w = f ? imit_device_fn(n) : NULL;
    return w ? w : f;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL imit_GetInstanceProcAddr(VkInstance inst, const char *n)
{
    PFN_vkVoidFunction f = imit.gipa(inst, n);
    if (!f || !inst) return f;
    if (!strcmp(n, "vkGetDeviceProcAddr")) {
        imit.gdpa = (PFN_vkGetDeviceProcAddr)f;
        return (PFN_vkVoidFunction)imit_GetDeviceProcAddr;
    }
    if (!strcmp(n, "vkCreateDevice")) {
        imit.create_device = (PFN_vkCreateDevice)f;
        imit.gdpa = (PFN_vkGetDeviceProcAddr)imit.gipa(inst, "vkGetDeviceProcAddr");
        imit.props = (PFN_vkGetPhysicalDeviceMemoryProperties)imit.gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
        return (PFN_vkVoidFunction)imit_CreateDevice;
    }
    if (!strcmp(n, "vkGetPhysicalDeviceMemoryProperties")) {
        imit.props = (PFN_vkGetPhysicalDeviceMemoryProperties)f;
        return (PFN_vkVoidFunction)imit_GetPhysicalDeviceMemoryProperties;
    }
    if (!strcmp(n, "vkGetPhysicalDeviceMemoryProperties2") || !strcmp(n, "vkGetPhysicalDeviceMemoryProperties2KHR")) {
        imit.props2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)f;
        return (PFN_vkVoidFunction)imit_GetPhysicalDeviceMemoryProperties2;
    }
    PFN_vkVoidFunction w = imit_device_fn(n);
    return w ? w : f;
}

PFN_vkGetInstanceProcAddr vkb_mem_imitate_gipa(PFN_vkGetInstanceProcAddr gipa)
{
    if (!imit_on()) return gipa;
    imit.on = imit_on();
    imit.types = imit.on == 2 ? imit_xring : imit.on == 3 ? imit_o1 : imit_g720;
    imit.count = imit.on == 2   ? sizeof(imit_xring) / sizeof(imit_xring[0])
                 : imit.on == 3 ? sizeof(imit_o1) / sizeof(imit_o1[0])
                                : sizeof(imit_g720) / sizeof(imit_g720[0]);
    imit.gipa = gipa;
    VKB_INFO("imitating %s's memory table: %u types, images allow 0x%x", imit.on == 2 ? "the XRing O3" : imit.on == 3 ? "the XRing O1" : "the Mali-G720",
             imit.count, imit_image_bits());
    return imit_GetInstanceProcAddr;
}

/* ------------------------------------------------------------------ Mali's dma-buf fd properties, imitated */

/*
 * VKBRIDGE_FAKE_MISSING=...,dmabuftypes (host tests): Mali's vkGetMemoryFdPropertiesKHR for a
 * dma-buf lists only its host-visible cached type (G720 r44p1: 0x2, type 1 0xb), which the client
 * offers only as a shared type, and none of the types images go in. Here a dma-buf's fd properties
 * are the driver's host-visible cached types, and a dma-buf import in another type is refused with
 * VK_ERROR_INVALID_EXTERNAL_HANDLE (out of the spec either way; so a test sees the type the
 * server imported in).
 */
static struct {
    vkb_srv_table *t;
    uint32_t mask;
    PFN_vkGetMemoryFdPropertiesKHR props;
    PFN_vkAllocateMemory alloc;
} fdt_dev[16];

static int fdt_slot(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    for (int i = 0; i < 16 && fdt_dev[i].t; i++)
        if (fdt_dev[i].t == t) return i;
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL fdt_GetMemoryFdPropertiesKHR(VkDevice d, VkExternalMemoryHandleTypeFlagBits h, int fd,
                                                                    VkMemoryFdPropertiesKHR *p)
{
    int s = fdt_slot();
    VkResult r = fdt_dev[s].props(d, h, fd, p);
    if (r == VK_SUCCESS && h == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) p->memoryTypeBits = fdt_dev[s].mask;
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fdt_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a,
                                                         VkDeviceMemory *out)
{
    int s = fdt_slot();
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR &&
            ((const VkImportMemoryFdInfoKHR *)b)->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
            (ai->memoryTypeIndex >= 32 || !(fdt_dev[s].mask & (1u << ai->memoryTypeIndex)))) {
            VKB_INFO("imitated dma-buf fd properties: an import in type %u refused (the dma-buf takes 0x%x)", ai->memoryTypeIndex,
                     fdt_dev[s].mask);
            *out = VK_NULL_HANDLE;
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
    return fdt_dev[s].alloc(d, ai, a, out);
}

static void fdt_install(vkb_srv_table *t)
{
    if (!vkb_emu_faked("dmabuftypes") || !t->real.vkGetMemoryFdPropertiesKHR || !t->real.vkAllocateMemory) return;
    for (int i = 0; i < 16; i++) {
        if (fdt_dev[i].t && fdt_dev[i].t != t) continue;
        VkPhysicalDeviceMemoryProperties mp;
        t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
        uint32_t mask = 0;
        for (uint32_t k = 0; k < mp.memoryTypeCount && k < 32; k++)
            if ((mp.memoryTypes[k].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) &&
                !(mp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT))
                mask |= 1u << k;
        fdt_dev[i].mask = mask;
        fdt_dev[i].props = t->real.vkGetMemoryFdPropertiesKHR;
        fdt_dev[i].alloc = t->real.vkAllocateMemory;
        __atomic_store_n(&fdt_dev[i].t, t, __ATOMIC_RELEASE);
        t->real.vkGetMemoryFdPropertiesKHR = t->dt.vkGetMemoryFdPropertiesKHR = fdt_GetMemoryFdPropertiesKHR;
        t->real.vkAllocateMemory = t->dt.vkAllocateMemory = fdt_AllocateMemory;
        VKB_INFO("imitating Mali's dma-buf fd properties: a dma-buf takes memory types 0x%x only", mask);
        return;
    }
}

/* At the driver's level (t->real), before the layers above it take their copies of it. The
 * accounting thread starts with the first device too (not only at the first allocation): it
 * watches for low memory, which descriptor pools ask about with no memory allocated. */
void vkb_mem_device_install(vkb_srv_table *t)
{
    pthread_once(&st_once, st_start);
    fdt_install(t);
}

/* ------------------------------------------------------------------ what allocations did (logs) */

/* The driver's memory types in one line, at every device: an in-app server prints its self-test
 * only in the first session's log, and a tester's log should still show what images can use. */
void vkb_mem_log_types(vkb_srv_table *t, uint32_t id)
{
    VkPhysicalDeviceMemoryProperties mp;
    t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    if (fake_lazy()) fake_lazy_props(&mp);
    char line[512];
    int n = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount && n < (int)sizeof(line) - 40; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %u:0x%x/h%u", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
    line[n < (int)sizeof(line) ? n : (int)sizeof(line) - 1] = 0;
    VKB_INFO("device %u: driver memory types (type:flags/heap)%s; shared types 0x%x", id, line,
             t->pd ? t->pd->info.shareable_types : 0);
}

/* One line for the first allocation each device makes in each memory type (which path it took),
 * and one for every failure (the first 32 per process): what was asked, where, and the VkResult. */
static void log_alloc(vkb_srv_table *t, const VkMemoryAllocateInfo *ai, uint32_t type, uint8_t ded, const char *path, VkResult r)
{
    static int failures;
    uint32_t bit = type < 32 ? 1u << type : 0;
    if (r == VK_SUCCESS) {
        if (!bit || (__atomic_fetch_or(&t->mem_types_seen, bit, __ATOMIC_RELAXED) & bit)) return;
    } else if (__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED) >= 32) {
        return;
    }
    VkPhysicalDeviceMemoryProperties mp;
    t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    if (fake_lazy()) fake_lazy_props(&mp);
    VkMemoryPropertyFlags f = type < mp.memoryTypeCount ? mp.memoryTypes[type].propertyFlags : 0;
    uint32_t heap = type < mp.memoryTypeCount ? mp.memoryTypes[type].heapIndex : 0;
    const char *what = ded == 1 ? ", dedicated to an image" : ded == 2 ? ", dedicated to a buffer" : "";
    if (r == VK_SUCCESS)
        VKB_INFO("device %u: first allocation in memory type %u (flags 0x%x, heap %u): %llu bytes%s, %s", t->id, type, f, heap,
                 (unsigned long long)ai->allocationSize, what, path);
    else
        VKB_WARN("device %u: vkAllocateMemory of %llu bytes in memory type %u (flags 0x%x%s, heap %u)%s, %s, failed: %d", t->id,
                 (unsigned long long)ai->allocationSize, type, f,
                 f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? " - lazily allocated, for transient attachments only"
                 : f & VK_MEMORY_PROPERTY_PROTECTED_BIT ? " - protected" : "",
                 heap, what, path, r);
}

/* ------------------------------------------------------------------ scanout buffers (Wayland zero-copy) */

/*
 * The client's Wayland WSI, on a compositor that takes Android buffers (banner_ahb_v1, DroidDeck's
 * zero-copy layer mode), asks for each swapchain image's pages here instead of exporting them:
 * a gralloc AHardwareBuffer whose handle is written into the socket the client sent (the client
 * forwards the other end to the compositor, which puts the buffer itself on a SurfaceControl
 * layer), and whose dma-buf comes back to the client, which imports it for the image with the
 * buffer's own linear layout. The dma-buf path to the compositor stays exactly as before (the
 * fallback when the layer cannot be used), and every frame the game draws lands in the buffer the
 * display scans out - no copy anywhere. CPU_READ_RARELY keeps gralloc from compressing it (a
 * linear layout is the only one both the Vulkan import and the dma-buf importer can be told).
 * Our reference is dropped once the handle is in the socket: the handle in flight and the
 * imported dma-buf keep the buffer alive.
 */
#ifdef __ANDROID__
#define AHB_FORMAT_R8G8B8A8_UNORM 1u
#define AHB_FORMAT_B8G8R8A8_UNORM 5u /* HAL_PIXEL_FORMAT_BGRA_8888: a VNDK format AHardwareBuffer_allocate accepts */
#define AHB_USAGE_CPU_READ_RARELY 2ull
#define AHB_USAGE_GPU_SAMPLED_IMAGE (1ull << 8)
#define AHB_USAGE_GPU_COLOR_OUTPUT (1ull << 9)
#define AHB_USAGE_COMPOSER_OVERLAY (1ull << 11)
static void (*p_AHardwareBuffer_describe)(const struct AHardwareBuffer *, vkb_ahb_desc *);
static int (*p_AHardwareBuffer_isSupported)(const vkb_ahb_desc *);
static int (*p_AHardwareBuffer_sendHandleToUnixSocket)(const struct AHardwareBuffer *, int);

static int scanout_load(void)
{
    static int state;
    if (state) return state > 0;
    state = -1;
    if (!ahb_load()) return 0;
    void *h = dlopen("libnativewindow.so", RTLD_NOW);
    if (!h) h = dlopen("libandroid.so", RTLD_NOW);
    if (h) {
        p_AHardwareBuffer_describe = dlsym(h, "AHardwareBuffer_describe");
        p_AHardwareBuffer_isSupported = dlsym(h, "AHardwareBuffer_isSupported");
        p_AHardwareBuffer_sendHandleToUnixSocket = dlsym(h, "AHardwareBuffer_sendHandleToUnixSocket");
    }
    if (p_AHardwareBuffer_describe && p_AHardwareBuffer_sendHandleToUnixSocket) state = 1;
    else VKB_WARN("scanout: AHardwareBuffer_describe / sendHandleToUnixSocket unavailable");
    return state > 0;
}

static VkResult scanout_alloc(uint32_t w, uint32_t h, uint32_t vkformat, int sock, int *out_fd, uint32_t *pitch, uint64_t *size,
                              uint32_t *ahb_format)
{
    static uint32_t said_w, said_h, said_fmt, refused_fmt;
    if (!scanout_load()) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t f;
    switch (vkformat) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: f = AHB_FORMAT_R8G8B8A8_UNORM; break;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: f = AHB_FORMAT_B8G8R8A8_UNORM; break;
    default: return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (!w || !h || w > 16384 || h > 16384) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    vkb_ahb_desc d = {w, h, 1, f, AHB_USAGE_GPU_COLOR_OUTPUT | AHB_USAGE_GPU_SAMPLED_IMAGE | AHB_USAGE_COMPOSER_OVERLAY | AHB_USAGE_CPU_READ_RARELY,
                      0, 0, 0};
    if (p_AHardwareBuffer_isSupported && !p_AHardwareBuffer_isSupported(&d)) {
        if (refused_fmt != f) VKB_INFO("scanout: gralloc has no %ux%u AHardwareBuffer format %u for the display", w, h, f);
        refused_fmt = f;
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    struct AHardwareBuffer *ahb = NULL;
    int ar = p_AHardwareBuffer_allocate(&d, &ahb);
    if (ar != 0 || !ahb) {
        VKB_INFO("scanout: AHardwareBuffer_allocate(%ux%u format %u) failed: %d", w, h, f, ar);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VkResult r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    vkb_ahb_desc got;
    memset(&got, 0, sizeof(got));
    p_AHardwareBuffer_describe(ahb, &got);
    uint64_t need = (uint64_t)got.stride * 4u * h;
    const vkb_native_handle *nh = p_AHardwareBuffer_getNativeHandle(ahb);
    int pick = -1;
    off_t len = 0;
    for (int i = 0; nh && i < nh->numFds && pick < 0; i++) {
        off_t end = lseek(nh->data[i], 0, SEEK_END);
        if (end > 0 && (uint64_t)end >= need) {
            pick = nh->data[i];
            len = end;
        }
    }
    if (got.stride < w || pick < 0) {
        VKB_INFO("scanout: the %ux%u AHardwareBuffer has stride %u px and no dma-buf of %llu bytes in its handle (%d fds)", w, h,
                 got.stride, (unsigned long long)need, nh ? nh->numFds : -1);
        goto out;
    }
    *out_fd = fcntl(pick, F_DUPFD_CLOEXEC, 0);
    if (*out_fd < 0) {
        r = VK_ERROR_TOO_MANY_OBJECTS;
        goto out;
    }
    int sr = p_AHardwareBuffer_sendHandleToUnixSocket(ahb, sock);
    if (sr != 0) {
        VKB_INFO("scanout: AHardwareBuffer_sendHandleToUnixSocket failed: %d", sr);
        close(*out_fd);
        *out_fd = -1;
        goto out;
    }
    *pitch = got.stride * 4u;
    *size = (uint64_t)len;
    *ahb_format = f;
    r = VK_SUCCESS;
    if (said_w != w || said_h != h || said_fmt != f) {
        said_w = w;
        said_h = h;
        said_fmt = f;
        VKB_INFO("scanout: %ux%u AHardwareBuffers (format %u, stride %u px, %lld bytes, usage 0x%llx) for the compositor's display layer",
                 w, h, f, got.stride, (long long)len, (unsigned long long)got.usage);
    }
out:
    p_AHardwareBuffer_release(ahb);
    return r;
}
#endif

/* vkbAllocScanout: device, width, height, VkFormat, socket -> result, dma-buf, row pitch (bytes),
 * dma-buf size, AHARDWAREBUFFER_FORMAT_*. Not available off Android (VK_ERROR_FEATURE_NOT_PRESENT). */
void vkb_sv_vkbAllocScanout(vkb_srv_call *c)
{
    VkDevice dev = (VkDevice)(uintptr_t)vkb_dec_u64(&c->d);
    uint32_t w = vkb_dec_u32(&c->d), h = vkb_dec_u32(&c->d), format = vkb_dec_u32(&c->d);
    int sock = vkb_dec_fd(&c->d);
    vkb_srv_table *t = c->table;
    if (c->d.err || !t || !t->is_device || t->device != dev) {
        if (sock >= 0) close(sock);
        vkb_srv_bad_message(c);
        return;
    }
    VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
    int out_fd = -1;
    uint32_t pitch = 0, ahb_format = 0;
    uint64_t size = 0;
#ifdef __ANDROID__
    if (sock >= 0) r = scanout_alloc(w, h, format, sock, &out_fd, &pitch, &size, &ahb_format);
#else
    (void)w;
    (void)h;
    (void)format;
#endif
    if (sock >= 0) close(sock);
    vkb_enc_u32(&c->r, (uint32_t)r);
    vkb_enc_fd(&c->r, out_fd);
    if (out_fd >= 0) vkb_srv_close_after_send(c, out_fd);
    vkb_enc_u32(&c->r, pitch);
    vkb_enc_u64(&c->r, size);
    vkb_enc_u32(&c->r, ahb_format);
}

/* ------------------------------------------------------------------ handlers */

static void close_import_fds(const void *pNext)
{
    for (const VkBaseInStructure *b = pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) vkb_close_fd(((const VkImportMemoryFdInfoKHR *)b)->fd);
}

/*
 * A dma-buf import with a dedicated image or buffer, in a type the driver's fd properties leave
 * out, moved to one they list that the resource allows too, never lazily allocated or protected.
 * Without the struct (the resource unknown), or with no such type, the type stays: Mali took
 * dma-bufs in its type 0 though its fd properties say type 1 only. The
 * client offers its device-local type for every importable fd (client/memory.c): on Mali the fd's
 * own type is host-visible, which the client offers only as shared, so the fd's client bits were
 * 0 and gamescope's DMA-BUF patch for other GPUs found no type to import in.
 */
static void fit_import_type(vkb_srv_table *t, VkMemoryAllocateInfo *ai, int fd)
{
    const vkb_dispatch *dt = &t->real;
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (!dt->vkGetMemoryFdPropertiesKHR ||
        dt->vkGetMemoryFdPropertiesKHR(t->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) != VK_SUCCESS ||
        !fp.memoryTypeBits || ai->memoryTypeIndex >= 32 || (fp.memoryTypeBits & (1u << ai->memoryTypeIndex)))
        return;
    const VkMemoryDedicatedAllocateInfo *d = NULL;
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) d = (const VkMemoryDedicatedAllocateInfo *)b;
    if (!d || (!d->image && !d->buffer)) return;
    VkMemoryRequirements req;
    if (d->image)
        dt->vkGetImageMemoryRequirements(t->device, d->image, &req);
    else
        dt->vkGetBufferMemoryRequirements(t->device, d->buffer, &req);
    uint32_t want = req.memoryTypeBits;
    VkPhysicalDeviceMemoryProperties mp;
    dt->vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    uint32_t ordinary = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (!(mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT))) ordinary |= 1u << i;
    uint32_t bits = fp.memoryTypeBits & want & ordinary;
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
        VKB_INFO("dma-buf import: type %u is not one the dma-buf takes (types 0x%x, the %s 0x%x); %s %d", ai->memoryTypeIndex,
                 fp.memoryTypeBits, d->image ? "image" : "buffer", want,
                 bits ? "imported in type" : "kept, none fits", bits ? __builtin_ctz(bits) : (int)ai->memoryTypeIndex);
    if (bits) ai->memoryTypeIndex = (uint32_t)__builtin_ctz(bits);
}

void vkb_sv_vkAllocateMemory(vkb_srv_call *c)
{
    VkDevice dev = (VkDevice)(uintptr_t)vkb_dec_u64(&c->d);
    VkMemoryAllocateInfo ai;
    int have = vkb_dec_u8(&c->d);
    if (have) vkb_dec_VkMemoryAllocateInfo(&c->d, &ai);
    uint32_t mode = vkb_dec_u32(&c->d);
    int client_fd = vkb_dec_fd(&c->d);
    vkb_srv_table *t = c->table;
    if (c->d.err || !have || !t || !t->is_device || t->device != dev) {
        if (client_fd >= 0) close(client_fd);
        vkb_srv_bad_message(c);
        return;
    }
    VkResult r;
    vkb_srv_mem *m = NULL;
    int out_fd = -1;
    uint8_t ded = vkb_memstat_dedicated(&ai); /* before the strategies drop the struct from the chain */
    if (mode == VKB_ALLOC_SHARED || mode == VKB_ALLOC_SHARED_CACHED) {
        uint32_t s = t->pd ? t->pd->info.strategy : VKB_MEM_NONE;
        /* Cached pages only in a type the self-test passed them in (the client asks only then). */
        int cached = mode == VKB_ALLOC_SHARED_CACHED && t->pd && ai.memoryTypeIndex < 32 &&
                     (t->pd->info.cached_types & (1u << ai.memoryTypeIndex));
        if (mode == VKB_ALLOC_SHARED_CACHED && !cached)
            VKB_ONCE(VKB_LOG_WARN, "cached readback asked for in memory type %u, which the self-test did not pass; uncached pages instead",
                     ai.memoryTypeIndex);
        int opaque = vkb_opaque_shared_info(t, &ai); /* server_opaque.c */
        r = alloc_shared(c->dt, dev, s, &ai, client_fd, cached, &m, &out_fd);
        if (r == VK_SUCCESS && opaque) vkb_opaque_shared_done(m, out_fd);
        char path[48];
        snprintf(path, sizeof(path), "shared (%s%s)", vkb_mem_strategy_name(s), cached ? ", cached" : "");
        log_alloc(t, &ai, ai.memoryTypeIndex, ded, path, r);
    } else {
        if (client_fd >= 0) close(client_fd);
        m = calloc(1, sizeof(*m));
        m->size = ai.allocationSize;
        m->type = ai.memoryTypeIndex;
        /* An import that also asks to be exportable (Zink re-imports the buffers it shares with
         * export info on) keeps the imported pages: export emulation here would hand it fresh ones,
         * and everything drawn to the import would land where nobody looks (tablet run 24: every
         * buffer Xwayland presented was black). Its later exports give back the imported dma-buf. */
        int opaque = vkb_opaque_alloc_info(t, &ai); /* OPAQUE_FD as dma-bufs (server_opaque.c) */
        int import_fd = -1;
        for (const VkBaseInStructure *b = ai.pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR &&
                ((const VkImportMemoryFdInfoKHR *)b)->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
                import_fd = ((const VkImportMemoryFdInfoKHR *)b)->fd;
        int emulating = t->pd && (t->pd->info.emu & VKB_EMU_DMABUF_EXPORT) && wants_dmabuf_export(&ai);
        const char *path = import_fd >= 0 ? "dma-buf import" : opaque ? "opaque fd as dma-buf" : "plain";
        if (import_fd >= 0) {
            fit_import_type(t, &ai, import_fd);
            m->type = ai.memoryTypeIndex;
            /* A dma-buf smaller than the import asks for is refused by the driver
             * (VK_ERROR_INVALID_EXTERNAL_HANDLE): said with both sizes, the first few times. */
            off_t have_len = lseek(import_fd, 0, SEEK_END);
            if (have_len >= 0 && (uint64_t)have_len < ai.allocationSize) {
                static int told;
                if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
                    VKB_WARN("dma-buf import: %llu bytes asked for, the dma-buf has %llu (an importer's image laid out unlike the "
                             "exporter's?)", (unsigned long long)ai.allocationSize, (unsigned long long)have_len);
            }
            lseek(import_fd, 0, SEEK_SET);
        }
        uint32_t type = ai.memoryTypeIndex;
        if (fake_lazy() && import_fd < 0 && !opaque) {
            /* Mali refuses ordinary memory in its lazily allocated type (the imitation above). */
            VkPhysicalDeviceMemoryProperties mp;
            t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
            if (ai.memoryTypeIndex < mp.memoryTypeCount && lazy_imitated(mp.memoryTypes[ai.memoryTypeIndex].propertyFlags)) {
                r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                path = "plain (lazily allocated, imitated)";
                goto plain_done;
            }
        }
        if (emulating && import_fd < 0) {
            /* The chain as it came: alloc_exportable unlinks structures from it. */
            const VkBaseInStructure *links[32];
            int nl = 0;
            for (const VkBaseInStructure *b = ai.pNext; b && nl < 32; b = b->pNext) links[nl++] = b;
            int whole = nl < 32;
            r = alloc_exportable(t, &ai, m);
            path = m->ahb ? "export emulation (AHardwareBuffer)" : "export emulation (dma-heap)";
            type = ai.memoryTypeIndex;
            /* Pages the dma-heap would not give, or the driver would not import: the memory is still
             * made, without a dma-buf (most allocations asking for export are never exported; one
             * that is fails at vkGetMemoryFdKHR instead of here). VKBRIDGE_EXPORT_FALLBACK=0: not. */
            const char *fb = vkb_srv_env_global("VKBRIDGE_EXPORT_FALLBACK");
            if (r != VK_SUCCESS && whole && !(fb && *fb == '0')) {
                log_alloc(t, &ai, type, ded, path, r);
                ai.pNext = nl ? links[0] : NULL;
                for (int i = 0; i < nl; i++) ((VkBaseInStructure *)links[i])->pNext = i + 1 < nl ? links[i + 1] : NULL;
                chain_drop(&ai.pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
                ai.memoryTypeIndex = type = m->type;
                r = c->dt->vkAllocateMemory(dev, &ai, NULL, &m->mem);
                path = "plain, after export emulation failed (no dma-buf to export)";
                if (r == VK_SUCCESS) VKB_ONCE(VKB_LOG_WARN, "export emulation failed; memory made without a dma-buf instead (exporting it will fail)");
            }
        } else {
            int keep = emulating ? fcntl(import_fd, F_DUPFD_CLOEXEC, 0) : -1;
            if (emulating) chain_drop(&ai.pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
            r = c->dt->vkAllocateMemory(dev, &ai, NULL, &m->mem);
            if (r == VK_SUCCESS && keep >= 0) m->export_fd1 = keep + 1;
            else if (keep >= 0) close(keep);
        }
        if (r != VK_SUCCESS && opaque) r = vkb_opaque_alloc_retry(t, &ai, m);
        if (r != VK_SUCCESS && t->emu && vkb_emu_strip_opaque(&ai, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO))
            r = c->dt->vkAllocateMemory(dev, &ai, NULL, &m->mem);
    plain_done:
        log_alloc(t, &ai, type, ded, path, r);
        if (r != VK_SUCCESS) {
            close_import_fds(ai.pNext);
            free(m);
            m = NULL;
        }
    }
    if (m) {
        m->stat_ded = ded;
        vkb_mem_track(t, m);
    }
    vkb_enc_u32(&c->r, (uint32_t)r);
    vkb_enc_u64(&c->r, m ? (uint64_t)(uintptr_t)m->mem : 0);
    vkb_enc_fd(&c->r, out_fd);
    if (out_fd >= 0) vkb_srv_close_after_send(c, out_fd);
    vkb_enc_u64(&c->r, m ? (uint64_t)m->size : 0);
}

void vkb_sv_vkFreeMemory(vkb_srv_call *c)
{
    VkDevice dev = (VkDevice)(uintptr_t)vkb_dec_u64(&c->d);
    VkDeviceMemory mem;
    vkb_dec_bytes(&c->d, &mem, sizeof(mem));
    vkb_srv_table *t = c->table;
    if (c->d.err || !t || !t->is_device || t->device != dev) {
        vkb_srv_bad_message(c);
        return;
    }
    if (!mem) return;
    vkb_srv_mem *m = vkb_mem_find(t, mem, 1);
    if (m) {
        vkb_mem_release(t, m);
    } else {
        VKB_WARN("vkFreeMemory of untracked memory");
        c->dt->vkFreeMemory(dev, mem, NULL);
    }
}

static VkResult ensure_server_map(vkb_srv_table *t, vkb_srv_mem *m)
{
    if (m->mapped) return VK_SUCCESS;
    pthread_mutex_lock(&t->lock);
    VkResult r = VK_SUCCESS;
    if (!m->mapped) r = t->real.vkMapMemory(t->device, m->mem, 0, VK_WHOLE_SIZE, 0, &m->mapped);
    pthread_mutex_unlock(&t->lock);
    return r;
}

static void flush_or_invalidate(vkb_srv_call *c, int flush)
{
    VkDevice dev = (VkDevice)(uintptr_t)vkb_dec_u64(&c->d);
    uint32_t n = vkb_dec_u32(&c->d);
    vkb_srv_table *t = c->table;
    if (c->d.err || !t || !t->is_device || t->device != dev || n > 65536) {
        vkb_srv_bad_message(c);
        return;
    }
    VkMappedMemoryRange *ranges = vkb_dec_alloc(&c->d, (n ? n : 1) * sizeof(*ranges));
    for (uint32_t i = 0; ranges && i < n; i++) vkb_dec_VkMappedMemoryRange(&c->d, &ranges[i]);
    if (c->d.err) {
        vkb_srv_bad_message(c);
        return;
    }
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < n && r == VK_SUCCESS; i++) {
        vkb_srv_mem *m = vkb_mem_find(t, ranges[i].memory, 0);
        if (m) r = ensure_server_map(t, m);
    }
    /* Cached readback: the client's writes cleaned to memory before the driver's flush. The
     * allocation as a whole (a dma-buf sync has no range). Several ranges of one allocation in a
     * row are synced once. */
    VkDeviceMemory last = VK_NULL_HANDLE;
    for (uint32_t i = 0; flush && r == VK_SUCCESS && i < n; i++) {
        vkb_srv_mem *m = ranges[i].memory != last ? vkb_mem_find(t, ranges[i].memory, 0) : NULL;
        last = ranges[i].memory;
        if (m && m->sync_fd1 && dmabuf_sync(m, 1) < 0) r = VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (r == VK_SUCCESS && n) {
        r = flush ? c->dt->vkFlushMappedMemoryRanges(dev, n, ranges) : c->dt->vkInvalidateMappedMemoryRanges(dev, n, ranges);
    }
    /* ... and the client's stale lines dropped after the driver's invalidate. */
    last = VK_NULL_HANDLE;
    for (uint32_t i = 0; !flush && r == VK_SUCCESS && i < n; i++) {
        vkb_srv_mem *m = ranges[i].memory != last ? vkb_mem_find(t, ranges[i].memory, 0) : NULL;
        last = ranges[i].memory;
        if (m && m->sync_fd1 && dmabuf_sync(m, 0) < 0) r = VK_ERROR_MEMORY_MAP_FAILED;
    }
    vkb_enc_u32(&c->r, (uint32_t)r);
}

void vkb_sv_vkFlushMappedMemoryRanges(vkb_srv_call *c) { flush_or_invalidate(c, 1); }
void vkb_sv_vkInvalidateMappedMemoryRanges(vkb_srv_call *c) { flush_or_invalidate(c, 0); }

/* ------------------------------------------------------------------ self-test */

typedef struct {
    const vkb_dispatch *idt;
    vkb_dispatch dt;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    uint32_t qf;
    VkPhysicalDeviceMemoryProperties mp;
    VkDeviceSize import_align;
    uint32_t classes[16]; /* memoryTypeBits of the probed images and unshared buffers (probe_image_types) */
    int nclasses;
    int more_usages; /* the sharing test looks for a buffer usage that allows the type (resources disagree) */
    int unflagged;   /* the test buffer is made without the strategy's external handle type (the fallback below) */
    uint32_t told;   /* test buffers' masks logged, per strategy and unflagged (log_test_buffer) */
} selftest;

/* Buffer usages the probes try: what apps make (all in one, vertex/index, indirect, uniform,
 * storage, texel, staging, readback). The XRing O3's buffers allow different types by usage. */
static const VkBufferUsageFlags probe_usages[] = {
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
};
#define PROBE_USAGES (sizeof(probe_usages) / sizeof(probe_usages[0]))

/* The host-visible types of a mask. */
static uint32_t hv_bits(const selftest *st, uint32_t bits)
{
    uint32_t hv = 0;
    for (uint32_t t = 0; t < st->mp.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (st->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) hv |= 1u << t;
    return hv;
}

/* A self-test buffer's memoryTypeBits. VKBRIDGE_FAKE_MISSING=extbufhv (host tests): what the
 * Xclipse 530's driver is thought to do (Galaxy A55, Samsung 24.0.539, 2026-10-26 logs): a buffer
 * made with external handle types allows no host-visible type. */
static uint32_t st_buffer_bits(selftest *st, VkBuffer b, VkExternalMemoryHandleTypeFlags handles, VkMemoryRequirements *r)
{
    st->dt.vkGetBufferMemoryRequirements(st->dev, b, r);
    static int fake = -1;
    if (fake < 0) fake = vkb_emu_faked("extbufhv");
    if (fake && handles) r->memoryTypeBits &= ~hv_bits(st, r->memoryTypeBits);
    return r->memoryTypeBits;
}

/* memoryTypeBits of a buffer of that usage and those external handle types; 0: not made. */
static uint32_t buffer_bits(selftest *st, VkBufferUsageFlags usage, VkExternalMemoryHandleTypeFlags handles, VkDeviceSize size)
{
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, handles};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, handles ? &ext : NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b;
    if (st->dt.vkCreateBuffer(st->dev, &bci, NULL, &b) != VK_SUCCESS) return 0;
    VkMemoryRequirements r;
    st_buffer_bits(st, b, handles, &r);
    st->dt.vkDestroyBuffer(st->dev, b, NULL);
    return r.memoryTypeBits;
}

/* Once per strategy (and once more unflagged): what the driver did with the sharing test's buffer
 * and the probe usages, with and without the handle type. Before this, a driver that refused every
 * type at buffer creation left no line at all (Xclipse 530: "types 0x0 shared" and nothing else). */
static void log_test_buffer(selftest *st, uint32_t s, VkExternalMemoryHandleTypeFlags handle, VkBufferUsageFlags usage, VkResult r,
                            uint32_t bits)
{
    uint32_t bit = 1u << ((s & 15) * 2 + (st->unflagged ? 1 : 0));
    if (st->told & bit) return;
    st->told |= bit;
    char line[256] = "";
    int ln = 0;
    for (size_t u = 0; handle && u < PROBE_USAGES; u++)
        ln += snprintf(line + ln, ln < (int)sizeof(line) ? sizeof(line) - (size_t)ln : 0, " 0x%x", buffer_bits(st, probe_usages[u], handle, 65536));
    uint32_t plain = buffer_bits(st, usage, 0, 256u * 1024u);
    VKB_INFO("memory sharing: %s test buffer (usage 0x%x) with handle types 0x%x%s: create %d, types 0x%x (host-visible 0x%x); "
             "without handle types 0x%x (host-visible 0x%x)%s%s",
             vkb_mem_strategy_name(s), usage, handle, st->unflagged ? " (unflagged)" : "", r, bits, hv_bits(st, bits), plain,
             hv_bits(st, plain), handle ? "; probe usages with the handle type:" : "", line);
}

/* Once, for dmaheap: the memory types a dma-heap fd imports into (vkGetMemoryFdPropertiesKHR). */
static void log_dmaheap_fd_types(selftest *st)
{
    int hfd = dmaheap_fd();
    if (hfd < 0 || !st->dt.vkGetMemoryFdPropertiesKHR) {
        VKB_INFO("dmaheap: no fd properties (%s)", hfd < 0 ? "no dma-heap" : "vkGetMemoryFdPropertiesKHR not loaded");
        return;
    }
    struct dma_heap_allocation_data hd = {65536, 0, O_RDWR | O_CLOEXEC, 0};
    if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &hd) < 0) {
        VKB_INFO("dmaheap: no fd properties (allocating 64 KiB failed: %s)", strerror(errno));
        return;
    }
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult pr = st->dt.vkGetMemoryFdPropertiesKHR(st->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, (int)hd.fd, &fp);
    close((int)hd.fd);
    VKB_INFO("dmaheap: a dma-heap fd imports into memory types 0x%x (host-visible 0x%x; result %d)", fp.memoryTypeBits,
             hv_bits(st, fp.memoryTypeBits), pr);
}

static int submit_and_wait(selftest *st, VkCommandBuffer cb)
{
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    if (st->dt.vkCreateFence(st->dev, &fci, NULL, &f) != VK_SUCCESS) return 0;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    int ok = st->dt.vkQueueSubmit(st->queue, 1, &si, f) == VK_SUCCESS &&
             st->dt.vkWaitForFences(st->dev, 1, &f, VK_TRUE, 5000000000ull) == VK_SUCCESS;
    st->dt.vkDestroyFence(st->dev, f, NULL);
    return ok;
}

#define TEST_SIZE (256u * 1024u)

/* One allocation of `type` with strategy `s`, checked from both sides. 1 = shared and coherent
 * (no flush needed), 2 = shared when flushed, 0 = not shared. cached: cached readback pages, with
 * the cache maintenance the server does for the client (always flushed). */
static int test_type_mode(selftest *st, uint32_t s, uint32_t type, int no_flush, int cached);

/* Without flushes first: imported pages from an uncached heap are coherent in practice even where
 * the driver's type does not say HOST_COHERENT (Mali-G720: dma-heap imports land in type 1,
 * HOST_CACHED without COHERENT, and pass with no flush). Only if that fails, with flushes. */
static int test_type(selftest *st, uint32_t s, uint32_t type)
{
    int coherent_flag = (st->mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!coherent_flag && !vkb_srv_env_global("VKBRIDGE_SELFTEST_NOFLUSH")) {
        int r = test_type_mode(st, s, type, 1, 0);
        if (r) {
            VKB_INFO("  %s, type %u: coherent in practice (no flushes needed)", vkb_mem_strategy_name(s), type);
            return 1;
        }
    }
    return test_type_mode(st, s, type, 0, 0);
}

/* What flush_or_invalidate does for a client: the dma-buf sync around the driver's call. */
static void st_flush(selftest *st, vkb_srv_mem *m, const VkMappedMemoryRange *whole, int flush)
{
    if (flush) {
        dmabuf_sync(m, 1);
        st->dt.vkFlushMappedMemoryRanges(st->dev, 1, whole);
    } else {
        st->dt.vkInvalidateMappedMemoryRanges(st->dev, 1, whole);
        dmabuf_sync(m, 0);
    }
}

static int test_type_mode(selftest *st, uint32_t s, uint32_t type, int no_flush, int cached)
{
    const vkb_dispatch *dt = &st->dt;
    int result = 0;
    /* unflagged: the buffer without the handle type, as the client then makes its buffers */
    VkExternalMemoryHandleTypeFlags handle = st->unflagged ? 0 : strategy_handle_type(s);
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, handle};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, handle ? &ext : NULL, 0, TEST_SIZE,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf = VK_NULL_HANDLE;
    VkMemoryRequirements req;
    /* A type this buffer does not allow is tried with another usage that does (and a transfer
     * one, for the copies below), when the driver's resources disagree (more_usages): on the
     * XRing O3 the indirect and texel buffers allow types this one does not. */
    for (size_t u = 0;; u++) {
        VkResult cr = dt->vkCreateBuffer(st->dev, &bci, NULL, &buf);
        if (cr == VK_SUCCESS) st_buffer_bits(st, buf, handle, &req);
        if (u == 0) log_test_buffer(st, s, handle, bci.usage, cr, cr == VK_SUCCESS ? req.memoryTypeBits : 0);
        if (cr != VK_SUCCESS) return 0;
        if (req.memoryTypeBits & (1u << type)) break;
        dt->vkDestroyBuffer(st->dev, buf, NULL);
        if (!st->more_usages || u >= PROBE_USAGES) return 0;
        bci.usage = probe_usages[u] | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (bci.usage != (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        VKB_INFO("  %s, type %u: tested with a buffer of usage 0x%x (the default one does not allow it)", vkb_mem_strategy_name(s), type,
                 bci.usage);
    VkDeviceSize size = req.size;
    if (s == VKB_MEM_HOSTPTR) size = (size + st->import_align - 1) & ~(st->import_align - 1);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size, type};
    int cfd = -1;
    if (s == VKB_MEM_HOSTPTR) {
        cfd = vkb_memfd("vkbridge-selftest", (size_t)size);
        if (cfd < 0) {
            dt->vkDestroyBuffer(st->dev, buf, NULL);
            return 0;
        }
    }
    int keep_fd = s == VKB_MEM_HOSTPTR ? fcntl(cfd, F_DUPFD_CLOEXEC, 0) : -1;
    vkb_srv_mem *m = NULL;
    int out_fd = -1;
    VkResult r = alloc_shared(dt, st->dev, s, &ai, cfd, cached, &m, &out_fd);
    const char *cn = cached ? " (cached readback)" : "";
    if (r != VK_SUCCESS) {
        VKB_INFO("  %s, type %u%s: allocation failed (%d)", vkb_mem_strategy_name(s), type, cn, r);
        if (keep_fd >= 0) close(keep_fd);
        dt->vkDestroyBuffer(st->dev, buf, NULL);
        return 0;
    }
    int map_fd = s == VKB_MEM_HOSTPTR ? keep_fd : out_fd;
    uint8_t *cmap = mmap(NULL, (size_t)m->size, PROT_READ | PROT_WRITE, MAP_SHARED, map_fd, 0);
    if (map_fd >= 0) close(map_fd);
    void *smap = NULL;
    if (cmap == MAP_FAILED) {
        VKB_INFO("  %s, type %u: the client cannot map the memory (%s)", vkb_mem_strategy_name(s), type, strerror(errno));
        cmap = NULL;
        goto out;
    }
    if (dt->vkBindBufferMemory(st->dev, buf, m->mem, 0) != VK_SUCCESS ||
        dt->vkMapMemory(st->dev, m->mem, 0, VK_WHOLE_SIZE, 0, &smap) != VK_SUCCESS) {
        VKB_INFO("  %s, type %u: bind/map failed", vkb_mem_strategy_name(s), type);
        goto out;
    }
    int coherent = (st->mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    /* VKBRIDGE_SELFTEST_NOFLUSH=1 forces it for every type. */
    if (no_flush || vkb_srv_env_global("VKBRIDGE_SELFTEST_NOFLUSH")) coherent = 1;
    if (cached) coherent = 0; /* always flushed: what the client does with these pages */
    VkMappedMemoryRange whole = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, m->mem, 0, VK_WHOLE_SIZE};

    /* 1: the two CPU mappings are the same pages. */
    for (uint32_t i = 0; i < TEST_SIZE / 4; i += 997) ((volatile uint32_t *)cmap)[i] = 0x11220000u + i;
    __sync_synchronize();
    for (uint32_t i = 0; i < TEST_SIZE / 4; i += 997) {
        if (((volatile uint32_t *)smap)[i] != 0x11220000u + i) {
            VKB_INFO("  %s, type %u: CPU mappings differ at word %u", vkb_mem_strategy_name(s), type, i);
            goto out;
        }
    }
    /* 2: GPU writes, client reads.  3: client writes, GPU copies, client reads the copy. */
    for (uint32_t i = TEST_SIZE / 8; i < TEST_SIZE / 8 + TEST_SIZE / 32; i++) ((volatile uint32_t *)cmap)[i] = 0x5a000000u ^ (i * 2654435761u);
    if (!coherent) st_flush(st, m, &whole, 1);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, st->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    if (dt->vkAllocateCommandBuffers(st->dev, &cai, &cb) != VK_SUCCESS) goto out;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    dt->vkBeginCommandBuffer(cb, &bi);
    dt->vkCmdFillBuffer(cb, buf, 0, TEST_SIZE / 8, 0xA5A5A5A5u);
    /* Layout (bytes): [0, N/8) GPU fill; [N/2, N/2 + N/8) written by the client and copied by
     * the GPU to [3N/4, 3N/4 + N/8). */
    VkBufferCopy region = {TEST_SIZE / 2, TEST_SIZE / 4 * 3, TEST_SIZE / 8};
    dt->vkCmdCopyBuffer(cb, buf, buf, 1, &region);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    dt->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    dt->vkEndCommandBuffer(cb);
    int submitted = submit_and_wait(st, cb);
    dt->vkFreeCommandBuffers(st->dev, st->pool, 1, &cb);
    if (!submitted) {
        VKB_INFO("  %s, type %u: GPU submission failed", vkb_mem_strategy_name(s), type);
        goto out;
    }
    if (!coherent) st_flush(st, m, &whole, 0);
    for (uint32_t i = 0; i < TEST_SIZE / 32; i++) {
        if (((volatile uint32_t *)cmap)[i] != 0xA5A5A5A5u) {
            VKB_INFO("  %s, type %u: the client does not see a GPU write (word %u = 0x%08x)", vkb_mem_strategy_name(s), type, i,
                     ((volatile uint32_t *)cmap)[i]);
            goto out;
        }
    }
    for (uint32_t i = 0; i < TEST_SIZE / 32; i++) {
        uint32_t want = 0x5a000000u ^ ((TEST_SIZE / 8 + i) * 2654435761u);
        uint32_t got = ((volatile uint32_t *)cmap)[TEST_SIZE / 16 * 3 + i];
        if (got != want) {
            VKB_INFO("  %s, type %u: the GPU does not see a client write (word %u = 0x%08x, want 0x%08x)", vkb_mem_strategy_name(s),
                     type, i, got, want);
            goto out;
        }
    }
    result = coherent ? 1 : 2;
    VKB_INFO("  %s, type %u (flags 0x%x)%s: shared OK%s", vkb_mem_strategy_name(s), type, st->mp.memoryTypes[type].propertyFlags, cn,
             coherent ? ", coherent" : ", needs flushes");
out:
    if (cmap) munmap(cmap, (size_t)m->size);
    if (smap) dt->vkUnmapMemory(st->dev, m->mem);
    dt->vkDestroyBuffer(st->dev, buf, NULL);
    dt->vkFreeMemory(st->dev, m->mem, NULL);
    mem_release_backing(m);
    return result;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

/* What the driver says it can do with each kind of external buffer memory (the device runs). */
static void log_external_props(const vkb_dispatch *idt, VkPhysicalDevice pd)
{
    if (!idt->vkGetPhysicalDeviceExternalBufferProperties) return;
    static const struct { uint32_t bit; const char *name; } types[] = {
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "opaque fd"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "dma-buf"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, "host allocation"},
#ifdef __ANDROID__
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID, "AHB"},
#endif
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        VkPhysicalDeviceExternalBufferInfo bi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0,
                                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                 (VkExternalMemoryHandleTypeFlagBits)types[i].bit};
        VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        idt->vkGetPhysicalDeviceExternalBufferProperties(pd, &bi, &ep);
        const VkExternalMemoryProperties *p = &ep.externalMemoryProperties;
        VKB_INFO("external buffers, %s: features 0x%x (%s%s%s), export from 0x%x, compatible 0x%x", types[i].name,
                 p->externalMemoryFeatures,
                 p->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ? "dedicated-only " : "",
                 p->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? "exportable " : "",
                 p->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ? "importable" : "",
                 p->exportFromImportedHandleTypes, p->compatibleHandleTypes);
    }
}

/*
 * Cached readback, for the strategy chosen: each shareable type the driver calls HOST_CACHED is
 * tried with cached pages and the client's flushes and invalidates (the server's dma-buf syncs).
 * Only HOST_CACHED types: the server's own mapping (vkMapMemory, for the driver's flushes) is then
 * cached too, never a cached and an uncached mapping of one page. A pass says the pages, the import
 * and the syncs work; it cannot prove that no stale line ever survives (a race), which is why the
 * pages are offered only per VKBRIDGE_CACHED_READBACK (vkb_mem_cached_gate). The test runs either
 * way, so the log has the answer. VKBRIDGE_CACHED_READBACK=0 skips it.
 */
static uint32_t cached_selftest(selftest *st, uint32_t s, uint32_t shareable)
{
    const char *e = vkb_srv_env_global("VKBRIDGE_CACHED_READBACK");
    if (e && *e == '0') return 0;
    if (s != VKB_MEM_DMABUF && s != VKB_MEM_DMAHEAP && s != VKB_MEM_AHB) {
        VKB_INFO("cached readback: not with %s (no dma-buf to sync through)", vkb_mem_strategy_name(s));
        return 0;
    }
    uint32_t ok = 0;
    for (uint32_t t = 0; t < st->mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags f = st->mp.memoryTypes[t].propertyFlags;
        if (!(shareable & (1u << t)) || !(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) continue;
        if (test_type_mode(st, s, t, 0, 1)) ok |= 1u << t;
    }
    VKB_INFO("cached readback: %s, types 0x%x pass with cached pages", vkb_mem_strategy_name(s), ok);
    return ok;
}

/* What the client is told (vkbQueryServer): the self-test's types, when VKBRIDGE_CACHED_READBACK
 * (server environment or <cache dir>/env) and the mode allow them.
 *   0     off;
 *   1     the client's own frame readback only (VKB_CACHED_WSI);
 *   apps  that, and a HOST_CACHED type without HOST_COHERENT for apps (VKB_CACHED_APPS);
 *   unset: 1 in performance mode, 0 in safe mode. */
void vkb_mem_cached_gate(vkb_server_pd_info *info, int mode)
{
    const char *e = vkb_srv_env("VKBRIDGE_CACHED_READBACK");
    uint32_t flags;
    if (e && *e) flags = *e == '0' ? 0 : !strcmp(e, "apps") ? VKB_CACHED_WSI | VKB_CACHED_APPS : VKB_CACHED_WSI;
    else flags = mode == VKB_MODE_PERFORMANCE ? VKB_CACHED_WSI : 0;
    if (!flags || !info->cached_types) {
        info->cached_types = 0;
        info->cached_flags = 0;
        return;
    }
    info->cached_flags = flags;
    static uint32_t said = ~0u;
    if (said != (flags | info->cached_types << 8)) {
        said = flags | info->cached_types << 8;
        VKB_INFO("cached readback: offered in types 0x%x to %s (%s)", info->cached_types,
                 flags & VKB_CACHED_APPS ? "the client's frame readback and apps" : "the client's frame readback",
                 e && *e ? "VKBRIDGE_CACHED_READBACK" : "performance mode");
    }
}

/*
 * The memory types ordinary resources can all live in: the AND of the memoryTypeBits of a few
 * optimal-tiling images (colour, depth, storage) and a plain buffer. The client backs its
 * device-local type (where images and GPU-only buffers go) with an ordinary device-local type from
 * these. Picking just the driver's first such type left every image without a type on a driver
 * whose images do not list that one (Mali-G2-Ultra, XRing O3, driver 56: issue #1).
 */
static int ordinary_dl(VkMemoryPropertyFlags f)
{
    return (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT));
}

static uint32_t probe_image_types(selftest *st)
{
    static const struct {
        VkFormat fmt;
        VkImageUsageFlags usage;
    } imgs[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
        {VK_FORMAT_D16_UNORM, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
    };
    uint32_t images = ~0u, seen = 0;
    for (size_t i = 0; i < sizeof(imgs) / sizeof(imgs[0]); i++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, imgs[i].fmt, {64, 64, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, imgs[i].usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        if (st->dt.vkCreateImage(st->dev, &ci, NULL, &img) != VK_SUCCESS) continue;
        VkMemoryRequirements r;
        st->dt.vkGetImageMemoryRequirements(st->dev, img, &r);
        st->dt.vkDestroyImage(st->dev, img, NULL);
        images &= r.memoryTypeBits;
        st->classes[st->nclasses++] = r.memoryTypeBits;
        seen++;
    }
    /* Depth-stencil too, for the device-local types (dl_types), not for this mask. */
    static const VkFormat ds[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    for (size_t i = 0; i < 2; i++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, ds[i], {64, 64, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0,
                                NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        VkImageFormatProperties ifp;
        if (st->idt->vkGetPhysicalDeviceImageFormatProperties(st->pd, ds[i], VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &ifp) != VK_SUCCESS ||
            st->dt.vkCreateImage(st->dev, &ci, NULL, &img) != VK_SUCCESS)
            continue;
        VkMemoryRequirements r;
        st->dt.vkGetImageMemoryRequirements(st->dev, img, &r);
        st->dt.vkDestroyImage(st->dev, img, NULL);
        st->classes[st->nclasses++] = r.memoryTypeBits;
        break;
    }
    /* Unshared buffers of each usage (the first: every usage, the mask's plain buffer). */
    uint32_t buffers = ~0u;
    for (size_t u = 0; u < PROBE_USAGES; u++) {
        uint32_t bits = buffer_bits(st, probe_usages[u], 0, 65536);
        if (!bits) continue;
        if (u == 0) buffers = bits;
        st->classes[st->nclasses++] = bits;
    }
    if (!seen) return 0;
    /* Images and buffers in one type if there is one; else what the images allow (buffers are
     * the less picky). */
    uint32_t both = images & buffers, any_both = 0, any_img = 0;
    for (uint32_t t = 0; t < st->mp.memoryTypeCount && t < 32; t++) {
        VkMemoryPropertyFlags f = st->mp.memoryTypes[t].propertyFlags;
        if (fake_lazy() && lazy_imitated(f)) f |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        if (!ordinary_dl(f)) continue;
        if (both & (1u << t)) any_both = 1;
        if (images & (1u << t)) any_img = 1;
    }
    uint32_t types = any_both ? both : any_img ? images : 0;
    VKB_INFO("memory: images allow types 0x%x, plain buffers 0x%x: ordinary resources in 0x%x", images, buffers, types);
    if (!types) VKB_WARN("memory: no ordinary device-local type that images allow (images 0x%x); the client keeps its old choice", images);
    return types;
}

static uint32_t ordinary_dl_bits(selftest *st, uint32_t bits)
{
    uint32_t m = 0;
    for (uint32_t t = 0; t < st->mp.memoryTypeCount && t < 32; t++) {
        VkMemoryPropertyFlags f = st->mp.memoryTypes[t].propertyFlags;
        if (fake_lazy() && lazy_imitated(f)) f |= VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        if ((bits & (1u << t)) && ordinary_dl(f)) m |= 1u << t;
    }
    return m;
}

/*
 * The client's device-local types without host access (vkb_server_pd_info.dl_types): one is
 * enough where every image and buffer allows the one the client backs it with (the first ordinary
 * device-local type images allow, as the client picks it from image_types): then 0, and nothing
 * changes. On the XRing O3 (issue #1) images allow type 6 and buffers only 0x3: every buffer's
 * mask came out without a device-local type. Then one more for each probed resource kind that
 * allows none of those chosen (its first ordinary device-local type), at most 4. `handles`: the
 * external handle types the client puts on its buffers (they change the masks), 0: none.
 */
static uint32_t probe_dl_types(selftest *st, uint32_t image_types, VkExternalMemoryHandleTypeFlags handles, int quiet)
{
    uint32_t classes[16 + PROBE_USAGES];
    int n = 0;
    for (int i = 0; i < st->nclasses; i++) classes[n++] = st->classes[i];
    for (size_t u = 0; handles && u < PROBE_USAGES; u++) {
        uint32_t bits = buffer_bits(st, probe_usages[u], handles, 65536);
        if (bits) classes[n++] = bits;
    }
    uint32_t first = ordinary_dl_bits(st, image_types);
    if (!first) first = ordinary_dl_bits(st, ~0u);
    if (!first) return 0;
    uint32_t chosen = first & -first;
    char line[256] = "";
    int ln = 0;
    for (int i = 0; i < n && __builtin_popcount(chosen) < 4; i++) {
        uint32_t dl = ordinary_dl_bits(st, classes[i]);
        if (!dl || (dl & chosen)) continue;
        chosen |= dl & -dl;
        ln += snprintf(line + ln, ln < (int)sizeof(line) ? sizeof(line) - (size_t)ln : 0, " 0x%x->%d", classes[i], __builtin_ctz(dl));
    }
    if (__builtin_popcount(chosen) < 2) return 0;
    if (!quiet)
        VKB_INFO("memory: resources disagree on device-local types: the client gets types 0x%x (images' %d; resource masks%s)", chosen,
                 __builtin_ctz(first), line);
    return chosen;
}

#ifdef __ANDROID__
/* One BLOB AHardwareBuffer as the export emulation makes them (64 KiB, CPU usage cpu), its dma-buf
 * imported into a buffer on the self-test's device, mapped where a host-visible type takes it, a
 * word written and read back: 1 when all of it works, else 0 and why. Only the symbols resolving
 * said nothing about the phones this path is for (Android 10-12 grallocs, ION, kernel 4.14-4.19):
 * BLOB with GPU_DATA_BUFFER may be refused, the pages may sit in another fd of the handle, the
 * import may fail. VKBRIDGE_FAKE_MISSING=ahbexport (device tests): the allocation refused. */
static int ahb_export_try(selftest *st, uint64_t cpu, char *why, size_t cap)
{
    const uint64_t len = 64u * 1024u;
    vkb_ahb_desc d = {(uint32_t)len, 1, 1, AHB_FORMAT_BLOB, cpu | AHB_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    struct AHardwareBuffer *ahb = NULL;
    int faked = vkb_emu_faked("ahbexport");
    int ar = faked ? -12 : p_AHardwareBuffer_allocate(&d, &ahb);
    if (ar != 0 || !ahb) {
        snprintf(why, cap, "allocate refused (%d)%s", ar, faked ? " [VKBRIDGE_FAKE_MISSING=ahbexport]" : "");
        return 0;
    }
    const vkb_native_handle *nh = p_AHardwareBuffer_getNativeHandle(ahb);
    int ok = 0, fd = -1;
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    int pick = ahb_pages_fd(nh, len);
    if (pick < 0 || (fd = fcntl(pick, F_DUPFD_CLOEXEC, 0)) < 0) {
        snprintf(why, cap, "no fd of %llu bytes among the handle's %d", (unsigned long long)len, nh ? nh->numFds : -1);
        goto out;
    }
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = st->dt.vkGetMemoryFdPropertiesKHR
                     ? st->dt.vkGetMemoryFdPropertiesKHR(st->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp)
                     : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (r != VK_SUCCESS) {
        snprintf(why, cap, "the driver will not describe its dma-buf (%d)", r);
        goto out;
    }
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, len, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if ((r = st->dt.vkCreateBuffer(st->dev, &bci, NULL, &b)) != VK_SUCCESS) {
        b = VK_NULL_HANDLE;
        snprintf(why, cap, "a dma-buf buffer refused (%d)", r);
        goto out;
    }
    VkMemoryRequirements mr;
    st->dt.vkGetBufferMemoryRequirements(st->dev, b, &mr);
    uint32_t bits = 0, type = UINT32_MAX;
    for (uint32_t i = 0; i < st->mp.memoryTypeCount; i++)
        if (!(st->mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT))) bits |= 1u << i;
    bits &= fp.memoryTypeBits & mr.memoryTypeBits;
    for (uint32_t i = 0; i < st->mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((bits & (1u << i)) && (st->mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) type = i;
    if (type == UINT32_MAX && bits) type = (uint32_t)__builtin_ctz(bits);
    if (type == UINT32_MAX || mr.size > len) {
        snprintf(why, cap, "no memory type takes the dma-buf (0x%x) and a buffer (0x%x, %llu bytes)", fp.memoryTypeBits, mr.memoryTypeBits,
                 (unsigned long long)mr.size);
        goto out;
    }
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, len, type};
    if ((r = st->dt.vkAllocateMemory(st->dev, &ai, NULL, &m)) != VK_SUCCESS) {
        m = VK_NULL_HANDLE;
        snprintf(why, cap, "the import refused (%d, type %u)", r, type);
        goto out;
    }
    fd = -1; /* the driver's now */
    if ((r = st->dt.vkBindBufferMemory(st->dev, b, m, 0)) != VK_SUCCESS) {
        snprintf(why, cap, "binding the imported memory refused (%d)", r);
        goto out;
    }
    if (!(st->mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        /* gamescope's images need the import, not a CPU mapping */
        snprintf(why, cap, "%d fd(s) in the handle, imported into type %u (not host-visible: not mapped)", nh->numFds, type);
        ok = 1;
        goto out;
    }
    void *p = NULL;
    if ((r = st->dt.vkMapMemory(st->dev, m, 0, VK_WHOLE_SIZE, 0, &p)) != VK_SUCCESS || !p) {
        snprintf(why, cap, "mapping the imported memory refused (%d)", r);
        goto out;
    }
    ((volatile uint32_t *)p)[100] = 0x5eed1234u;
    uint32_t got = ((volatile uint32_t *)p)[100];
    st->dt.vkUnmapMemory(st->dev, m);
    ok = got == 0x5eed1234u;
    snprintf(why, cap, "%d fd(s) in the handle, imported into type %u, mapped: %s", nh->numFds, type, ok ? "read back" : "read back WRONG");
out:
    if (b) st->dt.vkDestroyBuffer(st->dev, b, NULL);
    if (m) st->dt.vkFreeMemory(st->dev, m, NULL);
    if (fd >= 0) close(fd);
    p_AHardwareBuffer_release(ahb);
    return ok;
}

/* Where there is no dma-heap: the AHardwareBuffer path tried once with the export emulation's own
 * CPU usage ("rarely", uncached), again with "often" (some grallocs refuse "rarely" for BLOB), and
 * only a pass lets vkb_mem_export_backing offer it. */
static void ahb_export_check(selftest *st)
{
    if (ahb_export_ok >= 0 || vkb_mem_dmaheap_available() || !ahb_load()) return;
    char rarely[160], often[160];
    if (ahb_export_try(st, 2ull | (2ull << 4), rarely, sizeof(rarely))) {
        ahb_export_ok = 1;
        snprintf(ahb_export_text, sizeof(ahb_export_text), "check passed: %s", rarely);
    } else if (ahb_export_try(st, AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN, often, sizeof(often))) {
        ahb_export_ok = 1;
        ahb_export_cpu = AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN;
        snprintf(ahb_export_text, sizeof(ahb_export_text), "check passed with CPU usage often: %s (rarely: %s)", often, rarely);
    } else {
        ahb_export_ok = 0;
        snprintf(ahb_export_text, sizeof(ahb_export_text), "check FAILED: %s; with CPU usage often: %s", rarely, often);
    }
    if (ahb_export_ok) VKB_INFO("dma-buf export: no dma-heap; AHardwareBuffer pages %s", ahb_export_text);
    else VKB_WARN("dma-buf export: no dma-heap, and the AHardwareBuffer %s - gamescope is offered no exportable images", ahb_export_text);
}
#endif

void vkb_mem_selftest(vkb_pd_knowledge *k, VkInstance inst, const vkb_dispatch *idt, VkPhysicalDevice pd)
{
    selftest st;
    memset(&st, 0, sizeof(st));
    st.idt = idt;
    st.pd = pd;
    idt->vkGetPhysicalDeviceMemoryProperties(pd, &st.mp);

    uint32_t nq = 0;
    idt->vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    idt->vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    st.qf = 0;
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            st.qf = i;
            break;
        }

    uint32_t ne = 0;
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof(*ex));
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);

    uint32_t candidates[4];
    int nc = 0;
    const char *force = vkb_srv_env_global("VKBRIDGE_MEMORY");
    char outcome[400] = ""; /* each strategy's fate, for the summary line */
    int on = 0;
#define OUTCOME(...) on += snprintf(outcome + on, on < (int)sizeof(outcome) ? sizeof(outcome) - (size_t)on : 0, __VA_ARGS__)
    /* GameNative's order on Mali: the driver's own export, a dma-heap import, then an AHB. */
    uint32_t order[] = {VKB_MEM_HOSTPTR, VKB_MEM_DMABUF, VKB_MEM_DMAHEAP, VKB_MEM_AHB};
    for (int i = 0; i < 4; i++) {
        uint32_t s = order[i];
        if (force && strcmp(force, vkb_mem_strategy_name(s))) continue;
        uint32_t n;
        const char *const *req = vkb_mem_required_extensions(s, &n);
        if (!req) continue;
        int ok = 1;
        for (uint32_t j = 0; j < n; j++)
            if (strcmp(req[j], "VK_KHR_external_memory") && strcmp(req[j], "VK_KHR_sampler_ycbcr_conversion") &&
                strcmp(req[j], "VK_KHR_dedicated_allocation") && strcmp(req[j], "VK_KHR_get_memory_requirements2") &&
                (!has_ext(ex, ne, req[j]) || vkb_emu_faked(req[j])))
                ok = 0;
        /* VKBRIDGE_FAKE_MISSING=hostptr (host tests): as on a driver without VK_EXT_external_memory_host */
        if (s == VKB_MEM_HOSTPTR && vkb_emu_faked("hostptr")) ok = 0;
        /* A driver that exports no dma-buf (Mali: buffers report dma-buf features 0x4, importable
         * only) fails dmabuf at its first vkGetMemoryFdKHR: not tried. */
        if (ok && s == VKB_MEM_DMABUF && idt->vkGetPhysicalDeviceExternalBufferProperties) {
            VkPhysicalDeviceExternalBufferInfo bi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0,
                                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
            VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
            idt->vkGetPhysicalDeviceExternalBufferProperties(pd, &bi, &ep);
            uint32_t feat = ep.externalMemoryProperties.externalMemoryFeatures;
            if (!(feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
                VKB_INFO("memory sharing: dmabuf unavailable (the driver exports no dma-bufs: features 0x%x)", feat);
                OUTCOME(" dmabuf: no export (0x%x);", feat);
                continue;
            }
        }
        if (ok) candidates[nc++] = s;
        else {
            VKB_INFO("memory sharing: %s unavailable (driver lacks its extensions)", vkb_mem_strategy_name(s));
            OUTCOME(" %s: no extensions;", vkb_mem_strategy_name(s));
        }
    }

    /* One test device with every candidate's extensions. */
    const char *exts[16];
    uint32_t nx = 0;
    for (int i = 0; i < nc; i++) {
        uint32_t n;
        const char *const *req = vkb_mem_required_extensions(candidates[i], &n);
        for (uint32_t j = 0; j < n; j++) {
            if (!has_ext(ex, ne, req[j])) continue;
            int dup = 0;
            for (uint32_t q = 0; q < nx; q++) dup |= !strcmp(exts[q], req[j]);
            if (!dup && nx < 16) exts[nx++] = req[j];
        }
    }
#ifdef __ANDROID__
    /* the AHardwareBuffer export check imports a dma-buf whatever strategy is tried (ahb_export_check) */
    if (!vkb_mem_dmaheap_available() && ahb_load())
        for (uint32_t j = 0; j < 3; j++) {
            int dup = 0;
            for (uint32_t q = 0; q < nx; q++) dup |= !strcmp(exts[q], ext_dmabuf[j]);
            if (!dup && nx < 16 && has_ext(ex, ne, ext_dmabuf[j])) exts[nx++] = ext_dmabuf[j];
        }
#endif
    free(ex);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, st.qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, nx, exts, NULL};
    if (idt->vkCreateDevice(pd, &dci, NULL, &st.dev) != VK_SUCCESS) {
        VKB_ERR("memory self-test: cannot create a device");
        return;
    }
    st.dt = *idt;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vkb_gipa(inst, "vkGetDeviceProcAddr");
    vkb_mem_gdpa = gdpa;
    log_external_props(idt, pd);
    vkb_dispatch_load_device(&st.dt, gdpa, st.dev);
    st.dt.vkGetDeviceQueue(st.dev, st.qf, 0, &st.queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, st.qf};
    st.dt.vkCreateCommandPool(st.dev, &pci, NULL, &st.pool);

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hostp};
    idt->vkGetPhysicalDeviceProperties2(pd, &p2);
    st.import_align = hostp.minImportedHostPointerAlignment ? hostp.minImportedHostPointerAlignment : 4096;
    if (st.import_align < 4096) st.import_align = 4096;

    k->info.strategy = VKB_MEM_NONE;
    k->info.image_types = probe_image_types(&st);
    k->info.dl_types = probe_dl_types(&st, k->info.image_types, 0, 0);
    for (int i = 0; i < nc; i++) {
        uint32_t s = candidates[i];
        uint32_t shareable = 0, coherent = 0;
        /* The client's buffers carry this strategy's handle type: their masks with it. */
        uint32_t dl = probe_dl_types(&st, k->info.image_types, strategy_handle_type(s), 1);
        st.more_usages = dl != 0;
        if (s == VKB_MEM_DMAHEAP) log_dmaheap_fd_types(&st);
        for (int pass = 0; pass < 2; pass++) {
            for (uint32_t t = 0; t < st.mp.memoryTypeCount; t++) {
                if (!(st.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
                if (st.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT) continue;
                int res = test_type(&st, s, t);
                if (res) shareable |= 1u << t;
                if (res == 1) coherent |= 1u << t;
            }
            /* AHBs again with uncached pages, which the driver may take into a coherent type */
            if (coherent || s != VKB_MEM_AHB || ahb_uncached()) break;
            VKB_INFO("ahb: no coherent type takes a cached AHB; trying again with uncached pages (CPU usage rarely)");
            ahb_rarely = 1;
            shareable = 0;
        }
        if (s == VKB_MEM_AHB && !coherent && ahb_rarely && !vkb_srv_env_global("VKBRIDGE_AHB_RARELY")) ahb_rarely = 0; /* no better: as before */
        if (coherent) {
            k->info.strategy = s;
            k->info.buffer_handle_types = strategy_handle_type(s);
            k->info.shareable_types = shareable;
            k->info.coherent_types = coherent;
            k->info.import_alignment = st.import_align;
            k->info.cached_types = cached_selftest(&st, s, shareable);
            k->info.dl_types = probe_dl_types(&st, k->info.image_types, strategy_handle_type(s), 0);
            OUTCOME(" %s: chosen, types 0x%x shared, 0x%x coherent, 0x%x cached readback;", vkb_mem_strategy_name(s), shareable, coherent,
                    k->info.cached_types);
            break;
        }
        VKB_INFO("memory sharing: %s failed the self-test", vkb_mem_strategy_name(s));
        OUTCOME(" %s: failed (types 0x%x shared, none coherent);", vkb_mem_strategy_name(s), shareable);
    }
    /*
     * Only when every method failed: the dma-buf methods again, the test buffer made WITHOUT the
     * external handle type (the memory still exported or imported as a dma-buf). The Xclipse 530
     * (Galaxy A55, Samsung driver, no VK_EXT_external_memory_host) is thought to give buffers made
     * with a handle type no host-visible type, so every type failed before any allocation and the
     * client had no memory an app can map. Binding external memory to a buffer made without its
     * handle type is outside the spec (VUID-vkBindBufferMemory-memory-02985 for exported memory,
     * -02726 for imported). It is used only because this self-test proves it end to end on the
     * device (both CPU mappings the same pages, a GPU fill and copy seen by the client, the client's
     * write seen by the GPU), and only where nothing in the spec works. The client then makes its
     * buffers without the handle type (VKB_MEMF_UNFLAGGED_BUFFERS); images are not affected.
     * VKBRIDGE_UNFLAGGED_BUFFERS=0 turns it off.
     */
    const char *uf = vkb_srv_env_global("VKBRIDGE_UNFLAGGED_BUFFERS"); /* <cache dir>/env too (read before the self-test) */
    for (int pref = 0; pref < 2 && k->info.strategy == VKB_MEM_NONE && !(uf && *uf == '0'); pref++) {
        uint32_t s = pref == 0 ? VKB_MEM_DMAHEAP : VKB_MEM_DMABUF;
        int cand = 0;
        for (int i = 0; i < nc; i++) cand |= candidates[i] == s;
        if (!cand) continue;
        st.unflagged = 1;
        st.more_usages = k->info.dl_types != 0; /* the client's unflagged buffers have the plain masks */
        uint32_t shareable = 0, coherent = 0;
        for (uint32_t t = 0; t < st.mp.memoryTypeCount; t++) {
            if (!(st.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            if (st.mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT) continue;
            int res = test_type(&st, s, t);
            if (res) shareable |= 1u << t;
            if (res == 1) coherent |= 1u << t;
        }
        if (coherent) {
            k->info.strategy = s;
            k->info.buffer_handle_types = 0;
            k->info.mem_flags |= VKB_MEMF_UNFLAGGED_BUFFERS;
            k->info.shareable_types = shareable;
            k->info.coherent_types = coherent;
            k->info.import_alignment = st.import_align;
            k->info.cached_types = cached_selftest(&st, s, shareable);
            VKB_WARN("memory sharing: %s with buffers made without its handle type (outside the Vulkan spec; this device's self-test "
                     "passed it end to end, and no other method works)", vkb_mem_strategy_name(s));
            OUTCOME(" %s: chosen (unflagged buffers), types 0x%x shared, 0x%x coherent, 0x%x cached readback;", vkb_mem_strategy_name(s),
                    shareable, coherent, k->info.cached_types);
        } else {
            OUTCOME(" %s unflagged: failed (types 0x%x shared, none coherent);", vkb_mem_strategy_name(s), shareable);
        }
        st.unflagged = 0;
    }
    /* One line with every strategy's fate (the per-type lines above say why). */
    VKB_INFO("memory sharing summary for %s:%s", k->name, outcome);
#undef OUTCOME
    if (k->info.strategy == VKB_MEM_NONE)
        VKB_ERR("memory sharing: no method works on %s; Linux programs will get no host-visible memory", k->name);
#ifdef __ANDROID__
    ahb_export_check(&st);
#endif

    st.dt.vkDestroyCommandPool(st.dev, st.pool, NULL);
    st.dt.vkDestroyDevice(st.dev, NULL);
}
