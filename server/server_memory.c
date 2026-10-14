/* SPDX-License-Identifier: GPL-3.0-or-later
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
#include <malloc.h>
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
 */
enum { CLS_PLAIN, CLS_HOSTPTR, CLS_DMABUF, CLS_AHB, CLS_DMAHEAP, CLS_EXPORT, CLS_N };
static const char *const cls_name[CLS_N] = {"plain", "hostptr", "dmabuf", "ahb", "dmaheap", "export"};
#define ST_TYPES 32
#define ST_PIDS 8
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

#define MB(x) ((long long)((x) >> 20))

static long status_kb(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    return p ? strtol(p + strlen(key), NULL, 10) : 0;
}

static void *st_thread(void *arg)
{
    (void)arg;
    uint64_t last_sig = 0, last_allocs = 0, last_frees = 0;
    int64_t last_ab = 0, last_fb = 0;
    for (;;) {
        sleep(5);
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
        pthread_mutex_lock(&st.lock);
        uint64_t sig = (uint64_t)(rss >> 14) ^ (st.allocs * 0x9E3779B97F4A7C15ull) ^ (st.frees << 32) ^ (uint64_t)st.img.n * 31 ^ (uint64_t)st.buf.n;
        if (sig == last_sig) {
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
        ADD("mem: rss %ldM (anon %ldM file %ldM shmem %ldM) heap %lldM fds %d | live %lldM/%lld:", rss >> 10, anon >> 10, file >> 10,
            shm >> 10, MB((int64_t)(size_t)mi.uordblks), nfd - 2, MB(live), (long long)nlive);
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
#undef ADD
        pthread_mutex_unlock(&st.lock);
        VKB_INFO("%s | bc scratch %lldM", line, MB(vkb_emu_bcn_scratch_live()));
    }
    return NULL;
}

static void st_start(void)
{
    const char *e = getenv("VKBRIDGE_MEM_STATS");
    if (e && *e == '0') return;
    pthread_t th;
    if (pthread_create(&th, NULL, st_thread, NULL) == 0) pthread_detach(th);
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
 * and the client both write through it without cache maintenance), else the cached system heap. */
static int dmaheap_fd(void)
{
    static int fd = -2;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (fd == -2) {
        const char *names[] = {"/dev/dma_heap/system-uncached", "/dev/dma_heap/system"};
        if (getenv("VKBRIDGE_DMAHEAP_CACHED")) names[0] = "/dev/dma_heap/system";
        fd = -1;
        for (int i = 0; i < 2 && fd < 0; i++) {
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
                             int client_fd, vkb_srv_mem **out, int *out_fd)
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
        if (client_fd < 0) break;
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
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            break;
        }
        VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, info.pNext,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p};
        info.pNext = &imp;
        r = dt->vkAllocateMemory(dev, &info, NULL, &m->mem);
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
        *out_fd = fd;
        break;
    }
    case VKB_MEM_DMAHEAP: {
        int hfd = dmaheap_fd();
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
        if (pr != VK_SUCCESS || !(fp.memoryTypeBits & (1u << ai->memoryTypeIndex))) {
            VKB_INFO("dmaheap: fd properties %d, memory types 0x%x (want type %u)", pr, fp.memoryTypeBits, ai->memoryTypeIndex);
            close(fd);
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            break;
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
        /* CPU usage decides how gralloc caches the pages; VKBRIDGE_AHB_RARELY=1 asks for uncached. */
        uint64_t cpu = getenv("VKBRIDGE_AHB_RARELY") ? (2ull | (2ull << 4)) : (AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_CPU_WRITE_OFTEN);
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
        /* Mali's gralloc can put more than one fd in the handle (metadata beside the pages): the
         * memory is the one at least as large as the allocation. */
        int pick = -1;
        for (int i = 0; i < nh->numFds && pick < 0; i++) {
            off_t end = lseek(nh->data[i], 0, SEEK_END);
            if (end >= (off_t)sz) pick = nh->data[i];
            if (nh->numFds > 1) VKB_INFO("ahb: handle fd %d of %d is %lld bytes", i + 1, nh->numFds, (long long)end);
        }
        if (pick < 0) {
            VKB_INFO("ahb: no fd in the handle covers %llu bytes", (unsigned long long)sz);
            dt->vkFreeMemory(dev, m->mem, NULL);
            m->mem = VK_NULL_HANDLE;
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            break;
        }
        *out_fd = fcntl(pick, F_DUPFD_CLOEXEC, 0);
        break;
    }
#endif
    default:
        break;
    }
    if (client_fd >= 0) close(client_fd);
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
 */
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
    int hfd = dmaheap_fd();
    if (hfd < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    uint64_t len = (ai->allocationSize + 4095) & ~4095ull;
    struct dma_heap_allocation_data hd = {len, 0, O_RDWR | O_CLOEXEC, 0};
    if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &hd) < 0) {
        VKB_ERR("export emulation: dma-heap allocation of %llu bytes failed: %s", (unsigned long long)len, strerror(errno));
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    int fd = (int)hd.fd;
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
        close(fd);
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
        close(fd);
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
    VKB_DBG("export emulation: %llu bytes from the dma-heap, type %u", (unsigned long long)len, info.memoryTypeIndex);
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
    if (v < 0) v = vkb_emu_faked("lazymem");
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

void vkb_mem_instance_install(vkb_srv_table *t)
{
    if (!fake_lazy()) return;
    if (t->real.vkGetPhysicalDeviceMemoryProperties) t->dt.vkGetPhysicalDeviceMemoryProperties = lazy_GetPhysicalDeviceMemoryProperties;
    if (t->real.vkGetPhysicalDeviceMemoryProperties2) t->dt.vkGetPhysicalDeviceMemoryProperties2 = lazy_GetPhysicalDeviceMemoryProperties2;
    VKB_INFO("imitating Mali's memory types: device-local types without host access are lazily allocated");
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
    if (mode == VKB_ALLOC_SHARED) {
        uint32_t s = t->pd ? t->pd->info.strategy : VKB_MEM_NONE;
        int opaque = vkb_opaque_shared_info(t, &ai); /* server_opaque.c */
        r = alloc_shared(c->dt, dev, s, &ai, client_fd, &m, &out_fd);
        if (r == VK_SUCCESS && opaque) vkb_opaque_shared_done(m, out_fd);
        char path[48];
        snprintf(path, sizeof(path), "shared (%s)", vkb_mem_strategy_name(s));
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
            path = "export emulation (dma-heap)";
            type = ai.memoryTypeIndex;
            /* Pages the dma-heap would not give, or the driver would not import: the memory is still
             * made, without a dma-buf (most allocations asking for export are never exported; one
             * that is fails at vkGetMemoryFdKHR instead of here). VKBRIDGE_EXPORT_FALLBACK=0: not. */
            const char *fb = getenv("VKBRIDGE_EXPORT_FALLBACK");
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
    if (r == VK_SUCCESS && n) {
        r = flush ? c->dt->vkFlushMappedMemoryRanges(dev, n, ranges) : c->dt->vkInvalidateMappedMemoryRanges(dev, n, ranges);
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
} selftest;

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
 * (no flush needed), 2 = shared when flushed, 0 = not shared. */
static int test_type_mode(selftest *st, uint32_t s, uint32_t type, int no_flush);

/* Without flushes first: imported pages from an uncached heap are coherent in practice even where
 * the driver's type does not say HOST_COHERENT (Mali-G720: dma-heap imports land in type 1,
 * HOST_CACHED without COHERENT, and pass with no flush). Only if that fails, with flushes. */
static int test_type(selftest *st, uint32_t s, uint32_t type)
{
    int coherent_flag = (st->mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!coherent_flag && !getenv("VKBRIDGE_SELFTEST_NOFLUSH")) {
        int r = test_type_mode(st, s, type, 1);
        if (r) {
            VKB_INFO("  %s, type %u: coherent in practice (no flushes needed)", vkb_mem_strategy_name(s), type);
            return 1;
        }
    }
    return test_type_mode(st, s, type, 0);
}

static int test_type_mode(selftest *st, uint32_t s, uint32_t type, int no_flush)
{
    const vkb_dispatch *dt = &st->dt;
    int result = 0;
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, strategy_handle_type(s)};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext, 0, TEST_SIZE,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf = VK_NULL_HANDLE;
    if (dt->vkCreateBuffer(st->dev, &bci, NULL, &buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    dt->vkGetBufferMemoryRequirements(st->dev, buf, &req);
    if (!(req.memoryTypeBits & (1u << type))) {
        dt->vkDestroyBuffer(st->dev, buf, NULL);
        return 0;
    }
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
    VkResult r = alloc_shared(dt, st->dev, s, &ai, cfd, &m, &out_fd);
    if (r != VK_SUCCESS) {
        VKB_INFO("  %s, type %u: allocation failed (%d)", vkb_mem_strategy_name(s), type, r);
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
    if (no_flush || getenv("VKBRIDGE_SELFTEST_NOFLUSH")) coherent = 1;
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
    if (!coherent) dt->vkFlushMappedMemoryRanges(st->dev, 1, &whole);
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
    if (!coherent) dt->vkInvalidateMappedMemoryRanges(st->dev, 1, &whole);
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
    VKB_INFO("  %s, type %u (flags 0x%x): shared OK%s", vkb_mem_strategy_name(s), type, st->mp.memoryTypes[type].propertyFlags,
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
    const char *force = getenv("VKBRIDGE_MEMORY");
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
                !has_ext(ex, ne, req[j]))
                ok = 0;
        if (ok) candidates[nc++] = s;
        else VKB_INFO("memory sharing: %s unavailable (driver lacks its extensions)", vkb_mem_strategy_name(s));
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
    for (int i = 0; i < nc; i++) {
        uint32_t s = candidates[i];
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
            k->info.buffer_handle_types = strategy_handle_type(s);
            k->info.shareable_types = shareable;
            k->info.coherent_types = coherent;
            k->info.import_alignment = st.import_align;
            break;
        }
        VKB_INFO("memory sharing: %s failed the self-test", vkb_mem_strategy_name(s));
    }
    if (k->info.strategy == VKB_MEM_NONE)
        VKB_ERR("memory sharing: no method works on %s; Linux programs will get no host-visible memory", k->name);

    st.dt.vkDestroyCommandPool(st.dev, st.pool, NULL);
    st.dt.vkDestroyDevice(st.dev, NULL);
}
