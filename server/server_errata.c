/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Driver errata: known Mali driver bugs, each with a fix that sits just above the driver. The
 * errata layer replaces t->real.* right after server_quirks.c (vkb_sv_vkCreateDevice), so every
 * layer installed later reaches the driver through it - the bridge's own emulation calls (the
 * divisor emulation's binds, the BC decoder's views, the null-descriptor stand-ins) as well as
 * the app's.
 *
 * Which errata are on is decided once per device, at vkCreateDevice, from:
 *   1  the startup probe's result for the erratum's probe, where it has one (server_probe.c,
 *      through vkb_errata_set_probe_source: fail where the probe saw the bug's own pattern, pass,
 *      or not run - a probe that crashed, hung or could not run decides nothing);
 *   2  the Mali driver version - Arm's own driver only (driverID ARM_PROPRIETARY; vendor 0x13B5 where
 *      the driver has no driverID: server_driverid.c), not Mesa PanVK with Arm's vendor ID - decoded from driverVersion ((r << 22) | (p << 12)) and from the
 *      driverInfo build string ("v1.r44p1-01eac0", "v1.r49p1-03bet0" for a beta), against the
 *      erratum's affected ranges, some of them for one GPU family only (job manager GPUs, told by
 *      maxDrawIndirectCount <= 1, or command stream frontend ones) or one GPU;
 *   3  the mode: safe (the default) or performance (server_mode.c).
 * Each erratum says per mode which of these turn it on: always, a failed probe, the driver in an
 * affected range, an Arm driver whose version cannot be decoded. One line per erratum that ends up
 * on says why, and one line sums up the device - in full once per server process for each (GPU,
 * driver, mode, decisions and their reasons); a later device with the very same ones logs one line
 * "errata: device N: same as device M (<mode>): on ..." (the full lines at debug level).
 *
 * Switches (from the environment or <cache dir>/env, read at each vkCreateDevice):
 *   VKBRIDGE_ERRATA=+name,-name,...   forces a fix on or off (a bare name is +name); forcing a fix
 *                                     on is the regression test of it on an unaffected driver
 *   VKBRIDGE_ERRATA_IMITATE=name,...  host builds only: a shim beneath the fix introduces the bug,
 *                                     so a test shows the bug without the fix and none with it
 *   VKBRIDGE_FAKE_DRIVER_VERSION=r49p1[:jm|:csf]   the version gates believe another driver (also
 *                                     a driverInfo string, "v1.r49p1-03bet0"); the GPU counts as Arm
 *   VKBRIDGE_ERRATA_PROBE=probe:pass|fail,...      host builds only: probe results as if the
 *                                     probe had recorded them (tests of the decisions; on a device
 *                                     VKBRIDGE_PROBE_FAKE=<probe>:fail fails the real probe)
 *   VKBRIDGE_WAIT_FIX=0               v0.4's name for -wait_retry; VKBRIDGE_WAIT_FAKE_ERROR (how the
 *                                     wait imitation fails, server_wait.c) imitates on any build, as does
 *                                     VKBRIDGE_WAIT_FAKE_LOST
 *   VKBRIDGE_NO_FP16=1 / =0           +fp16_hide / -fp16_hide (also a per-game preset key); it wins
 *                                     over VKBRIDGE_ERRATA for that one erratum
 *
 * The probe sees the imitations too: an erratum's probe runs with that erratum's imitation
 * beneath it when VKBRIDGE_ERRATA_IMITATE names it (vkb_errata_probe_begin), so a host test shows
 * the probe telling the bug, and the decision following it, on a driver without it.
 *
 * The decisions are a function of (driver, mode, probe results, switches) and nothing else, so the
 * session report shows them for both modes (vkb_errata_explain) whether or not a device was made.
 *
 * Names (not VKBRIDGE_QUIRKS: server_quirks.c means shader discard): wait_retry (MK-01),
 * viewport_flip (MK-02), bitwise_scalar (MK-03), dyn_reemit (MK-04), vb_split (MK-05),
 * view_quarantine (MK-06), view_counts (MK-07), clamp_zero (MK-32), cull_nontri (MK-16),
 * dedicated_import (MK-39), fp16_hide (MK-20), stencil_layout (MK-11), gs_hide (issue #11), and example: a no-op on
 * vkCreateImageView that counts the calls reaching it (and its imitation the calls beneath it),
 * the pattern a fix follows.
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARM_VENDOR 0x13B5u
#define V(r, p) (((uint32_t)(r) << 10) | (uint32_t)(p)) /* comparable: r major, p minor */
#define V_MAX 0xffffffffu

enum { G_ALWAYS = 1, G_PROBE_FAIL = 2, G_IN_RANGE = 4, G_VERSION_UNKNOWN = 8 };
enum { FAM_ANY, FAM_JM, FAM_CSF };
enum { R_BETA = 1, R_SAFE = 2 }; /* a range for beta builds only / in safe mode only */

typedef struct {
    uint32_t from, fixed; /* affected: from <= v < fixed (V_MAX: not fixed yet) */
    int family;           /* FAM_*: the GPUs it was seen on */
    const char *gpu;      /* a deviceName substring, or NULL */
    const char *note;
    int flags;            /* R_* */
} erratum_range;

typedef struct {
    const char *name, *mk;
    const char *probe;      /* the startup probe that tests it, or NULL */
    int probe_gates;        /* 0: the probe only records (the result goes to the report) */
    unsigned safe, perf;    /* G_* that turn it on, per mode */
    erratum_range ranges[4];
    void (*install)(vkb_srv_table *t, vkb_errata_dev *e); /* NULL: no fix in this build yet */
    void (*imitate)(vkb_srv_table *t, vkb_errata_dev *e); /* the bug beneath the fix (host) */
    const char *elsewhere; /* fixed outside this layer (its own switch) */
} erratum;

static void example_install(vkb_srv_table *t, vkb_errata_dev *e);
static void example_imitate(vkb_srv_table *t, vkb_errata_dev *e);

/* Ranges from Arm's errata and the Mali known-issues survey. OEM drivers can carry backports,
 * which is why a probe result, where there is one, outranks the range in performance mode. */
static const erratum errata[VKB_ERRATA_COUNT] = {
    [VKB_ERRATUM_WAIT_RETRY] = {"wait_retry", "MK-01", "wait_eintr", 0, G_ALWAYS, G_ALWAYS,
        {{V(0, 0), V_MAX, FAM_ANY, NULL, "every blob driver"}}, vkb_wait_retry_install, vkb_wait_retry_imitate},
    [VKB_ERRATUM_VIEWPORT_FLIP] = {"viewport_flip", "MK-02", "viewport_flip", 1, G_PROBE_FAIL, G_PROBE_FAIL,
        {{V(54, 2), V(54, 3), FAM_ANY, "G715", "a single report"}}, vkb_errata_flip_install, vkb_errata_flip_imitate},
    [VKB_ERRATUM_BITWISE_SCALAR] = {"bitwise_scalar", "MK-03", "bitwise_vec", 1, G_PROBE_FAIL | G_IN_RANGE, G_PROBE_FAIL,
        {{V(18, 0), V(54, 0), FAM_ANY, NULL, NULL}}, vkb_errata_bitwise_install, vkb_errata_bitwise_imitate},
    [VKB_ERRATUM_DYN_REEMIT] = {"dyn_reemit", "MK-04", "dyn_state", 1, G_ALWAYS, G_PROBE_FAIL | G_IN_RANGE,
        {{V(0, 0), V(44, 1), FAM_ANY, NULL, "depth/stencil/cull/stride; ANGLE: below r44p1"},
         {V(48, 0), V(54, 0), FAM_ANY, NULL, "EDS3 conservative raster, line mode, sample mask"},
         {V(46, 0), V(51, 0), FAM_JM, NULL, "job manager GPUs"}},
        vkb_errata_dyn_install, vkb_errata_dyn_imitate},
    [VKB_ERRATUM_VB_SPLIT] = {"vb_split", "MK-05", "vb_strides", 0, G_ALWAYS, G_ALWAYS,
        {{V(0, 0), V(48, 0), FAM_ANY, NULL, "ANGLE: below r48; Arm: r42p0-r47p0"}}, vkb_errata_vb_install,
        vkb_errata_vb_imitate},
    /* Safe mode: on every driver. Arm lists r49p1-r51p0, but Ori and the Will of the Wisps on a
     * G720 r44p1 (14T Pro) flicked to a stale magenta picture after a quality change while ~30
     * images a second were made and destroyed: the pattern of this erratum, on a driver outside
     * the list. It costs held-back handles, not memory (the app's frees go through). */
    [VKB_ERRATUM_VIEW_QUARANTINE] = {"view_quarantine", "MK-06", "rendering_churn", 0, G_ALWAYS, G_IN_RANGE,
        {{V(49, 1), V(49, 3), FAM_ANY, NULL, NULL}, {V(50, 0), V(51, 1), FAM_ANY, NULL, NULL}},
        vkb_errata_views_install, vkb_errata_views_install},
    [VKB_ERRATUM_VIEW_COUNTS] = {"view_counts", "MK-07", "view3d_layers", 0, G_ALWAYS, G_ALWAYS,
        {{V(19, 0), V(54, 1), FAM_ANY, NULL, NULL}}, vkb_errata_views_install, vkb_errata_views_install},
    /* Arm: fixed in r49p1. The G925's r49p1 is a beta build ("v1.r49p1-03bet0"), which the errata
     * notice does not cover: whether it has the fix is not known, so safe mode treats it as
     * affected; performance mode goes by Arm. */
    [VKB_ERRATUM_CLAMP_ZERO] = {"clamp_zero", "MK-32", NULL, 0, G_IN_RANGE | G_VERSION_UNKNOWN, G_IN_RANGE,
        {{V(38, 1), V(49, 1), FAM_ANY, NULL, NULL},
         {V(49, 1), V(49, 2), FAM_ANY, NULL, "a beta build: Arm's fix in r49p1 not confirmed for it", R_BETA | R_SAFE},
         {V(50, 0), V(51, 1), FAM_ANY, NULL, NULL}},
        vkb_errata_clampzero_install, vkb_errata_clampzero_imitate},
    /* Culling never applies to points and lines, so turning it off for them is exact on any
     * driver: always on in safe mode; in performance mode below r52 (ANGLE's gate, Arm erratum
     * 3786517: r41p0-r49p2, r50p0-r51p0). Pipelines with a geometry or tessellation stage are left
     * alone (their rasterized primitive is the shader's, not the topology's). */
    [VKB_ERRATUM_CULL_NONTRI] = {"cull_nontri", "MK-16", NULL, 0, G_ALWAYS, G_IN_RANGE,
        {{V(0, 0), V(52, 0), FAM_ANY, NULL, "ANGLE: below r52; Arm: r41p0-r49p2, r50p0-r51p0"}},
        vkb_errata_cull_install, vkb_errata_cull_imitate},
    /* GameNative mesa PR #8 / Lee Gao's "WSI woes on Mali": a VkMemoryDedicatedAllocateInfo on a
     * dma-buf/AHB import whose image or buffer was not made with that handle type fails or crashes
     * the allocation; fixed somewhere between r32p1 and r54p1 (the exact version is not known).
     * The struct is a hint, so dropping it there is safe on any driver. */
    [VKB_ERRATUM_DEDICATED_IMPORT] = {"dedicated_import", "MK-39", NULL, 0, G_IN_RANGE | G_VERSION_UNKNOWN, G_IN_RANGE,
        {{V(0, 0), V(54, 1), FAM_ANY, NULL, "fixed between r32p1 and r54p1"}},
        vkb_errata_dedicated_install, vkb_errata_dedicated_imitate},
    /* ncnn#7054: wrong fp16 arithmetic on the Pixel 8's Mali-G715 with drivers r54p1 and r54p3,
     * right with r47p0 and in fp32. DXVK computes in 16 bits whenever shaderFloat16 is offered
     * (and reports D3D11 min precision with shaderInt16 too). The fix hides 16-bit float
     * arithmetic and storage from the clients and takes them off device creation
     * (server_errata_fp16.c), so DXVK and vkd3d-proton take their 32-bit paths. The probe tests a
     * few patterns, not the one ncnn hit (not known), so safe mode also goes by the G715's r54
     * drivers; performance mode by the probe alone. */
    [VKB_ERRATUM_FP16_HIDE] = {"fp16_hide", "MK-20", "fp16_math", 1, G_PROBE_FAIL | G_IN_RANGE, G_PROBE_FAIL,
        {{V(54, 0), V(55, 0), FAM_ANY, "G715", "ncnn#7054: r54p1, r54p3"}}, vkb_errata_fp16_install, vkb_errata_fp16_imitate},
    /* Arm erratum 4021056: a separate stencil layout (VkAttachment*StencilLayout) is ignored and
     * the combined one used for both aspects; a stencil written under a read-only depth layout
     * can be discarded at the end of the pass. The fix (one combined layout for both) is exact on
     * any driver. */
    [VKB_ERRATUM_STENCIL_LAYOUT] = {"stencil_layout", "MK-11", NULL, 0, G_IN_RANGE | G_VERSION_UNKNOWN, G_IN_RANGE,
        {{V(41, 0), V(54, 1), FAM_ANY, NULL, "Arm 4021056, fixed in r54p1"}},
        vkb_errata_stencil_install, vkb_errata_stencil_imitate},
    /* MK-05 where the probe found single-binding calls wrong too (the Galaxy A16's G68 r38p1, every
     * attribute read with one stride): the split cannot fix it, so the strides are baked into
     * pipeline variants (server_errata_vbbake.c). The probe gates it; its install also wants the
     * probe's single-binding shape, which a multi-binding failure alone does not show. */
    [VKB_ERRATUM_VB_BAKE] = {"vb_bake", "MK-05", "vb_strides", 1, G_PROBE_FAIL, G_PROBE_FAIL, {{0}},
        vkb_errata_vbbake_install, vkb_errata_vbbake_imitate},
    /* Issue #11 (Galaxy S10+, Mali-G76, "v1.r38p1-01bet0", 2026-10-25): the beta driver segfaults
     * (address 0) compiling every VS+GS+FS pipeline Zink made for Xwayland's glamor and Steam's
     * update window - three contained crashes poison the device, Xwayland dies of the device loss
     * and takes Steam and gamescope with it. The released r38p1 (a Galaxy G57's "-01eac0") builds
     * the same geometry shaders without a crash, so the range is r38p1 beta builds only. The fix
     * (server_errata_gs.c) hides geometry shaders from the processes VKBRIDGE_GS_HIDE_PROCS lists
     * (Xwayland and Steam's own: Zink then draws without them); games keep them (DXVK requires
     * geometryShader). Safe and performance mode alike: the crash is the driver's either way. */
    [VKB_ERRATUM_GS_HIDE] = {"gs_hide", "#11", NULL, 0, G_IN_RANGE, G_IN_RANGE,
        {{V(38, 1), V(38, 2), FAM_ANY, NULL, "a beta build: Zink's geometry shaders crash its compiler", R_BETA}},
        vkb_errata_gs_install, NULL},
    [VKB_ERRATUM_EXAMPLE] = {"example", NULL, NULL, 0, 0, 0, {{0}}, example_install, example_imitate},
};

/* The driver, as the version gates see it */
typedef struct {
    int arm;           /* Arm's driver (driverID ARM_PROPRIETARY, else vendor 0x13B5), or a faked version */
    int arm_vendor;    /* vendor 0x13B5, whichever driver (Mesa PanVK too) */
    int known;         /* r and p decoded */
    uint32_t r, p;
    int beta;
    int family;        /* FAM_JM / FAM_CSF */
    const char *from;  /* where the version came from */
    char build[VK_MAX_DRIVER_INFO_SIZE];
} drv_version;

static int (*probe_source)(const char *probe);
static vkb_errata_dev *last_dev;
/* The last device's next_fix / next_imi, for a call with no device left to go by (the driver's
 * functions are the same for every device) */
static vkb_dispatch last_next_fix, last_next_imi;
static pthread_mutex_t last_next_lock = PTHREAD_MUTEX_INITIALIZER;

void vkb_errata_set_probe_source(int (*fn)(const char *probe))
{
    probe_source = fn;
}

const char *vkb_errata_name(int id)
{
    return id >= 0 && id < VKB_ERRATA_COUNT ? errata[id].name : "?";
}

int vkb_errata_on(const vkb_srv_table *t, int id)
{
    return t && t->errata && id >= 0 && id < VKB_ERRATA_COUNT && (t->errata->on >> id & 1);
}

static int find(const char *name, size_t len)
{
    for (int i = 0; i < VKB_ERRATA_COUNT; i++)
        if (strlen(errata[i].name) == len && !strncmp(errata[i].name, name, len)) return i;
    return -1;
}

/* name lists: "a,b" or "+a,-b"; *plus / *minus get the bits, unknown names are warned about */
static void parse_names(const char *var, const char *v, uint32_t *plus, uint32_t *minus)
{
    while (v && *v) {
        while (*v == ',' || *v == ' ') v++;
        if (!*v) break;
        int sign = 1;
        if (*v == '+' || *v == '-') sign = *v++ == '+';
        size_t len = strcspn(v, ", ");
        int id = find(v, len);
        if (id < 0)
            VKB_WARN("errata: %s: no erratum named %.*s", var, (int)len, v);
        else if (sign) {
            *plus |= 1u << id;
            if (minus) *minus &= ~(1u << id);
        } else if (minus) {
            *minus |= 1u << id;
            *plus &= ~(1u << id);
        } else
            VKB_WARN("errata: %s: -%.*s means nothing here", var, (int)len, v);
        v += len;
    }
}

/* "r49p1", "v1.r49p1-03bet0", "...r44p1-01eac0...": 1 when an r<major>p<minor> is found */
int vkb_errata_parse_version(const char *s, uint32_t *r, uint32_t *p, int *beta)
{
    for (const char *c = s; c && *c; c++) {
        unsigned a, b;
        int n = 0;
        if ((*c == 'r' || *c == 'R') && (c == s || !((c[-1] | 32) >= 'a' && (c[-1] | 32) <= 'z')) &&
            sscanf(c + 1, "%u%*[pP]%u%n", &a, &b, &n) == 2 && n > 0) {
            *r = a, *p = b;
            if (beta) *beta = strstr(c + 1 + n, "bet") != NULL;
            return 1;
        }
    }
    return 0;
}

/* The driver as the version gates see it, from its properties, driverID (0: not asked; what the
 * self-test asked decides, server_driverid.c) and driverInfo string. Arm's driver only: another
 * driver with Arm's vendor ID (Mesa PanVK) has a Mesa driverVersion that decodes into the ranges. */
static void version_from(const VkPhysicalDeviceProperties *pr, const char *info, uint32_t driver_id, drv_version *d)
{
    memset(d, 0, sizeof(*d));
    d->family = pr->limits.maxDrawIndirectCount <= 1 ? FAM_JM : FAM_CSF;
    d->arm = vkb_driver_is_arm(pr, driver_id);
    d->arm_vendor = pr->vendorID == ARM_VENDOR;
    snprintf(d->build, sizeof(d->build), "%s", info ? info : "");

    const char *fake = vkb_srv_env("VKBRIDGE_FAKE_DRIVER_VERSION");
    if (fake && *fake) {
        d->arm = 1;
        d->known = vkb_errata_parse_version(fake, &d->r, &d->p, &d->beta);
        if (!d->known) VKB_WARN("errata: VKBRIDGE_FAKE_DRIVER_VERSION=%s is not r<major>p<minor>; the version counts as unknown", fake);
        if (strstr(fake, ":jm")) d->family = FAM_JM;
        if (strstr(fake, ":csf")) d->family = FAM_CSF;
        snprintf(d->build, sizeof(d->build), "%s", fake);
        d->from = "VKBRIDGE_FAKE_DRIVER_VERSION";
        return;
    }
    if (!d->arm) return;
    uint32_t vr = pr->driverVersion >> 22, vp = (pr->driverVersion >> 12) & 0x3ff;
    uint32_t ir, ip;
    int ib = 0, have = info && vkb_errata_parse_version(info, &ir, &ip, &ib);
    if (have) {
        d->known = 1, d->r = ir, d->p = ip, d->beta = ib;
        d->from = "driverInfo";
        if (ir != vr || ip != vp)
            VKB_WARN("errata: driverInfo \"%s\" says r%up%u, driverVersion 0x%08x says r%up%u; going by driverInfo",
                     info, ir, ip, pr->driverVersion, vr, vp);
    } else if (vr >= 1 && vr < 100) {
        d->known = 1, d->r = vr, d->p = vp;
        d->from = "driverVersion";
    }
}

static void driver_version(const vkb_srv_table *t, VkPhysicalDevice pd, drv_version *d, char *name, size_t name_cap)
{
    VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
    if (t->real.vkGetPhysicalDeviceProperties2 && t->api_version >= VK_API_VERSION_1_2)
        t->real.vkGetPhysicalDeviceProperties2(pd, &p2);
    else {
        p2.pNext = NULL;
        t->real.vkGetPhysicalDeviceProperties(pd, &p2.properties);
    }
    snprintf(name, name_cap, "%s", p2.properties.deviceName);
    version_from(&p2.properties, dp.driverInfo, dp.driverID ? dp.driverID : vkb_driver_id(&t->real, pd, t->api_version), d);
}

/* The Arm driver's release (38 for r38p1), -1 when not Arm's or not decoded; *arm: Arm's own driver, *jm: a job manager GPU. */
int vkb_errata_arm_release_pd(const vkb_srv_table *t, VkPhysicalDevice pd, int *arm, int *jm)
{
    if (arm) *arm = 0;
    if (jm) *jm = 0;
    if (!t || !pd) return -1;
    drv_version d;
    char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    driver_version(t, pd, &d, gpu, sizeof(gpu));
    if (arm) *arm = d.arm;
    if (jm) *jm = d.family == FAM_JM;
    return d.arm && d.known ? (int)d.r : -1;
}

static const erratum_range *in_range(const erratum *e, const drv_version *d, const char *gpu, int perf)
{
    if (!d->arm || !d->known) return NULL;
    uint32_t v = V(d->r, d->p);
    for (int i = 0; i < 4; i++) {
        const erratum_range *g = &e->ranges[i];
        if (!g->from && !g->fixed) break;
        if (v < g->from || v >= g->fixed) continue;
        if (g->family != FAM_ANY && g->family != d->family) continue;
        if (g->gpu && !strstr(gpu, g->gpu)) continue;
        if (((g->flags & R_BETA) && !d->beta) || ((g->flags & R_SAFE) && perf)) continue;
        return g;
    }
    return NULL;
}

static void range_str(const erratum_range *g, char *out, size_t cap)
{
    char last[24];
    if (g->fixed == V_MAX)
        snprintf(last, sizeof(last), "on");
    else if (g->fixed & 0x3ff)
        snprintf(last, sizeof(last), "r%up%u", g->fixed >> 10, (g->fixed & 0x3ff) - 1);
    else
        snprintf(last, sizeof(last), "r%u", (g->fixed >> 10) - 1);
    snprintf(out, cap, "r%up%u-%s%s%s%s", g->from >> 10, g->from & 0x3ff, last,
             g->family == FAM_JM ? ", job manager GPUs" : g->family == FAM_CSF ? ", CSF GPUs" : "",
             g->gpu ? ", " : "", g->gpu ? g->gpu : "");
    if (g->note) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, " (%s)", g->note);
    }
}

/* VKBRIDGE_ERRATA_PROBE=name:pass|fail,... (host builds: tests of the decisions), else the probe's
 * record */
static int probe_result(const char *probe)
{
#ifdef __ANDROID__
    const char *v = NULL;
#else
    const char *v = vkb_srv_env("VKBRIDGE_ERRATA_PROBE");
#endif
    size_t pl = strlen(probe);
    for (const char *c = v; c && *c;) {
        while (*c == ',' || *c == ' ') c++;
        size_t len = strcspn(c, ", ");
        if (len > pl + 1 && !strncmp(c, probe, pl) && c[pl] == ':') {
            if (!strncmp(c + pl + 1, "fail", 4)) return VKB_ERRATA_PROBE_FAIL;
            if (!strncmp(c + pl + 1, "pass", 4)) return VKB_ERRATA_PROBE_PASS;
        }
        c += len;
    }
    return probe_source ? probe_source(probe) : VKB_ERRATA_PROBE_NOT_RUN;
}

static int performance_mode(void)
{
    return vkb_mode() == VKB_MODE_PERFORMANCE;
}

/* The bugs VKBRIDGE_ERRATA_IMITATE names (host builds), and VKBRIDGE_WAIT_FAKE_ERROR's on any */
static uint32_t imitated_set(int warn)
{
    uint32_t imitate = 0;
    const char *imi = vkb_srv_env("VKBRIDGE_ERRATA_IMITATE");
#ifdef __ANDROID__
    if (warn && imi && *imi) VKB_WARN("errata: VKBRIDGE_ERRATA_IMITATE is for host tests; ignored");
#else
    if (warn) parse_names("VKBRIDGE_ERRATA_IMITATE", imi, &imitate, NULL);
    else
        for (const char *v = imi; v && *v;) { /* quietly: the probe asks before every probe */
            while (*v == ',' || *v == ' ' || *v == '+') v++;
            size_t len = strcspn(v, ", ");
            int id = len ? find(v, len) : -1;
            if (id >= 0) imitate |= 1u << id;
            v += len;
        }
#endif
    const char *wfake = vkb_srv_env("VKBRIDGE_WAIT_FAKE_ERROR");
    if (wfake && *wfake && strcmp(wfake, "0")) imitate |= 1u << VKB_ERRATUM_WAIT_RETRY;
    if (vkb_wait_fake_lost()) imitate |= 1u << VKB_ERRATUM_WAIT_RETRY; /* VKBRIDGE_WAIT_FAKE_LOST, server_wait.c */
    return imitate;
}

/* The switches every decision reads: VKBRIDGE_ERRATA, and v0.4's VKBRIDGE_WAIT_FIX=0 */
typedef struct {
    uint32_t plus, minus;
    int wait_fix_off;
    int no_fp16; /* VKBRIDGE_NO_FP16: 1 (+fp16_hide), -1 (=0: -fp16_hide), 0 unset */
} switches;

static void read_switches(switches *w)
{
    w->plus = w->minus = 0;
    parse_names("VKBRIDGE_ERRATA", vkb_srv_env("VKBRIDGE_ERRATA"), &w->plus, &w->minus);
    /* v0.4's switch for the wait fix: VKBRIDGE_WAIT_FIX=0 is -wait_retry */
    const char *wfix = vkb_srv_env("VKBRIDGE_WAIT_FIX");
    w->wait_fix_off = wfix && !strcmp(wfix, "0") && !(w->plus >> VKB_ERRATUM_WAIT_RETRY & 1);
    if (w->wait_fix_off) w->minus |= 1u << VKB_ERRATUM_WAIT_RETRY;
    /* VKBRIDGE_NO_FP16 (a per-game preset key): =1 is +fp16_hide, =0 -fp16_hide */
    const char *nf = vkb_srv_env("VKBRIDGE_NO_FP16");
    uint32_t fb = 1u << VKB_ERRATUM_FP16_HIDE;
    w->no_fp16 = !nf || !*nf ? 0 : strcmp(nf, "0") ? 1 : -1;
    if (w->no_fp16 > 0) w->plus |= fb, w->minus &= ~fb;
    if (w->no_fp16 < 0) w->minus |= fb, w->plus &= ~fb;
}

/* The switch that forced erratum i on (plus) or off, as the log and the report name it */
static const char *forced_by(const switches *w, int i, int plus, char *out, size_t cap)
{
    if (i == VKB_ERRATUM_FP16_HIDE && w->no_fp16) snprintf(out, cap, "VKBRIDGE_NO_FP16=%s", plus ? "1" : "0");
    else if (!plus && i == VKB_ERRATUM_WAIT_RETRY && w->wait_fix_off) snprintf(out, cap, "VKBRIDGE_WAIT_FIX=0");
    else snprintf(out, cap, "VKBRIDGE_ERRATA=%c%s", plus ? '+' : '-', errata[i].name);
    return out;
}

#define CAT(...) (o += o < cap ? (size_t)snprintf(why + o, cap - o, __VA_ARGS__) : 0)

/* One erratum's decision: 1 when on; *targeted: on for more than "always" alone; why: the reason
 * (the one logged and shown in the report); *forced_off: VKBRIDGE_ERRATA (or VKBRIDGE_WAIT_FIX=0)
 * turned it off where it would be on for why */
static int decide_erratum(int i, const drv_version *d, const char *gpu, int perf, const switches *w, int *targeted, char *why, size_t cap,
                          int *forced_off)
{
    const erratum *x = &errata[i];
    unsigned gates = perf ? x->perf : x->safe;
    const char *mode = perf ? "performance mode" : "safe mode";
    size_t o = 0;
    why[0] = 0;
    int pr = x->probe ? probe_result(x->probe) : VKB_ERRATA_PROBE_NOT_RUN;
    const erratum_range *g = in_range(x, d, gpu, perf);
    int want = 0;
    if (gates & G_ALWAYS) want = 1, CAT("always");
    int by_probe = (gates & G_PROBE_FAIL) && x->probe_gates && pr == VKB_ERRATA_PROBE_FAIL;
    if (by_probe) want = 1, CAT("%sprobe %s failed", o ? "; " : "", x->probe);
    if ((gates & G_IN_RANGE) && g) {
        char rs[128];
        range_str(g, rs, sizeof(rs));
        want = 1, CAT("%sr%up%u in %s", o ? "; " : "", d->r, d->p, rs);
    }
    if ((gates & G_VERSION_UNKNOWN) && d->arm && !d->known) want = 1, CAT("%sdriver version unknown", o ? "; " : "");
    if (want) CAT("; %s", mode);
    int on = want;
    *forced_off = 0;
    if (w->plus >> i & 1) {
        char sw[64];
        if (!want) o = 0, why[0] = 0;
        CAT("%s%s", o ? "; " : "", forced_by(w, i, 1, sw, sizeof(sw)));
        on = 1;
    }
    if (w->minus >> i & 1) {
        *forced_off = want ? 1 : 0;
        on = 0;
    }
    if (x->probe && pr != VKB_ERRATA_PROBE_NOT_RUN && on && !(by_probe && !(w->plus >> i & 1)))
        CAT("; probe %s %s%s", x->probe, pr == VKB_ERRATA_PROBE_FAIL ? "failed" : "passed", x->probe_gates ? "" : ", recorded only");
    /* the driver is affected as far as is known, not only "always" */
    *targeted = on && (g || pr == VKB_ERRATA_PROBE_FAIL || (d->arm && !d->known) || (w->plus >> i & 1));
    return on;
}

int vkb_errata_explain(const VkPhysicalDeviceProperties *props, const char *driver_info, vkb_errata_row rows[VKB_ERRATA_COUNT])
{
    drv_version d;
    version_from(props, driver_info, 0, &d);
    switches w;
    read_switches(&w);
    int n = 0;
    for (int i = 0; i < VKB_ERRATA_COUNT; i++) {
        const erratum *x = &errata[i];
        if (!x->mk) continue;
        vkb_errata_row *r = &rows[n++];
        memset(r, 0, sizeof(*r));
        r->id = i;
        r->name = x->name;
        r->mk = x->mk;
        r->probe = x->probe;
        r->probe_gates = x->probe_gates;
        r->probe_result = x->probe ? probe_result(x->probe) : VKB_ERRATA_PROBE_NOT_RUN;
        r->has_fix = x->install != NULL;
        for (int m = 0; m < 2; m++) {
            int targeted, forced_off;
            r->on[m] = decide_erratum(i, &d, props->deviceName, m, &w, &targeted, r->why[m], sizeof(r->why[m]), &forced_off);
            if (forced_off) {
                char was[sizeof(r->why[m])];
                snprintf(was, sizeof(was), "%s", r->why[m]);
                char sw[64];
                snprintf(r->why[m], sizeof(r->why[m]), "%s; it would be on: %.200s", forced_by(&w, i, 0, sw, sizeof(sw)), was);
            } else if (!r->on[m] && !r->why[m][0])
                snprintf(r->why[m], sizeof(r->why[m]), "%s", "not needed here");
        }
    }
    return n;
}

/* The texts logged in full so far in this server process (a device's decision block, an install's
 * settings line), each with the device that logged it; past SEEN_MAX everything logs in full. */
#define SEEN_MAX 256
static struct {
    uint64_t hash;
    char *text;
    uint32_t dev;
} seen[SEEN_MAX];
static int seen_n;
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

/* 1 (and *first: the device that logged it) when the text was logged in full before; else 0, and
 * the text is recorded as logged by dev */
static int seen_before(const char *text, uint32_t dev, uint32_t *first)
{
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) h = (h ^ *c) * 1099511628211ull;
    int r = 0;
    pthread_mutex_lock(&seen_lock);
    for (int i = 0; i < seen_n; i++)
        if (seen[i].hash == h && !strcmp(seen[i].text, text)) {
            if (first) *first = seen[i].dev;
            r = 1;
            break;
        }
    if (!r && seen_n < SEEN_MAX && (seen[seen_n].text = strdup(text))) {
        seen[seen_n].hash = h;
        seen[seen_n].dev = dev;
        seen_n++;
    }
    pthread_mutex_unlock(&seen_lock);
    return r;
}

/* Set while a device whose decision block logs in full installs its fixes: their settings lines
 * log in full with it */
static __thread int init_full;

void vkb_errata_info_once(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (!seen_before(msg, 0, NULL) || init_full)
        vkb_log(VKB_LOG_INFO, "%s", msg);
    else if (vkb_log_level() >= VKB_LOG_DEBUG)
        vkb_log(VKB_LOG_DEBUG, "%s", msg);
}

/* Appends one line to a decision block */
static void block_add(char *b, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void block_add(char *b, size_t cap, const char *fmt, ...)
{
    size_t n = strlen(b);
    if (n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + n, cap - n, fmt, ap);
    va_end(ap);
    n = strlen(b);
    if (n + 1 < cap) b[n] = '\n', b[n + 1] = 0;
}

void vkb_errata_device_init(vkb_srv_table *t)
{
    vkb_errata_dev *e = calloc(1, sizeof(*e));
    if (!e) return;
    t->errata = e;
    char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    drv_version d;
    driver_version(t, t->physical, &d, gpu, sizeof(gpu));
    int perf = performance_mode();
    const char *mode = perf ? "performance mode" : "safe mode";

    switches w;
    read_switches(&w);
    uint32_t imitate = imitated_set(1);

    char drv[400];
    if (!d.arm && d.arm_vendor)
        snprintf(drv, sizeof(drv), "not Arm's driver (driverID %s)", vkb_driver_id_name(vkb_driver_id(&t->real, t->physical, t->api_version)));
    else if (!d.arm)
        snprintf(drv, sizeof(drv), "not an Arm GPU");
    else if (!d.known)
        snprintf(drv, sizeof(drv), "Arm driver, version unknown (\"%s\")", d.build);
    else
        snprintf(drv, sizeof(drv), "driver r%up%u%s (%s%s%s%s), %s", d.r, d.p, d.beta ? " beta" : "", d.from,
                 *d.build ? " \"" : "", d.build, *d.build ? "\"" : "", d.family == FAM_JM ? "job manager" : "CSF");

    char on_list[256] = "", off_list[256] = "";
    char block[VKB_ERRATA_COUNT * 480 + 1024] = "";
    for (int i = 0; i < VKB_ERRATA_COUNT; i++) {
        const erratum *x = &errata[i];
        char why[320];
        int targeted, forced_off;
        int on = decide_erratum(i, &d, gpu, perf, &w, &targeted, why, sizeof(why), &forced_off);
        if (forced_off) {
            char sw[64];
            block_add(block, sizeof(block), "errata: %s OFF (%s; it would be on: %s)", x->name, forced_by(&w, i, 0, sw, sizeof(sw)), why);
        }
        char *l = on ? on_list : off_list;
        size_t n = strlen(l);
        snprintf(l + n, 256 - n, "%s%s", n ? "," : "", x->name);
        if (on) {
            e->on |= 1u << i;
            if (targeted) e->targeted |= 1u << i;
            if (w.plus >> i & 1) e->forced |= 1u << i;
            if (x->install)
                block_add(block, sizeof(block), "errata: %s ON (%s)%s%s", x->name, why, x->mk ? " - " : "", x->mk ? x->mk : "");
            else if (x->elsewhere)
                block_add(block, sizeof(block), "errata: %s ON (%s) - %s, fixed in %s", x->name, why, x->mk, x->elsewhere);
            else
                block_add(block, sizeof(block), "errata: %s ON (%s) - %s, no fix in this build yet", x->name, why, x->mk);
        }
    }

    /* Logged in full once per (GPU, driver, mode, decisions with their reasons, imitations). The
     * driver is the version decoded, not where it was read: a device made through a Vulkan 1.0/1.1
     * instance has no driverInfo and reads driverVersion, the same r44p1 (on the tablet a third
     * full block came from that alone). An undecoded version keys on its build string. */
    char drv_key[400];
    if (d.arm && d.known)
        snprintf(drv_key, sizeof(drv_key), "arm r%up%u%s %s", d.r, d.p, d.beta ? " beta" : "", d.family == FAM_JM ? "jm" : "csf");
    else
        snprintf(drv_key, sizeof(drv_key), "%s", drv);
    char key[sizeof(block) + 1400];
    snprintf(key, sizeof(key), "%s%s|%s|%s|on %s|off %s|imitate 0x%x", block, gpu, drv_key, mode, on_list, off_list, imitate);
    uint32_t first = 0;
    int same = seen_before(key, t->id, &first);
    int level = same ? VKB_LOG_DEBUG : VKB_LOG_INFO;
    if (level <= vkb_log_level())
        for (char *l = block, *nl; *l; l = nl + 1) {
            if (!(nl = strchr(l, '\n'))) break;
            vkb_log(level, "%.*s", (int)(nl - l), l);
        }

    /* The imitations first (beneath), then the fixes over them. */
    e->next_imi = t->real;
    for (int i = 0; i < VKB_ERRATA_COUNT; i++)
        if ((imitate >> i & 1) && errata[i].imitate) {
            errata[i].imitate(t, e);
            e->imitated |= 1u << i;
            VKB_WARN("errata: %s bug imitated beneath the fix (VKBRIDGE_ERRATA_IMITATE)", errata[i].name);
        } else if (imitate >> i & 1)
            VKB_WARN("errata: %s has no imitation in this build", errata[i].name);
    e->next_fix = t->real;
    init_full = !same;
    for (int i = 0; i < VKB_ERRATA_COUNT; i++)
        if ((e->on >> i & 1) && errata[i].install) errata[i].install(t, e);
    init_full = 0;
    pthread_mutex_lock(&last_next_lock);
    last_next_fix = e->next_fix;
    last_next_imi = e->next_imi;
    pthread_mutex_unlock(&last_next_lock);
    __atomic_store_n(&last_dev, e, __ATOMIC_RELEASE);
    if (same) {
        VKB_INFO("errata: device %u: same as device %u (%s): on %s", t->id, first, mode, *on_list ? on_list : "none");
        VKB_DBG("errata: device %u, %s, %s, %s: on %s; off %s", t->id, gpu, drv, mode, *on_list ? on_list : "none",
                *off_list ? off_list : "none");
    } else
        VKB_INFO("errata: device %u, %s, %s, %s: on %s; off %s", t->id, gpu, drv, mode, *on_list ? on_list : "none",
                 *off_list ? off_list : "none");
}

void vkb_errata_device_destroy(vkb_srv_table *t)
{
    vkb_errata_dev *e = t->errata;
    if (!e) return;
    vkb_errata_views_destroy(t, e);
    vkb_errata_bitwise_destroy(t, e);
    vkb_errata_cmd_destroy(t, e);
    vkb_errata_flip_destroy(t, e);
    vkb_errata_clampzero_destroy(t, e);
    vkb_errata_mem_destroy(t, e);
    vkb_errata_ds_destroy(t, e);
    vkb_errata_vbbake_destroy(e);
    if ((e->on | e->imitated) >> VKB_ERRATUM_EXAMPLE & 1)
        VKB_INFO("errata: example: %llu vkCreateImageView call(s) through the fix, %llu through the imitation beneath it",
                 (unsigned long long)e->example_calls, (unsigned long long)e->example_imitated);
    vkb_errata_dev *exp = e;
    __atomic_compare_exchange_n(&last_dev, &exp, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    t->errata = NULL;
    free(e);
}

/* The startup probe's erratum device on the probe's thread while one of its probes runs with an
 * imitation beneath it (vkb_errata_probe_begin) */
static __thread vkb_errata_dev *probe_cur;

vkb_errata_dev *vkb_errata_cur(void)
{
    if (probe_cur) return probe_cur;
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->errata ? t->errata : __atomic_load_n(&last_dev, __ATOMIC_ACQUIRE);
}

/* What a fix (imitation 0) or an imitation (1) calls next for the call's device: the driver's
 * functions, or for a fix the imitation's beneath it. With no device to go by, the last one's. */
const vkb_dispatch *vkb_errata_next(int imitation)
{
    vkb_errata_dev *e = vkb_errata_cur();
    if (e) return imitation ? &e->next_imi : &e->next_fix;
    return imitation ? &last_next_imi : &last_next_fix;
}

/* A device extension the clients are not shown because of an erratum, on this physical device
 * (t: its instance's table), or NULL. vb_split (MK-05): VK_EXT_vertex_input_dynamic_state below r48
 * (its vkCmdSetVertexInputEXT sets strides another way); VKBRIDGE_ERRATA=-vb_split keeps it. */
const char *vkb_errata_hidden_ext(const vkb_srv_table *t, VkPhysicalDevice pd)
{
    if (!t || !t->real.vkGetPhysicalDeviceProperties) return NULL;
    uint32_t plus = 0, minus = 0;
    parse_names("VKBRIDGE_ERRATA", vkb_srv_env("VKBRIDGE_ERRATA"), &plus, &minus);
    if (minus >> VKB_ERRATUM_VB_SPLIT & 1) return NULL;
    char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    drv_version d;
    driver_version(t, pd, &d, gpu, sizeof(gpu));
    if (!d.arm || !d.known || V(d.r, d.p) >= V(48, 0)) return NULL;
    VKB_ONCE(VKB_LOG_INFO, "errata: vb_split: VK_EXT_vertex_input_dynamic_state hidden from clients (%s, driver r%up%u, below r48) - MK-05",
             gpu, d.r, d.p);
    return "VK_EXT_vertex_input_dynamic_state";
}

int vkb_errata_fp16_hidden(const vkb_srv_table *t, VkPhysicalDevice pd, char *why, size_t cap)
{
    char buf[320];
    if (!why) why = buf, cap = sizeof(buf);
    why[0] = 0;
    if (!t || !t->real.vkGetPhysicalDeviceProperties) return 0;
    char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    drv_version d;
    driver_version(t, pd, &d, gpu, sizeof(gpu));
    switches w;
    read_switches(&w);
    int targeted, forced_off;
    return decide_erratum(VKB_ERRATUM_FP16_HIDE, &d, gpu, performance_mode(), &w, &targeted, why, cap, &forced_off);
}

/* gs_hide's driver half: the erratum's decision for this physical device (the range, or
 * VKBRIDGE_ERRATA=+/-gs_hide); the process half is server_errata_gs.c's */
int vkb_errata_gs_driver(const vkb_srv_table *t, VkPhysicalDevice pd, char *why, size_t cap)
{
    char buf[320];
    if (!why) why = buf, cap = sizeof(buf);
    why[0] = 0;
    if (!t || !t->real.vkGetPhysicalDeviceProperties) return 0;
    char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    drv_version d;
    driver_version(t, pd, &d, gpu, sizeof(gpu));
    switches w;
    read_switches(&w);
    int targeted, forced_off;
    return decide_erratum(VKB_ERRATUM_GS_HIDE, &d, gpu, performance_mode(), &w, &targeted, why, cap, &forced_off);
}

/* example: what a fix looks like. Its wrapper calls the function it replaced (example_next), so it
 * chains with the image-view errata's wrapper of the same function; so does its imitation. */
static VKAPI_ATTR VkResult VKAPI_CALL ex_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                         VkImageView *out)
{
    vkb_errata_dev *e = vkb_errata_cur();
    __atomic_add_fetch(&e->example_calls, 1, __ATOMIC_RELAXED);
    return e->example_next(d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL ex_imi_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                             VkImageView *out)
{
    vkb_errata_dev *e = vkb_errata_cur();
    __atomic_add_fetch(&e->example_imitated, 1, __ATOMIC_RELAXED);
    return e->example_imi_next(d, ci, a, out);
}

static void example_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    e->example_next = t->real.vkCreateImageView;
    ERRATA_HOOK(t, vkCreateImageView, ex_CreateImageView);
}

static void example_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    e->example_imi_next = t->real.vkCreateImageView;
    ERRATA_HOOK(t, vkCreateImageView, ex_imi_CreateImageView);
}

/* ------------------------------------------------------------------ the startup probe's imitations */

typedef struct probe_imi {
    vkb_srv_table t;
    vkb_errata_dev *e;
} probe_imi;

void *vkb_errata_probe_begin(int id, vkb_dispatch *d, VkDevice dev, VkPhysicalDevice pd, uint32_t api)
{
    if (id < 0 || id >= VKB_ERRATA_COUNT || !(imitated_set(0) >> id & 1)) return NULL;
    void (*imitate)(vkb_srv_table *, vkb_errata_dev *) = errata[id].imitate;
    if (!imitate) return NULL;
    probe_imi *h = calloc(1, sizeof(*h));
    vkb_errata_dev *e = calloc(1, sizeof(*e));
    if (!h || !e) {
        free(h);
        free(e);
        return NULL;
    }
    h->t.id = 0xffff;
    h->t.is_device = 1;
    h->t.device = dev;
    h->t.physical = pd;
    h->t.api_version = api;
    h->t.real = h->t.dt = *d;
    e->probe = 1;
    e->imitated = 1u << id;
    e->next_imi = h->t.real;
    imitate(&h->t, e);
    e->next_fix = h->t.real;
    h->e = e;
    probe_cur = e;
    *d = h->t.real;
    return h;
}

void vkb_errata_probe_end(void *handle)
{
    probe_imi *h = handle;
    if (!h) return;
    probe_cur = NULL;
    vkb_errata_views_destroy(&h->t, h->e);
    vkb_errata_bitwise_destroy(&h->t, h->e);
    vkb_errata_cmd_destroy(&h->t, h->e);
    vkb_errata_flip_destroy(&h->t, h->e);
    vkb_errata_clampzero_destroy(&h->t, h->e);
    vkb_errata_mem_destroy(&h->t, h->e);
    vkb_errata_ds_destroy(&h->t, h->e);
    free(h->e);
    free(h);
}
