/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * SPIR-V fixes for Mali's shader compiler, applied to every module the app hands the driver.
 *
 * DXVK (1.7.3 and later) guards texture reads with OpSelect(cond, texel, vec4(0)) where cond is an
 * OpConstantComposite of OpSpecConstantTrue/False. Mali's compiler folds that composite as if it
 * were a plain constant - taking the spec constants at their default - and every texture reads
 * black. Declaring the composite OpSpecConstantComposite (same operands, same length) keeps it
 * specializable and the fold away. Found by leegao (bionic-vulkan-wrapper issue #93,
 * "OpCompositeConstant" write-up, 2025-08-10); Vortek patches the same pattern.
 *
 * The code is patched in place: it lives in the server's receive buffer (vkCreateShaderModule's
 * pCode, or a VkShaderModuleCreateInfo chained to a pipeline stage), so nothing is copied.
 * On for ARM GPUs; VKBRIDGE_SPIRV_FIX=1/0 forces it (host tests).
 *
 * Also here, for the same GPUs and Samsung's Xclipse: Mali (r44) fails to build a compute pipeline
 * whose layout holds immutable YCbCr samplers (VK_ERROR_INITIALIZATION_FAILED), and gamescope's
 * compositing layout has 16 of them; the Xclipse 960 fails the same pipelines with
 * VK_ERROR_UNKNOWN (135 of gamescope's 144, every game black with sound, MaliBridge#20). In compute
 * bindings they are replaced by one plain linear sampler: RGB content composites as before; only
 * NV12 surfaces (video) lose their YCbCr conversion. VKBRIDGE_YCBCR_SWAP=1/0 forces it. On every
 * GPU the samplers that stay are counted per layout, so the first compute pipeline the driver
 * refuses is logged with the number its layout holds (vkb_spvfix_compute_failed, from the crash
 * guard, which sees every pipeline's result already). VKBRIDGE_FAKE_MISSING=ycbcrcompute (host
 * tests) imitates the refusal beneath every layer (vkb_spvfix_fake_init).
 *
 * And: geometry and tessellation shaders get their loose per-vertex built-ins folded into
 * gl_PerVertex blocks (spirv_pervertex.c); loose ones crash Mali's compiler (tablet run 8, Steam's
 * first Zink pipeline). Those modules are rewritten into new code, not in place.
 *
 * And, with its own switch: workgroup variables declared with an initializer zeroed by one invocation
 * of each workgroup (spirv_wgzero.h): Mali's own initialization of them cost Crash Bandicoot 4's
 * DXVK 3.x compute shaders 69 of 186 ms of GPU time a frame. On for ARM GPUs; VKBRIDGE_WG_ZERO=1/0
 * forces it.
 *
 * And, with its own switch: an optimization barrier after every left shift by a constant
 * (spirv_shlbarrier.c, Lee Gao's MaliOptimizationBarrierPass): Mali's compiler folds such a shift
 * into the memory accesses around it and can turn them speculative - GPU faults, a lost device.
 * On for ARM GPUs (as in bionic-vulkan-wrapper, where it is on by default for Arm and Qualcomm);
 * VKBRIDGE_SHL_BARRIER=1/0 forces it (on a host GPU it alone installs these wrappers, without the
 * fixes above). Its output is named by the code it came from (vkb_errata_spv_origin), so the hang
 * detector's dumps and stuck-pipeline lists keep their names.
 *
 * And, off unless asked for (VKBRIDGE_SHARED_BARRIER=1 or 2, a per-game preset key): workgroup
 * barriers between shared-memory writes and the reads after them in compute shaders
 * (spirv_shmbarrier.c; 2 also inside workgroup-uniform ifs and loops, spirv_uniform.c), for games whose shaders count on 32-wide subgroups in lockstep where
 * Mali's are 16 wide (Resident Evil 2's occlusion culling). Its output is named by the code it
 * came from too.
 *
 * And, off unless asked for (VKBRIDGE_WAVE32=1, a per-game preset key; on only where the GPU's
 * subgroups are narrower than 32, =force anywhere): compute shaders whose subgroup operations
 * count on 32 lanes get virtual 32-lane subgroups done through shared memory (spirv_wave32.c),
 * for D3D12 games made for 32-wide waves. After shm_barrier; named by the code it came from too.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "server_errata.h"
#include "spirv_pervertex.h"
#include "spirv_shlbarrier.h"
#include "spirv_lockstep.h"
#include "spirv_shmbarrier.h"
#include "spirv_wave32.h"
#include "spirv_wgzero.h"

#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define OP_SPEC_CONSTANT_TRUE 48
#define OP_SPEC_CONSTANT_FALSE 49
#define OP_SPEC_CONSTANT 50
#define OP_SPEC_CONSTANT_COMPOSITE 51
#define OP_SPEC_CONSTANT_OP 52
#define OP_CONSTANT_COMPOSITE 44
#define OP_FUNCTION 54

/* The last modules created, kept so a pipeline the driver refuses can be dumped for a look
 * (<pipeline cache dir>/failed-<process>-<pid>-d<device>-<n>-<stage>.spv, dump_code): the driver says nothing about why. */
#define KEEP 64
static pthread_mutex_t dump_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct kept_module {
    VkShaderModule module;
    uint32_t *code;
    size_t size;
} kept_module;

typedef struct vkb_spvfix {
    PFN_vkCreateShaderModule create_module;
    PFN_vkCreateGraphicsPipelines create_gfx;
    PFN_vkCreateComputePipelines create_compute;
    unsigned long patched; /* composites changed, for the log */
    pthread_mutex_t lock;
    kept_module kept[KEEP];
    unsigned next_kept, dumped;
    PFN_vkCreateSampler create_sampler;
    PFN_vkDestroySampler destroy_sampler;
    PFN_vkCreateDescriptorSetLayout create_dsl;
    VkSampler ycbcr[64]; /* samplers created with a YCbCr conversion */
    unsigned nycbcr;
    VkSampler plain;     /* their stand-in in compute layouts */
    int ycbcr_swap;      /* VKBRIDGE_YCBCR_SWAP: the stand-in used (else the samplers are only counted) */
    PFN_vkDestroyDescriptorSetLayout destroy_dsl;
    PFN_vkCreatePipelineLayout create_pl;
    PFN_vkDestroyPipelineLayout destroy_pl;
    struct ycbcr_layout {
        uint64_t layout;
        unsigned n;
    } dsl_y[64], pl_y[64]; /* set and pipeline layouts that reached the driver with YCbCr compute samplers, how many */
    unsigned ndsl_y, npl_y;
    int told_fail;         /* the first refused compute pipeline logged */
    int fixes;           /* VKBRIDGE_SPIRV_FIX: the composites, per-vertex blocks, YCbCr samplers */
    int shl;             /* VKBRIDGE_SHL_BARRIER: barriers after constant left shifts */
    int wgzero;          /* VKBRIDGE_WG_ZERO: workgroup variable initializers done by one invocation */
    unsigned long wgz_modules, wgz_vars;
    unsigned long shl_modules, shl_barriers, shl_seen;
    int shm;             /* VKBRIDGE_SHARED_BARRIER: barriers between shared-memory writes and reads
                            (1: at the top level, 2: also inside workgroup-uniform ifs and loops) */
    int shm_dump;        /* VKBRIDGE_SHARED_BARRIER_DUMP=1: the compute modules with shared memory saved */
    unsigned long shm_inside, shm_constructs, shm_uniform;
    unsigned long shm_modules, shm_control, shm_memory, shm_seen, shm_shared, shm_subgroup, shm_saved;
    unsigned long shm_adapts, shm_anysize; /* of shm_shared: the subgroup operations adapt to the size / only BroadcastFirst, Elect */
    unsigned long dump_max;                /* VKBRIDGE_SHARED_BARRIER_DUMP_MAX: at most this many saved (default 1024) */
    int wave32;                            /* VKBRIDGE_WAVE32: 32-lane subgroups emulated in compute shaders */
    unsigned shared_limit, hw_subgroup;    /* maxComputeSharedMemorySize, the GPU's subgroup size */
    unsigned long w32_seen, w32_fixed, w32_adapts, w32_anysize, w32_modules, w32_ops, w32_barriers, w32_left;
    char *shm_only, *shm_skip;             /* VKBRIDGE_SHARED_BARRIER_ONLY / _SKIP: app hashes (or their first hex digits) */
    char *shm_lockstep;                    /* VKBRIDGE_SHARED_BARRIER_LOCKSTEP: app hashes whose strided loops get a uniform
                                              trip count first (spirv_lockstep.h), or "all" */
    unsigned long shm_lock_modules, shm_lock_loops;
    unsigned long shm_filtered;            /* modules shm_barrier would have changed, left by those lists */
    int subst;                             /* VKBRIDGE_SPIRV_SUBST=1: <cache dir>/spv-subst/<app hash>.spv used in place of a module */
    unsigned long subst_n;
} vkb_spvfix;

static vkb_spvfix *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->spvfix : NULL;
}

/* Patches one module in place; returns the number of composites changed. */
static unsigned fix_module(uint32_t *w, size_t bytes)
{
    size_t n = bytes / 4;
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 24)) return 0;
    uint8_t *spec = calloc(bound, 1);
    if (!spec) return 0;
    unsigned changed = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        if (op == OP_FUNCTION) break; /* constants all come before the first function */
        if ((op == OP_SPEC_CONSTANT_TRUE || op == OP_SPEC_CONSTANT_FALSE || op == OP_SPEC_CONSTANT ||
             op == OP_SPEC_CONSTANT_COMPOSITE || op == OP_SPEC_CONSTANT_OP) && len >= 3 && w[i + 2] < bound) {
            spec[w[i + 2]] = 1;
        } else if (op == OP_CONSTANT_COMPOSITE && len >= 3 && w[i + 2] < bound) {
            int any = 0;
            for (uint32_t k = 3; k < len; k++) any |= w[i + k] < bound && spec[w[i + k]];
            if (any) {
                /* A composite of spec constants is itself one: declared so, it is no longer folded. */
                w[i] = (len << 16) | OP_SPEC_CONSTANT_COMPOSITE;
                spec[w[i + 2]] = 1;
                changed++;
            }
        }
        i += len;
    }
    free(spec);
    return changed;
}

static void note(vkb_spvfix *f, unsigned changed)
{
    if (!changed) return;
    unsigned long before = __atomic_fetch_add(&f->patched, changed, __ATOMIC_RELAXED);
    if (!before) VKB_INFO("SPIR-V fix: spec-constant composites redeclared for Mali's compiler (first in this device)");
}

static void fix_stages(vkb_spvfix *f, uint32_t n, const VkPipelineShaderStageCreateInfo *st)
{
    for (uint32_t i = 0; i < n; i++)
        for (const VkBaseInStructure *b = st[i].pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                const VkShaderModuleCreateInfo *mi = (const void *)b;
                note(f, fix_module((uint32_t *)mi->pCode, mi->codeSize));
            }
}

/* A stage's short name in a dump's file name */
static const char *stage_tag(VkShaderStageFlagBits st)
{
    switch (st) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vs";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tcs";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tes";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "gs";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "fs";
    case VK_SHADER_STAGE_COMPUTE_BIT: return "cs";
    default: return "stage";
    }
}

static int write_file(const char *path, const uint32_t *code, size_t size)
{
    FILE *o = fopen(path, "wb");
    if (!o) return 0;
    int ok = fwrite(code, 1, size, o) == size;
    return fclose(o) == 0 && ok;
}

/* failed-<process>-<pid>-d<device>-<n>-<stage>.spv in the pipeline cache directory (pulled with
 * run-as), and a copy in the session folder's failed-spv/ (the server log's directory: Send logs
 * and Share logs zip that folder, so the shaders come with the logs). Named per process and
 * device: one counter per device used to make Xwayland's dumps overwrite the ones Steam's update
 * window had made seconds before (issue #11). 16 per device, as before; the cache directory keeps
 * the newest DUMPS_KEPT of them over sessions. */
#define DUMPS_KEPT 64
static void prune_dumps(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dump {
        char name[256];
        time_t t;
    } *all = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (strncmp(e->d_name, "failed-", 7) || l < 11 || strcmp(e->d_name + l - 4, ".spv") || l >= 256) continue;
        char p[1000];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            struct dump *m = realloc(all, (cap = cap ? cap * 2 : 64) * sizeof(*all));
            if (!m) break;
            all = m;
        }
        snprintf(all[n].name, sizeof(all[n].name), "%s", e->d_name);
        all[n++].t = st.st_mtime;
    }
    closedir(d);
    while (n > DUMPS_KEPT) { /* the oldest out, one at a time (a few files) */
        size_t old = 0;
        for (size_t i = 1; i < n; i++)
            if (all[i].t < all[old].t) old = i;
        char p[1000];
        snprintf(p, sizeof(p), "%s/%s", dir, all[old].name);
        unlink(p);
        all[old] = all[--n];
    }
    free(all);
}

static void dump_code(vkb_spvfix *f, const uint32_t *code, size_t size, const char *why, const char *tag)
{
    const char *dir = vkb_pcache_dir();
    if (!dir) return;
    pthread_mutex_lock(&dump_lock);
    if (f->dumped >= 16) {
        pthread_mutex_unlock(&dump_lock);
        return;
    }
    unsigned nth = f->dumped++;
    const vkb_srv_table *t = vkb_srv_current_table();
    const vkb_srv_proc *pr = t ? t->proc : vkb_srv_current_proc();
    char who[40] = "";
    for (const char *c = pr && pr->comm[0] ? pr->comm : "server"; *c && strlen(who) < 31; c++) {
        size_t l = strlen(who);
        who[l] = (*c == '/' || *c == ' ' || *c == '.' || (unsigned char)*c < 32) ? '_' : *c;
        who[l + 1] = 0;
    }
    char name[160];
    snprintf(name, sizeof(name), "failed-%s-%d-d%u-%u-%s.spv", who, pr ? pr->pid : 0, t ? t->id : 0, nth, tag);
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (nth == 0) prune_dumps(dir);
    int ok = write_file(path, code, size);
    if (!ok && errno == ENOENT && vkb_cache_dir_ensure()) ok = write_file(path, code, size); /* the cache directory was removed */
    char sdir[620], spath[800];
    int copied = 0;
    if (vkb_report_dir(sdir, sizeof(sdir)) && *sdir) {
        snprintf(spath, sizeof(spath), "%s/failed-spv", sdir);
        mkdir(spath, 0700);
        snprintf(spath, sizeof(spath), "%s/failed-spv/%s", sdir, name);
        copied = write_file(spath, code, size);
    }
    pthread_mutex_unlock(&dump_lock);
    if (ok || copied) VKB_ERR("%s: shader saved to %s%s", why, ok ? path : spath, ok && copied ? " (and to the session folder's failed-spv/)" : "");
}

/* The per-vertex block rewrite of one module's code; NULL when it is not needed (or not possible). */
static uint32_t *wrap_pervertex(const uint32_t *code, size_t size, size_t *out_size)
{
    uint32_t *out = NULL;
    const char *why = NULL;
    if (vkb_spv_wrap_pervertex(code, size, &out, out_size, &why)) {
        static int told;
        if (!__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
            VKB_INFO("SPIR-V fix: per-vertex built-ins of geometry/tessellation shaders folded into gl_PerVertex blocks");
        return out;
    }
    if (why) VKB_WARN("SPIR-V fix: a geometry/tessellation shader kept its loose built-ins (%s); Mali may crash on it", why);
    return NULL;
}

/* A module's app hash in a comma-separated list of hashes, each the whole 16 hex digits or its
 * first ones (at least 4) */
static int hash_listed(const char *list, uint64_t h)
{
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    for (const char *p = list; p && *p;) {
        while (*p == ',' || *p == ' ') p++;
        size_t n = strcspn(p, ", ");
        if (n >= 4 && n <= 16 && !strncasecmp(p, hex, n)) return 1;
        p += n;
    }
    return 0;
}

/* VKBRIDGE_SPIRV_SUBST=1 (a debugging switch): a module whose app hash names a file in <pipeline
 * cache dir>/spv-subst/ (<16 hex digits>.spv, pushed there with run-as) gets that file's code in
 * its place, before the other rewrites; for trying a changed shader without a new build. NULL
 * when there is none (or it is not SPIR-V). */
static uint32_t *subst_code(vkb_spvfix *f, const uint32_t *code, size_t size, size_t *out_size)
{
    const char *dir = vkb_pcache_dir();
    if (!f->subst || !dir) return NULL;
    uint64_t h = vkb_spv_app_hash(code, size);
    char path[700];
    snprintf(path, sizeof(path), "%s/spv-subst/%016llx.spv", dir, (unsigned long long)h);
    FILE *in = fopen(path, "rb");
    if (!in) return NULL;
    uint32_t *out = NULL;
    long n = fseek(in, 0, SEEK_END) == 0 ? ftell(in) : -1;
    if (n >= 20 && n < (64l << 20) && !(n & 3) && fseek(in, 0, SEEK_SET) == 0 && (out = malloc((size_t)n)) &&
        fread(out, 1, (size_t)n, in) == (size_t)n && out[0] == 0x07230203u) {
        *out_size = (size_t)n;
        __atomic_add_fetch(&f->subst_n, 1, __ATOMIC_RELAXED);
        VKB_INFO("SPIR-V subst: module %016llx (%zu bytes) replaced by spv-subst/%016llx.spv (%ld bytes)", (unsigned long long)h, size,
                 (unsigned long long)h, n);
    } else {
        VKB_WARN("SPIR-V subst: %s is not a SPIR-V module; not used", path);
        free(out);
        out = NULL;
    }
    fclose(in);
    return out;
}

/* The wg_zero rewrite of one module's code; NULL when nothing changes. */
static uint32_t *wg_zero(vkb_spvfix *f, const uint32_t *code, size_t size, size_t *out_size)
{
    if (!f->wgzero) return NULL;
    uint32_t *out = NULL;
    unsigned vars = 0;
    const char *why = NULL;
    int r = vkb_spv_wg_zero(code, size, &out, out_size, &vars, &why);
    if (r < 0) {
        VKB_ONCE(VKB_LOG_WARN, "SPIR-V fix: wg_zero: a module left as it is (%s)", why);
        return NULL;
    }
    if (r == 0) return NULL;
    unsigned long before = __atomic_fetch_add(&f->wgz_modules, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->wgz_vars, vars, __ATOMIC_RELAXED);
    if (!before) VKB_INFO("SPIR-V fix: wg_zero: workgroup variable initializers done by one invocation (first module: %u)", vars);
    return out;
}

/* The shl_barrier rewrite of one module's code; NULL when nothing changes. */
static uint32_t *shl_barrier(vkb_spvfix *f, const uint32_t *code, size_t size, size_t *out_size)
{
    if (!f->shl) return NULL;
    __atomic_add_fetch(&f->shl_seen, 1, __ATOMIC_RELAXED);
    uint32_t *out = NULL;
    vkb_shlbarrier_stats st;
    const char *why = NULL;
    int r = vkb_spv_shl_barrier(code, size, &out, out_size, &st, &why);
    if (r < 0) {
        VKB_ONCE(VKB_LOG_WARN, "SPIR-V fix: shl_barrier: a module left as it is (%s)", why);
        return NULL;
    }
    if (r == 0) return NULL;
    unsigned long before = __atomic_fetch_add(&f->shl_modules, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->shl_barriers, st.scalar + st.vector, __ATOMIC_RELAXED);
    if (!before)
        VKB_INFO("SPIR-V fix: shl_barrier: optimization barriers after constant left shifts (first module: %u scalar, %u vector)",
                 st.scalar, st.vector);
    return out;
}

/* VKBRIDGE_SHARED_BARRIER_DUMP=1: a compute module with shared memory, or (with wave32 on) with
 * subgroup operations, saved as the app gave it, for a look at its pattern (<pipeline cache
 * dir>/shm-spirv/<app hash>-<c>c<i>i<e>e<m>m<tags>.spv: the barriers shm_barrier put in (top
 * level, inside uniform constructs, at entries, memory); tags: -sg its subgroup operations may
 * count on a size, -ad they adapt to it, -bf only BroadcastFirst / Elect, -w32 wave32 emulated
 * it, -w32left it could not, -left shm_barrier could not; at most VKBRIDGE_SHARED_BARRIER_DUMP_MAX) */
static void save_shared(vkb_spvfix *f, const uint32_t *code, size_t size, int r, const vkb_shmbarrier_stats *st, int cls, int w32)
{
    const char *dir = vkb_pcache_dir();
    if (!dir || __atomic_fetch_add(&f->shm_saved, 1, __ATOMIC_RELAXED) >= f->dump_max) return;
    char path[700];
    snprintf(path, sizeof(path), "%s/shm-spirv", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/shm-spirv/%016llx-%uc%ui%ue%um%s%s%s.spv", dir, (unsigned long long)vkb_spv_app_hash(code, size),
             r > 0 ? st->control : 0, r > 0 ? st->inside : 0, r > 0 ? st->entry : 0, r > 0 ? st->memory : 0,
             cls == VKB_SG_FIXED ? "-sg" : cls == VKB_SG_ADAPTS ? "-ad" : cls == VKB_SG_ANY_SIZE ? "-bf" : "",
             w32 > 0 ? "-w32" : w32 < 0 ? "-w32left" : "", r < 0 ? "-left" : "");
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fwrite(code, 1, size, o);
    fclose(o);
}

/* The shm_barrier rewrite of one module's code; NULL when nothing changes. *r: the pass's
 * answer, *cls: the class of its subgroup operations when it has shared memory (else -1) */
static uint32_t *shm_barrier(vkb_spvfix *f, const uint32_t *code, size_t size, size_t *out_size, vkb_shmbarrier_stats *st, int *r,
                             int *cls)
{
    memset(st, 0, sizeof(*st));
    *r = 0, *cls = -1;
    if (!f->shm) return NULL;
    __atomic_add_fetch(&f->shm_seen, 1, __ATOMIC_RELAXED);
    uint32_t *out = NULL, *lock = NULL;
    const char *why = NULL;
    const uint32_t *in = code;
    size_t in_size = size, lock_size = 0;
    unsigned nloops = 0;
    if (f->shm_lockstep && (!strcmp(f->shm_lockstep, "all") || hash_listed(f->shm_lockstep, vkb_spv_app_hash(code, size)))) {
        const char *lwhy = NULL;
        int lr = vkb_spv_lockstep_loops(code, size, &lock, &lock_size, &nloops, &lwhy);
        if (lr < 0) VKB_ONCE(VKB_LOG_WARN, "SPIR-V fix: shm_barrier: lockstep: a module's loops left as they are (%s)", lwhy);
        if (lr > 0) in = lock, in_size = lock_size;
    }
    *r = vkb_spv_shm_barrier(in, in_size, f->shm, &out, out_size, st, &why);
    if (lock && *r == 0) /* the loops changed, no barrier added */
        out = lock, *out_size = lock_size, *r = 1;
    else free(lock);
    if (*r > 0 && nloops) {
        __atomic_add_fetch(&f->shm_lock_modules, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&f->shm_lock_loops, nloops, __ATOMIC_RELAXED);
        VKB_INFO("SPIR-V fix: shm_barrier: lockstep: module %016llx: %u strided loop(s) given the trip count of invocation 0, "
                 "then %u control barrier(s) inside uniform constructs, %u at entries, %u memory",
                 (unsigned long long)vkb_spv_app_hash(code, size), nloops, st->inside, st->entry, st->memory);
    }
    if (st->shared) {
        __atomic_add_fetch(&f->shm_shared, 1, __ATOMIC_RELAXED);
        /* BroadcastFirst / Elect alone (vkd3d-proton's uniform descriptor index) give the same
         * result at any size: counted apart; the rest may count on one */
        *cls = vkb_spv_subgroup_class(code, size, NULL);
        if (*cls == VKB_SG_ANY_SIZE) __atomic_add_fetch(&f->shm_anysize, 1, __ATOMIC_RELAXED);
        if (*cls == VKB_SG_ADAPTS) __atomic_add_fetch(&f->shm_adapts, 1, __ATOMIC_RELAXED);
        if (*cls >= VKB_SG_ADAPTS && !__atomic_fetch_add(&f->shm_subgroup, 1, __ATOMIC_RELAXED))
            VKB_INFO("SPIR-V fix: shm_barrier: a compute shader with shared memory also uses subgroup operations (barriers do "
                     "not fix one that counts on a subgroup size this GPU does not have: VKBRIDGE_WAVE32)");
        __atomic_add_fetch(&f->shm_constructs, st->constructs, __ATOMIC_RELAXED);
        __atomic_add_fetch(&f->shm_uniform, st->uniform, __ATOMIC_RELAXED);
    }
    if (*r < 0) {
        VKB_ONCE(VKB_LOG_WARN, "SPIR-V fix: shm_barrier: a compute module left as it is (%s)", why);
        return NULL;
    }
    if (*r == 0) return NULL;
    if (f->shm_only || f->shm_skip) {
        /* only some modules (by app hash): the others keep their code, and their stats say 0 */
        uint64_t h = vkb_spv_app_hash(code, size);
        if ((f->shm_only && !hash_listed(f->shm_only, h)) || (f->shm_skip && hash_listed(f->shm_skip, h))) {
            __atomic_add_fetch(&f->shm_filtered, 1, __ATOMIC_RELAXED);
            free(out);
            *r = 0;
            st->control = st->inside = st->entry = st->memory = 0;
            return NULL;
        }
    }
    unsigned long before = __atomic_fetch_add(&f->shm_modules, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->shm_control, st->control + st->entry + st->inside, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->shm_inside, st->inside, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->shm_memory, st->memory, __ATOMIC_RELAXED);
    if (!before)
        VKB_INFO("SPIR-V fix: shm_barrier: barriers between shared-memory writes and reads in compute shaders, level %d (first "
                 "module: %u control, %u inside uniform constructs, %u at a construct's entry, %u memory)",
                 f->shm, st->control, st->inside, st->entry, st->memory);
    return out;
}

/* The wave32 rewrite of one module's code; NULL when nothing changes. orig: the app's code (its
 * name in the log). *r: the pass's answer. */
static uint32_t *wave32(vkb_spvfix *f, const uint32_t *code, size_t size, const uint32_t *orig, size_t orig_size, size_t *out_size,
                        vkb_wave32_stats *st, int *r)
{
    memset(st, 0, sizeof(*st));
    *r = 0;
    if (!f->wave32) return NULL;
    uint32_t *out = NULL;
    const char *why = NULL;
    *r = vkb_spv_wave32(code, size, f->shared_limit, &out, out_size, st, &why);
    if (!st->compute) return NULL;
    __atomic_add_fetch(&f->w32_seen, 1, __ATOMIC_RELAXED);
    if (st->cls == VKB_SG_FIXED) __atomic_add_fetch(&f->w32_fixed, 1, __ATOMIC_RELAXED);
    if (st->cls == VKB_SG_ADAPTS) __atomic_add_fetch(&f->w32_adapts, 1, __ATOMIC_RELAXED);
    if (st->cls == VKB_SG_ANY_SIZE) __atomic_add_fetch(&f->w32_anysize, 1, __ATOMIC_RELAXED);
    if (*r < 0) {
        /* each of the first 16 by name: which shaders a game still runs 16 wide */
        if (__atomic_fetch_add(&f->w32_left, 1, __ATOMIC_RELAXED) < 16)
            VKB_INFO("SPIR-V fix: wave32: compute module %016llx left as it is (%s; %u invocations)",
                     (unsigned long long)vkb_spv_app_hash(orig, orig_size), why, st->invocations);
        return NULL;
    }
    if (*r == 0) return NULL;
    unsigned long before = __atomic_fetch_add(&f->w32_modules, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->w32_ops, st->ops, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->w32_barriers, st->barriers, __ATOMIC_RELAXED);
    if (!before)
        VKB_INFO("SPIR-V fix: wave32: 32-lane subgroups emulated in compute shaders (first module %016llx: %u subgroup operation(s), "
                 "%u built-in(s), %u barrier(s), %u bytes of shared memory, %u invocations)",
                 (unsigned long long)vkb_spv_app_hash(orig, orig_size), st->ops, st->builtins, st->barriers, st->shared, st->invocations);
    return out;
}

/* The rewrites of one module's code (wg_zero, per-vertex blocks when pervertex, then shl_barrier, then
 * shm_barrier, then wave32): new
 * code (malloc'd) or NULL when nothing changes. shl_barrier's output is named by the code it came
 * from (vkb_errata_spv_origin); the per-vertex rewrite's keeps its own name, as before. */
static uint32_t *rewrite(vkb_spvfix *f, const uint32_t *code, size_t size, int pervertex, size_t *out_size)
{
    size_t s1 = 0, s2 = 0;
    uint32_t *c1 = subst_code(f, code, size, &s1), *c2;
    if (c1) vkb_errata_spv_origin(c1, s1, code, size);
    if ((c2 = wg_zero(f, c1 ? c1 : code, c1 ? s1 : size, &s2))) {
        vkb_errata_spv_origin(c2, s2, c1 ? c1 : code, c1 ? s1 : size);
        free(c1);
        c1 = c2, s1 = s2;
    }
    if (f->fixes && pervertex && (c2 = wrap_pervertex(c1 ? c1 : code, c1 ? s1 : size, &s2))) {
        free(c1);
        c1 = c2, s1 = s2;
    }
    c2 = shl_barrier(f, c1 ? c1 : code, c1 ? s1 : size, &s2);
    if (c2) {
        vkb_errata_spv_origin(c2, s2, c1 ? c1 : code, c1 ? s1 : size);
        free(c1);
        c1 = c2, s1 = s2;
    }
    vkb_shmbarrier_stats ss;
    int sr, cls;
    c2 = shm_barrier(f, c1 ? c1 : code, c1 ? s1 : size, &s2, &ss, &sr, &cls);
    if (c2) {
        vkb_errata_spv_origin(c2, s2, c1 ? c1 : code, c1 ? s1 : size);
        free(c1);
        c1 = c2, s1 = s2;
    }
    vkb_wave32_stats ws;
    int wr;
    c2 = wave32(f, c1 ? c1 : code, c1 ? s1 : size, code, size, &s2, &ws, &wr);
    if (c2) {
        vkb_errata_spv_origin(c2, s2, c1 ? c1 : code, c1 ? s1 : size);
        free(c1);
        c1 = c2, s1 = s2;
    }
    if (f->shm_dump && (ss.shared || (ws.compute && ws.cls >= VKB_SG_ADAPTS)))
        save_shared(f, code, size, sr, &ss, ss.shared ? cls : ws.cls, wr);
    if (c1) *out_size = s1;
    return c1;
}

/* New code a pipeline call uses in place of a stage's inline module; freed after the call */
typedef struct {
    uint32_t **p;
    unsigned n, cap;
} repl_list;

static void repl_add(repl_list *l, uint32_t *code)
{
    if (l->n == l->cap) {
        unsigned cap = l->cap ? l->cap * 2 : 16;
        uint32_t **p = realloc(l->p, cap * sizeof(*p));
        if (!p) return; /* (never: the new code stays in use, leaked) */
        l->p = p, l->cap = cap;
    }
    l->p[l->n++] = code;
}

static void repl_free(repl_list *l)
{
    for (unsigned k = 0; k < l->n; k++) free(l->p[k]);
    free(l->p);
}

/* A stage's inline modules rewritten (the create infos are the server's own copies: changed in place) */
static void rewrite_stage(vkb_spvfix *f, const VkPipelineShaderStageCreateInfo *st, repl_list *l)
{
    int pervertex = (st->stage & (VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) != 0;
    if (!f->shl && !f->shm && !f->wave32 && !f->subst && !f->wgzero && !(f->fixes && pervertex)) return;
    for (VkBaseInStructure *b = (VkBaseInStructure *)st->pNext; b; b = (VkBaseInStructure *)b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
        VkShaderModuleCreateInfo *mi = (VkShaderModuleCreateInfo *)b;
        size_t sz = 0;
        uint32_t *code = rewrite(f, mi->pCode, mi->codeSize, pervertex, &sz);
        if (!code) continue;
        repl_add(l, code);
        mi->pCode = code;
        mi->codeSize = sz;
    }
}

static VkResult VKAPI_CALL fx_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *in_ci, const VkAllocationCallbacks *a,
                                                 VkShaderModule *out)
{
    vkb_spvfix *f = cur();
    if (f->fixes) note(f, fix_module((uint32_t *)in_ci->pCode, in_ci->codeSize));
    VkShaderModuleCreateInfo wrapped;
    const VkShaderModuleCreateInfo *ci = in_ci;
    size_t wsize = 0;
    uint32_t *wcode = rewrite(f, in_ci->pCode, in_ci->codeSize, 1, &wsize);
    if (wcode) {
        wrapped = *in_ci;
        wrapped.pCode = wcode;
        wrapped.codeSize = wsize;
        ci = &wrapped;
    }
    VkResult r = f->create_module(device, ci, a, out);
    if (r != VK_SUCCESS) {
        dump_code(f, ci->pCode, ci->codeSize, "vkCreateShaderModule failed", "module");
        free(wcode);
        return r;
    }
    uint32_t *copy = malloc(ci->codeSize);
    if (copy) {
        memcpy(copy, ci->pCode, ci->codeSize);
        pthread_mutex_lock(&f->lock);
        kept_module *k = &f->kept[f->next_kept++ % KEEP];
        free(k->code);
        *k = (kept_module){*out, copy, ci->codeSize};
        pthread_mutex_unlock(&f->lock);
    }
    free(wcode);
    return r;
}

static void dump_stage(vkb_spvfix *f, const VkPipelineShaderStageCreateInfo *st, VkResult r)
{
    char why[96];
    snprintf(why, sizeof(why), "pipeline creation failed (%d), stage 0x%x", r, st->stage);
    for (const VkBaseInStructure *b = st->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *mi = (const void *)b;
            dump_code(f, mi->pCode, mi->codeSize, why, stage_tag(st->stage));
            return;
        }
    pthread_mutex_lock(&f->lock);
    /* newest first: a destroyed module's handle comes back for the next one made (issue #11's
     * dumps showed one handle as the VS of one pipeline and the FS of the next: the oldest entry
     * with the handle was taken) */
    for (int j = 1; j <= KEEP; j++) {
        kept_module *k = &f->kept[(f->next_kept - j) % KEEP];
        if (k->module == st->module && k->code) {
            dump_code(f, k->code, k->size, why, stage_tag(st->stage));
            break;
        }
    }
    pthread_mutex_unlock(&f->lock);
}

static VkResult VKAPI_CALL fx_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                      const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                      VkPipeline *out)
{
    vkb_spvfix *f = cur();
    /* Inline modules (in the request's buffer) that need a rewrite (per-vertex blocks, shl_barrier)
     * get new code for the call. */
    repl_list repl = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (f->fixes) fix_stages(f, ci[i].stageCount, ci[i].pStages);
        for (uint32_t s = 0; s < ci[i].stageCount; s++) rewrite_stage(f, &ci[i].pStages[s], &repl);
    }
    VkResult r = f->create_gfx(device, cache, n, ci, a, out);
    if (r < 0)
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t s = 0; s < ci[i].stageCount; s++) dump_stage(f, &ci[i].pStages[s], r);
    repl_free(&repl);
    return r;
}

static VkResult VKAPI_CALL fx_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                     const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                     VkPipeline *out)
{
    vkb_spvfix *f = cur();
    repl_list repl = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (f->fixes) fix_stages(f, 1, &ci[i].stage);
        rewrite_stage(f, &ci[i].stage, &repl);
    }
    VkResult r = f->create_compute(device, cache, n, ci, a, out);
    if (r < 0)
        for (uint32_t i = 0; i < n; i++) dump_stage(f, &ci[i].stage, r);
    repl_free(&repl);
    return r;
}

static VkResult VKAPI_CALL fx_CreateSampler(VkDevice device, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out)
{
    vkb_spvfix *f = cur();
    VkResult r = f->create_sampler(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) {
            pthread_mutex_lock(&f->lock);
            if (f->nycbcr < sizeof(f->ycbcr) / sizeof(f->ycbcr[0])) f->ycbcr[f->nycbcr++] = *out;
            if (!f->plain && f->ycbcr_swap) {
                VkSamplerCreateInfo pi = *ci;
                pi.pNext = NULL;
                if (f->create_sampler(device, &pi, NULL, &f->plain) != VK_SUCCESS) f->plain = VK_NULL_HANDLE;
            }
            pthread_mutex_unlock(&f->lock);
        }
    return r;
}

static void VKAPI_CALL fx_DestroySampler(VkDevice device, VkSampler s, const VkAllocationCallbacks *a)
{
    vkb_spvfix *f = cur();
    pthread_mutex_lock(&f->lock);
    for (unsigned i = 0; i < f->nycbcr; i++)
        if (f->ycbcr[i] == s) f->ycbcr[i] = f->ycbcr[--f->nycbcr];
    pthread_mutex_unlock(&f->lock);
    f->destroy_sampler(device, s, a);
}

static int is_ycbcr(vkb_spvfix *f, VkSampler s)
{
    for (unsigned i = 0; i < f->nycbcr; i++)
        if (f->ycbcr[i] == s) return 1;
    return 0;
}

/* A layout's count in one of the lists; 0 when not there. Under f->lock. */
static unsigned y_find(const struct ycbcr_layout *l, unsigned n, uint64_t h)
{
    for (unsigned i = 0; i < n; i++)
        if (l[i].layout == h) return l[i].n;
    return 0;
}

static void y_add(struct ycbcr_layout *l, unsigned *n, unsigned cap, uint64_t h, unsigned count)
{
    for (unsigned i = 0; i < *n; i++)
        if (l[i].layout == h) {
            l[i].n = count;
            return;
        }
    if (*n < cap) l[(*n)++] = (struct ycbcr_layout){h, count};
}

static void y_drop(struct ycbcr_layout *l, unsigned *n, uint64_t h)
{
    for (unsigned i = 0; i < *n; i++)
        if (l[i].layout == h) {
            l[i] = l[--*n];
            return;
        }
}

static VkResult VKAPI_CALL fx_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                        const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
    vkb_spvfix *f = cur();
    pthread_mutex_lock(&f->lock);
    int any = 0;
    unsigned kept = 0; /* YCbCr samplers left in compute bindings: what the driver gets */
    for (uint32_t i = 0; i < ci->bindingCount && f->nycbcr; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        if (!(b->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) || !b->pImmutableSamplers) continue;
        for (uint32_t k = 0; k < b->descriptorCount; k++)
            if (is_ycbcr(f, b->pImmutableSamplers[k])) {
                if (f->ycbcr_swap && f->plain) {
                    /* The binding array lives in the request's buffer: swapped in place. */
                    ((VkSampler *)b->pImmutableSamplers)[k] = f->plain;
                    any = 1;
                } else {
                    kept++;
                }
            }
    }
    pthread_mutex_unlock(&f->lock);
    static int told;
    if (any && !__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
        VKB_WARN("compute layout: immutable YCbCr samplers replaced (this GPU cannot build such pipelines); NV12 surfaces lose YCbCr "
                 "conversion");
    VkResult r = f->create_dsl(device, ci, a, out);
    if (r == VK_SUCCESS && kept) {
        pthread_mutex_lock(&f->lock);
        y_add(f->dsl_y, &f->ndsl_y, 64, (uint64_t)(uintptr_t)*out, kept);
        pthread_mutex_unlock(&f->lock);
    }
    return r;
}

static void VKAPI_CALL fx_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout l, const VkAllocationCallbacks *a)
{
    vkb_spvfix *f = cur();
    if (__atomic_load_n(&f->ndsl_y, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&f->lock);
        y_drop(f->dsl_y, &f->ndsl_y, (uint64_t)(uintptr_t)l);
        pthread_mutex_unlock(&f->lock);
    }
    f->destroy_dsl(device, l, a);
}

static VkResult VKAPI_CALL fx_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a,
                                                   VkPipelineLayout *out)
{
    vkb_spvfix *f = cur();
    VkResult r = f->create_pl(device, ci, a, out);
    if (r != VK_SUCCESS || !__atomic_load_n(&f->ndsl_y, __ATOMIC_RELAXED)) return r;
    pthread_mutex_lock(&f->lock);
    unsigned n = 0;
    for (uint32_t i = 0; i < ci->setLayoutCount; i++) n += y_find(f->dsl_y, f->ndsl_y, (uint64_t)(uintptr_t)ci->pSetLayouts[i]);
    if (n) y_add(f->pl_y, &f->npl_y, 64, (uint64_t)(uintptr_t)*out, n);
    pthread_mutex_unlock(&f->lock);
    return r;
}

static void VKAPI_CALL fx_DestroyPipelineLayout(VkDevice device, VkPipelineLayout l, const VkAllocationCallbacks *a)
{
    vkb_spvfix *f = cur();
    if (__atomic_load_n(&f->npl_y, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&f->lock);
        y_drop(f->pl_y, &f->npl_y, (uint64_t)(uintptr_t)l);
        pthread_mutex_unlock(&f->lock);
    }
    f->destroy_pl(device, l, a);
}

/* The immutable YCbCr samplers a pipeline layout gave the driver in its compute bindings. */
static unsigned layout_ycbcr(vkb_srv_table *t, VkPipelineLayout l)
{
    vkb_spvfix *f = t ? t->spvfix : NULL;
    if (!f || !__atomic_load_n(&f->npl_y, __ATOMIC_RELAXED)) return 0;
    pthread_mutex_lock(&f->lock);
    unsigned n = y_find(f->pl_y, f->npl_y, (uint64_t)(uintptr_t)l);
    pthread_mutex_unlock(&f->lock);
    return n;
}

void vkb_spvfix_compute_failed(vkb_srv_table *t, uint32_t n, const VkComputePipelineCreateInfo *ci, VkResult r)
{
    vkb_spvfix *f = t ? t->spvfix : NULL;
    if (!f || __atomic_exchange_n(&f->told_fail, 1, __ATOMIC_RELAXED)) return;
    unsigned y = 0;
    for (uint32_t i = 0; i < n && !y; i++) y = layout_ycbcr(t, ci[i].layout);
    VKB_WARN("device %u: compute pipeline failed (%d): layout has %u immutable YCbCr sampler(s)%s (first failure; YCbCr compute "
             "samplers %s)", t->id, r, y, n > 1 ? " (a batch)" : "", f->ycbcr_swap ? "swapped" : "kept");
}

/* VKBRIDGE_FAKE_MISSING=ycbcrcompute (host tests): a driver that cannot build a compute pipeline
 * whose layout holds immutable YCbCr samplers, as the Xclipse 960 (VK_ERROR_UNKNOWN, after its
 * compiler has run). Installed beneath every layer, before the crash guard, so the bridge's
 * report and logs see it as a driver failure; the count is the one the layouts reached the
 * driver with (after the swap). */
static struct {
    VkDevice device;
    vkb_srv_table *t;
    PFN_vkCreateComputePipelines next;
} fake_y[16];

static VkResult VKAPI_CALL fake_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                      const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                      VkPipeline *out)
{
    int s = 0;
    for (int i = 0; i < 16; i++)
        if (__atomic_load_n(&fake_y[i].device, __ATOMIC_ACQUIRE) == device) {
            s = i;
            break;
        }
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        if (layout_ycbcr(fake_y[s].t, ci[i].layout)) {
            out[i] = VK_NULL_HANDLE;
            r = VK_ERROR_UNKNOWN;
            continue;
        }
        VkResult ri = fake_y[s].next(device, cache, 1, &ci[i], a, &out[i]);
        if (ri < 0) out[i] = VK_NULL_HANDLE;
        if (ri != VK_SUCCESS && r == VK_SUCCESS) r = ri;
    }
    return r;
}

void vkb_spvfix_fake_init(vkb_srv_table *t)
{
    if (!vkb_emu_faked("ycbcrcompute") || !t->real.vkCreateComputePipelines) return;
    for (int i = 0; i < 16; i++)
        if (!fake_y[i].device || fake_y[i].device == t->device) {
            fake_y[i].t = t;
            fake_y[i].next = t->real.vkCreateComputePipelines;
            __atomic_store_n(&fake_y[i].device, t->device, __ATOMIC_RELEASE);
            if (t->dt.vkCreateComputePipelines == t->real.vkCreateComputePipelines) t->dt.vkCreateComputePipelines = fake_CreateComputePipelines;
            t->real.vkCreateComputePipelines = fake_CreateComputePipelines;
            VKB_INFO("device %u: imitating a driver that cannot build compute pipelines with immutable YCbCr samplers "
                     "(VKBRIDGE_FAKE_MISSING=ycbcrcompute)", t->id);
            return;
        }
}

void vkb_spvfix_device_init(vkb_srv_table *t)
{
    VkPhysicalDeviceProperties p;
    t->real.vkGetPhysicalDeviceProperties(t->physical, &p);
    const char *env = vkb_srv_env("VKBRIDGE_SPIRV_FIX");
    /* Arm's driver (by driverID: not Mesa PanVK, also vendor 0x13B5; server_driverid.c), or a GPU the
     * startup probe saw fold DXVK's guard at the defaults (server_probe.c) */
    int arm = vkb_arm_driver(&t->real, t->physical, t->api_version);
    int on = env ? strcmp(env, "0") != 0 : arm || vkb_probe_flag(VKB_PROBE_FLAG_SPIRV_FIX);
    /* shl_barrier: Arm's driver (a no-op the compiler cannot fold; the cost is one ALU op per constant
     * shift and, once, the driver's pipeline cache entries of the modules it changes) */
    const char *shl_env = vkb_srv_env("VKBRIDGE_SHL_BARRIER");
    int shl = shl_env && *shl_env ? strcmp(shl_env, "0") != 0 : arm;
    const char *wgz_env = vkb_srv_env("VKBRIDGE_WG_ZERO");
    int wgz = wgz_env && *wgz_env ? strcmp(wgz_env, "0") != 0 : arm;
    /* shm_barrier: only when asked for (a per-game preset): it adds barriers, which cost time */
    const char *shm_env = vkb_srv_env("VKBRIDGE_SHARED_BARRIER");
    int shm = shm_env && *shm_env && strcmp(shm_env, "0") != 0 ? (strcmp(shm_env, "2") ? 1 : 2) : 0;
    /* wave32: only when asked for (a per-game preset), and only where it is needed: it adds
     * barriers and shared memory */
    VkPhysicalDeviceSubgroupProperties sgp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sgp};
    if (t->real.vkGetPhysicalDeviceProperties2 && p.apiVersion >= VK_API_VERSION_1_1) t->real.vkGetPhysicalDeviceProperties2(t->physical, &p2);
    const char *w32_env = vkb_srv_env("VKBRIDGE_WAVE32");
    int w32_force = w32_env && !strcmp(w32_env, "force");
    int w32_asked = w32_env && *w32_env && strcmp(w32_env, "0") != 0 && strcmp(w32_env, "off") != 0;
    int w32 = w32_asked && (w32_force || (sgp.subgroupSize && sgp.subgroupSize < 32));
    if (w32_asked && !w32)
        VKB_INFO("device %u: wave32 not needed: subgroups are %u wide (VKBRIDGE_WAVE32=force to have it anyway)", t->id, sgp.subgroupSize);
    const char *dump_env = vkb_srv_env("VKBRIDGE_SHARED_BARRIER_DUMP");
    int shm_dump = (shm || w32) && dump_env && *dump_env && strcmp(dump_env, "0") != 0;
    const char *max_env = vkb_srv_env("VKBRIDGE_SHARED_BARRIER_DUMP_MAX");
    const char *only_env = vkb_srv_env("VKBRIDGE_SHARED_BARRIER_ONLY"), *skip_env = vkb_srv_env("VKBRIDGE_SHARED_BARRIER_SKIP");
    const char *subst_env = vkb_srv_env("VKBRIDGE_SPIRV_SUBST");
    int subst = subst_env && *subst_env && strcmp(subst_env, "0") != 0;
    /* Compute layouts with immutable YCbCr samplers (gamescope's composite shaders): Mali and
     * Samsung's Xclipse do not build such pipelines (Xclipse 960: 135 of gamescope's 144 fail with
     * VK_ERROR_UNKNOWN, every game black, MaliBridge#20). VKBRIDGE_YCBCR_SWAP=0/1 overrides. */
    const char *yc_env = vkb_srv_env("VKBRIDGE_YCBCR_SWAP");
    int ycs = yc_env && *yc_env ? strcmp(yc_env, "0") != 0 : on || p.vendorID == 0x144D;
    int spv = on || shl || shm || w32 || subst || wgz;
    /* Installed on every GPU: the sampler and layout hooks count YCbCr samplers for the failure
     * line (they run when such objects are made, never per frame); the module and pipeline hooks
     * only where a SPIR-V change is on (they keep copies of every module). */
    if (!t->dt.vkCreateShaderModule || !t->dt.vkCreateSampler || !t->dt.vkDestroySampler || !t->dt.vkCreateDescriptorSetLayout ||
        !t->dt.vkDestroyDescriptorSetLayout || !t->dt.vkCreatePipelineLayout || !t->dt.vkDestroyPipelineLayout)
        return;
    vkb_spvfix *f = calloc(1, sizeof(*f));
    if (!f) return;
    f->fixes = on;
    f->ycbcr_swap = ycs;
    f->shl = shl;
    f->wgzero = wgz;
    f->shm = shm;
    f->shm_dump = shm_dump;
    f->dump_max = max_env && *max_env ? strtoul(max_env, NULL, 10) : 1024;
    f->wave32 = w32;
    f->shared_limit = p.limits.maxComputeSharedMemorySize;
    f->hw_subgroup = sgp.subgroupSize;
    f->shm_only = shm && only_env && *only_env ? strdup(only_env) : NULL;
    f->shm_skip = shm && skip_env && *skip_env ? strdup(skip_env) : NULL;
    const char *lock_env = vkb_srv_env("VKBRIDGE_SHARED_BARRIER_LOCKSTEP");
    f->shm_lockstep = shm >= 2 && lock_env && *lock_env && strcmp(lock_env, "0") ? strdup(lock_env) : NULL;
    f->subst = subst;
    pthread_mutex_init(&f->lock, NULL);
    vkb_dispatch *dt = &t->dt;
    f->create_module = dt->vkCreateShaderModule;
    f->create_gfx = dt->vkCreateGraphicsPipelines;
    f->create_compute = dt->vkCreateComputePipelines;
    if (spv) {
        dt->vkCreateShaderModule = fx_CreateShaderModule;
        dt->vkCreateGraphicsPipelines = fx_CreateGraphicsPipelines;
        dt->vkCreateComputePipelines = fx_CreateComputePipelines;
    }
    f->create_sampler = dt->vkCreateSampler;
    f->destroy_sampler = dt->vkDestroySampler;
    f->create_dsl = dt->vkCreateDescriptorSetLayout;
    f->destroy_dsl = dt->vkDestroyDescriptorSetLayout;
    f->create_pl = dt->vkCreatePipelineLayout;
    f->destroy_pl = dt->vkDestroyPipelineLayout;
    dt->vkCreateSampler = fx_CreateSampler;
    dt->vkDestroySampler = fx_DestroySampler;
    dt->vkCreateDescriptorSetLayout = fx_CreateDescriptorSetLayout;
    dt->vkDestroyDescriptorSetLayout = fx_DestroyDescriptorSetLayout;
    dt->vkCreatePipelineLayout = fx_CreatePipelineLayout;
    dt->vkDestroyPipelineLayout = fx_DestroyPipelineLayout;
    t->spvfix = f;
    if (!spv && !ycs && !(yc_env && *yc_env)) return; /* only counting: nothing to say */
    VKB_INFO("device %u: Mali SPIR-V fixes %s, YCbCr compute samplers %s%s, wg_zero %s%s, shl_barrier %s%s%s", t->id,
             on ? "on" : "off", ycs ? "swapped" : "kept", yc_env && *yc_env ? " (VKBRIDGE_YCBCR_SWAP)" : "", wgz ? "on" : "off",
             wgz_env && *wgz_env ? " (VKBRIDGE_WG_ZERO)" : "", shl ? "on" : "off", shl_env && *shl_env ? " (VKBRIDGE_SHL_BARRIER)" : "", shm == 2   ? ", shm_barrier on, level 2 (VKBRIDGE_SHARED_BARRIER)"
             : shm ? ", shm_barrier on (VKBRIDGE_SHARED_BARRIER)"
                   : "");
    if (f->shm_only || f->shm_skip)
        VKB_INFO("device %u: shm_barrier only for %s%s%s%s", t->id, f->shm_only ? "VKBRIDGE_SHARED_BARRIER_ONLY=" : "",
                 f->shm_only ? f->shm_only : "", f->shm_skip ? ", not for VKBRIDGE_SHARED_BARRIER_SKIP=" : "", f->shm_skip ? f->shm_skip : "");
    if (f->shm_lockstep) VKB_INFO("device %u: shm_barrier lockstep loops for VKBRIDGE_SHARED_BARRIER_LOCKSTEP=%s", t->id, f->shm_lockstep);
    if (subst) VKB_INFO("device %u: SPIR-V subst on (VKBRIDGE_SPIRV_SUBST): modules named in spv-subst/ replaced", t->id);
    if (w32)
        VKB_INFO("device %u: wave32 on (VKBRIDGE_WAVE32%s): compute shaders that count on 32-lane subgroups get them emulated "
                 "(this GPU's are %u wide, %u bytes of shared memory)", t->id, w32_force ? "=force" : "", sgp.subgroupSize, f->shared_limit);
}

void vkb_spvfix_device_destroy(vkb_srv_table *t)
{
    vkb_spvfix *f = t->spvfix;
    if (!f) return;
    t->dt.vkCreateShaderModule = f->create_module;
    t->dt.vkCreateGraphicsPipelines = f->create_gfx;
    t->dt.vkCreateComputePipelines = f->create_compute;
    t->dt.vkCreateSampler = f->create_sampler;
    t->dt.vkDestroySampler = f->destroy_sampler;
    t->dt.vkCreateDescriptorSetLayout = f->create_dsl;
    t->dt.vkDestroyDescriptorSetLayout = f->destroy_dsl;
    t->dt.vkCreatePipelineLayout = f->create_pl;
    t->dt.vkDestroyPipelineLayout = f->destroy_pl;
    if (f->plain) t->real.vkDestroySampler(t->device, f->plain, NULL);
    if (f->patched) VKB_INFO("device %u: SPIR-V fix changed %lu composites", t->id, f->patched);
    if (f->shl)
        VKB_INFO("device %u: shl_barrier: %lu of %lu module(s) rewritten, %lu barrier(s)", t->id, f->shl_modules, f->shl_seen,
                 f->shl_barriers);
    if (f->shm)
        VKB_INFO("device %u: shm_barrier: %lu of %lu module(s) rewritten, %lu control and %lu memory barrier(s); %lu compute "
                 "module(s) with shared memory, %lu of them with subgroup operations (%lu adapt to the subgroup size), %lu with only "
                 "BroadcastFirst/Elect (any size); level %d: %lu control barrier(s) inside uniform constructs, %lu of %lu construct(s) "
                 "uniform%s",
                 t->id, f->shm_modules, f->shm_seen, f->shm_control, f->shm_memory, f->shm_shared, f->shm_subgroup, f->shm_adapts,
                 f->shm_anysize, f->shm, f->shm_inside, f->shm_uniform, f->shm_constructs,
                 f->shm_dump ? " (saved to shm-spirv/)" : "");
    if (f->wave32)
        VKB_INFO("device %u: wave32: %lu of %lu compute module(s) emulated at 32 lanes, %lu subgroup operation(s), %lu barrier(s); "
                 "%lu count on a subgroup size, %lu adapt to it, %lu only BroadcastFirst/Elect; %lu left as they were%s",
                 t->id, f->w32_modules, f->w32_seen, f->w32_ops, f->w32_barriers, f->w32_fixed, f->w32_adapts, f->w32_anysize,
                 f->w32_left, f->shm_dump ? " (saved to shm-spirv/)" : "");
    if (f->shm_lockstep)
        VKB_INFO("device %u: shm_barrier: lockstep: %lu module(s), %lu loop(s) given a uniform trip count", t->id, f->shm_lock_modules,
                 f->shm_lock_loops);
    if (f->shm_filtered)
        VKB_INFO("device %u: shm_barrier: %lu module(s) left as they were by VKBRIDGE_SHARED_BARRIER_ONLY/_SKIP", t->id, f->shm_filtered);
    if (f->subst) VKB_INFO("device %u: SPIR-V subst: %lu module(s) replaced", t->id, f->subst_n);
    if (f->shm_dump && f->shm_saved > f->dump_max)
        VKB_INFO("device %u: shm-spirv/: %lu module(s) saved, %lu more not (VKBRIDGE_SHARED_BARRIER_DUMP_MAX=%lu)", t->id, f->dump_max,
                 f->shm_saved - f->dump_max, f->dump_max);
    for (int i = 0; i < KEEP; i++) free(f->kept[i].code);
    free(f->shm_only);
    free(f->shm_skip);
    free(f->shm_lockstep);
    pthread_mutex_destroy(&f->lock);
    free(f);
    t->spvfix = NULL;
}
