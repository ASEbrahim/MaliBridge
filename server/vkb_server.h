/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * MaliBridge - server side (bionic on the device, glibc for host tests).
 *
 * The server owns the real Vulkan objects. Linux programs in the session talk to it through the
 * client ICD (client/) over a Unix socket; every connection is one client thread.
 */
#ifndef VKB_SERVER_H
#define VKB_SERVER_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "vkb_server_iface.h"
#include "vkb_proto.h"
#include "vkb_log.h"
#include "gen/vkb_server_gen.h"

#define VKB_MAX_TABLES 65536
#define VKB_MAX_PDS 16

/* A client process (all connections that said hello with the same token). */
#define VKB_SEQ_RING 65536

typedef struct vkb_srv_proc {
    uint64_t token;
    int pid;
    int conns;
    char comm[32];                 /* the client's process name (per-program pipeline caches) */
    char pkey[48];                 /* the client's pipeline cache key ("name@key" in the hello), or "" */
    struct vkb_preset *preset;     /* server_presets.c: the game's preset (by Steam app id), or NULL */
    int hang_arm;                  /* server_hang.c: the fault-armed hang detector for this launch: 0 not looked, 1 on, 2 off */
    struct vkb_srv_proc *next;
    /* Ordering of the process's asynchronous requests (vkb_msg_hdr.seq/barrier). */
    pthread_mutex_t olock;
    pthread_cond_t ocond;
    uint64_t watermark;            /* every async seq <= this has run */
    uint8_t done[VKB_SEQ_RING];    /* seqs above the watermark that have run */
    int conn_closed;               /* a connection ended (a seq may never arrive) */
    int owaiters;                  /* threads in order_wait */
    struct vkb_hitch *hitch;       /* server_hitch.c: what the process's frames spent their time on (VKBRIDGE_HITCH_MS), or NULL */
    uint32_t heap_free_mb;         /* server_presets.c: the default heap cap from the memory free at its first query (MB + 1; 0: not yet; atomic) */
    int start_short;               /* server_presets.c: it started with less memory free than the default heap cap (atomic; the bridge's
                                    * VK_EXT_memory_budget is offered to it: vkb_emu_membudget_for) */
    /* server_conn.c: the swapchain's own fence waits (VKB_F_PRESENT_WAIT): time waiting for the process's earlier
     * asynchronous requests, time in the handler (the driver's wait), how many (atomics; server_perf.c reads them) */
    uint64_t pw_order_ns, pw_srv_ns, pw_n;
    /* server_conn.c vkb_srv_count_work: the GPU work the process recorded and sent, counted as each request runs
     * (render pass instances begun, buffer-to-image copies, vkCreateBuffer, queue submits; atomics; server_perf.c
     * prints this window's share on the perf line) */
    uint64_t wk_rpass, wk_b2i, wk_bufs, wk_submits;
    int fast_create;               /* VKBRIDGE_FAST_CREATE as told at its first vkbQueryServer: 0 not yet, 1 off, 2 on (atomic) */
    int hk_slot;                   /* server_heapkind.c: its slot + 1 in the by-kind memory table (0: not yet; atomic) */
    uint64_t hud_last_ns;          /* server_syncfd.c: Steam's performance overlay (mangoapp): when its last frame's acquire was
                                    * signalled (VKBRIDGE_HUD_FPS; atomic) */
    uint64_t hud_frames, hud_paced, hud_paced_ns; /* its acquires, how many waited for the cap, and how long in all (atomics) */
} vkb_srv_proc;

/* The last commands of a replayed stream whose ids a connection keeps for other threads to read (conn->replay_ring). */
#define VKB_REPLAY_RING 16

typedef struct vkb_srv_conn {
    int sock;
    vkb_srv_proc *proc;
    /* Receive buffer: messages are parsed out of it in place (srv_recv). */
    uint8_t *rb;
    size_t rcap, rpos, rlen;
    int fdq[2 * VKB_MAX_FDS]; /* descriptors received ahead of their message's turn */
    int fdn;
    vkb_arena arena;
    pthread_t thread;
    uint32_t running;     /* the request it is running, command id + 1 (0: none; atomic, read by order_stall) */
    uint64_t running_ns;  /* since when (CLOCK_MONOTONIC_COARSE) */
    /* vkEndCommandBuffer replays the recorded stream and then calls the driver's own End. Where it is, for the
     * stall lines and the report (other threads read these; relaxed atomics, a stale value is fine): replay_pos 0 =
     * no replay, 1..replay_total = the command being replayed (1-based), replay_total + 1 = the driver's End;
     * replay_ring holds the command ids by position, so a reader can name the last few. */
    uint32_t replay_pos, replay_total;
    uint16_t replay_ring[VKB_REPLAY_RING];
    int tid; /* the thread's kernel id, for the stall probe (/proc/self/task/<tid>) */
    /* What its thread is doing, for order_wait's gap check (atomic): VKB_CONN_IDLE waiting for a message with none
     * begun, VKB_CONN_ORDER in order_wait, VKB_CONN_BUSY anything else (a message partly read, run, answered). */
    uint32_t phase;
} vkb_srv_conn;
enum { VKB_CONN_IDLE, VKB_CONN_ORDER, VKB_CONN_BUSY, VKB_CONN_PARKED };

/* Startup knowledge about each physical device (keyed by deviceUUID). */
typedef struct vkb_pd_knowledge {
    uint8_t uuid[VK_UUID_SIZE];
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    vkb_server_pd_info info;      /* in force: the mode's (vkb_pds_select_mode) */
    vkb_server_pd_info info_mode[2]; /* per VKB_MODE_*: the probe's decisions in each mode */
    int per_mode;                 /* info_mode filled (the probe ran) */
    int valid;
} vkb_pd_knowledge;

/* A tracked device memory allocation. */
typedef struct vkb_srv_mem {
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint32_t type;
    uint32_t strategy;
    void *host_ptr;          /* hostptr: server's mapping of the client's memfd */
    size_t host_len;
    void *mapped;            /* server-side vkMapMemory pointer (for flush/invalidate) */
    void *ahb;               /* AHardwareBuffer* (Android) */
    int export_fd1;          /* VKB_EMU_DMABUF_EXPORT: the dma-buf fd + 1 handed out as the export (0: none) */
    int sync_fd1;            /* cached readback: the shared dma-buf + 1, for DMA_BUF_IOCTL_SYNC (0: not cached) */
    struct dab_entry *dab;   /* VKB_EMU_DMABUF_AHB: the AHardwareBuffer pages exported or imported (server_dmabuf_ahb.c) */
    uint8_t stat_cls, stat_ded; /* accounting (server_memory.c): strategy class, dedicated to an image (1) / buffer (2) */
    int stat_pid;
    struct vkb_srv_mem *next;
} vkb_srv_mem;

struct vkb_emu_device;

typedef struct vkb_srv_table {
    uint32_t id;
    int is_device;
    int zink;                                /* instance: its engine is Mesa's Zink (VKB_EMU_BGR10_VTX) */
    int vkd3d;                               /* instance: its engine is vkd3d-proton (server_emu_sparse.c) */
    int dxvk;                                /* instance: its engine is DXVK (VKB_EMU_MEM_BUDGET) */
    VkPhysicalDevice membudget_pd;           /* instance: the device whose VK_EXT_memory_budget the server answers (server_memory.c) */
    int promote_ui;                          /* instance: gamescope's - Vulkan promotion even where untested (server_promote.c) */
    vkb_srv_proc *proc;
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physical;               /* device tables */
    struct vkb_srv_table *parent;            /* device -> instance */
    vkb_dispatch dt;                         /* what handlers call (may hold emulation wrappers) */
    vkb_dispatch real;                       /* the driver's own entry points */
    uint32_t api_version;
    const vkb_pd_knowledge *pd;
    pthread_mutex_t lock;
    vkb_srv_mem *mems[256];                  /* hash by handle */
    struct vkb_emu_device *emu;
    struct vkb_pcache *pcache;               /* server_pcache.c */
    struct vkb_dsmap *dsmap;                 /* server_dsmap.c */
    struct vkb_spvfix *spvfix;               /* server_spvfix.c */
    struct vkb_nulldesc *nulldesc;           /* server_nulldesc.c */
    struct vkb_bvmap *bvmap;                 /* server_bvmap.c */
    struct vkb_errata_dev *errata;           /* server_errata.c */
    uint32_t mem_types_seen;                 /* memory types this device has allocated in (logged once each) */
    struct vkb_report_dev *report;           /* server_report.c */
    struct vkb_async *async;                 /* server_async.c */
    struct vkb_vidyn *vidyn;                 /* server_vidyn.c */
    struct vkb_timeline *timeline;           /* server_timeline.c (VKB_EMU_TIMELINE) */
    struct vkb_dab_dev *dab;                 /* server_dmabuf_ahb.c (VKB_EMU_DMABUF_AHB) */
    PFN_vkGetPhysicalDeviceFeatures2 vidyn_f2; /* instance: the feature query beneath server_vidyn.c's */
    /* instance: the queries beneath gs_hide's (server_errata_gs.c), set only for its processes */
    PFN_vkGetPhysicalDeviceFeatures gs_f1;
    PFN_vkGetPhysicalDeviceFeatures2 gs_f2;
    PFN_vkGetPhysicalDeviceProperties gs_p1;
    PFN_vkGetPhysicalDeviceProperties2 gs_p2;
    /* instance: pv_zink_name (PV-08, server_errata_zink.c) for an instance made under the neutral engine name: the
     * queries beneath its wrappers, which hide again what PowerVR's "mesa zink" profile hid, vertexInputDynamicState aside */
    int zink_renamed;
    PFN_vkGetPhysicalDeviceFeatures zk_f1;
    PFN_vkGetPhysicalDeviceFeatures2 zk_f2;
    PFN_vkEnumerateDeviceExtensionProperties zk_ext;
    /* device: VK_EXT_device_fault for the loss record (server_lostlog.c): 1 enabled, 2 imitated (host tests), 0 not
     * offered (or off), -1 refused at vkCreateDevice (made without it), -2 listed without its deviceFault feature */
    int device_fault;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 pvm_f2; /* instance: the image-format query beneath pv_mutable_linear's (server_errata_pvmutable.c) */
} vkb_srv_table;

/* server_main.c */
extern PFN_vkGetInstanceProcAddr vkb_gipa;
extern vkb_dispatch vkb_global_dt;
extern int vkb_verbose;

void vkb_log_summary(void);

/* server_conn.c: the command buffer whose stream this thread is replaying (NULL otherwise), how many
 * of its commands have started, and the names of the last VKB_REPLAY_RING of them (for the crash
 * line). */
extern __thread VkCommandBuffer vkb_srv_replay_cb;
extern __thread uint32_t vkb_srv_replay_n;
extern __thread const char *vkb_srv_replay_ring[VKB_REPLAY_RING];
/* What the call this thread is making was asked to do, for the crash line (empty: nothing noted).
 * Cleared at each request. vkb_srv_note_pipeline_layout fills it before a pipeline layout reaches
 * the driver (a G710 r32p1 driver crashed inside vkCreatePipelineLayout with nothing more to go on). */
extern __thread char vkb_srv_crash_detail[512];
void vkb_srv_note_pipeline_layout(const VkPipelineLayoutCreateInfo *ci);
/* server_crashctx.c: records of the images, memory and graphics pipelines made last, and the crash
 * line's account of the dying call (cmd: its name) from the notes in common/vkb_crashctx.h. The
 * describe functions only read memory and format (the crash handler's). */
void vkb_crashctx_memory(VkDeviceMemory mem, uint64_t size, uint32_t type, uint64_t dedicated_image, const char *path);
int vkb_crashctx_describe(char *buf, size_t cap, const char *cmd);
int vkb_crashctx_where(char *buf, size_t cap, const void *addr); /* the faulting thread's name and what the address is in */
/* server_lost.c: a lost device stays one program's problem (a player log) */
void vkb_lost_device_init(vkb_srv_table *t);    /* beneath every layer: the latch (and its host imitation) */
void vkb_lost_device_destroy(vkb_srv_table *t);
void vkb_lost_mark(vkb_srv_table *t, const char *why); /* the bridge takes the device as lost (a contained bind fault) */
int vkb_lost_is(const vkb_srv_table *t);
int vkb_srv_instance_orphans(vkb_srv_table *inst); /* server_conn.c: an instance's devices left alive, cleaned up before it (0: one is kept) */
int vkb_lost_stopped(const vkb_srv_table *t);   /* a device (or an instance with one) never to be touched again */
int vkb_lost_refuse(const vkb_srv_table *t);    /* run_request: refuse the request */
void vkb_lost_stop(vkb_srv_table *t, const char *why); /* taken as lost and stopped: refused, never destroyed */
int vkb_lost_settle(vkb_srv_table *t);          /* before a device goes: its loss record's driver query done (0), or stuck: stopped (1) */
/* server_lostlog.c: what a lost device was given (the loss record, ledger 225) */
int vkb_ll_wanted(void);    /* not off (VKBRIDGE_LOST_REPORT=0) */
int vkb_ll_test_mode(void); /* VKBRIDGE_TEST_DEVICE_FAULT: 0 none, 1 va, 2 crash, 3 hang, 4 refuse */
void vkb_ll_cb_recorded(VkCommandBuffer cb, uint32_t cmds, size_t bytes, uint32_t draws, uint32_t disp, uint32_t copies,
                        const uint64_t *pipes, uint32_t npipes); /* server_conn.c: a client's stream ended */
void vkb_ll_pipeline(VkPipeline p, const uint64_t *h, const uint32_t *stg, uint32_t nh, int compute); /* server_guard.c */
int vkb_lost_async(const vkb_srv_table *t, const char *cmd); /* server_conn.c: an asynchronous call's lost answer on a lost device, counted (1) */
typedef struct vkb_srv_stuck_req {
    int pid;
    char comm[32];
    const char *cmd;
    double secs;
    char probe[300];
} vkb_srv_stuck_req;
int vkb_srv_stuck(double min_s, vkb_srv_stuck_req *out, int max); /* server_conn.c: requests stuck that wait by no design */
int vkb_srv_end_proc(int pid, const char *why);                   /* server_conn.c: a program's session ended, the driver untouched */
void vkb_lost_fault(int sig, const void *addr, uintptr_t pc, const char *lib, const char *lib_name, uintptr_t lib_off);
void vkb_crash_log_raw(const char *line, size_t n); /* server_main.c: a line from the crash handler */
int vkb_crash_frames(char *out, size_t cap, void *ucontext); /* server_crashsym.c: "crash-frame #NN pc ... (BuildId: ...)" lines */
void vkb_crashctx_test_init(vkb_srv_table *t); /* VKBRIDGE_TEST_CRASH_CMD (host tests) */
/* server_conn.c, for the crash line: the client this thread serves (NULL: none), and the calls
 * other connection threads are inside of ("" if none). */
const vkb_srv_proc *vkb_srv_crash_client(void);
int vkb_srv_crash_others(char *buf, size_t cap);
/* A call in the driver at a contained fault, copied inside the signal handler (server_guard.c): a request of a
 * connection thread (cmd = command id + 1) or a guarded compile on any other thread (cmd 0: the server's background workers). */
typedef struct vkb_crash_call {
    int tid, pid;           /* pid: the request's program (0 for a compile) */
    uint32_t cmd;           /* command id + 1; 0: a compile */
    uint32_t device;        /* a compile's device (table id) */
    uint64_t since_ns;      /* when it began: a request on CLOCK_MONOTONIC_COARSE (conn->running_ns), a compile on CLOCK_MONOTONIC */
    const char *call, *ctx; /* a compile: its call, and what its thread was running (vkb_srv_current_cmd), or NULL; static strings */
    char comm[32];          /* a request's program */
    unsigned char torn;     /* its marker changed while it was copied (the call ended or a new one began meanwhile) */
} vkb_crash_call;
/* server_conn.c: the connection threads' requests running now, other than thread `self`'s, into out[max] (*more: those past max).
 * Async-signal-safe: atomic loads and plain copies, no lock. */
int vkb_srv_crash_snap(vkb_crash_call *out, int max, int self, int *more);

/* server_guard.c */
void vkb_guard_device_init(vkb_srv_table *t);
void vkb_guard_device_destroy(vkb_srv_table *t);
int vkb_guard_mem_faults(const vkb_srv_table *t); /* memory faults contained on this device so far */
void vkb_guard_fault(int sig, void *addr, int code, uintptr_t pc, void *uc); /* crash handler: jumps back into a guarded driver call, if any */
/* crash handler, an uncontained fault: the pipeline compiles in flight become crash suspects for the next start (stuck-pipelines x=1) */
void vkb_guard_crash_suspects(int sig, const char *lib, int in_call);
/* fn(arg) armed like a driver compile (the startup probe): a fault in it, or a run past limit_s
 * (0: VKBRIDGE_COMPILE_TIMEOUT_S, < 0: none), jumps back; *sig: the fault's signal. */
enum { VKB_GUARD_DONE = 0, VKB_GUARD_CRASHED = 1, VKB_GUARD_TIMED_OUT = 2 };
int vkb_guard_run(const char *what, double limit_s, void (*fn)(void *), void *arg, int *sig);
int vkb_guard_armable(void); /* containment on and not used up */
void vkb_guard_watch_start(void); /* the compile watchdog's thread, started now if not yet (the switches read again) */
/* a fault or a time-out vkb_guard_run contained in a driver call of device dev: counted, and the memory fault
 * watchdog started (a call that then blocks on a lock the driver held ends its program) */
void vkb_guard_contained_elsewhere(const char *call, uint32_t dev);
/* One guard around every driver call a connection thread makes for its client (server_conn.c conn_main):
 * loop(arg) runs with a base arm; a fault outside any specific arm (compile, memory, End) jumps back and
 * vkb_guard_serve returns 1 with *f filled (loop is then called again by the caller after its recovery);
 * 0 when loop returned. Without containment (VKBRIDGE_NO_CRASH_GUARD=1, VKBRIDGE_REQ_GUARD=0) loop runs
 * unarmed. Armed once per thread: no work per request. */
typedef struct vkb_guard_caught {
    int sig, code;
    void *addr;
    uintptr_t pc;
    unsigned snap; /* server_guard.c: the calls in flight at the fault, copied in the handler (0: none) */
    uintptr_t ra[6]; /* the return addresses above the fault (frame records), newest first */
    int nra;
} vkb_guard_caught;
int vkb_guard_serve(void (*loop)(void *), void *arg, vkb_guard_caught *f);
/* After vkb_guard_serve caught a fault in a client's request: the CONTAINED line (call: the request or the
 * replayed command; where: its place in a replay, or NULL), the fault's detail, the device t stopped (taken
 * as lost, refused, never destroyed; t NULL or not a device: nothing stopped), the stall watchdog started.
 * Returns the number contained in the process. */
int vkb_guard_request_contained(vkb_srv_table *t, const char *call, const char *where, const vkb_guard_caught *f);
const uint32_t *vkb_guard_lut_standin(size_t *size); /* the compile watchdog's UE4 LUT stand-in */
/* The compile watchdog's interrupt in a mode (VKBRIDGE_COMPILE_TIMEOUT_S wins); why: its source */
double vkb_guard_timeout_s(int mode, const char **why);

/* server_probe.c: the startup capability probe (device profile in the cache directory). Runtime
 * flags its results set: */
#define VKB_PROBE_FLAG_LUT_NOW 0x1u     /* UE4 LUT pipelines get their stand-in at once (compiler spin) */
#define VKB_PROBE_FLAG_GS_FOLD 0x2u     /* layer-only geometry stages folded into the vertex shader */
#define VKB_PROBE_FLAG_SPIRV_FIX 0x4u   /* server_spvfix.c on whatever the vendor */
#define VKB_PROBE_FLAG_TX_ETC2 0x8u     /* BC transcoders that did not build: those formats decoded */
#define VKB_PROBE_FLAG_TX_EAC 0x10u
#define VKB_PROBE_FLAG_TX_ASTC 0x20u     /* BC2 / BC3 -> ASTC */
#define VKB_PROBE_FLAG_TX_ASTC_HDR 0x40u
#define VKB_PROBE_FLAG_TX_BC7 0x10000u   /* BC7 -> ASTC: its own (the biggest; BC2 and BC3 build where it does not). Was 0x8000, MK_FP16's bit: a died bc_transcoders read as MK-20 */
#define VKB_PROBE_FLAG_TX_ALL 0x10078u
/* BC converter variants the probe found wrong on this driver (bc_variants: did not build, hung, or a byte differed from
 * the old variant's output): one dispatch a run (REGION_TABLE) off, the palette search off for BC1 -> ETC2 / BC4 / BC5 -> EAC */
#define VKB_PROBE_FLAG_BC_TAB 0x40000u
#define VKB_PROBE_FLAG_BC_PAL_ETC2 0x80000u
#define VKB_PROBE_FLAG_BC_PAL_EAC 0x100000u
/* ... and VKBRIDGE_BC_FAST's BC1 -> ETC2 palette variants (the palette ones stay) */
#define VKB_PROBE_FLAG_BC_FAST_ETC2 0x400000u
/* ... and its BC7 -> ASTC variants (today's stay). 0x400000 and 0x1000000 stay the BC fast ones (test49p's bits); the PowerVR
 * line had them too (PV_NAMES, PV_V3D_ARRAY1) and moved in test50 (PV_NAMES 0x40000000, PV_V3D_ARRAY1 bit 32: 33 flags, so 64 bits) */
#define VKB_PROBE_FLAG_BC_FAST_ASTC 0x1000000u
#define VKB_PROBE_FLAG_BC_ALL 0x15c0000u
#define VKB_PROBE_FLAG_WAIT_LOST 0x20000u /* waits: a timeline wait that timed out answered VK_ERROR_DEVICE_LOST, the device
                                           * fine after (Arm job-manager drivers); server_wait.c checks such answers */
/* The MK probes (one per Mali driver erratum it can tell): the bug's own pattern was seen. Their
 * results reach the errata decisions (server_errata.c) through vkb_errata_set_probe_source. */
#define VKB_PROBE_FLAG_MK_WAIT 0x80u        /* wait_eintr: a wait failed or ended early after a signal (MK-01) */
#define VKB_PROBE_FLAG_MK_FLIP 0x100u       /* viewport_flip: a negative-height viewport drawn upside down (MK-02) */
#define VKB_PROBE_FLAG_MK_BITWISE 0x200u    /* bitwise_vec: wrong lanes from swizzle-fed vector bitwise ops (MK-03) */
#define VKB_PROBE_FLAG_MK_DYN 0x400u        /* dyn_state: dynamic state lost or stale across a pipeline bind (MK-04) */
#define VKB_PROBE_FLAG_MK_VB 0x800u         /* vb_strides: multi-binding strides applied by array index (MK-05) */
#define VKB_PROBE_FLAG_MK_VB_SINGLE 0x1000u /* vb_strides: single-binding calls wrong too - the split cannot fix it (option B) */
#define VKB_PROBE_FLAG_MK_CHURN 0x2000u     /* rendering_churn: a re-made view rendered with stale state (MK-06) */
#define VKB_PROBE_FLAG_MK_VIEW3D 0x4000u    /* view3d_layers: a 3D image's REMAINING slices view wrong (MK-07) */
#define VKB_PROBE_FLAG_MK_FP16 0x8000u      /* fp16_math: 16-bit float arithmetic gave wrong results (MK-20) */
#define VKB_PROBE_FLAG_MK_ALL 0xff80u
/* The PowerVR probes (PV-nn: Imagination's driver), used like the MK ones. 0x200000 is the first bit free on this line
 * (0x40000-0x100000 are the BC variants' here; feat/powervr-bridge used 0x80000 for PV-02) */
#define VKB_PROBE_FLAG_PV_MUTABLE 0x200000u /* pv_mutable_linear: a mutable-format LINEAR dma-buf image query refused though the image works (PV-02) */
/* PV-01: 0x40000000, the last 32-bit bit free (test50: 0x400000, PVR-ALL's pick, is VKBRIDGE_BC_FAST's BC1 -> ETC2 verdict on the
 * PERF line; feat/powervr-bridge had PV-01 at 0x40000, the BC table's bit on this line) */
#define VKB_PROBE_FLAG_PV_NAMES 0x40000000u /* pv_names: an input reads zero beside an output of the same OpName (PV-01) */
#define VKB_PROBE_FLAG_PV_BGRA_STORE 0x800000u /* pv_bgra_store: storage writes to a B8G8R8A8 image land in R8G8B8A8 byte order (PV-07) */
#define VKB_PROBE_FLAG_PV_ALL 0x40a00000u
/* view3d_layers on PowerVR (PV-04): the base layer of a 3D image's 2D-array view ignored with explicit counts too (the pattern),
 * and which forms of the fix the same run proved on the driver (the others are only set beside it). feat/powervr-bridge had the
 * pattern at 0x800000, pv_bgra_store's bit on this line; it is 0x8000000 here (PV-03's _STORAGE_READ bit there, not on this line) */
#define VKB_PROBE_FLAG_PV_VIEW3D 0x8000000u        /* the pattern: view_counts cannot fix it */
#define VKB_PROBE_FLAG_PV_V3D_ARRAY1 0x100000000ull /* a one-slice 2D-array view keeps its base layer (views of one slice need no fix) */
#define VKB_PROBE_FLAG_PV_V3D_2D1 0x2000000u       /* a 2D view of a slice keeps its base layer (a one-slice view is rendered through one) */
#define VKB_PROBE_FLAG_PV_V3D_COPY 0x4000000u      /* rendering through a 2D-array image of the bridge's own, copied in and out, is right */
#define VKB_PROBE_FLAG_PV_V3D_FB 0x10000000u       /* a feedback loop in a 3D image's multi-slice 2D-array view reads wrong where the same loop in a plain 2D-array
                                                    * image is right (sub-case 5 and 7; PowerVR CXTP 25.3): the second pattern of pv_view3d_base */
#define VKB_PROBE_FLAG_PV_V3D_FBCOPY 0x20000000u   /* ... and the loop through the copy form (a 2D-array image of the bridge's own, the pass split at the
                                                    * barrier) is right (sub-case 6) */
#define VKB_PROBE_FLAG_PV_V3D_ALL 0x13e000000ull
#define VKB_PROBE_FLAG_PV_FBLOOP 0x80000000u       /* view3d_layers sub-case 8 (pv_fbloop_split, PV-06; PowerVR CXTP 25.3, Pixel 11): a feedback loop in a plain
                                                    * 2D-array image reads wrong (sub-case 7) and the same loop with the pass ended at the barrier and begun again
                                                    * with LOAD, no copy, is right */
int vkb_probe_flag(uint64_t f);
/* The bc_variants probe on this server's GPU: 1 it ran (VKB_PROBE_FLAG_BC_* say what it turned off), 0 not yet (left to
 * the background part, or no profile yet), -1 it will not run in this process (the probe off, could not set up, skipped).
 * Lock-free: read on BC image creation. */
int vkb_probe_bc_variants(void);
/* BC fast converters on only on measured GPUs (G720, G925); the bc_variants probe checks their variants only then (or with VKBRIDGE_BC_FAST set); the rest of it is test47's */
int vkb_bc_gpu_measured(const char *name);
int vkb_bc_fast_gate(const char *deviceName, int *explicit_env);
void vkb_probe_device(vkb_pd_knowledge *k, VkInstance inst, const vkb_dispatch *idt, VkPhysicalDevice pd);
void vkb_probe_start_background(void); /* what did not fit the start's budget */
void vkb_probe_write_md(FILE *f);
void vkb_probe_write_json(FILE *f);
int vkb_probe_issues(void (*cb)(void *arg, const char *line), void *arg);
/* What a GPU's probe results decide in a mode (VKB_MODE_*), re-run on the raw results as recorded
 * (switching mode never re-probes): the emulation mask (from what the GPU had before the probe)
 * and the runtime flags. 0 when the GPU has no profile. For feat/safe-mode's per-mode masks. */
int vkb_probe_decide_mode(const uint8_t uuid[VK_UUID_SIZE], int mode, uint32_t *emu, uint64_t *flags);
/* How the process ends, for a probe in flight (async-signal-safe): a stop (SIGTERM, the app gone)
 * leaves it to be run again, a crash outside the probe's guard makes it died. */
void vkb_probe_mark_end(int sig, int crashed);
void vkb_probe_crash_marks(int all); /* 1: every fatal signal but SIGKILL reaches vkb_probe_mark_end */
void vkb_probe_clients_gone(int gone); /* 1: the last client process left (a kill now is the session's end) */

/* server_report.c: the session report (<log dir>/malibridge-report.md and .json) */
void vkb_report_session(const char *log_path); /* a session starts (its server log's path; NULL: none) */
int vkb_report_dir(char *out, size_t cap);      /* the session folder (the report's): 1 when known */
void vkb_report_log(int level, const char *fmt, const char *msg); /* every log line */
void vkb_report_hello(vkb_srv_proc *p);
void vkb_report_gone(vkb_srv_proc *p);
void vkb_report_instance(vkb_srv_table *t, const VkInstanceCreateInfo *ci);
void vkb_report_device(vkb_srv_table *inst, const VkDeviceCreateInfo *ci, VkResult r, const char *dropped_features,
                       const char *not_passed_exts);
void vkb_report_device_init(vkb_srv_table *t); /* device-lost watch on submissions and waits */
void vkb_report_device_destroy(vkb_srv_table *t);
void vkb_report_lost_cause(int pid, const char *line); /* the loss record's line (server_lostlog.c): the first per process */
void vkb_report_features(const VkPhysicalDeviceFeatures *want, const VkPhysicalDeviceFeatures *have, char *out, size_t cap);
const char *vkb_srv_env_list(char *out, size_t cap); /* <cache dir>/env as applied (server_pcache.c) */
void vkb_report_compile(vkb_srv_table *t, int compute, uint32_t n, double ms, VkResult r, const char *slow_stages);
/* Pipelines the driver built with creation feedback, and how many of them it found in the pipeline cache (server_guard.c). */
void vkb_report_cache_hits(vkb_srv_table *t, uint64_t built, uint64_t hits);
int vkb_report_frames(uint64_t *presents, double *t_last); /* busiest live process's presents (frames line) */
/* server_heapkind.c: memory by kind of object (memkind:/memtop: lines). vkb_hk_cmd[cmd]: 0, or the command's kind
 * (bits 0-4) | 0x40 destroy/free | 0x80 measured; run_request brackets such a handler with begin/end. */
extern uint8_t vkb_hk_cmd[];
long vkb_hk_begin(uint8_t hk);
void vkb_hk_end(uint8_t hk, vkb_srv_proc *p, long before_kb);
long vkb_hk_anon_kb(void);
int vkb_hk_kind_line(char *buf, size_t cap, int force);
int vkb_hk_top_line(char *buf, size_t cap, int low_entered);
void vkb_hk_tick(int low_entered, int lines); /* the mem: thread, every 5 s: memkind/memtop lines when due */
double vkb_srv_running_longest(int pid, const char **name, char *detail, size_t cap); /* server_conn.c: the process's longest running request (s, 0: none); detail (may be NULL): where inside it (the replayed command, the thread's state) or "" */

/* server_async.c: graphics pipelines compiled in the background (VKBRIDGE_ASYNC_COMPILE) */
void vkb_async_device_init(vkb_srv_table *t);
void vkb_async_device_destroy(vkb_srv_table *t); /* first: its workers call every layer beneath */

/* server_promote.c: Vulkan 1.2 / 1.3 offered on a driver below them, from its extensions
 * (VKBRIDGE_PROMOTE_API; VKBRIDGE_FAKE_API=1.x takes the host driver as that version) */
void vkb_promote_decide(const vkb_dispatch *idt, VkPhysicalDevice pd); /* the self-test, per GPU */
void vkb_promote_instance_install(vkb_srv_table *t);
/* vkCreateDevice: core feature structures the driver lacks turned into the extensions', those
 * extensions enabled; returns the extension list (*n updated; may be a new array in a) */
const char **vkb_promote_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, VkDeviceCreateInfo *ci, const char **exts,
                                            uint32_t *n, vkb_arena *a);
PFN_vkGetDeviceProcAddr vkb_promote_gdpa(PFN_vkGetDeviceProcAddr real); /* VKBRIDGE_FAKE_API: core names a 1.1 driver lacks */
uint32_t vkb_promote_fake_api(void); /* VKBRIDGE_FAKE_API (host tests) as a version, 0: off */
void vkb_promote_device_init(vkb_srv_table *t);    /* before t->dt is made: SPIR-V lowered beneath every layer */
void vkb_promote_device_destroy(vkb_srv_table *t);
/* server_children.c: the driver's pools, pipelines, modules, layouts, caches and query pools per
 * device; destroy_left (a process gone without destroying its device): what is left is destroyed */
void vkb_children_device_init(vkb_srv_table *t);
void vkb_children_device_destroy(vkb_srv_table *t, int destroy_left);
void vkb_children_fake_init(vkb_srv_table *t); /* host tests: VKBRIDGE_FAKE_MISSING=pvrdestroy */
/* The version offered and the driver's for a GPU, and what the next version lacks; 0: not known */
int vkb_promote_describe(const uint8_t uuid[VK_UUID_SIZE], uint32_t *offered, uint32_t *drv, const char **missing);
void vkb_promote_write_apps_file(void);           /* <cache dir>/apps-vulkan, after the self-test */

/* server_spvfix.c */
void vkb_spvfix_device_init(vkb_srv_table *t);
void vkb_spvfix_device_destroy(vkb_srv_table *t);
/* host tests: VKBRIDGE_FAKE_MISSING=ycbcrcompute, beneath every layer (before the crash guard) */
void vkb_spvfix_fake_init(vkb_srv_table *t);
/* a compute pipeline the driver refused: the first is logged with its layout's YCbCr samplers */
void vkb_spvfix_compute_failed(vkb_srv_table *t, uint32_t n, const VkComputePipelineCreateInfo *ci, VkResult r);

/* server_pools.c */
void vkb_pools_device_init(vkb_srv_table *t);
void vkb_pools_device_destroy(vkb_srv_table *t);
uint64_t vkb_pools_overflow_live(void); /* overflow descriptor pools alive, every device */

/* server_trace.c */
void vkb_trace_device_init(vkb_srv_table *t);
int vkb_nocomp_wanted(void); /* VKBRIDGE_NO_COMPRESSION (experiment) */
void vkb_nocomp_device_init(vkb_srv_table *t, int enabled);
void vkb_quirks_device_init(vkb_srv_table *t); /* server_quirks.c */

/* server_errata.c: driver errata, fixed just above the driver */
enum {
    VKB_ERRATUM_WAIT_RETRY,      /* MK-01 waits fail with -3 after a signal */
    VKB_ERRATUM_VIEWPORT_FLIP,   /* MK-02 negative viewport height ignored */
    VKB_ERRATUM_BITWISE_SCALAR,  /* MK-03 integer vector bitwise op on a swizzle miscompiled */
    VKB_ERRATUM_DYN_REEMIT,      /* MK-04 dynamic state lost across pipeline binds */
    VKB_ERRATUM_VB_SPLIT,        /* MK-05 vkCmdBindVertexBuffers2 strides by array index */
    VKB_ERRATUM_VIEW_QUARANTINE, /* MK-06 vkCmdBeginRendering reuses stale view state */
    VKB_ERRATUM_VIEW_COUNTS,     /* MK-07 3D image as 2D array with VK_REMAINING_ARRAY_LAYERS */
    VKB_ERRATUM_CLAMP_ZERO,      /* MK-32 min/max/clamp with a zero limit on a constant-selected value */
    VKB_ERRATUM_CULL_NONTRI,     /* MK-16 vkCmdSetCullMode culls points and lines */
    VKB_ERRATUM_DEDICATED_IMPORT, /* MK-39 a dedicated-allocation struct on an import fails or crashes */
    VKB_ERRATUM_FP16_HIDE,       /* MK-20 16-bit float arithmetic wrong (Mali-G715 r54) */
    VKB_ERRATUM_STENCIL_LAYOUT,  /* MK-11 a separate stencil layout in a render pass ignored */
    VKB_ERRATUM_VB_BAKE,         /* MK-05 option B: dynamic strides wrong in single-binding calls too */
    VKB_ERRATUM_GS_HIDE,         /* r38p1 beta segfaults compiling Zink's geometry shaders */
    VKB_ERRATUM_SPLIT_VARYING,   /* MK-47 a fragment input at a Component > 0 of a whole-vector output reads wrong */
    VKB_ERRATUM_IMPORT_SERIAL,   /* MK-48 r44: an import beside another device's memory calls faults in the driver */
    VKB_ERRATUM_PV_MUTABLE_LINEAR, /* PV-02 PowerVR: the query for a mutable-format LINEAR dma-buf image says no, the image works */
    VKB_ERRATUM_PV_BGRA_STORE,   /* PV-07 PowerVR: storage writes to a B8G8R8A8 image land in R8G8B8A8 byte order */
    VKB_ERRATUM_PV_NAMES,        /* PV-01 PowerVR: a vertex input reads zero beside an output of its OpName */
    VKB_ERRATUM_PV_VIEW3D_BASE,  /* PV-04 PowerVR: a 3D image's 2D-array view renders from slice 0 whatever its base layer (explicit counts too) */
    VKB_ERRATUM_PV_FBLOOP_SPLIT, /* PV-06 PowerVR: an in-pass feedback loop (ATTACHMENT_FEEDBACK_LOOP_OPTIMAL, a barrier inside the pass) reads wrong; the pass is split at the barrier */
    VKB_ERRATUM_PV_ZINK_NAME,    /* PV-08: PowerVR's driver hides features from the engine name "mesa zink" */
    VKB_ERRATUM_FRAGDEPTH_NODEPTH, /* MK-49 G925 r49p1: a fragment shader writing FragDepth into a rendering with no depth attachment never finishes */
    VKB_ERRATUM_EXAMPLE,         /* a no-op, the pattern of a fix */
    VKB_ERRATA_COUNT
};
enum { VKB_ERRATA_PROBE_NOT_RUN, VKB_ERRATA_PROBE_PASS, VKB_ERRATA_PROBE_FAIL };
void vkb_errata_device_init(vkb_srv_table *t);
void vkb_errata_device_destroy(vkb_srv_table *t);
int vkb_errata_on(const vkb_srv_table *t, int id); /* VKB_ERRATUM_* decided on for this device */
const char *vkb_errata_name(int id);
void vkb_errata_set_probe_source(int (*fn)(const char *probe)); /* the probe's record: VKB_ERRATA_PROBE_* */
/* server_mode.c: the mode, safe (the default) or performance - VKBRIDGE_MODE in <cache dir>/env,
 * then in the environment, then <cache dir>/mode (Setup), then the default */
enum { VKB_MODE_SAFE, VKB_MODE_PERFORMANCE };
enum { VKB_MODE_SRC_DEFAULT, VKB_MODE_SRC_FILE, VKB_MODE_SRC_ENV, VKB_MODE_SRC_CACHE_ENV, VKB_MODE_SRC_PRESET_FILE, VKB_MODE_SRC_PRESET_BUILTIN };
int vkb_mode(void);         /* as last read (the first call reads it) */
int vkb_mode_refresh(void); /* read again (each vkbQueryServer and vkCreateDevice); logs a change */
int vkb_mode_source(void);  /* VKB_MODE_SRC_* */
const char *vkb_mode_name(int mode);
const char *vkb_mode_source_name(void); /* "Setup", "the default", ... */
const char *vkb_mode_summary(int mode);
/* Each MK erratum's decision in both modes ([VKB_MODE_*]) for a GPU and driver, with the reason,
 * as vkCreateDevice would make it now (the session report); returns the rows filled */
typedef struct vkb_errata_row {
    int id;
    const char *name, *mk, *probe;
    int probe_gates, probe_result, has_fix;
    int on[2];
    char why[2][320];
} vkb_errata_row;
int vkb_errata_explain(const VkPhysicalDeviceProperties *props, const char *driver_info, vkb_errata_row rows[VKB_ERRATA_COUNT]);
/* The startup probe of erratum id on its probe device: when VKBRIDGE_ERRATA_IMITATE names it (host
 * builds), its imitation goes beneath *d, which the probe then calls - the probe sees the bug an
 * affected driver has. Returns what vkb_errata_probe_end takes (NULL: nothing imitated). */
void *vkb_errata_probe_begin(int id, vkb_dispatch *d, VkDevice dev, VkPhysicalDevice pd, uint32_t api);
int vkb_wait_fake_lost(void); /* server_wait.c: VKBRIDGE_WAIT_FAKE_LOST (the imitation of VKB_PROBE_FLAG_WAIT_LOST's bug) */
void vkb_errata_probe_end(void *h);
/* FNV-1a of a module's SPIR-V as the app made it: a module an erratum rewrote is named by the
 * code it was rewritten from (server_errata_bitwise.c); what layers beneath the errata hash with */
uint64_t vkb_spv_app_hash(const uint32_t *code, size_t size);
int vkb_errata_parse_version(const char *s, uint32_t *r, uint32_t *p, int *beta); /* r<major>p<minor> in s */
const char *vkb_errata_hidden_ext(const vkb_srv_table *inst, VkPhysicalDevice pd); /* an erratum's hidden device extension */

/* server_vidyn.c: VK_EXT_vertex_input_dynamic_state for Zink. The mode on a physical device: off
 * (the client sees the driver's, or nothing), native (the driver's) or emulated (the server's). */
enum { VKB_VIDYN_OFF = 0, VKB_VIDYN_NATIVE = 1, VKB_VIDYN_EMULATED = 2 };
int vkb_vidyn_mode(const vkb_srv_table *inst, VkPhysicalDevice pd);
int vkb_vidyn_variants(const vkb_srv_table *inst, VkPhysicalDevice pd); /* the layer makes pipeline variants there */
/* Extended dynamic state 1 and 2 for Zink where the driver has no usable ones (Mali Vulkan 1.1
 * drivers; the A1 fix's): 1 when the server offers its own, made static in the vertex input's
 * pipeline variants (server_vidyn.c). vkb_vidyn_device_create_info's result has VKB_VIDYN_EDS then. */
int vkb_eds_mode(const vkb_srv_table *inst, VkPhysicalDevice pd);
#define VKB_VIDYN_EDS 4
void vkb_vidyn_instance_install(vkb_srv_table *t);
/* vkCreateDevice: the extension and its feature off the driver's request where emulated; returns
 * the layer's mode for the device (0: no layer) */
int vkb_vidyn_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, uint32_t emu, VkDeviceCreateInfo *ci, const char **exts,
                                 uint32_t *n);
void vkb_vidyn_device_init(vkb_srv_table *t, int mode);
void vkb_vidyn_device_destroy(vkb_srv_table *t);
/* fp16_hide (MK-20, server_errata_fp16.c): 1 when 16-bit float arithmetic and storage are hidden
 * from the clients of this physical device (inst: its instance's table) - the erratum's decision
 * as vkCreateDevice makes it, VKBRIDGE_NO_FP16 included; why (may be NULL) gets the reason */
int vkb_errata_fp16_hidden(const vkb_srv_table *inst, VkPhysicalDevice pd, char *why, size_t cap);
void vkb_errata_fp16_instance_install(vkb_srv_table *inst); /* the feature and property queries */
void vkb_errata_fp16_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, VkDeviceCreateInfo *ci); /* strips what an app enables anyway */
/* gs_hide (, server_errata_gs.c): 1 when geometry shaders are hidden from this process on
 * this physical device - the erratum's decision for the driver (VKBRIDGE_ERRATA=+/-gs_hide
 * included) and the process one of VKBRIDGE_GS_HIDE_PROCS; why (may be NULL) gets the reason */
int vkb_errata_gs_hidden(const vkb_srv_table *inst, VkPhysicalDevice pd, char *why, size_t cap);
int vkb_errata_gs_driver(const vkb_srv_table *inst, VkPhysicalDevice pd, char *why, size_t cap); /* the driver half alone */
int vkb_errata_gs_process(const vkb_srv_proc *p);    /* the process half alone: its name is in the list */
void vkb_errata_gs_instance_install(vkb_srv_table *inst); /* over every other query wrapper: last */
void vkb_errata_gs_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, VkDeviceCreateInfo *ci);
/* pv_mutable_linear (PV-02, server_errata_pvmutable.c): the erratum's decision for this physical device
 * (the probe, VKBRIDGE_ERRATA=+/-pv_mutable_linear); the pattern of query it answers; the image-format
 * query's wrapper (instance); 1 when VKBRIDGE_ERRATA_IMITATE names erratum id (host builds) */
int vkb_errata_pvmutable_driver(const vkb_srv_table *inst, VkPhysicalDevice pd, char *why, size_t cap);
int vkb_errata_pvmutable_pattern(const VkPhysicalDeviceImageFormatInfo2 *info);
int vkb_errata_pvmutable_probe_refuses(const VkPhysicalDeviceImageFormatInfo2 *info);
void vkb_errata_pvmutable_instance_install(vkb_srv_table *inst);
int vkb_errata_imitating(int id);
/* pv_bgra_store (PV-07, server_errata_pvbgra.c): the top of a device's table (after vkb_mem_export_install): memory of an
 * image made R8G8B8A8 for a B8G8R8A8 client registered at export for the compositor; an image's format as the driver
 * holds it (the dma-buf-over-AHB record) */
void vkb_errata_pvbgra_export_install(vkb_srv_table *t);
VkFormat vkb_errata_pvbgra_format(vkb_srv_table *t, VkImage image, VkFormat f);
/* pv_zink_name (PV-08, server_errata_zink.c): the self-test's profile probe per physical device; the
 * probe's record (VKB_ERRATA_PROBE_*); vkCreateInstance's engine name for a client (NULL: as it is) */
void vkb_zink_profile_probe(const uint8_t uuid[VK_UUID_SIZE], uint32_t vendor, uint32_t driver_id, uint32_t api);
int vkb_zink_profile_result(void);
const char *vkb_errata_zink_engine(const char *engine, char *why, size_t cap);
int vkb_errata_zink_name_on(char *why, size_t cap); /* server_errata.c: the erratum's decision for an instance */
void vkb_errata_zink_instance_install(vkb_srv_table *inst); /* a renamed instance: the profile kept but for vertexInputDynamicState */
int vkb_hang_wanted(void); /* VKBRIDGE_HANG_DETECT: vkCreateDevice also enables VK_EXT_device_fault */
void vkb_hang_device_init(vkb_srv_table *t);
void vkb_hang_arm_next(vkb_srv_table *t, const char *what); /* server_lostlog.c: a loss with a GPU fault address */
void vkb_hang_arm_hung(vkb_srv_table *t, const char *what); /* server_wait.c: a GPU wait given up with the device not lost */
void vkb_hang_fault_addr(vkb_srv_table *t, VkDeviceAddress a); /* server_lostlog.c: the driver's fault address, looked up */
void vkb_qlock_device_init(vkb_srv_table *t); /* server_qlock.c: per-queue locks, beneath every layer */
void vkb_qlock_device_destroy(vkb_srv_table *t);
/* server_pvsusp.c (errata pv_suspend, PowerVR): suspended/resumed dynamic renderings reach the driver as whole passes */
void vkb_pvsusp_device_init(vkb_srv_table *t);
void vkb_pvsusp_device_destroy(vkb_srv_table *t);
void vkb_hang_device_destroy(vkb_srv_table *t);

/* server_gpuprof.c */
int vkb_gprof_wanted(void); /* VKBRIDGE_GPU_PROF */
void vkb_gprof_device_init(vkb_srv_table *t);
void vkb_gprof_device_destroy(vkb_srv_table *t);
/* server_hwcnt.c: Mali hardware counters (VKBRIDGE_GPU_PROF=counters): a summary every 5 s */
void vkb_hwcnt_start(void);
void vkb_trace_device_destroy(vkb_srv_table *t);
/* server_pldedup.c: pipeline layouts made the same way share one driver layout (PowerVR; VKBRIDGE_PL_SHARE) */
void vkb_pldedup_device_init(vkb_srv_table *t);
void vkb_pldedup_device_destroy(vkb_srv_table *t);

/* server_nulldesc.c */
void vkb_nulldesc_device_init(vkb_srv_table *t);
void vkb_nulldesc_device_destroy(vkb_srv_table *t);

/* server_bvmap.c */
void vkb_bvmap_device_init(vkb_srv_table *t);
void vkb_bvmap_device_destroy(vkb_srv_table *t);

/* server_dsmap.c */
void vkb_dsmap_device_init(vkb_srv_table *t);
void vkb_dsmap_device_destroy(vkb_srv_table *t);

/* server_pcache.c */
void vkb_pcache_set_dir(const char *dir);
const char *vkb_pcache_dir(void);
/* Writers into the cache directory (server_pcache.c): it may be removed under a running server */
int vkb_mkdirs(const char *path);                   /* mkdir -p; 0 or -1 with errno */
int vkb_cache_dir_ensure(void);                     /* made again if removed (said once); 1: there */
int vkb_cache_subdir(const char *sub);              /* <cache dir>/<sub> made if missing; 0 or -1 */
int vkb_cache_write_failed(const char *what, int e); /* ENOSPC said once; 1: caller reports e */
void vkb_cache_write_ok(void);                      /* a write went through (space again) */
void vkb_srv_env_reload(void);             /* <cache dir>/env overrides, read at each vkCreateDevice */
void vkb_srv_env_refresh(void);            /* the same when the file changed since (vkbQueryServer) */
/* A switch, first found wins: on a client's connection thread that game's preset file, then
 * <cache dir>/env, the server's environment, then that game's built-in preset (server_presets.c) */
const char *vkb_srv_env(const char *name);
enum { VKB_ENV_SRC_NONE, VKB_ENV_SRC_CACHE_ENV, VKB_ENV_SRC_ENV, VKB_ENV_SRC_PRESET_FILE, VKB_ENV_SRC_PRESET_BUILTIN };
const char *vkb_srv_env_src(const char *name, int *src);   /* the same, and where it came from (VKB_ENV_SRC_*) */
const char *vkb_srv_env_global(const char *name);          /* without the preset: server-wide state */
const char *vkb_srv_env_override(const char *name);        /* <cache dir>/env's value alone, or NULL */
const char *vkb_env_src_name(int src);                     /* "<cache dir>/env", "the environment", ... */
void vkb_pcache_device_init(vkb_srv_table *t);
void vkb_pcache_device_destroy(vkb_srv_table *t);
void vkb_pcache_wedged(vkb_srv_table *t, const char *call); /* the compile watchdog: a timeout after a contained crash on t */
int vkb_pcache_aside(const vkb_srv_table *t);                /* 1: t's cache wedged the driver and was set aside */
/* Pipelines made through the persistent caches so far, and the caches' size in the driver at their
 * last save (every device; 0 and 0 without them). */
void vkb_pcache_stats(uint64_t *pipelines, uint64_t *bytes);
/* a pipeline the server builds for itself, through the device's cache (VK_NULL_HANDLE without one) */
VkResult vkb_pcache_create_compute(vkb_srv_table *t, const VkComputePipelineCreateInfo *ci, VkPipeline *out);
/* the device's persistent cache: its program's file handed the driver nothing at its start (a program new to this driver), or none */
int vkb_pcache_started_empty(vkb_srv_table *t);
/* the program's cache file (things kept beside it), NULL: none, or one never written */
const char *vkb_pcache_file(vkb_srv_table *t);
/* a cache for the server's own compiles on a device without a table (the startup probe's) */
VkPipelineCache vkb_pcache_own_open(const vkb_dispatch *r, const VkPhysicalDeviceProperties *gpu, VkDevice d, const char *name, char *path,
                                    size_t cap);
void vkb_pcache_own_close(const vkb_dispatch *r, VkDevice d, VkPipelineCache c, const char *path);

/* server_hitch.c: per-hitch attribution (VKBRIDGE_HITCH_MS). Notes are dropped unless the process
 * has it on (one pointer test); times are CLOCK_MONOTONIC ns. */
enum { VKB_HITCH_COMPILE, VKB_HITCH_BC, VKB_HITCH_ALLOC, VKB_HITCH_PCACHE, VKB_HITCH_CLASSES };
uint64_t vkb_hitch_now(void);
uint32_t vkb_hitch_ms_setting(void); /* VKBRIDGE_HITCH_MS for this thread's client (presets, <cache dir>/env); 0: off */
void vkb_hitch_enable(vkb_srv_proc *p, uint32_t ms);
static inline int vkb_hitch_on(const vkb_srv_proc *p) { return p && __atomic_load_n(&p->hitch, __ATOMIC_RELAXED) != NULL; }
void vkb_hitch_note(vkb_srv_proc *p, int cls, uint64_t t0, uint64_t t1, uint32_t n, uint64_t bytes);
/* a BC conversion recorded: its texels and the most GPU time it can take on this GPU (server_emu_bcn.c) */
/* inflight_ns: added to the conversions in flight on the GPU (bc_inflight_est_ms), taken off by vkb_hitch_bc_done when the
 * command buffer's work is over; vkb_hitch_bc_calib: a command buffer's estimate against its measured GPU time (F0) */
void vkb_hitch_note_bc(vkb_srv_proc *p, uint64_t t0, uint64_t t1, uint64_t texels, uint64_t gpu_est_ns, uint64_t inflight_ns);
void vkb_hitch_bc_done(vkb_srv_proc *p, uint64_t inflight_ns);
void vkb_hitch_bc_calib(uint64_t est_ns, uint64_t gpu_ns);
void vkb_hitch_compile(vkb_srv_proc *p, const char *what, uint32_t n, uint64_t t0, uint64_t t1, int result);
void vkb_hitch_proc_gone(vkb_srv_proc *p); /* the summary line, and the ring freed */
int vkb_test_hitch_delay_ms(void);         /* server_emu_bcn.c: VKBRIDGE_TEST_HITCH_DELAY_MS (host tests) */

/* server_perf.c: always-on performance diagnostics: per 10-s window "perf:" and
 * "perf-threads:" lines, and the report's Performance section. VKBRIDGE_PERF_DIAG=0: off. */
#define VKB_PERF_CAUSES 6 /* server_hitch.c's hitch causes: compile, bc, alloc, pcache, wait, other */
typedef struct vkb_perf_win {
    uint64_t t0, t1, susp_ns, frames, over, repeats, worst_ns; /* one of the client's per-second windows */
    uint64_t causes[VKB_PERF_CAUSES];                           /* its hitches by cause */
    double app_ms, present_ms, present_rt_ms, compile_ms, bc_ms, alloc_ms;
    uint32_t ft_p50_us, ft_p90_us;
} vkb_perf_win;
int vkb_perf_on(void);
void vkb_perf_window(vkb_srv_proc *p, const vkb_perf_win *w); /* server_hitch.c, at each window message */
void vkb_perf_proc_gone(vkb_srv_proc *p);
void vkb_perf_session(void);                 /* a new session: the run's sums of processes still alive start again */
void vkb_perf_report(FILE *f, int json);     /* the report's section (under the report's lock) */
int vkb_report_thermal(void);                /* server_report.c: Android's thermal status from vkbridge-clocks.log, -1 unknown */

/* server_latency.c: the latency log (VKBRIDGE_LATLOG); the lines come from the client's vkbLatency */
uint32_t vkb_present_wait_setting(void); /* VKBRIDGE_PRESENT_WAIT the same way: 0 unset, VKB_PRESENT_WAIT_ON/OFF */
uint32_t vkb_frame_queue_setting(void); /* VKBRIDGE_FRAME_QUEUE for this thread's client, the same way: 0 off, 1-8 */
uint32_t vkb_lat_setting(void); /* for this thread's client (presets, <cache dir>/env, the environment): 1 on */

/* server_conn.c */
void vkb_srv_serve(int listen_fd);
void vkb_srv_stats_reload(void); /* VKBRIDGE_STATS read again (after vkb_srv_env_reload) */
vkb_srv_table *vkb_table_get(uint32_t id);
uint32_t vkb_table_register(vkb_srv_table *t);
void vkb_table_unregister(vkb_srv_table *t);
void vkb_srv_reply_status(vkb_srv_call *c, uint32_t status);
/* The table of the call being executed on this thread (emulation wrappers use it). */
vkb_srv_table *vkb_srv_current_table(void);
void vkb_cbfail_device_init(vkb_srv_table *t); /* server_conn.c */
void vkb_cbfail_device_destroy(vkb_srv_table *t);
void vkb_syncfd_device_init(vkb_srv_table *t);   /* server_syncfd.c: sync-fd exports with a deadline */
int vkb_syncfd_device_destroy(vkb_srv_table *t); /* 1: an export is still in the driver, leave the device alive */
int vkb_syncfd_order_ms(uint32_t table, uint32_t cmd); /* the order wait's deadline for that request, 0: none */
uint32_t vkb_syncfd_client_knobs(uint32_t driver_id);  /* VKB_KNOB_SYNCFD_SIDE_SHIFT bits for vkbQueryServer's reply */
/* server_conn.c: the current request's order wait ran out (vkb_syncfd_order_ms); finish: wait the
 * rest of it now; done: go on without it */
int vkb_srv_order_skipped(void);
void vkb_srv_order_finish(void);
void vkb_srv_order_done(void);
/* server_conn.c, VKBRIDGE_FAST_CREATE (VKB_F_FAST_CREATE): a create the client let run ahead of its process's queued work. pre:
 * the handler's first act before the driver's create (image or buffer create info, the other NULL): at once where nothing
 * queued can change what it makes, else the order wait as without the bit. reply: the tail of vkCreateImage's reply (the
 * image's requirements). set: what the process was told at its first vkbQueryServer (1 off, 2 on). */
/* (pre and reply: common/vkb_server_iface.h, for the generated handlers) */
void vkb_srv_fast_create_set(vkb_srv_proc *p, int on);
/* server_emu_bcn.c: 1 when the switches ask for a BC rule that weighs bytes alive (the game's preset, <cache dir>/env, the
 * environment): VKB_KNOB_FAST_CREATE_BC_SHIFT OFF */
int vkb_emu_bcn_rules_wanted(void);
/* A request that waits on the CPU for a later request of its program (an emulated timeline's wait-before-signal,
 * server_timeline.c): 1 before the wait, 0 after. order_wait does not take it for one still running. */
void vkb_srv_parked(int on);
void vkb_sparse_instance_install(vkb_srv_table *t); /* server_emu_sparse.c */
void vkb_sparse_device_init(vkb_srv_table *t);
void vkb_sparse_device_destroy(vkb_srv_table *t);
void vkb_syncfb_device_init(vkb_srv_table *t); /* server_syncfb.c */
void vkb_syncfb_device_destroy(vkb_srv_table *t);
void vkb_srv_set_current_table(vkb_srv_table *t);
void vkb_proc_cleanup(vkb_srv_proc *p);
vkb_srv_proc *vkb_srv_current_proc(void); /* the client process this connection thread serves, or NULL */

/* server_presets.c: per-game presets, by Steam app id (<cache dir>/presets/<appid>.env and
 * presets_builtin.inc) */
void vkb_preset_load(vkb_srv_proc *p);   /* at the process's hello: its app id is known */
void vkb_preset_free(vkb_srv_proc *p);
/* this process's preset value of name, or NULL; *src VKB_ENV_SRC_PRESET_FILE or _BUILTIN */
const char *vkb_preset_get(const vkb_srv_proc *p, const char *name, int *src);
const char *vkb_preset_text(const vkb_srv_proc *p, char *out, size_t cap); /* "K=V ... (from file)", or "" */
/* "preset for steam<appid>: a built-in preset exists but was not tested on <gpu>; ...", or "" */
const char *vkb_preset_note(const vkb_srv_proc *p, char *out, size_t cap);
uint32_t vkb_preset_client_knobs(void);  /* vkb_server_pd_info.client_knobs for this thread's client */
uint32_t vkb_preset_perf_src(void);      /* vkb_server_pd_info.perf_src, the same way */
void vkb_preset_client_values(vkb_server_pd_info *info, vkb_srv_proc *proc); /* its .heap_max_mb, .spoof_gpu and .present_timeout_s, the same way */
uint64_t vkb_memtotal_mb(void); /* RAM in MB (MemTotal; VKBRIDGE_FAKE_MEMTOTAL_MB for tests), 0: unknown */
/* server_compile_gate.c: at most VKBRIDGE_COMPILE_THREADS pipeline compiles in the driver at once (1 while
 * memory is low). enter waits for a slot and returns whether it took one; leave gives back what enter took. */
int vkb_compile_gate_enter(VkPipelineCreateFlags flags);
void vkb_compile_gate_leave(int took);
int vkb_compile_gate_default(int cores);
void vkb_compile_gate_stats(int *limit, int *peak, uint64_t *waits, uint64_t *tight_waits, double *wait_s, double *wait_max_s);
uint64_t vkb_memavail_kb(void); /* MemAvailable in KB (VKBRIDGE_FAKE_MEMAVAIL_MB for tests), 0: unknown */
uint64_t vkb_swaptotal_mb(void); /* SwapTotal in MB (VKBRIDGE_FAKE_SWAPTOTAL_MB for tests), 0: none or unknown */
uint64_t vkb_swapfree_mb(void); /* SwapFree in MB (VKBRIDGE_FAKE_SWAPFREE_MB for tests) */
uint32_t vkb_heap_default_cap_mb(int mode, uint64_t ram_mb); /* the safe-mode heap cap for 8 and 12 GB devices, 0: none */

/* server_instance.c */
void vkb_selftest_all(void);
extern int vkb_selftest_too_old;
const vkb_pd_knowledge *vkb_pd_lookup(const vkb_dispatch *dt, VkPhysicalDevice pd);
extern vkb_pd_knowledge vkb_pds[VKB_MAX_PDS];
int vkb_pds_select_mode(void); /* the mode read again, its emulation masks in force; returns it */
extern int vkb_npds;

/* server_memory.c */
extern PFN_vkGetDeviceProcAddr vkb_mem_gdpa;
int vkb_mem_dmaheap_available(void);
int vkb_mem_export_backing(void); /* the dma-buf export emulation's pages: 1 dma-heap, 2 AHardwareBuffer (its check passed), 0 none */
const char *vkb_mem_export_backing_name(void);
const char *vkb_mem_ahb_export_result(void); /* the start's check of the AHardwareBuffer pages; "" where not checked */
void vkb_mem_export_install(vkb_srv_table *t);
void vkb_mem_instance_install(vkb_srv_table *t); /* VKBRIDGE_FAKE_MISSING=lazymem: Mali's memory types (host tests) */
void vkb_mem_mutable_instance_install(vkb_srv_table *t); /* PR #31: a refused mutable LINEAR-modifier dma-buf query asked without the flag */
void vkb_mem_device_install(vkb_srv_table *t);   /* VKBRIDGE_FAKE_MISSING=dmabuftypes: Mali's dma-buf fd properties (host tests) */
/* VKBRIDGE_FAKE_MISSING=xringmem or g720mem: another Mali's memory table beneath the whole server (host tests) */
PFN_vkGetInstanceProcAddr vkb_mem_imitate_gipa(PFN_vkGetInstanceProcAddr gipa);
void vkb_mem_log_types(vkb_srv_table *t, uint32_t id);
void vkb_mem_selftest(vkb_pd_knowledge *k, VkInstance inst, const vkb_dispatch *idt, VkPhysicalDevice pd);
/* Cached readback as the client is told: the self-test's types gated by VKBRIDGE_CACHED_READBACK
 * and the mode (VKB_MODE_*); clears cached_types/cached_flags when off. */
void vkb_mem_cached_gate(vkb_server_pd_info *info, int mode);
const char *const *vkb_mem_required_extensions(uint32_t strategy, uint32_t *count);
void vkb_mem_track(vkb_srv_table *dev, vkb_srv_mem *m);
vkb_srv_mem *vkb_mem_find(vkb_srv_table *dev, VkDeviceMemory mem, int remove);
void vkb_mem_release(vkb_srv_table *dev, vkb_srv_mem *m);
void vkb_mem_free_all(vkb_srv_table *dev);
/* Accounting, one line every ~5 s while it changes (VKBRIDGE_MEM_STATS=0: off). */
uint8_t vkb_memstat_dedicated(const VkMemoryAllocateInfo *ai);
void vkb_memstat_image(VkFormat bc, int64_t bytes, int64_t bc_src_bytes, int64_t bc_decoded_bytes); /* bc 0: not BC */
/* A non-BC image's shape for the memory line's per-format table (bytes < 0: destroyed). */
#define VKB_IMG_SHAPE_TARGET 1 /* colour/depth attachment or storage */
#define VKB_IMG_SHAPE_MIPS 2   /* not a target, a mip chain */
#define VKB_IMG_SHAPE_OVER 4   /* and 2D with a side over 1024 */
void vkb_memstat_image_shape(VkFormat format, int shape, int64_t bytes);
void vkb_memstat_buffer(int64_t bytes);
void vkb_memstat_query(int pid, int64_t *pid_bytes, int64_t *total, int64_t *images, int64_t *bc_images, int64_t *bc_src);
/* 1 while the device's memory is low (MemAvailable under an eighth of RAM, until it is over a sixth
 * again; VKBRIDGE_FAKE_MEMAVAIL_MB / _MEMTOTAL_MB for tests): what the server holds without need is
 * let go. Watched every ~5 s by the accounting thread, VKBRIDGE_MEM_STATS=0 too. */
int vkb_mem_low(void);
/* MemAvailable under a sixth of RAM (the mark low memory clears at), or low: no new optional memory (BC small decode) */
int vkb_mem_tight(void);

/* OPAQUE_FD on dma-bufs (server_opaque.c) */
void vkb_opaque_instance_install(vkb_srv_table *t);
int vkb_opaque_alloc_info(vkb_srv_table *t, VkMemoryAllocateInfo *ai);
VkResult vkb_opaque_alloc_retry(vkb_srv_table *t, VkMemoryAllocateInfo *ai, vkb_srv_mem *m);
int vkb_opaque_shared_info(vkb_srv_table *t, VkMemoryAllocateInfo *ai);
void vkb_opaque_shared_done(vkb_srv_mem *m, int out_fd);
const VkMemoryGetFdInfoKHR *vkb_opaque_get_fd_info(vkb_srv_table *t, const VkMemoryGetFdInfoKHR *gi, VkMemoryGetFdInfoKHR *tmp);

/* server_apicap.c: Steam mode's own processes (Xwayland, steam, steamwebhelper) shown Vulkan 1.1 and
 * gamescope 1.2 on the black-screen gate: Arm job-manager Malis below r54 with VK_EXT_extended_dynamic_state
 * (the Steam-mode black screen/#15/#28, #11;
 * VKBRIDGE_STEAM_API_CAP, vkbQueryServer's api_cap). */
int vkb_errata_arm_release_pd(const vkb_srv_table *t, VkPhysicalDevice pd, int *arm, int *jm);
int vkb_black_gate(const vkb_srv_table *t, VkPhysicalDevice pd, const char **why);
uint32_t vkb_steam_api_cap(const vkb_srv_table *t, VkPhysicalDevice pd);

/* server_driverid.c: Arm's own driver told from others with Arm's vendor ID (Mesa PanVK) by driverID.
 * vkb_driver_id: the VkDriverId, 0 when the driver cannot say (asked once per physical device, kept
 * for the process; inst_api: the instance's version). vkb_driver_is_arm: driverID ARM_PROPRIETARY,
 * or without one vendor 0x13B5 (driver_id 0: what was asked before). */
uint32_t vkb_driver_id(const vkb_dispatch *dt, VkPhysicalDevice pd, uint32_t inst_api);
int vkb_driver_is_arm(const VkPhysicalDeviceProperties *p, uint32_t driver_id);
int vkb_driver_is_powervr(const VkPhysicalDeviceProperties *p, uint32_t driver_id); /* IMAGINATION_PROPRIETARY, else vendor 0x1010 */
int vkb_arm_driver(const vkb_dispatch *dt, VkPhysicalDevice pd, uint32_t inst_api);
const char *vkb_driver_id_name(uint32_t id);
void vkb_driver_log(const vkb_dispatch *dt, VkPhysicalDevice pd, uint32_t inst_api); /* the self-test's line */
PFN_vkGetInstanceProcAddr vkb_driverid_imitate_gipa(PFN_vkGetInstanceProcAddr gipa); /* VKBRIDGE_FAKE_DRIVER (host tests) */

/* emulation (server_emu*.c) */
uint32_t vkb_emu_detect(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t *missing);
int vkb_emu_robust2_driver(const vkb_dispatch *dt, VkPhysicalDevice pd, VkBool32 *buf, VkBool32 *img, VkBool32 *null_desc);
int vkb_emu_robust2_image_stand_in(vkb_srv_table *inst, VkPhysicalDevice pd, uint32_t emu, const VkDeviceCreateInfo *ci);
PFN_vkGetInstanceProcAddr vkb_emu_imitate_gipa(PFN_vkGetInstanceProcAddr gipa);
/* BC6H -> ASTC 4x4 HDR: whether the device gets textureCompressionASTC_HDR for it (transcoding wanted,
 * the GPU has the feature; api: the instance's version): 0 no, 1 as Vulkan 1.3 core, 2 through
 * VK_EXT_texture_compression_astc_hdr (which vkCreateDevice then enables). VKBRIDGE_FAKE_MISSING=
 * ...,astchdr: no. */
int vkb_emu_bcn_astc_hdr(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t api);
void vkb_emu_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, uint32_t emu,
                                VkDeviceCreateInfo *ci, vkb_arena *a);
/* extension features the app turned on that the driver lacks, taken off (DXVK 1.x's geometryStreams and
 * shaderDrawParameters);
 * avail: the driver's extensions */
void vkb_emu_device_clamp_features(vkb_srv_table *inst, VkPhysicalDevice pd, VkDeviceCreateInfo *ci,
                                   const VkExtensionProperties *avail, uint32_t avail_n);
/* a refused vkCreateDevice: what the request asks for that the driver lacks, as one line into out (#32) */
void vkb_emu_device_lacks(vkb_srv_table *inst, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                          const VkExtensionProperties *avail, uint32_t avail_n, char *out, size_t cap);
void vkb_emu_device_init(vkb_srv_table *dev, uint32_t emu);
/* host imitation of a driver without maintenance5 (VKBRIDGE_FAKE_MISSING=maint5), at the driver boundary:
 * before every driver-near layer */
void vkb_emu_driver_imitate_init(vkb_srv_table *dev);
void vkb_emu_device_destroy(vkb_srv_table *dev);
/* first when a device goes: stops the emulations' threads, which call through the layers */
void vkb_emu_device_quiesce(vkb_srv_table *dev);

#endif
