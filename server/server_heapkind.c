/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The server's memory by kind of object. The "mem:" line says how big the
 * heap is, never whose: Motor Town's server heap reached ~2 GB with no owner (player
 * logs), RE3 on the Pixel's PowerVR grew 262 MB -> 8.5 GB in 10 s and only a one-off benchmark named
 * vkCreatePipelineLayout (ledger row 339). The driver's own allocations cannot be told apart by
 * address, so each kind is measured by what the process grew by while a request of that kind ran:
 *
 *   - every request (server_conn.c run_request) of a create/destroy/allocate/free command adds one to
 *     its kind's count (relaxed atomic adds; the table cmd -> kind is filled once at load);
 *   - the kinds that are never made per frame (pipelines, shader modules, pipeline layouts, descriptor
 *     set layouts, descriptor pools, pipeline caches, render passes, samplers) are also measured: the
 *     process's anonymous resident memory (/proc/self/statm, resident - shared; one pread of an fd kept
 *     open, no lock taken in the kernel or here) before and after the handler, the difference added to
 *     the kind and to the client's slot. Calls of these kinds running side by side share what grew
 *     (divided by how many were in flight). Images, buffers, memory and descriptor sets are only counted:
 *     their device memory is on the mem: line already, and they can be made every frame;
 *   - "memkind:" line, by the mem: thread (server_memory.c), at most every VKBRIDGE_MEMKIND_S (60) seconds
 *     and only when a count moved: per kind the live count (creates - destroys) and the growth (M);
 *   - "memtop:" line at the first memory low of the session, then each time the process's anonymous
 *     memory grew by another MEMTOP_STEP_MB since the last one (at most MEMTOP_MAX lines): the five
 *     largest (kind, client) owners with count and growth, and the process's anonymous memory and heap.
 *
 * VKBRIDGE_HEAPKIND=0 turns it off (no counts, no probes, no lines). Host test: host/heapkind_test.sh with
 * VKBRIDGE_TEST_HEAPKIND_GROW=<kind>:<MB> (each request of that kind leaks that much, as PowerVR's
 * vkCreatePipelineLayout keeps ~305 MB per layout).
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    HK_NONE,
    HK_PIPELINE,
    HK_SHADER,
    HK_LAYOUT,
    HK_SETLAYOUT,
    HK_DESCPOOL,
    HK_DESCSET,
    HK_PCACHE,
    HK_RENDERPASS,
    HK_SAMPLER,
    HK_IMAGE,
    HK_BUFFER,
    HK_MEMORY,
    HK_VIEW,
    HK_CMDPOOL,
    HK_OTHER,
    HK_N
};
static const char *const hk_name[HK_N] = {"-",      "pipeline", "shader", "layout", "setlayout", "descpool", "descset", "pcache",
                                          "renderpass", "sampler", "image", "buffer", "memory", "view",   "cmdpool", "other"};
/* measured kinds: never made per frame */
static const uint8_t hk_probe[HK_N] = {[HK_PIPELINE] = 1, [HK_SHADER] = 1,     [HK_LAYOUT] = 1,  [HK_SETLAYOUT] = 1,
                                       [HK_DESCPOOL] = 1, [HK_PCACHE] = 1,     [HK_RENDERPASS] = 1, [HK_SAMPLER] = 1};

#define HK_SLOTS 32
#define MEMTOP_STEP_MB 512
#define MEMTOP_MAX 12

/* cmd -> kind (low 5 bits) | 0x40 a destroy/free | 0x80 measured */
uint8_t vkb_hk_cmd[VKB_CMD_COUNT];

static int64_t k_n[HK_N], k_bytes[HK_N], k_calls[HK_N];
typedef struct {
    int pid; /* 0: free (claimed with a compare-exchange) */
    char comm[32];
    int64_t n[HK_N], bytes[HK_N];
} hk_slot;
static hk_slot slots[HK_SLOTS + 1]; /* the last: every client after the table filled */
static int statm_fd = -1;
static long page_kb = 4;
static uint32_t inflight;
static uint64_t probes;
static int grow_kind;
static size_t grow_bytes;
static void *volatile grow_keep;

static int kind_of(const char *n, int *destroy)
{
    *destroy = 0;
    const char *rest;
    if (!strncmp(n, "vkCreate", 8)) rest = n + 8;
    else if (!strncmp(n, "vkDestroy", 9)) rest = n + 9, *destroy = 1;
    else if (!strncmp(n, "vkAllocate", 10)) rest = n + 10;
    else if (!strncmp(n, "vkFree", 6)) rest = n + 6, *destroy = 1;
    else return HK_NONE;
    if (!strcmp(rest, "GraphicsPipelines") || !strcmp(rest, "ComputePipelines") || !strncmp(rest, "RayTracingPipelines", 19) ||
        !strcmp(rest, "Pipeline"))
        return HK_PIPELINE;
    if (!strcmp(rest, "ShaderModule")) return HK_SHADER;
    if (!strcmp(rest, "PipelineLayout")) return HK_LAYOUT;
    if (!strcmp(rest, "DescriptorSetLayout")) return HK_SETLAYOUT;
    if (!strcmp(rest, "DescriptorPool")) return HK_DESCPOOL;
    if (!strcmp(rest, "DescriptorSets")) return HK_DESCSET;
    if (!strcmp(rest, "PipelineCache")) return HK_PCACHE;
    if (!strncmp(rest, "RenderPass", 10)) return HK_RENDERPASS;
    if (!strcmp(rest, "Sampler")) return HK_SAMPLER;
    if (!strcmp(rest, "Image")) return HK_IMAGE;
    if (!strcmp(rest, "Buffer")) return HK_BUFFER;
    if (!strcmp(rest, "Memory")) return HK_MEMORY;
    if (!strcmp(rest, "ImageView") || !strcmp(rest, "BufferView")) return HK_VIEW;
    if (!strcmp(rest, "CommandPool") || !strcmp(rest, "CommandBuffers")) return HK_CMDPOOL;
    if (!strcmp(rest, "Instance") || !strcmp(rest, "Device")) return HK_NONE;
    return HK_OTHER;
}

__attribute__((constructor)) static void hk_init(void)
{
    const char *e = getenv("VKBRIDGE_HEAPKIND");
    if (e && *e == '0') return;
    long ps = sysconf(_SC_PAGESIZE);
    page_kb = ps > 0 ? ps / 1024 : 4;
    statm_fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    for (uint32_t i = 0; i < VKB_CMD_COUNT; i++) {
        int d, k = vkb_cmd_names[i] ? kind_of(vkb_cmd_names[i], &d) : HK_NONE;
        if (k) vkb_hk_cmd[i] = (uint8_t)(k | (d ? 0x40 : 0) | (hk_probe[k] && statm_fd >= 0 ? 0x80 : 0));
    }
    const char *g = getenv("VKBRIDGE_TEST_HEAPKIND_GROW");
    if (g && *g) {
        const char *c = strchr(g, ':');
        for (int k = 1; c && k < HK_N; k++)
            if ((size_t)(c - g) == strlen(hk_name[k]) && !strncmp(g, hk_name[k], (size_t)(c - g))) grow_kind = k, grow_bytes = (size_t)atol(c + 1) << 20;
    }
}

/* the process's anonymous resident memory, KB (resident - shared of /proc/self/statm); -1: unreadable */
long vkb_hk_anon_kb(void)
{
    if (statm_fd < 0) return -1;
    char b[128];
    ssize_t n = pread(statm_fd, b, sizeof(b) - 1, 0);
    if (n <= 0) return -1;
    b[n] = 0;
    char *p = b;
    strtol(p, &p, 10);
    long res = strtol(p, &p, 10), shr = strtol(p, &p, 10);
    return (res - shr) * page_kb;
}

long vkb_hk_begin(uint8_t hk)
{
    if (!(hk & 0x80)) return 0;
    __atomic_add_fetch(&inflight, 1, __ATOMIC_RELAXED);
    long a = vkb_hk_anon_kb();
    if (grow_kind && (hk & 0x1f) == grow_kind && !(hk & 0x40)) {
        /* test: the driver keeps this much per create (written, so it is resident) */
        char *p = malloc(grow_bytes);
        if (p) {
            memset(p, 0x5a, grow_bytes);
            *(void **)p = grow_keep; /* kept (a leak the compiler may not drop) */
            grow_keep = p;
        }
    }
    return a;
}

static hk_slot *slot_for(vkb_srv_proc *p)
{
    if (!p) return &slots[HK_SLOTS];
    int s = __atomic_load_n(&p->hk_slot, __ATOMIC_RELAXED);
    if (s) return &slots[s - 1];
    for (int i = 0; i < HK_SLOTS; i++) {
        int want = 0;
        if (__atomic_compare_exchange_n(&slots[i].pid, &want, p->pid, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            snprintf(slots[i].comm, sizeof(slots[i].comm), "%s", p->comm[0] ? p->comm : "?");
        else if (want != p->pid) continue;
        __atomic_store_n(&p->hk_slot, i + 1, __ATOMIC_RELAXED);
        return &slots[i];
    }
    __atomic_store_n(&p->hk_slot, HK_SLOTS + 1, __ATOMIC_RELAXED);
    return &slots[HK_SLOTS];
}

void vkb_hk_end(uint8_t hk, vkb_srv_proc *p, long before_kb)
{
    int k = hk & 0x1f;
    int64_t d1 = (hk & 0x40) ? -1 : 1;
    hk_slot *s = slot_for(p);
    __atomic_add_fetch(&k_n[k], d1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&k_calls[k], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->n[k], d1, __ATOMIC_RELAXED);
    if (!(hk & 0x80)) return;
    long after = vkb_hk_anon_kb();
    uint32_t now = __atomic_fetch_sub(&inflight, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&probes, 1, __ATOMIC_RELAXED);
    if (after < 0 || before_kb < 0) return;
    int64_t grew = (int64_t)(after - before_kb) * 1024 / (now ? now : 1);
    __atomic_add_fetch(&k_bytes[k], grew, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->bytes[k], grew, __ATOMIC_RELAXED);
}

static long long mb(int64_t b) { return (long long)((b + (b < 0 ? -(1 << 19) : (1 << 19))) >> 20); }

/* "memkind: ..." into buf when a count moved since the last call (or force); 0: nothing to say */
int vkb_hk_kind_line(char *buf, size_t cap, int force)
{
    static int64_t last_calls[HK_N];
    if (statm_fd < 0) return 0;
    int moved = 0;
    for (int k = 1; k < HK_N; k++) moved |= __atomic_load_n(&k_calls[k], __ATOMIC_RELAXED) != last_calls[k];
    if (!moved && !force) return 0;
    int n = snprintf(buf, cap, "memkind: live/grew");
    for (int k = 1; k < HK_N && n < (int)cap; k++) {
        int64_t c = __atomic_load_n(&k_calls[k], __ATOMIC_RELAXED);
        last_calls[k] = c;
        if (!c) continue;
        int64_t live = __atomic_load_n(&k_n[k], __ATOMIC_RELAXED);
        if (k == HK_DESCSET) /* pool resets free sets without a call: the allocations made, not a live count */
            n += snprintf(buf + n, cap - (size_t)n, " descset_allocs %lld", (long long)(c + live) / 2);
        else if (hk_probe[k]) n += snprintf(buf + n, cap - (size_t)n, " %s %lld/%lldM", hk_name[k], (long long)live, mb(__atomic_load_n(&k_bytes[k], __ATOMIC_RELAXED)));
        else n += snprintf(buf + n, cap - (size_t)n, " %s %lld", hk_name[k], (long long)live);
    }
    if (n < (int)cap) n += snprintf(buf + n, cap - (size_t)n, " | probes %llu", (unsigned long long)__atomic_load_n(&probes, __ATOMIC_RELAXED));
    return n > 0;
}

/* "memtop: ..." when due: the first memory low (low_entered), then every MEMTOP_STEP_MB of anonymous growth
 * since the last one. 0: not due. */
int vkb_hk_top_line(char *buf, size_t cap, int low_entered)
{
    static long last_kb = -1;
    static int said;
    if (statm_fd < 0) return 0;
    long anon = vkb_hk_anon_kb();
    if (anon < 0 || said >= MEMTOP_MAX) return 0;
    const char *why;
    if (low_entered && last_kb < 0) why = "memory low";
    else if (last_kb >= 0 && anon - last_kb >= (long)MEMTOP_STEP_MB << 10) why = "grew";
    else return 0;
    last_kb = anon;
    said++;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
#else
    struct mallinfo mi = mallinfo(); /* bionic: as the mem: line's heap */
#endif
    long long heap_mb = (long long)((size_t)mi.uordblks >> 20);
    struct {
        int64_t bytes, n;
        int k, s;
    } top[5];
    int nt = 0;
    for (int s = 0; s <= HK_SLOTS; s++) {
        if (s < HK_SLOTS && !__atomic_load_n(&slots[s].pid, __ATOMIC_ACQUIRE)) continue;
        for (int k = 1; k < HK_N; k++) {
            int64_t b = __atomic_load_n(&slots[s].bytes[k], __ATOMIC_RELAXED);
            if (b <= 0) continue;
            if (nt == 5 && b <= top[4].bytes) continue;
            int at = nt < 5 ? nt++ : 4;
            while (at > 0 && top[at - 1].bytes < b) top[at] = top[at - 1], at--;
            top[at].bytes = b, top[at].n = __atomic_load_n(&slots[s].n[k], __ATOMIC_RELAXED), top[at].k = k, top[at].s = s;
        }
    }
    int n = snprintf(buf, cap, "memtop: %s (%d of %d), server anon %ldM heap %lldM | owners kind/client/live/grew:", why, said, MEMTOP_MAX,
                     anon >> 10, heap_mb);
    for (int i = 0; i < nt && n < (int)cap; i++) {
        const hk_slot *sl = &slots[top[i].s];
        if (top[i].s == HK_SLOTS) n += snprintf(buf + n, cap - (size_t)n, " %s others %lld %lldM;", hk_name[top[i].k], (long long)top[i].n, mb(top[i].bytes));
        else
            n += snprintf(buf + n, cap - (size_t)n, " %s %s(pid %d) %lld %lldM;", hk_name[top[i].k], sl->comm, sl->pid, (long long)top[i].n,
                          mb(top[i].bytes));
    }
    if (!nt && n < (int)cap) n += snprintf(buf + n, cap - (size_t)n, " none measured (the growth is in counted-only kinds or outside requests)");
    return n > 0;
}

/* The mem: thread's turn (server_memory.c, every 5 s): memkind at most every VKBRIDGE_MEMKIND_S seconds (60) when a
 * count moved; memtop when due, with a memkind line just before it. */
void vkb_hk_tick(int low_entered, int lines)
{
    static int period = -1;
    static time_t last;
    if (statm_fd < 0) return;
    if (period < 0) {
        const char *e = vkb_srv_env_global("VKBRIDGE_MEMKIND_S");
        period = e && atoi(e) > 0 ? atoi(e) : 60;
    }
    char top[1024], kinds[1024];
    int due_top = vkb_hk_top_line(top, sizeof(top), low_entered);
    time_t now = time(NULL);
    if (due_top || (lines && now - last >= period)) {
        if (vkb_hk_kind_line(kinds, sizeof(kinds), due_top)) {
            VKB_INFO("%s", kinds);
            last = now;
        }
    }
    if (due_top) VKB_WARN("%s", top);
}
