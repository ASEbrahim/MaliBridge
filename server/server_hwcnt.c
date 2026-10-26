/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Evidence: what the GPU's time is spent waiting on. With VKBRIDGE_GPU_PROF=counters (the
 * timestamp profiler of server_gpuprof.c runs too) or =counters-only (counters alone: no
 * timestamps, no command hooks, nothing that changes the game's work) a thread in the server
 * reads Mali's hardware counters every 5 s and logs a summary: GPU active cycles and clock, how
 * busy the fragment, vertex/tiler and compute queues were, shader core utilisation, the
 * arithmetic, texture, load/store and varying units' issue rates, external memory traffic (per
 * second and per frame), tiler primitives, early-ZS and prepass kills against fragments shaded,
 * and a one-line verdict naming the bottleneck. The timestamps say which pass costs the most;
 * this says why.
 *
 * How: the counters come from the kbase driver itself, through a second /dev/mali0 file of our
 * own (the Vulkan driver keeps its own; this one never submits work: system-monitor context), and
 * its kinstr_prfcnt interface (CSF kbase, UK 1.17 and later: r3x and newer drivers; Arm's own
 * tools use it, no root needed): enumerate the counter blocks, ask for all counters of the front
 * end, tiler, memory (one block per L2 slice) and shader core blocks in manual mode, start, then
 * one synchronous sample per period; the samples are deltas since the previous one. Anything
 * missing (no /dev/mali0, an older or Job Manager kbase, a GPU this file has no counter map for,
 * SELinux saying no) is logged once and the rest of the profiler goes on.
 *
 * The ioctl structures and the counter indices are re-implemented from Arm's HWCPipe
 * (https://github.com/ARM-software/HWCPipe, backend/device and specification/database, MIT; its
 * licence is in third_party/HWCPipe/LICENSE.md), read against the kbase sources. Not taken over:
 * its C++ framework, JM/vinstr/panthor back ends and work-arounds for kbase versions without
 * working kinstr_prfcnt (UK < 1.17), which no 5th-generation Mali ships with. Changed for the
 * bridge: counters are summed per block type in one pass (no per-counter maps); the L2 slice
 * count is the number of memory blocks (HWCPipe's MaliConfigL2CacheCount multiplies by the slice
 * size); set_flags is only sent when the kernel asks for a set-up context, so on current kbase no
 * kernel context is made at all. Derived figures follow the counter database's equations for the
 * 5th-generation GPUs (Mali-G720/G620: arch 12; Immortalis-G925/Mali-G725/G625: 13; Mali G1: 14);
 * other GPUs say so once.
 *
 * Portions: Copyright (c) 2022-2026 Arm Limited. SPDX-License-Identifier: MIT (the ioctl layouts
 * and the counter index table below). Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 * KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_S 5
#define MAX_VALUES 256 /* counters per block: 128 on these GPUs */

/* ---- kbase ioctls (type 0x80) ---- */
struct kb_version {
    uint16_t major, minor;
};
struct kb_flags {
    uint32_t create_flags;
};
struct kb_props {
    uint64_t buffer;
    uint32_t size, flags;
};
struct kb_enum_info {
    uint32_t item_size, item_count;
    uint64_t list;
};
union kb_setup {
    struct {
        uint32_t count, size;
        uint64_t requests;
    } in;
    struct {
        uint32_t md_item_size, mmap_size;
    } out;
};
#define KB_VERSION_CHECK_JM _IOWR(0x80, 0, struct kb_version)
#define KB_VERSION_CHECK_CSF _IOWR(0x80, 52, struct kb_version)
#define KB_SET_FLAGS _IOW(0x80, 1, struct kb_flags)
#define KB_GET_GPUPROPS _IOW(0x80, 3, struct kb_props)
#define KB_PRFCNT_ENUM_INFO _IOWR(0x80, 56, struct kb_enum_info)
#define KB_PRFCNT_SETUP _IOWR(0x80, 57, union kb_setup)
#define KB_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED (1u << 1)

/* ---- kinstr_prfcnt (the setup's file, type 0xbf): flexible lists of 4-byte headers ---- */
#define PC_TYPE(list, sub) ((uint16_t)(((list) << 12) | (sub)))
#define PC_NONE 0
#define PC_ENUM_BLOCK PC_TYPE(0, 0) /* the list's end: a block of 0 instances and 0 values */
#define PC_ENUM_REQUEST PC_TYPE(0, 1)
#define PC_ENUM_SAMPLE_INFO PC_TYPE(0, 2)
#define PC_REQ_MODE PC_TYPE(1, 0)
#define PC_REQ_ENABLE PC_TYPE(1, 1)
#define PC_META_SAMPLE PC_TYPE(2, 0)
#define PC_META_CLOCK PC_TYPE(2, 1)
#define PC_META_BLOCK PC_TYPE(2, 2)
#define PC_API_VERSION 0
enum { PC_BLK_FE, PC_BLK_TILER, PC_BLK_MEMORY, PC_BLK_SC, PC_BLK_TYPES = 7 };
enum { PC_CMD_START = 1, PC_CMD_STOP, PC_CMD_SAMPLE_SYNC, PC_CMD_SAMPLE_ASYNC, PC_CMD_DISCARD };

struct pc_hdr {
    uint16_t type, version;
};
struct pc_enum_item {
    struct pc_hdr hdr;
    uint8_t pad[4];
    union {
        struct {
            uint8_t type, set, pad[2];
            uint16_t instances, values;
            uint64_t mask[2];
        } block;
        struct {
            uint16_t type, pad;
            uint32_t versions;
        } request;
        struct {
            uint32_t clock_domains, pad;
        } sample_info;
    } u;
};
struct pc_request {
    struct pc_hdr hdr;
    uint8_t pad[4];
    union {
        struct {
            uint8_t mode, pad[7]; /* 0: manual */
            uint64_t period_ns;
        } mode;
        struct {
            uint8_t type, set, pad[6];
            uint64_t mask[2];
        } enable;
    } u;
};
struct pc_meta {
    struct pc_hdr hdr;
    uint8_t pad[4];
    union {
        struct {
            uint64_t ts_start, ts_end, seq, user_data;
            uint32_t flags, pad; /* bit 0: overflow, bit 30: error */
        } sample;
        struct {
            uint32_t domains, pad;
            uint64_t cycles[4]; /* top-level GPU clock first */
        } clock;
        struct {
            uint8_t type, idx, set, pad;
            uint32_t state, values_offset, pad2;
        } block;
    } u;
};
struct pc_cmd {
    uint16_t cmd, pad[3];
    uint64_t user_data;
};
struct pc_access {
    uint64_t seq, offset;
};
_Static_assert(sizeof(struct pc_enum_item) == 32, "enum item");
_Static_assert(sizeof(struct pc_request) == 32, "request item");
_Static_assert(sizeof(struct pc_meta) == 48, "metadata item");
#define PC_ISSUE _IOW(0xbf, 0x00, struct pc_cmd)
#define PC_GET_SAMPLE _IOR(0xbf, 0x01, struct pc_access)
#define PC_PUT_SAMPLE _IOW(0xbf, 0x10, struct pc_access)

/* ---- the counters read, by block, with their index on arch 12 (G720/G620) and on arch 13-14
 * (G925/G725/G625, G1); -1: not on that GPU. Shift: the hardware counts in units of 1 << shift. */
enum { HB_FE, HB_TILER, HB_MEM, HB_SC, HB_N };
enum {
    C_GPU_ACTIVE, C_GPU_ITER_ACTIVE, C_ITER_COMP_ACTIVE, C_ITER_COMP_READY_BLOCKED, C_ITER_FRAG_ACTIVE,
    C_ITER_FRAG_READY_BLOCKED, C_ITER_TILER_ACTIVE, C_ITER_TILER_READY_BLOCKED, C_ITER_FRAG_TASK_COMPLETED,
    C_L2_EXT_READ, C_L2_EXT_READ_BEATS, C_L2_EXT_AR_STALL, C_L2_EXT_WRITE, C_L2_EXT_WRITE_BEATS, C_L2_EXT_W_STALL,
    C_L2_READ_LOOKUP, C_L2_WRITE_LOOKUP,
    C_SHADER_CORE_ACTIVE, C_FRAG_ACTIVE, C_COMPUTE_ACTIVE, C_EXEC_CORE_ACTIVE, C_EXEC_INSTR_FMA, C_EXEC_INSTR_CVT,
    C_EXEC_INSTR_SFU, C_EXEC_INSTR_SLOT_1, C_EXEC_STARVE_ARITH, C_TEX_FILT_NUM_OPERATIONS, C_TEX_TFCH_NUM_TCL_OPERATIONS,
    C_TEX_CFCH_NUM_DIRECT_PATH_OPERATIONS, C_TEX_CFCH_NUM_RP_OPERATIONS, C_TEX_MSGI_NUM_FLITS,
    C_TEX_CFCH_NUM_L1_CL_OPERATIONS, C_TEX_CFCH_NUM_L1_CT_OPERATIONS, C_TEX_TIDX_NUM_OPERATIONS, C_TEX_MSGO_NUM_MSG,
    C_TEX_MSGO_NUM_SINGLE_QUAD_MSG, C_LS_MEM_READ_FULL, C_LS_MEM_READ_SHORT, C_LS_MEM_WRITE_FULL, C_LS_MEM_WRITE_SHORT,
    C_LS_MEM_ATOMIC, C_VARY_SLOT_32, C_VARY_SLOT_16, C_FRAG_WARPS, C_FRAG_QUADS_RAST, C_FRAG_QUADS_EZS_TEST,
    C_FRAG_QUADS_EZS_KILL, C_FRAG_LZS_KILL, C_FRAG_SHADER_THREADS, C_FRAG_FPK_ACTIVE, C_QUAD_FPK_KILLER,
    C_FRAG_PRIM_RAST, C_FRAG_PTILES, C_FRAG_TRANS_ELIM, C_COMPUTE_WARPS, C_FRAG_PRIMITIVES_HSR_CULLED,
    C_FRAG_QUADS_HSR_BUF_TEST, C_FRAG_QUADS_HSR_BUF_KILLED, C_EXEC_MSG_STALLED_TEX, C_EXEC_MSG_STALLED_VARY,
    C_EXEC_MSG_STALLED_BLEND, C_EXEC_MSG_STALLED_ZS, C_EXEC_MSG_STALLED_LSC, C_BEATS_RD_LSC_EXT, C_BEATS_RD_TEX_EXT,
    C_BEATS_RD_FTC_EXT, C_BEATS_WR_TIB,
    C_TILER_ACTIVE, C_TRIANGLES, C_PRIM_VISIBLE, C_PRIM_FACE_CULLED, C_PRIM_FRUSTUM_CULLED, C_PRIM_SAMPLE_CULLED,
    C_PRIM_SCISSOR_CULLED,
    C_COUNT
};
static const struct {
    uint8_t blk, shift;
    int16_t idx[2];
} ctr[C_COUNT] = {
    [C_GPU_ACTIVE] = {HB_FE, 0, {4, 4}},
    [C_GPU_ITER_ACTIVE] = {HB_FE, 0, {6, 6}},
    [C_ITER_COMP_ACTIVE] = {HB_FE, 0, {24, 32}},
    [C_ITER_COMP_READY_BLOCKED] = {HB_FE, 0, {30, 38}},
    [C_ITER_FRAG_ACTIVE] = {HB_FE, 0, {32, 48}},
    [C_ITER_FRAG_READY_BLOCKED] = {HB_FE, 0, {38, 54}},
    [C_ITER_TILER_ACTIVE] = {HB_FE, 0, {16, 64}},
    [C_ITER_TILER_READY_BLOCKED] = {HB_FE, 0, {22, 70}},
    [C_ITER_FRAG_TASK_COMPLETED] = {HB_FE, 0, {34, 50}},
    [C_L2_EXT_READ] = {HB_MEM, 0, {29, 29}},
    [C_L2_EXT_READ_BEATS] = {HB_MEM, 0, {32, 32}},
    [C_L2_EXT_AR_STALL] = {HB_MEM, 0, {33, 33}},
    [C_L2_EXT_WRITE] = {HB_MEM, 0, {42, 42}},
    [C_L2_EXT_WRITE_BEATS] = {HB_MEM, 0, {47, 47}},
    [C_L2_EXT_W_STALL] = {HB_MEM, 0, {48, 48}},
    [C_L2_READ_LOOKUP] = {HB_MEM, 0, {26, 26}},
    [C_L2_WRITE_LOOKUP] = {HB_MEM, 0, {27, 27}},
    [C_SHADER_CORE_ACTIVE] = {HB_SC, 0, {53, 53}},
    [C_FRAG_ACTIVE] = {HB_SC, 0, {4, 4}},
    [C_COMPUTE_ACTIVE] = {HB_SC, 0, {22, 22}},
    [C_EXEC_CORE_ACTIVE] = {HB_SC, 0, {26, 26}},
    [C_EXEC_INSTR_FMA] = {HB_SC, 0, {27, 27}},
    [C_EXEC_INSTR_CVT] = {HB_SC, 0, {28, 28}},
    [C_EXEC_INSTR_SFU] = {HB_SC, 0, {29, 29}},
    [C_EXEC_INSTR_SLOT_1] = {HB_SC, 0, {-1, 118}},
    [C_EXEC_STARVE_ARITH] = {HB_SC, 0, {33, 33}},
    [C_TEX_FILT_NUM_OPERATIONS] = {HB_SC, 0, {39, 39}},
    [C_TEX_TFCH_NUM_TCL_OPERATIONS] = {HB_SC, 0, {92, 92}},
    [C_TEX_CFCH_NUM_DIRECT_PATH_OPERATIONS] = {HB_SC, 0, {88, 88}},
    [C_TEX_CFCH_NUM_RP_OPERATIONS] = {HB_SC, 0, {93, 93}},
    [C_TEX_MSGI_NUM_FLITS] = {HB_SC, 0, {35, 35}},
    [C_TEX_CFCH_NUM_L1_CL_OPERATIONS] = {HB_SC, 0, {89, 89}},
    [C_TEX_CFCH_NUM_L1_CT_OPERATIONS] = {HB_SC, 0, {90, 90}},
    [C_TEX_TIDX_NUM_OPERATIONS] = {HB_SC, 0, {94, 94}},
    [C_TEX_MSGO_NUM_MSG] = {HB_SC, 0, {42, 42}},
    [C_TEX_MSGO_NUM_SINGLE_QUAD_MSG] = {HB_SC, 0, {91, 91}},
    [C_LS_MEM_READ_FULL] = {HB_SC, 0, {44, 44}},
    [C_LS_MEM_READ_SHORT] = {HB_SC, 0, {45, 45}},
    [C_LS_MEM_WRITE_FULL] = {HB_SC, 0, {46, 46}},
    [C_LS_MEM_WRITE_SHORT] = {HB_SC, 0, {47, 47}},
    [C_LS_MEM_ATOMIC] = {HB_SC, 0, {48, 48}},
    [C_VARY_SLOT_32] = {HB_SC, 0, {50, 50}},
    [C_VARY_SLOT_16] = {HB_SC, 0, {51, 51}},
    [C_FRAG_WARPS] = {HB_SC, 0, {9, 9}},
    [C_FRAG_QUADS_RAST] = {HB_SC, 0, {11, 11}},
    [C_FRAG_QUADS_EZS_TEST] = {HB_SC, 0, {12, 12}},
    [C_FRAG_QUADS_EZS_KILL] = {HB_SC, 0, {14, 14}},
    [C_FRAG_LZS_KILL] = {HB_SC, 0, {16, 16}},
    [C_FRAG_SHADER_THREADS] = {HB_SC, 2, {69, 69}},
    [C_FRAG_FPK_ACTIVE] = {HB_SC, 0, {7, 7}},
    [C_QUAD_FPK_KILLER] = {HB_SC, 0, {20, 20}},
    [C_FRAG_PRIM_RAST] = {HB_SC, 0, {6, 6}},
    [C_FRAG_PTILES] = {HB_SC, 0, {18, 18}},
    [C_FRAG_TRANS_ELIM] = {HB_SC, 0, {19, 19}},
    [C_COMPUTE_WARPS] = {HB_SC, 0, {24, 24}},
    [C_FRAG_PRIMITIVES_HSR_CULLED] = {HB_SC, 0, {-1, 98}},
    [C_FRAG_QUADS_HSR_BUF_TEST] = {HB_SC, 0, {-1, 102}},
    [C_FRAG_QUADS_HSR_BUF_KILLED] = {HB_SC, 0, {-1, 103}},
    [C_EXEC_MSG_STALLED_TEX] = {HB_SC, 0, {-1, 112}},
    [C_EXEC_MSG_STALLED_VARY] = {HB_SC, 0, {-1, 113}},
    [C_EXEC_MSG_STALLED_BLEND] = {HB_SC, 0, {-1, 114}},
    [C_EXEC_MSG_STALLED_ZS] = {HB_SC, 0, {-1, 115}},
    [C_EXEC_MSG_STALLED_LSC] = {HB_SC, 0, {-1, 116}},
    [C_BEATS_RD_LSC_EXT] = {HB_SC, 0, {57, 57}},
    [C_BEATS_RD_TEX_EXT] = {HB_SC, 0, {59, 59}},
    [C_BEATS_RD_FTC_EXT] = {HB_SC, 0, {55, 55}},
    [C_BEATS_WR_TIB] = {HB_SC, 0, {62, 62}},
    [C_TILER_ACTIVE] = {HB_TILER, 0, {4, 4}},
    [C_TRIANGLES] = {HB_TILER, 0, {6, 6}},
    [C_PRIM_VISIBLE] = {HB_TILER, 0, {11, 11}},
    [C_PRIM_FACE_CULLED] = {HB_TILER, 0, {12, 12}},
    [C_PRIM_FRUSTUM_CULLED] = {HB_TILER, 0, {13, 13}},
    [C_PRIM_SAMPLE_CULLED] = {HB_TILER, 0, {14, 14}},
    [C_PRIM_SCISSOR_CULLED] = {HB_TILER, 0, {70, 70}},
};
#define SC_BUS_BYTES 16.0  /* shader core <-> L2 beat */
#define MAIN_TASK_PX 64.0  /* main (fragment) queue task: 64x64 pixels */
#define VAR_ISSUE_PER_CY 4 /* varying unit */

typedef struct hw_state {
    int dev, rd;            /* /dev/mali0 (ours), the counter reader */
    const uint8_t *map;     /* the reader's samples */
    size_t map_size, md_size;
    uint32_t nvalues;       /* counters per block */
    int col;                /* ctr[].idx column */
    unsigned arch, product;
    char gpu[40];
    unsigned ncores, nl2, nsc_blocks;
    double bus_bytes;       /* external bus beat */
    unsigned uk_major, uk_minor;
    uint8_t set;            /* counter set (primary unless the kernel was built otherwise) */
    uint8_t has[PC_BLK_TYPES];
    /* the last sample, summed per block type */
    double sum[HB_N][MAX_VALUES];
    uint64_t top_cycles, ts_start, ts_end;
    uint32_t flags;
    /* frames */
    uint64_t presents;
    double t_last, fps; /* fps < 0: not known yet */
    int have_frames, stale;
} hw_state;

static void say_off(const char *why, int err)
{
    if (err)
        VKB_INFO("gpu counters off: %s (%s)", why, strerror(err));
    else
        VKB_INFO("gpu counters off: %s", why);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

/* KBASE_IOCTL_GET_GPUPROPS: a list of (id << 2 | size code, value) pairs, little-endian. */
static int read_props(hw_state *h)
{
    struct kb_props p = {0};
    int n = xioctl(h->dev, KB_GET_GPUPROPS, &p);
    if (n <= 0) return n < 0 ? -errno : -EINVAL;
    uint8_t *buf = calloc(1, (size_t)n);
    if (!buf) return -ENOMEM;
    p.buffer = (uint64_t)(uintptr_t)buf;
    p.size = (uint32_t)n;
    if (xioctl(h->dev, KB_GET_GPUPROPS, &p) < 0) {
        int e = errno;
        free(buf);
        return -e;
    }
    uint64_t gpu_id = 0, shader_present = 0, l2_features = 0, l2_slices = 0;
    for (size_t at = 0; at + 4 <= (size_t)n;) {
        uint32_t key = (uint32_t)buf[at] | (uint32_t)buf[at + 1] << 8 | (uint32_t)buf[at + 2] << 16 | (uint32_t)buf[at + 3] << 24;
        at += 4;
        size_t sz = (size_t)1 << (key & 3);
        if (at + sz > (size_t)n) break;
        uint64_t v = 0;
        for (size_t b = 0; b < sz; b++) v |= (uint64_t)buf[at + b] << (8 * b);
        at += sz;
        switch (key >> 2) {
        case 15: l2_slices = v; break;      /* l2_num_l2_slices */
        case 25: shader_present = v; break; /* raw_shader_present */
        case 29: l2_features = v; break;    /* raw_l2_features */
        case 55: gpu_id = v; break;         /* raw_gpu_id */
        }
    }
    free(buf);
    /* GPU_ID: 64-bit layout when bits 31:28 are all ones (newer GPUs), else 4-bit fields */
    if (((gpu_id >> 28) & 0xf) == 0xf) {
        h->arch = (unsigned)(gpu_id >> 56) & 0xff;
        h->product = (unsigned)(gpu_id >> 32) & 0xff;
    } else {
        h->arch = (unsigned)(gpu_id >> 28) & 0xf;
        h->product = (unsigned)(gpu_id >> 16) & 0xf;
    }
    h->ncores = (unsigned)__builtin_popcountll(shader_present);
    h->nl2 = (unsigned)l2_slices;
    /* L2_FEATURES bits 31:24: log2 of the external bus width in bits */
    unsigned lg = (unsigned)(l2_features >> 24) & 0xff;
    h->bus_bytes = lg >= 3 && lg < 12 ? (double)(1u << lg) / 8.0 : 16.0;
    return 0;
}

static const char *gpu_name(unsigned arch, unsigned product)
{
    if (arch == 12) return product == 0 ? "Mali-G720" : product == 1 ? "Mali-G620" : NULL;
    if (arch == 13) return product == 0 ? "G925/G725" : product == 1 ? "Mali-G625" : NULL;
    if (arch == 14) return product == 0 ? "Mali G1-Ultra" : product == 1 ? "Mali G1-Premium" : product == 3 ? "Mali G1-Pro" : NULL;
    return NULL;
}

/* KBASE_IOCTL_KINSTR_PRFCNT_ENUM_INFO: first the item count and size, then the items. */
static int enum_blocks(hw_state *h)
{
    struct kb_enum_info ei = {0};
    if (xioctl(h->dev, KB_PRFCNT_ENUM_INFO, &ei) < 0) return -errno;
    if (ei.item_size < sizeof(struct pc_enum_item) || !ei.item_count || ei.item_count > 64) return -EINVAL;
    uint8_t *items = calloc(ei.item_count, ei.item_size);
    if (!items) return -ENOMEM;
    ei.list = (uint64_t)(uintptr_t)items;
    if (xioctl(h->dev, KB_PRFCNT_ENUM_INFO, &ei) < 0) {
        int e = errno;
        free(items);
        return -e;
    }
    int requests_ok = 0, values_seen = 0;
    for (uint32_t i = 0; i < ei.item_count; i++) {
        const struct pc_enum_item *it = (const struct pc_enum_item *)(items + (size_t)i * ei.item_size);
        if (it->hdr.type == PC_ENUM_BLOCK) {
            if (!it->u.block.instances && !it->u.block.values) break; /* the end */
            if (it->u.block.type >= PC_BLK_TYPES) continue;
            h->has[it->u.block.type] = (uint8_t)(it->u.block.instances > 255 ? 255 : it->u.block.instances);
            if (it->u.block.type == PC_BLK_MEMORY) h->nl2 = it->u.block.instances; /* one per L2 slice */
            if (it->u.block.type == PC_BLK_SC) h->nsc_blocks = it->u.block.instances;
            if (!values_seen) {
                h->nvalues = it->u.block.values;
                h->set = it->u.block.set;
                values_seen = 1;
            }
        } else if (it->hdr.type == PC_ENUM_REQUEST) {
            if (it->u.request.type <= 1 && (it->u.request.versions & (1u << PC_API_VERSION))) requests_ok |= 1 << it->u.request.type;
        }
    }
    free(items);
    if (requests_ok != 3) return -ENOTSUP; /* mode and enable requests of our version */
    if (!h->has[PC_BLK_FE] || !h->has[PC_BLK_SC] || !h->has[PC_BLK_MEMORY] || !h->has[PC_BLK_TILER]) return -ENODEV;
    if (!h->nvalues || h->nvalues > MAX_VALUES) return -ERANGE;
    return 0;
}

/* KBASE_IOCTL_KINSTR_PRFCNT_SETUP: manual sampling of every counter in the four block types we
 * read; the reader file's mapping holds the samples. */
static int open_reader(hw_state *h)
{
    struct pc_request req[6];
    memset(req, 0, sizeof(req));
    int n = 0;
    req[n].hdr.type = PC_REQ_MODE;
    req[n].u.mode.mode = 0;
    n++;
    static const uint8_t blocks[] = {PC_BLK_FE, PC_BLK_TILER, PC_BLK_MEMORY, PC_BLK_SC};
    for (size_t i = 0; i < sizeof(blocks); i++) {
        req[n].hdr.type = PC_REQ_ENABLE;
        req[n].u.enable.type = blocks[i];
        req[n].u.enable.set = h->set;
        req[n].u.enable.mask[0] = req[n].u.enable.mask[1] = ~0ull;
        n++;
    }
    n++; /* the zeroed end item */
    union kb_setup s;
    memset(&s, 0, sizeof(s));
    s.in.count = (uint32_t)n;
    s.in.size = sizeof(req[0]);
    s.in.requests = (uint64_t)(uintptr_t)req;
    int fd = xioctl(h->dev, KB_PRFCNT_SETUP, &s);
    if (fd < 0) return -errno;
    h->md_size = s.out.md_item_size;
    h->map_size = s.out.mmap_size;
    if (h->md_size < sizeof(struct pc_meta) || !h->map_size) {
        close(fd);
        return -EINVAL;
    }
    void *m = mmap(NULL, h->map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        int e = errno;
        close(fd);
        return -e;
    }
    h->map = m;
    h->rd = fd;
    return 0;
}

static int cmd(hw_state *h, uint16_t c, uint64_t user)
{
    struct pc_cmd x = {.cmd = c, .user_data = user};
    return xioctl(h->rd, PC_ISSUE, &x) < 0 ? -errno : 0;
}

/* One synchronous sample: summed per block type into h->sum. */
static int take_sample(hw_state *h, uint64_t seq)
{
    int r = cmd(h, PC_CMD_SAMPLE_SYNC, seq);
    if (r) return r;
    struct pc_access a = {0};
    if (xioctl(h->rd, PC_GET_SAMPLE, &a) < 0) return -errno;
    memset(h->sum, 0, sizeof(h->sum));
    h->top_cycles = 0;
    h->flags = 0;
    static const int8_t to_hb[PC_BLK_TYPES] = {HB_FE, HB_TILER, HB_MEM, HB_SC, -1, -1, -1};
    size_t bytes = (size_t)h->nvalues * sizeof(uint64_t);
    for (size_t off = a.offset; off + h->md_size <= h->map_size; off += h->md_size) {
        const struct pc_meta *m = (const struct pc_meta *)(h->map + off);
        if (m->hdr.type == PC_NONE) break;
        if (m->hdr.type == PC_META_SAMPLE) {
            h->ts_start = m->u.sample.ts_start;
            h->ts_end = m->u.sample.ts_end;
            h->flags = m->u.sample.flags;
        } else if (m->hdr.type == PC_META_CLOCK) {
            if (m->u.clock.domains) h->top_cycles = m->u.clock.cycles[0];
        } else if (m->hdr.type == PC_META_BLOCK) {
            int hb = m->u.block.type < PC_BLK_TYPES ? to_hb[m->u.block.type] : -1;
            if (hb < 0 || (size_t)m->u.block.values_offset + bytes > h->map_size) continue;
            const uint64_t *v = (const uint64_t *)(h->map + m->u.block.values_offset);
            for (uint32_t i = 4; i < h->nvalues; i++) h->sum[hb][i] += (double)v[i]; /* 0-3: block header */
        }
    }
    if (xioctl(h->rd, PC_PUT_SAMPLE, &a) < 0) return -errno;
    return 0;
}

static double V(const hw_state *h, int c)
{
    int i = ctr[c].idx[h->col];
    if (i < 0 || (uint32_t)i >= h->nvalues) return NAN;
    return ldexp(h->sum[ctr[c].blk][i], ctr[c].shift);
}

static double pct(double a, double b) { return b > 0 && !isnan(a) ? 100.0 * a / b : NAN; }
static double max2(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a > b ? a : b; }

/* "%.0f" that says "-" for a figure this GPU has no counter for */
static const char *num(char *buf, size_t cap, const char *fmt, double v)
{
    if (isnan(v) || isinf(v)) return "-";
    snprintf(buf, cap, fmt, v);
    return buf;
}
#define N0(v) num((char[24]){0}, 24, "%.0f", (v))
#define N1(v) num((char[24]){0}, 24, "%.1f", (v))
#define N2(v) num((char[24]){0}, 24, "%.2f", (v))

static void report(hw_state *h)
{
    double secs = h->ts_end > h->ts_start ? (h->ts_end - h->ts_start) / 1e9 : PERIOD_S;
    double act = V(h, C_GPU_ACTIVE);
    if (!(act > 0)) {
        VKB_INFO("gpu counters %.1fs (%s): GPU idle", secs, h->gpu);
        return;
    }
    /* frames in this window: the busiest client's present rate over its own last interval */
    double frames = NAN;
    uint64_t presents = 0;
    double t_last = 0;
    if (vkb_report_frames(&presents, &t_last)) {
        /* the line is re-read every 5 s, not in step with this window: a rate between changes,
         * kept while it does not change; three windows without a new present: no frames */
        if (h->have_frames && presents > h->presents && t_last > h->t_last) {
            h->fps = (presents - h->presents) / (t_last - h->t_last);
            h->stale = 0;
        } else if (h->have_frames && ++h->stale >= 3)
            h->fps = 0;
        if (presents != h->presents || !h->have_frames) {
            h->presents = presents;
            h->t_last = t_last;
        }
        h->have_frames = 1;
        if (h->fps >= 0) frames = h->fps * secs;
    }
    double fps = frames / secs;
    int per_frame = frames >= 1;
    double clock_hz = h->top_cycles ? h->top_cycles / secs : NAN;
    double busy = h->top_cycles ? act / h->top_cycles : NAN;
    double gpu_ms = !isnan(clock_hz) ? act / clock_hz * 1e3 : NAN;

    /* queues: work queued minus the cycles it waited for a free endpoint */
    double q_frag = pct(V(h, C_ITER_FRAG_ACTIVE) - V(h, C_ITER_FRAG_READY_BLOCKED), act);
    double q_vert = pct(V(h, C_ITER_TILER_ACTIVE) - V(h, C_ITER_TILER_READY_BLOCKED), act);
    double q_comp = pct(V(h, C_ITER_COMP_ACTIVE) - V(h, C_ITER_COMP_READY_BLOCKED), act);
    double tiler = pct(V(h, C_TILER_ACTIVE), act);

    /* shader cores */
    double sc_any = V(h, C_SHADER_CORE_ACTIVE), exec = V(h, C_EXEC_CORE_ACTIVE);
    double sc_busy = pct(sc_any, act * h->ncores);
    double sc_frag = pct(V(h, C_FRAG_ACTIVE), sc_any), sc_comp = pct(V(h, C_COMPUTE_ACTIVE), sc_any);
    double sc_exec = pct(exec, sc_any);
    double fma = V(h, C_EXEC_INSTR_FMA), cvt = V(h, C_EXEC_INSTR_CVT), sfu = V(h, C_EXEC_INSTR_SFU);
    double alu_cy;
    if (h->col == 1) { /* two issue slots */
        double s1 = V(h, C_EXEC_INSTR_SLOT_1), s0 = fma + cvt + sfu - s1;
        alu_cy = max2(max2(s0, s1), sfu * 4);
    } else {
        double other = cvt + sfu;
        alu_cy = max2(other + (fma - (fma < other ? fma : other)) / 2, sfu * 4);
    }
    double tex_cy = V(h, C_TEX_FILT_NUM_OPERATIONS);
    static const int tex_parts[] = {C_TEX_TFCH_NUM_TCL_OPERATIONS, C_TEX_CFCH_NUM_DIRECT_PATH_OPERATIONS, C_TEX_CFCH_NUM_RP_OPERATIONS,
                                    C_TEX_MSGI_NUM_FLITS, C_TEX_CFCH_NUM_L1_CL_OPERATIONS, C_TEX_CFCH_NUM_L1_CT_OPERATIONS,
                                    C_TEX_TIDX_NUM_OPERATIONS};
    for (size_t i = 0; i < sizeof(tex_parts) / sizeof(tex_parts[0]); i++) tex_cy = max2(tex_cy, V(h, tex_parts[i]));
    double ls_cy = V(h, C_LS_MEM_READ_FULL) + V(h, C_LS_MEM_READ_SHORT) + V(h, C_LS_MEM_WRITE_FULL) + V(h, C_LS_MEM_WRITE_SHORT) +
                   V(h, C_LS_MEM_ATOMIC);
    double var_cy = (V(h, C_VARY_SLOT_32) + V(h, C_VARY_SLOT_16)) / VAR_ISSUE_PER_CY;
    double u_alu = pct(alu_cy, exec), u_tex = pct(tex_cy, exec), u_ls = pct(ls_cy, exec), u_var = pct(var_cy, exec);
    double starve = pct(V(h, C_EXEC_STARVE_ARITH), exec);
    double bp_tex = pct(V(h, C_EXEC_MSG_STALLED_TEX), exec), bp_ls = pct(V(h, C_EXEC_MSG_STALLED_LSC), exec);
    double bp_var = pct(V(h, C_EXEC_MSG_STALLED_VARY), exec), bp_blend = pct(V(h, C_EXEC_MSG_STALLED_BLEND), exec);
    double bp_zs = pct(V(h, C_EXEC_MSG_STALLED_ZS), exec);

    /* external memory (all L2 slices) */
    double rd = V(h, C_L2_EXT_READ_BEATS) * h->bus_bytes, wr = V(h, C_L2_EXT_WRITE_BEATS) * h->bus_bytes;
    double nl2 = h->nl2 ? h->nl2 : 1;
    double st_rd = pct(V(h, C_L2_EXT_AR_STALL), act * nl2), st_wr = pct(V(h, C_L2_EXT_W_STALL), act * nl2);
    double miss_rd = pct(V(h, C_L2_EXT_READ), V(h, C_L2_READ_LOOKUP)), miss_wr = pct(V(h, C_L2_EXT_WRITE), V(h, C_L2_WRITE_LOOKUP));
    double tex_ext = V(h, C_BEATS_RD_TEX_EXT) * SC_BUS_BYTES, ls_ext = V(h, C_BEATS_RD_LSC_EXT) * SC_BUS_BYTES;
    double ftc_ext = V(h, C_BEATS_RD_FTC_EXT) * SC_BUS_BYTES, tile_wr = V(h, C_BEATS_WR_TIB) * SC_BUS_BYTES;

    /* fragments */
    double px = V(h, C_ITER_FRAG_TASK_COMPLETED) * MAIN_TASK_PX * MAIN_TASK_PX;
    double threads = V(h, C_FRAG_SHADER_THREADS);
    double quads = V(h, C_FRAG_QUADS_RAST), shaded = V(h, C_FRAG_WARPS) * 16 / 4;
    double ezs_kill = pct(V(h, C_FRAG_QUADS_EZS_KILL), quads), pre_kill = pct(V(h, C_FRAG_QUADS_HSR_BUF_KILLED), quads);
    double lzs_kill = pct(V(h, C_FRAG_LZS_KILL), quads), shaded_pc = pct(shaded, quads);
    double occl = pct(V(h, C_QUAD_FPK_KILLER), quads);
    double te = pct(V(h, C_FRAG_TRANS_ELIM), 4 * V(h, C_FRAG_PTILES));

    /* geometry */
    double vis = V(h, C_PRIM_VISIBLE), c_face = V(h, C_PRIM_FACE_CULLED), c_frus = V(h, C_PRIM_FRUSTUM_CULLED);
    double c_samp = V(h, C_PRIM_SAMPLE_CULLED), c_sciss = V(h, C_PRIM_SCISSOR_CULLED);
    double prims = vis + c_face + c_frus + c_samp + c_sciss;
    double div = per_frame ? frames : secs; /* per frame, or per second without frame counts */
    const char *per = per_frame ? "frame" : "s";

    char fl[64] = "";
    if (h->flags & 1) snprintf(fl, sizeof(fl), " [counters overflowed]");
    if (h->flags & (1u << 30)) snprintf(fl, sizeof(fl), " [sample error]");
    VKB_INFO("gpu counters %.1fs (%s MC%u, %u L2 slices, %.0f-byte bus)%s: GPU active %s Mcy/s = %s%% of its clock (avg %s MHz); "
             "%s fps -> per frame %s ms GPU active",
             secs, h->gpu, h->ncores, h->nl2, h->bus_bytes, fl, N0(act / secs / 1e6), N0(busy * 100), N0(clock_hz / 1e6),
             per_frame ? N1(fps) : "?", per_frame ? N1(gpu_ms / frames) : "-");
    VKB_INFO("  queues, %% of GPU active: fragment %s, vertex/tiler (binning) %s, compute %s; tiler unit %s",
             N0(q_frag), N0(q_vert), N0(q_comp), N0(tiler));
    VKB_INFO("  shader cores: busy %s%% of GPU active (of that: fragment %s%%, compute/binning %s%%), executing %s%%; "
             "unit issue %% of executing: ALU %s, texture %s, load/store %s, varying %s; issue starved %s%%; "
             "backpressure %%: texture %s, load/store %s, varying %s, blend %s, ZS %s",
             N0(sc_busy), N0(sc_frag), N0(sc_comp), N0(sc_exec), N0(u_alu), N0(u_tex), N0(u_ls), N0(u_var), N0(starve), N0(bp_tex),
             N0(bp_ls), N0(bp_var), N0(bp_blend), N0(bp_zs));
    VKB_INFO("  external memory: read %s GB/s (%s MB/%s), write %s GB/s (%s MB/%s); stalled %% of GPU active: read %s, write %s; "
             "L2 miss %%: read %s, write %s; read from DRAM by: texture %s, load/store %s, fragment front end %s MB/%s; "
             "tile writes %s MB/%s",
             N2(rd / secs / 1e9), N0(rd / div / 1e6), per, N2(wr / secs / 1e9), N0(wr / div / 1e6), per, N0(st_rd), N0(st_wr),
             N0(miss_rd), N0(miss_wr), N0(tex_ext / div / 1e6), N0(ls_ext / div / 1e6), N0(ftc_ext / div / 1e6), per,
             N0(tile_wr / div / 1e6), per);
    VKB_INFO("  geometry per %s: %s k triangles in, %s k primitives into the tiler, culled %%: facing %s, frustum %s, "
             "sample %s, scissor %s; visible %s k",
             per, N0(V(h, C_TRIANGLES) / div / 1e3), N0(prims / div / 1e3), N0(pct(c_face, prims)), N0(pct(c_frus, prims)),
             N0(pct(c_samp, prims)), N0(pct(c_sciss, prims)), N0(vis / div / 1e3));
    VKB_INFO("  fragments per %s: %s k pixels, %s fragments/pixel; %s k quads rasterized: early-ZS killed %s%%, prepass killed "
             "%s%%, late-ZS killed %s%%, shaded %s%%, occluders %s%%; unchanged tiles %s%%",
             per, N0(px / div / 1e3), N2(px > 0 ? threads / px : NAN), N0(quads / div / 1e3), N0(ezs_kill), N0(pre_kill), N0(lzs_kill),
             N0(shaded_pc), N0(occl), N0(te));

    /* the verdict */
    char v[400];
    int len = 0;
    if (isnan(busy)) len += snprintf(v + len, sizeof(v) - len, "GPU load unknown");
    else if (busy >= 0.9) len += snprintf(v + len, sizeof(v) - len, "GPU-bound (%.0f%% busy)", busy * 100);
    else if (busy >= 0.7) len += snprintf(v + len, sizeof(v) - len, "mostly GPU-bound (%.0f%% busy)", busy * 100);
    else len += snprintf(v + len, sizeof(v) - len, "GPU not saturated (%.0f%% busy: CPU, sync or present waits)", busy * 100);
    const char *stage = "fragment";
    double sq = isnan(q_frag) ? 0 : q_frag;
    if (q_vert > sq) stage = "vertex/tiler", sq = q_vert;
    if (q_comp > sq) stage = "compute", sq = q_comp;
    len += snprintf(v + len, sizeof(v) - len, ", %s-heavy (%.0f%% of active)", stage, sq);
    const char *unit = "ALU";
    double uu = isnan(u_alu) ? 0 : u_alu;
    if (u_tex > uu) unit = "texture", uu = u_tex;
    if (u_ls > uu) unit = "load/store", uu = u_ls;
    if (u_var > uu) unit = "varying", uu = u_var;
    double st = max2(st_rd, st_wr);
    if (uu >= 70) len += snprintf(v + len, sizeof(v) - len, ", %s-limited (%.0f%% issue)", unit, uu);
    else if (st >= 15) len += snprintf(v + len, sizeof(v) - len, ", external-memory-bound (bus stalled %.0f%%, %.1f GB/s)", st, (rd + wr) / secs / 1e9);
    else if (sc_busy < 60) len += snprintf(v + len, sizeof(v) - len, ", shader cores underfed (busy %.0f%%: front end, tiler or serialization)", sc_busy);
    else len += snprintf(v + len, sizeof(v) - len, ", no unit saturated (top: %s %.0f%%; latency-bound: memory waits or dependencies)", unit, uu);
    if (st >= 15 && uu >= 70) len += snprintf(v + len, sizeof(v) - len, ", bus stalled %.0f%%", st);
    if (tiler >= 70) len += snprintf(v + len, sizeof(v) - len, ", tiler busy %.0f%%", tiler);
    if (px > 0 && threads / px >= 2.5) len += snprintf(v + len, sizeof(v) - len, ", overdraw %.1f", threads / px);
    VKB_INFO("  verdict: %s", v);
}

static void *hw_main(void *arg)
{
    hw_state *h = arg;
    uint64_t seq = 1;
    int fails = 0;
    for (;;) {
        struct timespec ts = {PERIOD_S, 0};
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
        int r = take_sample(h, ++seq);
        if (r) {
            if (++fails == 1) VKB_INFO("gpu counters: a sample failed (%s)%s", strerror(-r), r == -EBUSY ? ", discarding queued samples" : "");
            if (r == -EBUSY) cmd(h, PC_CMD_DISCARD, 0);
            if (fails >= 3) {
                say_off("samples keep failing", -r);
                break;
            }
            continue;
        }
        fails = 0;
        report(h);
    }
    cmd(h, PC_CMD_STOP, 0);
    munmap((void *)h->map, h->map_size);
    close(h->rd);
    close(h->dev);
    free(h);
    return NULL;
}

static void start_once(void)
{
    hw_state *h = calloc(1, sizeof(*h));
    if (!h) return;
    h->rd = -1;
    h->fps = -1;
    h->dev = open("/dev/mali0", O_RDWR | O_CLOEXEC);
    if (h->dev < 0) {
        say_off("no /dev/mali0 (not a Mali kbase driver, or not allowed)", errno);
        free(h);
        return;
    }
    int r;
    /* the CSF handshake: send 0.0, the kernel answers with its version and keeps it */
    struct kb_version ver = {0, 0};
    if (xioctl(h->dev, KB_VERSION_CHECK_CSF, &ver) < 0 || ver.major != 1) {
        struct kb_version jm = {0, 0};
        if (xioctl(h->dev, KB_VERSION_CHECK_JM, &jm) == 0 && jm.major)
            say_off("a Job Manager Mali (pre-CSF): no counter map for it", 0);
        else
            say_off("the kbase version handshake failed", errno);
        goto fail;
    }
    h->uk_major = ver.major;
    h->uk_minor = ver.minor;
    if (ver.minor < 17) { /* kinstr_prfcnt before UK 1.17 needs work-arounds this file leaves out */
        char why[80];
        snprintf(why, sizeof(why), "kbase UK %u.%u is older than 1.17", ver.major, ver.minor);
        say_off(why, 0);
        goto fail;
    }
    r = read_props(h);
    if (r == -EPERM) { /* older kbase: a set-up context first (one that never submits work) */
        struct kb_flags f = {KB_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED};
        xioctl(h->dev, KB_SET_FLAGS, &f);
        r = read_props(h);
    }
    if (r) {
        say_off("reading the GPU properties failed", -r);
        goto fail;
    }
    const char *name = gpu_name(h->arch, h->product);
    if (!name) {
        char why[120];
        snprintf(why, sizeof(why), "no counter map for this GPU (arch %u, product %u) yet", h->arch, h->product);
        say_off(why, 0);
        goto fail;
    }
    snprintf(h->gpu, sizeof(h->gpu), "%s", name);
    h->col = h->arch >= 13;
    r = enum_blocks(h);
    if (r == -EPERM) {
        struct kb_flags f = {KB_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED};
        xioctl(h->dev, KB_SET_FLAGS, &f);
        r = enum_blocks(h);
    }
    if (r) {
        say_off(r == -ENOTSUP ? "the kernel's kinstr_prfcnt speaks another version" : "enumerating the counter blocks failed", r == -ENOTSUP ? 0 : -r);
        goto fail;
    }
    r = open_reader(h);
    if (r) {
        say_off("setting up the counter reader failed", -r);
        goto fail;
    }
    r = cmd(h, PC_CMD_START, 1);
    if (r) {
        say_off("starting the counters failed", -r);
        munmap((void *)h->map, h->map_size);
        close(h->rd);
        goto fail;
    }
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, hw_main, h)) {
        pthread_attr_destroy(&at);
        say_off("no thread", errno);
        cmd(h, PC_CMD_STOP, 0);
        munmap((void *)h->map, h->map_size);
        close(h->rd);
        goto fail;
    }
    pthread_attr_destroy(&at);
    pthread_setname_np(th, "vkb-hwcnt");
    VKB_INFO("gpu counters on: %s, %u shader cores (%u blocks), %u L2 slices, kbase UK %u.%u kinstr_prfcnt, %u counters per block "
             "(set %u); summary every %d s",
             h->gpu, h->ncores, h->nsc_blocks, h->nl2, h->uk_major, h->uk_minor, h->nvalues, h->set, PERIOD_S);
    return;
fail:
    close(h->dev);
    free(h);
}

void vkb_hwcnt_start(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, start_once);
}
