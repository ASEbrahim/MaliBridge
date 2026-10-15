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
    vkb_server_pd_info info;
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
int vkb_probe_flag(uint32_t f);
void vkb_probe_device(vkb_pd_knowledge *k, VkInstance inst, const vkb_dispatch *idt, VkPhysicalDevice pd);
void vkb_probe_start_background(void); /* what did not fit the start's budget */
void vkb_probe_write_md(FILE *f);
void vkb_probe_write_json(FILE *f);
int vkb_probe_issues(void (*cb)(void *arg, const char *line), void *arg);
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
    VKB_ERRATUM_EXAMPLE,         /* a no-op, the pattern of a fix */
    VKB_ERRATA_COUNT
};
enum { VKB_ERRATA_PROBE_NOT_RUN, VKB_ERRATA_PROBE_PASS, VKB_ERRATA_PROBE_FAIL };
void vkb_errata_device_init(vkb_srv_table *t);
void vkb_errata_device_destroy(vkb_srv_table *t);
int vkb_errata_on(const vkb_srv_table *t, int id); /* VKB_ERRATUM_* decided on for this device */
const char *vkb_errata_name(int id);
void vkb_errata_set_probe_source(int (*fn)(const char *probe)); /* the probe's record: VKB_ERRATA_PROBE_* */
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
void vkb_srv_env_reload(void);             /* <cache dir>/env overrides, read at each vkCreateDevice */
const char *vkb_srv_env(const char *name); /* an override, else getenv */
void vkb_pcache_device_init(vkb_srv_table *t);
void vkb_pcache_device_destroy(vkb_srv_table *t);

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

/* server_instance.c */
void vkb_selftest_all(void);
const vkb_pd_knowledge *vkb_pd_lookup(const vkb_dispatch *dt, VkPhysicalDevice pd);
extern vkb_pd_knowledge vkb_pds[VKB_MAX_PDS];
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

#endif
