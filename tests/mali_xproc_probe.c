/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * mali_xproc_probe - can a SECOND process map the Mali driver's own CPU mapping of Vulkan memory?
 *
 * A device-side experiment, not part of the bridge. The bridge shares host-visible memory through
 * dma-bufs it allocates and imports (server_memory.c); if instead the client process could map the
 * pages the Mali driver itself maps for vkMapMemory, any memory type (cached, coherent, whatever
 * the driver offers) could be shared with no import at all. wallentx's placed-memory layer for
 * DroidDeck (tools/hybris/patches/0003-placed-memory-layer.patch and 0004-mali-mapping-lock.patch,
 * MIT) shows the driver's mapping can be mapped again IN THE SAME PROCESS: it reads the
 * /dev/mali0 file offset of the vkMapMemory pointer from /proc/self/maps and calls
 * mmap(MAP_SHARED, maliFd, offset) in pieces of at most 1 MiB. This probe asks the cross-process
 * question: the server process allocates and maps, passes its /dev/mali0 fd over a Unix socket
 * (SCM_RIGHTS) to a separate client process (fork + exec, so the client never loads the driver,
 * like the bridge's guest), and the client maps the same offset. Then both directions are checked
 * with GPU writes and copies. Nothing here is copied from the patches; the /proc/self/maps lookup
 * follows their approach.
 *
 * Build (NDK; runs straight on the device driver):
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang -O2 -fPIE -pie \
 *       -I third_party/Vulkan-Headers-1.4.341/include tests/mali_xproc_probe.c -ldl -o mali_xproc_probe
 * Run:
 *   adb push mali_xproc_probe /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/mali_xproc_probe
 *   adb shell /data/local/tmp/mali_xproc_probe            every host-visible memory type, 4 MiB each
 *   adb shell /data/local/tmp/mali_xproc_probe --type 1 --size 16777216 --rounds 50 --chunk 1048576
 *   (--any: also on a desktop GPU, any /dev/ file counts - a check of the probe itself; with RADV
 *   the DRM render node's mmap offsets work across processes, so every step should pass there)
 *   (adb shell runs in the "shell" SELinux domain; the app's own process is "untrusted_app". If a
 *   step fails with EACCES/EPERM, the same binary run from the app - e.g. DroidDeck's terminal or a
 *   debuggable build's `run-as` - tells whether it is the domain or the driver.)
 *
 * Output: one line per check, "PASS", "FAIL" or "INFO", then per memory type a "VERDICT" line and a
 * last "SUMMARY" line. Exit status 0 when the probe ran to the end (whatever the verdicts), 1 when it
 * could not run (no Vulkan, no device). How to read it:
 *   INFO type T: flags 0x.. maps "<path>" off 0x.. (delta 0x..)
 *        where vkMapMemory's pointer lives. A path other than /dev/mali* means the driver's mapping
 *        is not a mapping of the device file (then nothing below applies: "not a mali mapping").
 *   PASS/FAIL type T: server re-maps in-process        wallentx's case, as a control
 *   PASS/FAIL type T: client mmap whole / chunked / at the server's address
 *        the cross-process question; a FAIL shows errno (EINVAL, EPERM, ENOMEM, ...). kbase checks
 *        the mapping against the context of the file, and some versions against the mm that
 *        created the context: EINVAL/EPERM here most likely means "not across processes".
 *   PASS/FAIL type T: server CPU -> client CPU / client CPU -> server CPU    the same pages
 *   PASS/FAIL type T: GPU write -> client read, N rounds                      GPU to client
 *   PASS/FAIL type T: client write -> GPU copy -> server/client read, N rounds client to GPU
 *        each round the client first reads what the GPU is about to overwrite, so that a CPU
 *        cache that is not kept coherent shows up as stale words. A non-coherent type
 *        (HOST_COHERENT missing) gets the server's vkFlush/vkInvalidate; the client has no way
 *        to do cache maintenance on such a mapping, so a FAIL on a non-coherent type with PASS on
 *        a coherent one is the expected shape, not a broken probe.
 *   PASS/FAIL type T: client mapping after the server's vkUnmapMemory        is the server's
 *        mapping needed while the client's lives (bridge: the server need not keep one)
 *   INFO type T: client read after vkFreeMemory: ...                         what a stale client
 *        mapping sees when the server frees (SIGBUS / old data / zeros); a bridge must never let
 *        it happen, but it says whether the kernel keeps the pages alive
 *   VERDICT type T: "second-process mapping works, coherent both ways" / "... works, GPU writes
 *        not seen without cache maintenance" / "client cannot map (<errno>)" / "not a mali mapping"
 * Expected on Mali (an assumption to be tested, not a measurement): the in-process control passes
 * (wallentx's layer runs), and the client mmap either fails with EINVAL/EPERM (kbase's per-process
 * checks) or works with the same coherence as the server's own mapping per memory type.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* ------------------------------------------------------------------ messages between the two processes */

enum {
    M_MAP = 1,      /* server -> client: map this (fd attached) */
    M_MAPPED,       /* client -> server: results of the map attempts */
    M_READ,         /* server -> client: compare words [a, a+n) with pattern (seed, kind) */
    M_WRITE,        /* server -> client: write pattern (seed, kind) to words [a, a+n) */
    M_TOUCH,        /* server -> client: read words [a, a+n) (pull them into the client's cache) */
    M_UNMAP,        /* server -> client: drop the mapping */
    M_QUIT,
    M_RESULT,       /* client -> server: result of READ/WRITE/TOUCH */
};

typedef struct {
    uint32_t op;
    uint32_t kind;          /* pattern: 0 = seed ^ i*K, 1 = constant seed */
    uint64_t size, offset, chunk, server_addr;
    uint64_t a, n;          /* words */
    uint32_t seed;
    /* M_MAPPED */
    int32_t err_whole, err_chunked, err_same;  /* 0 = worked, else errno; -1 = not tried */
    /* M_RESULT */
    int32_t ok;             /* 1 = as expected, 0 = mismatch, -1 = fault (SIGBUS/SIGSEGV) */
    uint64_t bad, first_bad_word;
    uint32_t first_bad_got, first_bad_want;
    uint32_t zeros;         /* words read as 0 (for the after-free read) */
    char maps_line[160];    /* the client's /proc/self/maps line for its mapping */
} msg;

static int send_msg(int s, const msg *m, int fd)
{
    struct iovec iov = {(void *)m, sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(s, &mh, MSG_NOSIGNAL) == (ssize_t)sizeof(*m) ? 0 : -1;
}

static int recv_msg(int s, msg *m, int *fd)
{
    struct iovec iov = {m, sizeof(*m)};
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    if (fd) *fd = -1;
    if (recvmsg(s, &mh, 0) != (ssize_t)sizeof(*m)) return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && fd) memcpy(fd, CMSG_DATA(c), sizeof(int));
    return 0;
}

static inline uint32_t pat(uint32_t kind, uint32_t seed, uint64_t i) { return kind ? seed : seed ^ (uint32_t)(i * 2654435761u); }

/* ------------------------------------------------------------------ the client process */

static sigjmp_buf fault_jmp;
static volatile sig_atomic_t fault_armed;
static void on_fault(int sig)
{
    if (fault_armed) siglongjmp(fault_jmp, sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void maps_line_for(const void *p, char *out, size_t cap)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    *out = 0;
    while (f && fgets(line, sizeof(line), f)) {
        uintptr_t lo, hi;
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &lo, &hi) == 2 && (uintptr_t)p >= lo && (uintptr_t)p < hi) {
            line[strcspn(line, "\n")] = 0;
            snprintf(out, cap, "%.*s", (int)cap - 1, line);
            break;
        }
    }
    if (f) fclose(f);
}

/* One READ / WRITE / TOUCH on the client's mapping; a SIGBUS or SIGSEGV is reported (ok = -1,
 * first_bad_got = the signal), not fatal. */
static __attribute__((noinline)) void client_access(volatile uint32_t *map, const msg *m, msg *r)
{
    if (!map) {
        r->ok = -1;
        return;
    }
    int sig = sigsetjmp(fault_jmp, 1);
    if (sig) {
        fault_armed = 0;
        r->ok = -1;
        r->first_bad_got = (uint32_t)sig;
        return;
    }
    fault_armed = 1;
    r->ok = 1;
    if (m->op == M_WRITE) {
        for (uint64_t i = 0; i < m->n; i++) map[m->a + i] = pat(m->kind, m->seed, i);
        __sync_synchronize();
    } else if (m->op == M_TOUCH) {
        uint32_t sink = 0;
        for (uint64_t i = 0; i < m->n; i += 4) sink += map[m->a + i];
        __asm__ volatile("" : : "r"(sink));
    } else {
        for (uint64_t i = 0; i < m->n; i++) {
            uint32_t got = map[m->a + i], want = pat(m->kind, m->seed, i);
            if (!got) r->zeros++;
            if (got != want) {
                if (!r->bad) r->first_bad_word = i, r->first_bad_got = got, r->first_bad_want = want;
                r->bad++;
            }
        }
        if (r->bad) r->ok = 0;
    }
    fault_armed = 0;
}

static int client_main(int s)
{
    signal(SIGBUS, on_fault);
    signal(SIGSEGV, on_fault);
    volatile uint32_t *map = NULL;
    size_t map_len = 0;
    void *chunks_base = NULL;
    for (;;) {
        msg m;
        int fd = -1;
        if (recv_msg(s, &m, &fd) < 0) return 0;
        msg r;
        memset(&r, 0, sizeof(r));
        switch (m.op) {
        case M_MAP: {
            r.op = M_MAPPED;
            r.err_whole = r.err_chunked = r.err_same = -1;
            size_t len = (size_t)m.size;
            /* A: the whole allocation in one mmap */
            void *a = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
            r.err_whole = a == MAP_FAILED ? errno : 0;
            /* B: contiguous, in pieces of at most m.chunk, into a reserved range (MAP_FIXED) */
            void *res = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (res != MAP_FAILED) {
                int err = 0;
                for (size_t off = 0; off < len && !err; off += (size_t)m.chunk) {
                    size_t c = len - off < m.chunk ? len - off : (size_t)m.chunk;
                    void *p = mmap((char *)res + off, c, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)(m.offset + off));
                    if (p == MAP_FAILED) err = errno;
                }
                r.err_chunked = err;
                if (err) munmap(res, len);
                else chunks_base = res;
            }
            /* C: at the server's own address (kbase's same-VA regions put CPU and GPU address equal) */
            void *c = mmap((void *)(uintptr_t)m.server_addr, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd,
                           (off_t)m.offset);
            if (c == MAP_FAILED) r.err_same = errno;
            else if (c != (void *)(uintptr_t)m.server_addr) {
                r.err_same = EADDRNOTAVAIL; /* an old kernel took the hint as a hint */
                munmap(c, len);
                c = MAP_FAILED;
            } else r.err_same = 0;
            /* The checks use the first that worked; the others are dropped. */
            void *use = a != MAP_FAILED ? a : chunks_base ? chunks_base : c != MAP_FAILED ? c : NULL;
            if (a != MAP_FAILED && use != a) munmap(a, len);
            if (chunks_base && use != chunks_base) munmap(chunks_base, len);
            if (c != MAP_FAILED && use != c) munmap(c, len);
            chunks_base = NULL;
            map = use;
            map_len = len;
            if (map) maps_line_for((const void *)map, r.maps_line, sizeof(r.maps_line));
            if (fd >= 0) close(fd); /* the mapping keeps the file */
            break;
        }
        case M_READ:
        case M_TOUCH:
        case M_WRITE:
            r.op = M_RESULT;
            client_access(map, &m, &r);
            break;
        case M_UNMAP:
            r.op = M_RESULT;
            if (map) munmap((void *)map, map_len);
            map = NULL;
            r.ok = 1;
            break;
        case M_QUIT:
            return 0;
        }
        if (send_msg(s, &r, -1) < 0) return 1;
    }
}

/* ------------------------------------------------------------------ the server process */

/* --any (a desktop check of the probe itself): any device file counts, not only /dev/mali* (a DRM
 * render node's mmap offsets work across processes, so the whole flow runs on a desktop GPU). */
static const char *dev_prefix = "/dev/mali";
static int checks, failures;
static void line(const char *tag, uint32_t type, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void line(const char *tag, uint32_t type, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s type %u: ", tag, type);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
    if (!strcmp(tag, "PASS") || !strcmp(tag, "FAIL")) checks++;
    if (!strcmp(tag, "FAIL")) failures++;
}

typedef struct {
    uintptr_t lo, hi;
    uint64_t offset;
    unsigned dev_major, dev_minor;
    uint64_t inode;
    char perms[8];
    char path[160];
} map_info;

/* The /proc/self/maps entry holding p. */
static int find_mapping(const void *p, map_info *mi)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line_[512];
    int found = 0;
    while (f && fgets(line_, sizeof(line_), f)) {
        map_info m;
        memset(&m, 0, sizeof(m));
        unsigned long long off, ino;
        int pos = 0;
        if (sscanf(line_, "%" SCNxPTR "-%" SCNxPTR " %7s %llx %x:%x %llu %n", &m.lo, &m.hi, m.perms, &off, &m.dev_major, &m.dev_minor,
                   &ino, &pos) < 7)
            continue;
        if ((uintptr_t)p < m.lo || (uintptr_t)p >= m.hi) continue;
        m.offset = off;
        m.inode = ino;
        snprintf(m.path, sizeof(m.path), "%s", pos ? line_ + pos : "");
        m.path[strcspn(m.path, "\n")] = 0;
        *mi = m;
        found = 1;
        break;
    }
    if (f) fclose(f);
    return found;
}

/* An fd of ours on the same device file (inode) as the mapping. */
static int find_mali_fd(const map_info *mi)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int found = -1;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char link[300], target[256];
        snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (strncmp(target, dev_prefix, strlen(dev_prefix))) continue;
        int fd = atoi(e->d_name);
        struct stat st;
        if (fstat(fd, &st) == 0 && (uint64_t)st.st_ino == mi->inode) {
            found = fd;
            break;
        }
    }
    if (d) closedir(d);
    return found;
}

#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define GD(name) name = (PFN_##name)gdpa(dev, #name)
static PFN_vkCreateBuffer vkCreateBuffer;
static PFN_vkDestroyBuffer vkDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
static PFN_vkAllocateMemory vkAllocateMemory;
static PFN_vkFreeMemory vkFreeMemory;
static PFN_vkBindBufferMemory vkBindBufferMemory;
static PFN_vkMapMemory vkMapMemory;
static PFN_vkUnmapMemory vkUnmapMemory;
static PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
static PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;
static PFN_vkGetDeviceQueue vkGetDeviceQueue;
static PFN_vkCreateCommandPool vkCreateCommandPool;
static PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer vkEndCommandBuffer;
static PFN_vkResetCommandBuffer vkResetCommandBuffer;
static PFN_vkCmdFillBuffer vkCmdFillBuffer;
static PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
static PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
static PFN_vkQueueSubmit vkQueueSubmit;
static PFN_vkQueueWaitIdle vkQueueWaitIdle;

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb;
static int sock = -1;

static int ask(msg *m, msg *reply)
{
    if (send_msg(sock, m, -1) < 0 || recv_msg(sock, reply, NULL) < 0) {
        printf("FAIL the client process went away\n");
        exit(1);
    }
    return reply->ok;
}

static int client_read(uint64_t a, uint64_t n, uint32_t kind, uint32_t seed, msg *r)
{
    msg m = {.op = M_READ, .kind = kind, .a = a, .n = n, .seed = seed};
    return ask(&m, r);
}

static int client_simple(uint32_t op, uint64_t a, uint64_t n, uint32_t kind, uint32_t seed)
{
    msg m = {.op = op, .kind = kind, .a = a, .n = n, .seed = seed}, r;
    return ask(&m, &r);
}

/* GPU work on buf: fill words [fa, fa+fn) with fill (fn may be 0), copy words [ca, ca+cn) to [cd, ...). */
static int gpu(VkBuffer buf, uint64_t fa, uint64_t fn, uint32_t fill, uint64_t ca, uint64_t cd, uint64_t cn)
{
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    if (fn) vkCmdFillBuffer(cb, buf, fa * 4, fn * 4, fill);
    if (cn) {
        VkBufferCopy region = {ca * 4, cd * 4, cn * 4};
        vkCmdCopyBuffer(cb, buf, buf, 1, &region);
    }
    VkMemoryBarrier rb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &rb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    return vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS;
}

static const char *err_name(int e)
{
    static char b[64];
    if (e == 0) return "ok";
    if (e < 0) return "not tried";
    snprintf(b, sizeof(b), "%s (errno %d)", strerror(e), e);
    return b;
}

static void probe_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t type, VkDeviceSize size, uint64_t chunk, int rounds)
{
    VkMemoryPropertyFlags flags = mp->memoryTypes[type].propertyFlags;
    int coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    if (vkCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS) {
        line("INFO", type, "vkCreateBuffer failed; skipped");
        return;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    if (!(req.memoryTypeBits & (1u << type))) {
        line("INFO", type, "buffers cannot use this type (types 0x%x); skipped", req.memoryTypeBits);
        vkDestroyBuffer(dev, buf, NULL);
        return;
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) {
        line("INFO", type, "vkAllocateMemory of %llu bytes failed: %d; skipped", (unsigned long long)req.size, r);
        vkDestroyBuffer(dev, buf, NULL);
        return;
    }
    vkBindBufferMemory(dev, buf, mem, 0);
    volatile uint32_t *sp = NULL;
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&sp);
    size = req.size;
    const uint64_t W = size / 4, Q = W / 4; /* words; four regions A B C D of Q words */
    map_info mi;
    if (!sp || !find_mapping((const void *)sp, &mi)) {
        line("FAIL", type, "vkMapMemory gave no pointer, or it is not in /proc/self/maps");
        goto out_free;
    }
    uint64_t offset = mi.offset + ((uintptr_t)sp - mi.lo);
    line("INFO", type, "flags 0x%x%s, %llu bytes; vkMapMemory %p maps \"%s\" %s off 0x%llx (+0x%llx into the vma %" PRIxPTR "-%" PRIxPTR ")",
         flags, coherent ? " (coherent)" : " (NOT coherent)", (unsigned long long)size, (void *)sp, mi.path, mi.perms,
         (unsigned long long)mi.offset, (unsigned long long)((uintptr_t)sp - mi.lo), mi.lo, mi.hi);
    if (strncmp(mi.path, dev_prefix, strlen(dev_prefix))) {
        line("INFO", type, "VERDICT: not a mali mapping (\"%s\")", mi.path);
        goto out_unmap;
    }
    if ((uintptr_t)sp + size > mi.hi) line("INFO", type, "the allocation spans more than one vma (only the first is described)");
    int mfd = find_mali_fd(&mi);
    if (mfd < 0) {
        line("FAIL", type, "no fd of this process is the mapping's device file (inode %llu)", (unsigned long long)mi.inode);
        goto out_unmap;
    }

    /* Control: wallentx's in-process case. */
    void *alias = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, (off_t)offset);
    int alias_err = alias == MAP_FAILED ? errno : 0;
    if (alias != MAP_FAILED) {
        sp[7] = 0x600DF00Du;
        __sync_synchronize();
        int same = ((volatile uint32_t *)alias)[7] == 0x600DF00Du;
        munmap(alias, size);
        line(same ? "PASS" : "FAIL", type, "server re-maps in-process (fd %d, whole, offset 0x%llx)%s", mfd, (unsigned long long)offset,
             same ? "" : ": mapped, but not the same bytes");
    } else {
        void *c1 = mmap(NULL, chunk < size ? chunk : size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, (off_t)offset);
        int e1 = c1 == MAP_FAILED ? errno : 0;
        if (c1 != MAP_FAILED) munmap(c1, chunk < size ? chunk : size);
        line(e1 ? "FAIL" : "PASS", type, "server re-maps in-process: whole %s, first chunk of %llu bytes %s", err_name(alias_err),
             (unsigned long long)chunk, err_name(e1));
    }

    /* The cross-process question. */
    msg m = {.op = M_MAP, .size = size, .offset = offset, .chunk = chunk, .server_addr = (uintptr_t)sp}, rep;
    if (send_msg(sock, &m, mfd) < 0 || recv_msg(sock, &rep, NULL) < 0) {
        printf("FAIL the client process went away\n");
        exit(1);
    }
    line(rep.err_whole ? "FAIL" : "PASS", type, "client mmap whole: %s", err_name(rep.err_whole));
    /* (A DRM render node refuses offsets inside an object, so on a desktop GPU this one fails.) */
    line(rep.err_chunked ? "FAIL" : "PASS", type, "client mmap in %llu-byte chunks (MAP_FIXED, contiguous): %s", (unsigned long long)chunk,
         err_name(rep.err_chunked));
    line(rep.err_same ? "FAIL" : "PASS", type, "client mmap at the server's address %p: %s", (void *)sp, err_name(rep.err_same));
    if (rep.err_whole && rep.err_chunked && rep.err_same) {
        line("INFO", type, "VERDICT: client cannot map (whole: %s)", err_name(rep.err_whole));
        goto out_unmap;
    }
    line("INFO", type, "client's mapping: %s", rep.maps_line);
    VkMappedMemoryRange whole = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};

    /* Server CPU -> client CPU, and back. */
    for (uint64_t i = 0; i < Q; i++) sp[i] = pat(0, 0x11110000u, i);
    __sync_synchronize();
    msg rr;
    int ok1 = client_read(0, Q, 0, 0x11110000u, &rr);
    line(ok1 == 1 ? "PASS" : "FAIL", type, "server CPU -> client CPU: %llu of %llu words wrong%s", (unsigned long long)rr.bad,
         (unsigned long long)Q, ok1 < 0 ? " (client faulted)" : "");
    client_simple(M_WRITE, Q, Q, 0, 0x22220000u);
    uint64_t bad2 = 0;
    for (uint64_t i = 0; i < Q; i++) bad2 += sp[Q + i] != pat(0, 0x22220000u, i);
    line(bad2 ? "FAIL" : "PASS", type, "client CPU -> server CPU: %llu of %llu words wrong", (unsigned long long)bad2, (unsigned long long)Q);
    int cpu_ok = ok1 == 1 && !bad2;

    /* GPU -> client: the client first pulls region B into its cache, the GPU fills it, the client
     * reads. The server invalidates for a non-coherent type (its own mapping only). */
    uint64_t bad_rounds3 = 0, faults3 = 0;
    char first3[200] = "";
    for (int k = 0; k < rounds; k++) {
        uint32_t fill = 0xA5000000u + (uint32_t)k;
        client_simple(M_TOUCH, Q, Q, 0, 0);
        if (!gpu(buf, Q, Q, fill, 0, 0, 0)) {
            line("FAIL", type, "GPU submission failed");
            goto out_client;
        }
        if (!coherent) vkInvalidateMappedMemoryRanges(dev, 1, &whole);
        int ok = client_read(Q, Q, 1, fill, &rr);
        if (ok != 1) {
            bad_rounds3++;
            faults3 += ok < 0;
            if (!*first3)
                snprintf(first3, sizeof(first3), "round %d: %llu words stale, word %llu = 0x%08x, want 0x%08x", k, (unsigned long long)rr.bad,
                         (unsigned long long)rr.first_bad_word, rr.first_bad_got, rr.first_bad_want);
        }
    }
    line(bad_rounds3 ? "FAIL" : "PASS", type, "GPU write -> client read: %llu of %d rounds wrong%s%s%s", (unsigned long long)bad_rounds3, rounds,
         faults3 ? " (client faulted)" : "", *first3 ? "; first: " : "", first3);

    /* Client -> GPU: the client writes region C, the GPU copies it to D, both CPUs read D. */
    uint64_t bad_rounds4 = 0, bad_srv4 = 0;
    char first4[200] = "";
    for (int k = 0; k < rounds; k++) {
        uint32_t seed = 0x33000000u + (uint32_t)k * 0x10001u;
        client_simple(M_TOUCH, 3 * Q, Q, 0, 0);
        client_simple(M_WRITE, 2 * Q, Q, 0, seed);
        if (!coherent) vkFlushMappedMemoryRanges(dev, 1, &whole);
        if (!gpu(buf, 0, 0, 0, 2 * Q, 3 * Q, Q)) {
            line("FAIL", type, "GPU submission failed");
            goto out_client;
        }
        if (!coherent) vkInvalidateMappedMemoryRanges(dev, 1, &whole);
        uint64_t sb = 0;
        for (uint64_t i = 0; i < Q; i++) sb += sp[3 * Q + i] != pat(0, seed, i);
        bad_srv4 += sb != 0;
        int ok = client_read(3 * Q, Q, 0, seed, &rr);
        if (ok != 1 || sb) {
            bad_rounds4++;
            if (!*first4)
                snprintf(first4, sizeof(first4), "round %d: server sees %llu wrong, client %llu (word %llu = 0x%08x, want 0x%08x)", k,
                         (unsigned long long)sb, (unsigned long long)rr.bad, (unsigned long long)rr.first_bad_word, rr.first_bad_got,
                         rr.first_bad_want);
        }
    }
    line(bad_rounds4 ? "FAIL" : "PASS", type, "client write -> GPU copy -> server/client read: %llu of %d rounds wrong (server wrong in %llu)%s%s",
         (unsigned long long)bad_rounds4, rounds, (unsigned long long)bad_srv4, *first4 ? "; first: " : "", first4);

    /* Does the client's mapping need the server's? */
    for (uint64_t i = 0; i < Q; i++) sp[i] = pat(0, 0x44440000u, i);
    __sync_synchronize();
    vkUnmapMemory(dev, mem);
    sp = NULL;
    if (!gpu(buf, Q, Q, 0x5A5A5A5Au, 0, 0, 0)) line("FAIL", type, "GPU submission failed");
    int ok5 = client_read(Q, Q, 1, 0x5A5A5A5Au, &rr);
    int ok5b = client_read(0, Q, 0, 0x44440000u, &rr);
    line(ok5 == 1 && ok5b == 1 ? "PASS" : "FAIL", type, "client mapping after the server's vkUnmapMemory: GPU fill %s, earlier CPU words %s",
         ok5 == 1 ? "seen" : ok5 < 0 ? "FAULT" : "wrong", ok5b == 1 ? "kept" : ok5b < 0 ? "FAULT" : "wrong");

    /* What a client mapping sees once the memory is freed (never to be relied on). */
    vkDestroyBuffer(dev, buf, NULL);
    buf = VK_NULL_HANDLE;
    vkFreeMemory(dev, mem, NULL);
    mem = VK_NULL_HANDLE;
    int ok6 = client_read(0, Q, 0, 0x44440000u, &rr);
    line("INFO", type, "client read after vkFreeMemory: %s (%llu of %llu words changed, %u zero)",
         ok6 < 0 ? (rr.first_bad_got == SIGBUS ? "SIGBUS" : "SIGSEGV") : ok6 == 1 ? "old data still there (pages kept alive by the mapping)" : "changed",
         (unsigned long long)rr.bad, (unsigned long long)Q, rr.zeros);

    {
        const char *v;
        if (!cpu_ok) v = "client maps, but not the same pages";
        else if (!bad_rounds3 && !bad_rounds4) v = "second-process mapping works, coherent both ways";
        else if (bad_rounds3 && !bad_rounds4) v = "second-process mapping works, GPU writes not seen without cache maintenance";
        else if (!bad_rounds3) v = "second-process mapping works, client writes not seen by the GPU without cache maintenance";
        else v = "second-process mapping works, not coherent in either direction";
        line("INFO", type, "VERDICT: %s%s", v, coherent ? "" : " (a non-coherent type)");
    }
out_client:
    client_simple(M_UNMAP, 0, 0, 0, 0);
out_unmap:
    if (sp) vkUnmapMemory(dev, mem);
out_free:
    if (buf) vkDestroyBuffer(dev, buf, NULL);
    if (mem) vkFreeMemory(dev, mem, NULL);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--client")) return client_main(atoi(argv[2]));
    int only = -1, rounds = 20;
    VkDeviceSize size = 4u << 20;
    uint64_t chunk = 1u << 20;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--any")) dev_prefix = "/dev/";
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--type")) only = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--size")) size = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--chunk")) chunk = strtoull(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--rounds")) rounds = atoi(argv[i + 1]);
    }
    if (size < 65536) size = 65536;
    if (chunk < 4096) chunk = 4096;

    /* The client first, before any driver is loaded here: a fresh process image (exec), joined by
     * a socket pair; nothing of the driver's is inherited. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        printf("FAIL socketpair: %s\n", strerror(errno));
        return 1;
    }
    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        int fd = fcntl(sv[1], F_DUPFD, 10); /* without CLOEXEC: kept across exec */
        char fds[16];
        snprintf(fds, sizeof(fds), "%d", fd);
        execl("/proc/self/exe", argv[0], "--client", fds, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    sock = sv[0];
    printf("INFO server pid %d, client pid %d (a separate process image)\n", (int)getpid(), (int)child);

    void *lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL no Vulkan loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = NULL;
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "mali_xproc_probe", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL vkCreateInstance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkGetPhysicalDeviceQueueFamilyProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAIL no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    uint32_t dv = props.driverVersion;
    printf("INFO %s, driver 0x%08x (r%up%u on Arm's encoding), API %u.%u\n", props.deviceName, dv, (dv >> 22) & 0x3ff, (dv >> 12) & 0x3ff,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        printf("INFO memory type %u: flags 0x%x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t qf = 0;
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
            qf = i;
            break;
        }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        printf("FAIL vkCreateDevice\n");
        return 1;
    }
    GD(vkCreateBuffer);
    GD(vkDestroyBuffer);
    GD(vkGetBufferMemoryRequirements);
    GD(vkAllocateMemory);
    GD(vkFreeMemory);
    GD(vkBindBufferMemory);
    GD(vkMapMemory);
    GD(vkUnmapMemory);
    GD(vkFlushMappedMemoryRanges);
    GD(vkInvalidateMappedMemoryRanges);
    GD(vkGetDeviceQueue);
    GD(vkCreateCommandPool);
    GD(vkAllocateCommandBuffers);
    GD(vkBeginCommandBuffer);
    GD(vkEndCommandBuffer);
    GD(vkResetCommandBuffer);
    GD(vkCmdFillBuffer);
    GD(vkCmdCopyBuffer);
    GD(vkCmdPipelineBarrier);
    GD(vkQueueSubmit);
    GD(vkQueueWaitIdle);
    vkGetDeviceQueue(dev, qf, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev, &cai, &cb);

    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        if (only >= 0 && (int)t != only) continue;
        VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || (f & VK_MEMORY_PROPERTY_PROTECTED_BIT)) continue;
        probe_type(&mp, t, size, chunk, rounds);
    }
    msg q = {.op = M_QUIT};
    send_msg(sock, &q, -1);
    int st = 0;
    waitpid(child, &st, 0);
    printf("SUMMARY %d checks, %d failed; client exit %d\n", checks, failures, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}
