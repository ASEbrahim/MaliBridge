/* SPDX-License-Identifier: GPL-3.0-or-later
 *
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
    struct vkb_srv_proc *next;
    /* Ordering of the process's asynchronous requests (vkb_msg_hdr.seq/barrier). */
    pthread_mutex_t olock;
    pthread_cond_t ocond;
    uint64_t watermark;            /* every async seq <= this has run */
    uint8_t done[VKB_SEQ_RING];    /* seqs above the watermark that have run */
    int conn_closed;               /* a connection ended (a seq may never arrive) */
    int owaiters;                  /* threads in order_wait */
} vkb_srv_proc;

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
} vkb_srv_conn;

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
    int export_fd1;          /* VKB_EMU_DMABUF_EXPORT: the dma-heap fd + 1 handed out as the export (0: none) */
    uint8_t stat_cls, stat_ded; /* accounting (server_memory.c): strategy class, dedicated to an image (1) / buffer (2) */
    int stat_pid;
    struct vkb_srv_mem *next;
} vkb_srv_mem;

struct vkb_emu_device;

typedef struct vkb_srv_table {
    uint32_t id;
    int is_device;
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
} vkb_srv_table;

/* server_main.c */
extern PFN_vkGetInstanceProcAddr vkb_gipa;
extern vkb_dispatch vkb_global_dt;
extern int vkb_verbose;

void vkb_log_summary(void);

/* server_guard.c */
void vkb_guard_device_init(vkb_srv_table *t);
void vkb_guard_device_destroy(vkb_srv_table *t);
void vkb_guard_fault(int sig, void *addr); /* crash handler: jumps back into a guarded driver call, if any */
/* fn(arg) armed like a driver compile (the startup probe): a fault in it, or a run past limit_s
 * (0: VKBRIDGE_COMPILE_TIMEOUT_S, < 0: none), jumps back; *sig: the fault's signal. */
enum { VKB_GUARD_DONE = 0, VKB_GUARD_CRASHED = 1, VKB_GUARD_TIMED_OUT = 2 };
int vkb_guard_run(const char *what, double limit_s, void (*fn)(void *), void *arg, int *sig);
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
#define VKB_PROBE_FLAG_TX_ASTC 0x20u
#define VKB_PROBE_FLAG_TX_ASTC_HDR 0x40u
#define VKB_PROBE_FLAG_TX_ALL 0x78u
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
#define VKB_PROBE_FLAG_MK_ALL 0x7f80u
int vkb_probe_flag(uint32_t f);
void vkb_probe_device(vkb_pd_knowledge *k, VkInstance inst, const vkb_dispatch *idt, VkPhysicalDevice pd);
void vkb_probe_start_background(void); /* what did not fit the start's budget */
void vkb_probe_write_md(FILE *f);
void vkb_probe_write_json(FILE *f);
int vkb_probe_issues(void (*cb)(void *arg, const char *line), void *arg);
/* What a GPU's probe results decide in a mode (VKB_MODE_*), re-run on the raw results as recorded
 * (switching mode never re-probes): the emulation mask (from what the GPU had before the probe)
 * and the runtime flags. 0 when the GPU has no profile. For feat/safe-mode's per-mode masks. */
int vkb_probe_decide_mode(const uint8_t uuid[VK_UUID_SIZE], int mode, uint32_t *emu, uint32_t *flags);
/* How the process ends, for a probe in flight (async-signal-safe): a stop (SIGTERM, the app gone)
 * leaves it to be run again, a crash outside the probe's guard makes it died. */
void vkb_probe_mark_end(int sig, int crashed);
void vkb_probe_crash_marks(int all); /* 1: every fatal signal but SIGKILL reaches vkb_probe_mark_end */

/* server_report.c: the session report (<log dir>/malibridge-report.md and .json) */
void vkb_report_session(const char *log_path); /* a session starts (its server log's path; NULL: none) */
void vkb_report_log(int level, const char *fmt, const char *msg); /* every log line */
void vkb_report_hello(vkb_srv_proc *p);
void vkb_report_gone(vkb_srv_proc *p);
void vkb_report_instance(vkb_srv_table *t, const VkInstanceCreateInfo *ci);
void vkb_report_device(vkb_srv_table *inst, const VkDeviceCreateInfo *ci, VkResult r, const char *dropped_features,
                       const char *not_passed_exts);
void vkb_report_device_init(vkb_srv_table *t); /* device-lost watch on submissions and waits */
void vkb_report_device_destroy(vkb_srv_table *t);
void vkb_report_features(const VkPhysicalDeviceFeatures *want, const VkPhysicalDeviceFeatures *have, char *out, size_t cap);
const char *vkb_srv_env_list(char *out, size_t cap); /* <cache dir>/env as applied (server_pcache.c) */
void vkb_report_compile(vkb_srv_table *t, int compute, uint32_t n, double ms, VkResult r, const char *slow_stages);

/* server_spvfix.c */
void vkb_spvfix_device_init(vkb_srv_table *t);
void vkb_spvfix_device_destroy(vkb_srv_table *t);

/* server_pools.c */
void vkb_pools_device_init(vkb_srv_table *t);
void vkb_pools_device_destroy(vkb_srv_table *t);

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
void vkb_errata_probe_end(void *h);
/* FNV-1a of a module's SPIR-V as the app made it: a module an erratum rewrote is named by the
 * code it was rewritten from (server_errata_bitwise.c); what layers beneath the errata hash with */
uint64_t vkb_spv_app_hash(const uint32_t *code, size_t size);
int vkb_errata_parse_version(const char *s, uint32_t *r, uint32_t *p, int *beta); /* r<major>p<minor> in s */
const char *vkb_errata_hidden_ext(const vkb_srv_table *inst, VkPhysicalDevice pd); /* an erratum's hidden device extension */
int vkb_hang_wanted(void); /* VKBRIDGE_HANG_DETECT: vkCreateDevice also enables VK_EXT_device_fault */
void vkb_hang_device_init(vkb_srv_table *t);
void vkb_hang_device_destroy(vkb_srv_table *t);

/* server_gpuprof.c */
int vkb_gprof_wanted(void); /* VKBRIDGE_GPU_PROF */
void vkb_gprof_device_init(vkb_srv_table *t);
void vkb_gprof_device_destroy(vkb_srv_table *t);
void vkb_trace_device_destroy(vkb_srv_table *t);

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
/* a pipeline the server builds for itself, through the device's cache (VK_NULL_HANDLE without one) */
VkResult vkb_pcache_create_compute(vkb_srv_table *t, const VkComputePipelineCreateInfo *ci, VkPipeline *out);
/* a cache for the server's own compiles on a device without a table (the startup probe's) */
VkPipelineCache vkb_pcache_own_open(const vkb_dispatch *r, const VkPhysicalDeviceProperties *gpu, VkDevice d, const char *name, char *path,
                                    size_t cap);
void vkb_pcache_own_close(const vkb_dispatch *r, VkDevice d, VkPipelineCache c, const char *path);

/* server_conn.c */
void vkb_srv_serve(int listen_fd);
void vkb_srv_stats_reload(void); /* VKBRIDGE_STATS read again (after vkb_srv_env_reload) */
vkb_srv_table *vkb_table_get(uint32_t id);
uint32_t vkb_table_register(vkb_srv_table *t);
void vkb_table_unregister(vkb_srv_table *t);
void vkb_srv_reply_status(vkb_srv_call *c, uint32_t status);
/* The table of the call being executed on this thread (emulation wrappers use it). */
vkb_srv_table *vkb_srv_current_table(void);
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

/* server_instance.c */
void vkb_selftest_all(void);
const vkb_pd_knowledge *vkb_pd_lookup(const vkb_dispatch *dt, VkPhysicalDevice pd);
extern vkb_pd_knowledge vkb_pds[VKB_MAX_PDS];
int vkb_pds_select_mode(void); /* the mode read again, its emulation masks in force; returns it */
extern int vkb_npds;

/* server_memory.c */
extern PFN_vkGetDeviceProcAddr vkb_mem_gdpa;
int vkb_mem_dmaheap_available(void);
void vkb_mem_export_install(vkb_srv_table *t);
void vkb_mem_instance_install(vkb_srv_table *t); /* VKBRIDGE_FAKE_MISSING=lazymem: Mali's memory types (host tests) */
void vkb_mem_log_types(vkb_srv_table *t, uint32_t id);
void vkb_mem_selftest(vkb_pd_knowledge *k, VkInstance inst, const vkb_dispatch *idt, VkPhysicalDevice pd);
const char *const *vkb_mem_required_extensions(uint32_t strategy, uint32_t *count);
void vkb_mem_track(vkb_srv_table *dev, vkb_srv_mem *m);
vkb_srv_mem *vkb_mem_find(vkb_srv_table *dev, VkDeviceMemory mem, int remove);
void vkb_mem_release(vkb_srv_table *dev, vkb_srv_mem *m);
void vkb_mem_free_all(vkb_srv_table *dev);
/* Accounting, one line every ~5 s while it changes (VKBRIDGE_MEM_STATS=0: off). */
uint8_t vkb_memstat_dedicated(const VkMemoryAllocateInfo *ai);
void vkb_memstat_image(VkFormat bc, int64_t bytes, int64_t bc_src_bytes, int64_t bc_decoded_bytes); /* bc 0: not BC */
void vkb_memstat_buffer(int64_t bytes);
void vkb_memstat_query(int pid, int64_t *pid_bytes, int64_t *total, int64_t *images, int64_t *bc_images, int64_t *bc_src);

/* OPAQUE_FD on dma-bufs (server_opaque.c) */
void vkb_opaque_instance_install(vkb_srv_table *t);
int vkb_opaque_alloc_info(vkb_srv_table *t, VkMemoryAllocateInfo *ai);
VkResult vkb_opaque_alloc_retry(vkb_srv_table *t, VkMemoryAllocateInfo *ai, vkb_srv_mem *m);
int vkb_opaque_shared_info(vkb_srv_table *t, VkMemoryAllocateInfo *ai);
void vkb_opaque_shared_done(vkb_srv_mem *m, int out_fd);
const VkMemoryGetFdInfoKHR *vkb_opaque_get_fd_info(vkb_srv_table *t, const VkMemoryGetFdInfoKHR *gi, VkMemoryGetFdInfoKHR *tmp);

/* emulation (server_emu*.c) */
uint32_t vkb_emu_detect(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t *missing);
/* BC6H -> ASTC 4x4 HDR: whether the device gets textureCompressionASTC_HDR for it (transcoding wanted,
 * the GPU has the feature; api: the instance's version): 0 no, 1 as Vulkan 1.3 core, 2 through
 * VK_EXT_texture_compression_astc_hdr (which vkCreateDevice then enables). VKBRIDGE_FAKE_MISSING=
 * ...,astchdr: no. */
int vkb_emu_bcn_astc_hdr(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t api);
void vkb_emu_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, uint32_t emu,
                                VkDeviceCreateInfo *ci, vkb_arena *a);
void vkb_emu_device_init(vkb_srv_table *dev, uint32_t emu);
void vkb_emu_device_destroy(vkb_srv_table *dev);
/* first when a device goes: stops the emulations' threads, which call through the layers */
void vkb_emu_device_quiesce(vkb_srv_table *dev);

#endif
