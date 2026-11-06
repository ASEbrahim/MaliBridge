/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The crash line's account of the call that died (server_main.c on_crash): what it was given
 * (common/vkb_crashctx.h notes) and what those handles are, from records of the images, memory
 * allocations and graphics pipelines made last.
 *
 * Why: Mali-G615 MC6 r44p1 - the one server died in the driver's vkBindImageMemory and
 * took every program with it; the line named the call and nothing else: not whose it was (binds
 * are asynchronous, any process's thread could have sent it), nor the image or the memory. a player log
 * (G615 MC2 r44p1): a near-null read in the driver's vkCmdDraw[Indexed], with no word on what
 * pipeline was bound.
 *
 * The records are rings written where the objects are made (not the frame path): a slot index
 * from one relaxed atomic add, the slot's fields, then its handle with release order; the crash
 * handler reads them without locks (a slot being written may read torn: its handle is cleared
 * first and set last, and a reader checks the handle again after copying). Lookups go newest
 * first, so a handle value the driver handed out again finds its newest object.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

__thread vkb_srv_args vkb_srv_args_now;
__thread uint64_t vkb_srv_replay_gfx, vkb_srv_replay_comp;
__thread float vkb_srv_replay_lw;
__thread uint32_t vkb_srv_replay_lw_set;

#define RING 512 /* each ring: a power of two */

typedef struct {
    uint64_t handle;
    uint64_t t_ms;
    uint64_t modifier, req_size, req_align;
    int32_t pid;
    uint32_t format, w, h, d, mips, layers, tiling, usage, flags, ext_types;
    uint32_t client_format, client_tiling;
    uint8_t has_modifier;
} img_rec;

typedef struct {
    uint64_t handle;
    uint64_t t_ms;
    uint64_t size, dedicated;
    int32_t pid;
    uint32_t type;
    const char *path; /* a string literal */
} mem_rec;

typedef struct {
    uint64_t handle;
    uint64_t t_ms;
    int32_t pid;
    vkb_srv_pipe_snap s;
} pipe_rec;

static img_rec imgs[RING];
static mem_rec mems[RING];
static pipe_rec pipes[RING];
static uint32_t img_n, mem_n, pipe_n;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* async-signal-safe */
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int cur_pid(void)
{
    const vkb_srv_proc *p = vkb_srv_current_proc();
    return p ? p->pid : 0;
}

void vkb_srv_note_image(vkb_srv_call *c, VkDevice device, const VkImageCreateInfo *ci, VkImage image, VkFormat asked_format,
                        VkImageTiling asked_tiling)
{
    if (!image || !c->table) return;
    const vkb_dispatch *real = &c->table->real;
    VkMemoryRequirements req = {0};
    if (!(ci->flags & VK_IMAGE_CREATE_DISJOINT_BIT) && real->vkGetImageMemoryRequirements) real->vkGetImageMemoryRequirements(device, image, &req);
    VkImageDrmFormatModifierPropertiesEXT mp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    int has_mod = ci->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && real->vkGetImageDrmFormatModifierPropertiesEXT &&
                  real->vkGetImageDrmFormatModifierPropertiesEXT(device, image, &mp) == VK_SUCCESS;
    img_rec *r = &imgs[__atomic_fetch_add(&img_n, 1, __ATOMIC_RELAXED) & (RING - 1)];
    __atomic_store_n(&r->handle, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->t_ms = now_ms();
    r->pid = cur_pid();
    r->format = (uint32_t)ci->format, r->w = ci->extent.width, r->h = ci->extent.height, r->d = ci->extent.depth;
    r->mips = ci->mipLevels, r->layers = ci->arrayLayers, r->tiling = (uint32_t)ci->tiling, r->usage = ci->usage, r->flags = ci->flags;
    r->ext_types = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) r->ext_types = ((const VkExternalMemoryImageCreateInfo *)b)->handleTypes;
    r->client_format = (uint32_t)asked_format, r->client_tiling = (uint32_t)asked_tiling;
    r->req_size = req.size, r->req_align = req.alignment;
    r->has_modifier = (uint8_t)has_mod, r->modifier = has_mod ? mp.drmFormatModifier : 0;
    __atomic_store_n(&r->handle, (uint64_t)(uintptr_t)image, __ATOMIC_RELEASE);
}

void vkb_crashctx_memory(VkDeviceMemory mem, uint64_t size, uint32_t type, uint64_t dedicated_image, const char *path)
{
    if (!mem) return;
    mem_rec *r = &mems[__atomic_fetch_add(&mem_n, 1, __ATOMIC_RELAXED) & (RING - 1)];
    __atomic_store_n(&r->handle, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->t_ms = now_ms();
    r->pid = cur_pid();
    r->size = size, r->type = type, r->dedicated = dedicated_image, r->path = path;
    __atomic_store_n(&r->handle, (uint64_t)(uintptr_t)mem, __ATOMIC_RELEASE);
}

void vkb_srv_note_pipelines_pre(vkb_srv_pipe_snap *s, uint32_t n, const VkGraphicsPipelineCreateInfo *ci)
{
    for (uint32_t i = 0; ci && i < n && i < VKB_PIPE_SNAP; i++) {
        const VkPipelineRasterizationStateCreateInfo *rs = ci[i].pRasterizationState;
        const VkPipelineInputAssemblyStateCreateInfo *ia = ci[i].pInputAssemblyState;
        s[i].has_rs = rs != NULL;
        s[i].line_width = rs ? rs->lineWidth : 0.0f;
        s[i].polygon_mode = rs ? (uint8_t)rs->polygonMode : 0;
        s[i].topology = ia ? (uint8_t)ia->topology : 0xff;
        s[i].dyn_line_width = 0;
        const VkPipelineDynamicStateCreateInfo *ds = ci[i].pDynamicState;
        for (uint32_t k = 0; ds && ds->pDynamicStates && k < ds->dynamicStateCount; k++)
            if (ds->pDynamicStates[k] == VK_DYNAMIC_STATE_LINE_WIDTH) s[i].dyn_line_width = 1;
    }
}

void vkb_srv_note_pipelines(const vkb_srv_pipe_snap *s, uint32_t n, const VkPipeline *out)
{
    int pid = cur_pid();
    for (uint32_t i = 0; out && i < n && i < VKB_PIPE_SNAP; i++) {
        if (!out[i]) continue;
        pipe_rec *r = &pipes[__atomic_fetch_add(&pipe_n, 1, __ATOMIC_RELAXED) & (RING - 1)];
        __atomic_store_n(&r->handle, 0, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        r->t_ms = now_ms();
        r->pid = pid;
        r->s = s[i];
        __atomic_store_n(&r->handle, (uint64_t)(uintptr_t)out[i], __ATOMIC_RELEASE);
    }
}

/* The newest record of handle h in a ring (copied into out): 1 if found. */
#define FIND(ring, count, h, out)                                                                       \
    ({                                                                                                  \
        int found_ = 0;                                                                                 \
        uint32_t n_ = __atomic_load_n(&(count), __ATOMIC_ACQUIRE);                                      \
        for (uint32_t k_ = 0; (h) && k_ < RING && k_ < n_ && !found_; k_++) {                           \
            uint32_t i_ = (n_ - 1 - k_) & (RING - 1);                                                   \
            if (__atomic_load_n(&(ring)[i_].handle, __ATOMIC_ACQUIRE) != (h)) continue;                 \
            *(out) = (ring)[i_];                                                                        \
            found_ = __atomic_load_n(&(ring)[i_].handle, __ATOMIC_ACQUIRE) == (h);                      \
        }                                                                                               \
        found_;                                                                                         \
    })

#define ADD(...)                                                                                        \
    do {                                                                                                \
        if (n >= 0 && (size_t)n < cap) n += snprintf(buf + n, cap - (size_t)n, __VA_ARGS__);           \
    } while (0)

static int describe_image(char *buf, size_t cap, uint64_t h, uint64_t now)
{
    int n = 0;
    img_rec r = {0};
    if (!FIND(imgs, img_n, h, &r)) {
        ADD("image %#llx: not among the last %d made", (unsigned long long)h, RING);
        return n;
    }
    ADD("image %#llx: %ux%ux%u format %u, %u mip(s) %u layer(s), tiling %u", (unsigned long long)h, r.w, r.h, r.d, r.format, r.mips,
        r.layers, r.tiling);
    if (r.has_modifier) ADD(" (modifier %#llx)", (unsigned long long)r.modifier);
    if (r.client_tiling != r.tiling) ADD(" (asked %u)", r.client_tiling);
    if (r.client_format != r.format) ADD(", asked as format %u", r.client_format);
    ADD(", usage %#x, flags %#x, external %#x%s, needs %llu bytes (alignment %llu), made %llu ms ago by pid %d", r.usage, r.flags,
        r.ext_types, r.ext_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ? " (dma-buf)" : "", (unsigned long long)r.req_size,
        (unsigned long long)r.req_align, (unsigned long long)(now - r.t_ms), r.pid);
    return n;
}

static int describe_memory(char *buf, size_t cap, uint64_t h, uint64_t offset, uint64_t now, uint64_t image)
{
    int n = 0;
    mem_rec r = {0};
    if (!FIND(mems, mem_n, h, &r)) {
        ADD("memory %#llx: not among the last %d allocated", (unsigned long long)h, RING);
        return n;
    }
    ADD("memory %#llx: %llu bytes, type %u, %s", (unsigned long long)h, (unsigned long long)r.size, r.type, r.path ? r.path : "?");
    if (r.dedicated) ADD(", dedicated to image %#llx%s", (unsigned long long)r.dedicated, r.dedicated == image ? " (this one)" : " (ANOTHER image)");
    ADD(", made %llu ms ago by pid %d", (unsigned long long)(now - r.t_ms), r.pid);
    img_rec ir = {0};
    if (image && FIND(imgs, img_n, image, &ir) && ir.req_size && offset + ir.req_size > r.size)
        ADD("; offset %llu + the image's %llu bytes is PAST its end", (unsigned long long)offset, (unsigned long long)ir.req_size);
    return n;
}

static int describe_pipeline(char *buf, size_t cap, const char *what, uint64_t h, uint64_t now)
{
    int n = 0;
    if (!h) {
        ADD("no %s pipeline bound in this command buffer", what);
        return n;
    }
    pipe_rec r = {0};
    if (!FIND(pipes, pipe_n, h, &r)) {
        ADD("%s pipeline %#llx", what, (unsigned long long)h);
        return n;
    }
    ADD("%s pipeline %#llx (made %llu ms ago by pid %d; topology %u, ", what, (unsigned long long)h, (unsigned long long)(now - r.t_ms),
        r.pid, r.s.topology);
    if (r.s.has_rs) ADD("polygon mode %u, line width %s%g)", r.s.polygon_mode, r.s.dyn_line_width ? "dynamic, static " : "", (double)r.s.line_width);
    else ADD("no rasterization state)");
    return n;
}

static int is_draw(const char *cmd)
{
    return cmd && (!strncmp(cmd, "vkCmdDraw", 9) || !strncmp(cmd, "vkCmdDispatch", 13) || !strncmp(cmd, "vkCmdClearAttachments", 21));
}

int vkb_crashctx_describe(char *buf, size_t cap, const char *cmd)
{
    int n = 0;
    uint64_t now = now_ms();
    const vkb_srv_args *a = &vkb_srv_args_now;
    if (a->kind == VKB_ARGS_BIND_IMAGE) {
        ADD("vkbridge-server E   bind of image %#llx to memory %#llx at offset %llu; ", (unsigned long long)a->a, (unsigned long long)a->b,
            (unsigned long long)a->c);
        if (n >= 0 && (size_t)n < cap) n += describe_image(buf + n, cap - (size_t)n, a->a, now);
        ADD("; ");
        if (n >= 0 && (size_t)n < cap) n += describe_memory(buf + n, cap - (size_t)n, a->b, a->c, now, a->a);
        ADD("\n");
    } else if (a->kind == VKB_ARGS_BIND_IMAGE2 && a->p) {
        const VkBindImageMemoryInfo *bi = a->p;
        for (uint32_t i = 0; i < a->n && i < 4; i++) {
            ADD("vkbridge-server E   bind %u of %u: image %#llx to memory %#llx at offset %llu%s; ", i + 1, a->n,
                (unsigned long long)(uintptr_t)bi[i].image, (unsigned long long)(uintptr_t)bi[i].memory,
                (unsigned long long)bi[i].memoryOffset, bi[i].pNext ? " (with a pNext chain)" : "");
            if (n >= 0 && (size_t)n < cap) n += describe_image(buf + n, cap - (size_t)n, (uint64_t)(uintptr_t)bi[i].image, now);
            ADD("; ");
            if (n >= 0 && (size_t)n < cap)
                n += describe_memory(buf + n, cap - (size_t)n, (uint64_t)(uintptr_t)bi[i].memory, bi[i].memoryOffset, now,
                                     (uint64_t)(uintptr_t)bi[i].image);
            ADD("\n");
        }
    }
    if (a->kind == VKB_ARGS_ALLOC) {
        ADD("vkbridge-server E   allocation of %llu bytes in memory type %llu, %s", (unsigned long long)a->a, (unsigned long long)a->b,
            a->p ? (const char *)a->p : "?");
        if (a->d) ADD(", dma-buf ino %llu", (unsigned long long)a->d);
        if (a->c) {
            ADD(", dedicated to ");
            if (n >= 0 && (size_t)n < cap) n += describe_image(buf + n, cap - (size_t)n, a->c, now);
        }
        ADD("\n");
    }
    if (vkb_srv_replay_cb && is_draw(cmd)) {
        ADD("vkbridge-server E   state: ");
        int compute = !strncmp(cmd, "vkCmdDispatch", 13);
        if (n >= 0 && (size_t)n < cap)
            n += describe_pipeline(buf + n, cap - (size_t)n, compute ? "compute" : "graphics", compute ? vkb_srv_replay_comp : vkb_srv_replay_gfx, now);
        if (vkb_srv_replay_lw_set) ADD("; last vkCmdSetLineWidth %g", (double)vkb_srv_replay_lw);
        ADD("\n");
    }
    if (n < 0) n = 0;
    if ((size_t)n >= cap) n = (int)cap - 1;
    return n;
}

/* Hex digits at *p (advanced past them). */
static uint64_t hex_at(const char **p, const char *end)
{
    uint64_t v = 0;
    for (; *p < end; (*p)++) {
        char ch = **p;
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
        if (d < 0) break;
        v = v << 4 | (uint64_t)d;
    }
    return v;
}

/* One /proc/self/maps line (without its newline), cut short and with its offset/device/inode
 * columns left out: "<start>-<end> <perms> <name>". */
static int map_line(char *buf, size_t cap, const char *l, const char *end)
{
    const char *sp = memchr(l, ' ', (size_t)(end - l)); /* after the range */
    if (!sp || end - sp < 5) return 0;
    int n = 0;
    ADD("%.*s %.4s", (int)(sp - l), l, sp + 1);
    /* the name: after the 5th field */
    const char *q = sp + 1;
    for (int f = 0; f < 4 && q < end; f++) {
        while (q < end && *q != ' ') q++;
        while (q < end && *q == ' ') q++;
    }
    if (q < end) ADD(" %.*s", (int)((end - q) > 96 ? 96 : (end - q)), q);
    else ADD(" (anonymous)");
    return n;
}

int vkb_crashctx_where(char *buf, size_t cap, const void *addr)
{
    int n = 0;
    char name[17] = "";
#ifdef PR_GET_NAME
    prctl(PR_GET_NAME, name, 0, 0, 0);
#endif
    name[16] = 0;
    ADD("vkbridge-server E   thread %ld \"%s\"", (long)syscall(SYS_gettid), name);
    /* What the faulting address is: a PROT_NONE part of the heap's reservation, a guard page, a
     * mapping of the GPU's memory (/dev/mali0, a dma-buf) or nothing at all (* SEGV_ACCERR in Mali's vkBindImageMemory/vkAllocateMemory at addresses far from any pointer the
     * bridge saw). /proc/self/maps read with open/read only, scanned line by line. */
    uint64_t want = (uint64_t)(uintptr_t)addr;
    int fd = want ? open("/proc/self/maps", O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0) {
        ADD("\n");
        return n;
    }
    static char chunk[8192], below[256], above[256], hit[256]; /* static: the crash handler's stack is small */
    size_t have = 0;
    below[0] = above[0] = hit[0] = 0;
    uint64_t below_end = 0;
    int eof = 0;
    while (!hit[0] && !above[0]) {
        if (!eof && have < sizeof(chunk)) {
            ssize_t r = read(fd, chunk + have, sizeof(chunk) - have);
            if (r <= 0) eof = 1;
            else have += (size_t)r;
        }
        char *nl = memchr(chunk, '\n', have);
        if (!nl) {
            if (eof || have == sizeof(chunk)) break;
            continue;
        }
        const char *l = chunk, *e = nl;
        const char *q = l;
        uint64_t lo = hex_at(&q, e);
        if (q < e && *q == '-') q++;
        uint64_t hi = hex_at(&q, e);
        char tmp[256];
        int k = map_line(tmp, sizeof(tmp), l, e);
        tmp[k < (int)sizeof(tmp) ? k : (int)sizeof(tmp) - 1] = 0;
        if (want >= lo && want < hi) memcpy(hit, tmp, sizeof(tmp));
        else if (hi <= want) {
            memcpy(below, tmp, sizeof(tmp));
            below_end = hi;
        } else if (lo > want)
            memcpy(above, tmp, sizeof(tmp));
        size_t used = (size_t)(nl + 1 - chunk);
        memmove(chunk, nl + 1, have - used);
        have -= used;
    }
    close(fd);
    if (hit[0]) ADD("; address %p is in %s", addr, hit);
    else {
        ADD("; address %p is in no mapping", addr);
        if (below[0]) ADD(" (%llu KB past %s", (unsigned long long)((want - below_end) >> 10), below);
        if (above[0]) ADD("%sbefore %s", below[0] ? "; " : " (", above);
        if (below[0] || above[0]) ADD(")");
    }
    ADD("\n");
    if (n < 0) n = 0;
    if ((size_t)n >= cap) n = (int)cap - 1;
    return n;
}

/* VKBRIDGE_TEST_CRASH_CMD=vkBindImageMemory, vkBindImageMemory2, vkAllocateMemory, vkCmdDraw, vkCmdDrawIndexed, vkEndCommandBuffer,
 * vkQueueSubmit or vkQueueSubmit2 (host
 * tests): beneath every layer, that driver call faults on purpose (a write to address 0x40) when a
 * client's request makes it - its bind, or its draw inside a command buffer's replay; the server's
 * own calls of it go on to the driver. The crash line must then name the client and the arguments.
 * =unbound: vkCmdDraw and vkCmdDrawIndexed fault only with no graphics pipeline bound (Mali's driver,
 * a player log); the replay leaves such draws out before they get there. */
/* per device (by table id): a pointer the driver gave one device is not another's once the server
 * outlives a contained fault (server_guard.c) and serves the next program */
static PFN_vkBindImageMemory test_bind_next[VKB_MAX_TABLES];
static PFN_vkBindImageMemory2 test_bind2_next[VKB_MAX_TABLES];
static PFN_vkAllocateMemory test_alloc_next[VKB_MAX_TABLES];
static PFN_vkCmdDraw test_draw_next[VKB_MAX_TABLES];
static PFN_vkCmdDrawIndexed test_drawi_next[VKB_MAX_TABLES];

static uint32_t test_id(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < VKB_MAX_TABLES ? t->id : 0;
}

/* VKBRIDGE_TEST_CRASH_ACCERR=1: the fault is kind (SEGV_ACCERR/5PA6): a 2-byte
 * store at an odd offset into a mapping without access, not a near-null write. */
/* VKBRIDGE_TEST_CRASH_LOCK=1 (with ONCE): the imitated driver call takes a lock of the driver's (one
 * for every device, as an allocator's) and the fault comes while it is held; the jump back leaves it
 * held, so every later such call of a client waits on it for good (risk T10 #9: the stall watchdog).
 * =dev: a lock of the faulting device's own (a GPU context's): only that device's later calls wait on it. */
static pthread_mutex_t test_drv_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t test_lock_dev;

static void test_fault_at(uintptr_t where, int read);

static void test_fault(void) { test_fault_at(0x40, 0); }

static void test_fault_at(uintptr_t where, int read)
{
    /* VKBRIDGE_TEST_CRASH_ONCE=1: only the first such call faults (a contained fault, then the call works) */
    static int faults;
    const char *once = getenv("VKBRIDGE_TEST_CRASH_ONCE"), *lk = getenv("VKBRIDGE_TEST_CRASH_LOCK");
    int lock = lk && (*lk == '1' || !strcmp(lk, "dev"));
    if (once && *once == '1' && __atomic_fetch_add(&faults, 1, __ATOMIC_RELAXED)) {
        if (lock && (*lk == '1' || test_id() == __atomic_load_n(&test_lock_dev, __ATOMIC_ACQUIRE))) {
            VKB_WARN("VKBRIDGE_TEST_CRASH_LOCK: the driver lock the fault left held is taken again (waits for good)");
            pthread_mutex_lock(&test_drv_lock);
            pthread_mutex_unlock(&test_drv_lock);
        }
        return;
    }
    if (lock) pthread_mutex_lock(&test_drv_lock); /* held at the fault: the jump back never unlocks it */
    if (lock) __atomic_store_n(&test_lock_dev, test_id(), __ATOMIC_RELEASE);
    VKB_WARN("VKBRIDGE_TEST_CRASH_CMD: faulting on purpose%s", lock ? " with a driver lock held" : "");
    const char *acc = getenv("VKBRIDGE_TEST_CRASH_ACCERR");
    if (acc && *acc == '1') {
        char *p = mmap(NULL, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) *(volatile uint16_t *)(p + 0x7e5e) = 1;
    }
    if (read) (void)*(volatile uint64_t *)where;
    else *(volatile int *)where = 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL test_BindImageMemory(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize o)
{
    if (vkb_srv_args_now.kind == VKB_ARGS_BIND_IMAGE) test_fault();
    return test_bind_next[test_id()](d, i, m, o);
}

static VKAPI_ATTR VkResult VKAPI_CALL test_BindImageMemory2(VkDevice d, uint32_t n, const VkBindImageMemoryInfo *bi)
{
    if (vkb_srv_args_now.kind == VKB_ARGS_BIND_IMAGE2) test_fault();
    return test_bind2_next[test_id()](d, n, bi);
}

static VKAPI_ATTR VkResult VKAPI_CALL test_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a,
                                                          VkDeviceMemory *out)
{
    if (vkb_srv_args_now.kind == VKB_ARGS_ALLOC) test_fault();
    return test_alloc_next[test_id()](d, ai, a, out);
}

/* "xfb_unbound": Mali's vkCmdBeginTransformFeedbackEXT with no graphics pipeline bound in the command
 * buffer (at the driver: what the layers above bound) reads through the missing pipeline (* 0xe78 on the G925: 0xe60 on the G615). The driver-level binds are noted per command buffer. */
#define XFB_CBS 1024
static struct {
    VkCommandBuffer cb;
    uint64_t bound;
} xfb_cbs[XFB_CBS];
static pthread_mutex_t xfb_lock = PTHREAD_MUTEX_INITIALIZER;
static PFN_vkCmdBindPipeline xfb_bind_next[VKB_MAX_TABLES];
static PFN_vkBeginCommandBuffer xfb_begin_next[VKB_MAX_TABLES];
static PFN_vkCmdBeginTransformFeedbackEXT xfb_next[VKB_MAX_TABLES];

static void xfb_note(VkCommandBuffer cb, uint64_t p, int set)
{
    pthread_mutex_lock(&xfb_lock);
    unsigned h = (unsigned)(((uintptr_t)cb >> 4) % XFB_CBS), i = h;
    do {
        if (xfb_cbs[i].cb == cb || !xfb_cbs[i].cb) {
            xfb_cbs[i].cb = cb;
            if (set) xfb_cbs[i].bound = p;
            break;
        }
        i = (i + 1) % XFB_CBS;
    } while (i != h);
    pthread_mutex_unlock(&xfb_lock);
}

static uint64_t xfb_bound(VkCommandBuffer cb)
{
    uint64_t p = 0;
    pthread_mutex_lock(&xfb_lock);
    unsigned h = (unsigned)(((uintptr_t)cb >> 4) % XFB_CBS), i = h;
    do {
        if (xfb_cbs[i].cb == cb) {
            p = xfb_cbs[i].bound;
            break;
        }
        if (!xfb_cbs[i].cb) break;
        i = (i + 1) % XFB_CBS;
    } while (i != h);
    pthread_mutex_unlock(&xfb_lock);
    return p;
}

static VKAPI_ATTR void VKAPI_CALL xfb_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) xfb_note(cb, (uint64_t)(uintptr_t)p, 1);
    xfb_bind_next[test_id()](cb, bp, p);
}

static VKAPI_ATTR VkResult VKAPI_CALL xfb_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    xfb_note(cb, 0, 1); /* a new recording binds anew */
    return xfb_begin_next[test_id()](cb, bi);
}

static VKAPI_ATTR void VKAPI_CALL xfb_CmdBeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs,
                                                                  const VkDeviceSize *offs)
{
    if (!xfb_bound(cb)) {
        VKB_WARN("VKBRIDGE_TEST_CRASH_CMD=xfb_unbound: transform feedback begun with no graphics pipeline bound: faulting as Mali does");
        (void)*(volatile uint32_t *)(uintptr_t)0xe78;
    }
    xfb_next[test_id()](cb, first, n, bufs, offs);
}

/* "vkEndCommandBuffer": the driver's End of a client's replayed command buffer reads address 0x18, as
 * a player log's did (Mali-G2-Ultra r56p0 beta, vkd3d-proton's query-reset command buffer); the server's own
 * command buffers end as before. */
static PFN_vkEndCommandBuffer test_end_next[VKB_MAX_TABLES];

static VKAPI_ATTR VkResult VKAPI_CALL test_EndCommandBuffer(VkCommandBuffer cb)
{
    if (vkb_srv_replay_cb == cb) test_fault_at(0x18, 1);
    return test_end_next[test_id()](cb);
}

/* "vkQueueSubmit" / "vkQueueSubmit2": a client's submission faults in the driver with a read of address 0x32,
 * as a player log's did (Mali-G1-Ultra, Dying Light: libGLES_mali.so+0xa090f4 in vkQueueSubmit2); only inside a
 * client's own submit request (the server's own submissions go on). */
static PFN_vkQueueSubmit test_submit_next[VKB_MAX_TABLES];
static PFN_vkQueueSubmit2 test_submit2_next[VKB_MAX_TABLES];
extern __thread const char *vkb_srv_current_cmd;

static int in_client_cmd(const char *name) { return vkb_srv_current_cmd && !strcmp(vkb_srv_current_cmd, name); }

static VKAPI_ATTR VkResult VKAPI_CALL test_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    if (in_client_cmd("vkQueueSubmit")) test_fault_at(0x32, 1);
    return test_submit_next[test_id()](q, n, si, f);
}

static VKAPI_ATTR VkResult VKAPI_CALL test_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f)
{
    if (in_client_cmd("vkQueueSubmit2")) test_fault_at(0x32, 1);
    return test_submit2_next[test_id()](q, n, si, f);
}

static int test_unbound; /* "unbound": only a draw with no graphics pipeline bound faults, as Mali's driver does */

static VKAPI_ATTR void VKAPI_CALL test_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    if (vkb_srv_replay_cb && (!test_unbound || !vkb_srv_replay_gfx)) test_fault();
    test_draw_next[test_id()](cb, vc, ic, fv, fi);
}

static VKAPI_ATTR void VKAPI_CALL test_CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t n, uint32_t fi, int32_t vo, uint32_t fin)
{
    if (vkb_srv_replay_cb && (!test_unbound || !vkb_srv_replay_gfx)) test_fault();
    test_drawi_next[test_id()](cb, ic, n, fi, vo, fin);
}

void vkb_crashctx_test_init(vkb_srv_table *t)
{
    const char *want = getenv("VKBRIDGE_TEST_CRASH_CMD");
    if (!want || !*want || t->id >= VKB_MAX_TABLES) return;
    if (!strcmp(want, "xfb_unbound")) {
        if (!t->real.vkCmdBeginTransformFeedbackEXT || !t->real.vkCmdBindPipeline || !t->real.vkBeginCommandBuffer) return;
#define XFB_HOOK(field, fn, next)                                                                    \
    do {                                                                                             \
        next[t->id] = t->real.field;                                                                 \
        if (t->dt.field == t->real.field) t->dt.field = fn;                                         \
        t->real.field = fn;                                                                          \
    } while (0)
        XFB_HOOK(vkCmdBindPipeline, xfb_CmdBindPipeline, xfb_bind_next);
        XFB_HOOK(vkBeginCommandBuffer, xfb_BeginCommandBuffer, xfb_begin_next);
        XFB_HOOK(vkCmdBeginTransformFeedbackEXT, xfb_CmdBeginTransformFeedbackEXT, xfb_next);
#undef XFB_HOOK
        VKB_WARN("VKBRIDGE_TEST_CRASH_CMD=xfb_unbound: transform feedback begun with no graphics pipeline bound faults (imitated Mali)");
        return;
    }
    if (!strcmp(want, "unbound")) {
        /* imitating Mali: a draw that reaches the driver with no graphics pipeline bound faults */
        test_unbound = 1;
        want = "vkCmdDraw";
        if (t->real.vkCmdDrawIndexed && t->real.vkCmdDrawIndexed != test_CmdDrawIndexed) {
            test_drawi_next[t->id] = t->real.vkCmdDrawIndexed;
            if (t->dt.vkCmdDrawIndexed == t->real.vkCmdDrawIndexed) t->dt.vkCmdDrawIndexed = test_CmdDrawIndexed;
            t->real.vkCmdDrawIndexed = test_CmdDrawIndexed;
        }
    }
#define TEST_HOOK(field, fn, next)                                                                   \
    do {                                                                                             \
        if (!strcmp(want, #field) && t->real.field && t->real.field != fn) {                        \
            next[t->id] = t->real.field;                                                             \
            if (t->dt.field == t->real.field) t->dt.field = fn;                                      \
            t->real.field = fn;                                                                      \
            VKB_WARN("VKBRIDGE_TEST_CRASH_CMD: a client's %s faults on purpose", #field);           \
        }                                                                                            \
    } while (0)
    TEST_HOOK(vkBindImageMemory, test_BindImageMemory, test_bind_next);
    TEST_HOOK(vkBindImageMemory2, test_BindImageMemory2, test_bind2_next);
    TEST_HOOK(vkAllocateMemory, test_AllocateMemory, test_alloc_next);
    TEST_HOOK(vkCmdDraw, test_CmdDraw, test_draw_next);
    TEST_HOOK(vkCmdDrawIndexed, test_CmdDrawIndexed, test_drawi_next);
    TEST_HOOK(vkEndCommandBuffer, test_EndCommandBuffer, test_end_next);
    TEST_HOOK(vkQueueSubmit, test_QueueSubmit, test_submit_next);
    TEST_HOOK(vkQueueSubmit2, test_QueueSubmit2, test_submit2_next);
#undef TEST_HOOK
}
