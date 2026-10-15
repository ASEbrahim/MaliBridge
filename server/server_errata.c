/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Driver errata: known Mali driver bugs, each with a fix that sits just above the driver. The
 * errata layer replaces t->real.* right after server_quirks.c (vkb_sv_vkCreateDevice), so every
 * layer installed later reaches the driver through it - the bridge's own emulation calls (the
 * divisor emulation's binds, the BC decoder's views, the null-descriptor stand-ins) as well as
 * the app's.
 *
 * Which errata are on is decided once per device, at vkCreateDevice, from:
 *   1  the startup probe's result for the erratum's probe, where it has one (pass / fail / not
 *      run; see vkb_errata_set_probe_source);
 *   2  the Mali driver version, decoded from driverVersion ((r << 22) | (p << 12)) and from the
 *      driverInfo build string ("v1.r44p1-01eac0", "v1.r49p1-03bet0" for a beta), against the
 *      erratum's affected ranges, some of them for one GPU family only (job manager GPUs, told by
 *      maxDrawIndirectCount <= 1, or command stream frontend ones) or one GPU;
 *   3  the mode: safe (the default) or performance (VKBRIDGE_MODE).
 * Each erratum says per mode which of these turn it on: always, a failed probe, the driver in an
 * affected range, an Arm driver whose version cannot be decoded. One line per erratum that ends up
 * on says why, and one line sums up the device.
 *
 * Switches (from the environment or <cache dir>/env, read at each vkCreateDevice):
 *   VKBRIDGE_ERRATA=+name,-name,...   forces a fix on or off (a bare name is +name); forcing a fix
 *                                     on is the regression test of it on an unaffected driver
 *   VKBRIDGE_ERRATA_IMITATE=name,...  host builds only: a shim beneath the fix introduces the bug,
 *                                     so a test shows the bug without the fix and none with it
 *   VKBRIDGE_FAKE_DRIVER_VERSION=r49p1[:jm|:csf]   the version gates believe another driver (also
 *                                     a driverInfo string, "v1.r49p1-03bet0"); the GPU counts as Arm
 *   VKBRIDGE_ERRATA_PROBE=probe:pass|fail,...      probe results as if the probe had recorded
 *                                     them (host tests of the decisions)
 *
 * Names (not VKBRIDGE_QUIRKS: server_quirks.c means shader discard): wait_retry (MK-01),
 * viewport_flip (MK-02), bitwise_scalar (MK-03), dyn_reemit (MK-04), vb_split (MK-05),
 * view_quarantine (MK-06), view_counts (MK-07), and example: a no-op on vkCreateImageView that
 * counts the calls reaching it (and its imitation the calls beneath it), the pattern a fix follows.
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARM_VENDOR 0x13B5u
#define V(r, p) (((uint32_t)(r) << 10) | (uint32_t)(p)) /* comparable: r major, p minor */
#define V_MAX 0xffffffffu

enum { G_ALWAYS = 1, G_PROBE_FAIL = 2, G_IN_RANGE = 4, G_VERSION_UNKNOWN = 8 };
enum { FAM_ANY, FAM_JM, FAM_CSF };

typedef struct {
    uint32_t from, fixed; /* affected: from <= v < fixed (V_MAX: not fixed yet) */
    int family;           /* FAM_*: the GPUs it was seen on */
    const char *gpu;      /* a deviceName substring, or NULL */
    const char *note;
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
        {{V(0, 0), V_MAX, FAM_ANY, NULL, "every blob driver"}}, .elsewhere = "server_wait.c, VKBRIDGE_WAIT_FIX"},
    [VKB_ERRATUM_VIEWPORT_FLIP] = {"viewport_flip", "MK-02", "viewport_flip", 1, G_PROBE_FAIL, G_PROBE_FAIL,
        {{V(54, 2), V(54, 3), FAM_ANY, "G715", "a single report"}}},
    [VKB_ERRATUM_BITWISE_SCALAR] = {"bitwise_scalar", "MK-03", "bitwise_vec", 1, G_PROBE_FAIL | G_IN_RANGE, G_PROBE_FAIL,
        {{V(18, 0), V(54, 0), FAM_ANY, NULL, NULL}}},
    [VKB_ERRATUM_DYN_REEMIT] = {"dyn_reemit", "MK-04", "dyn_state", 1, G_ALWAYS, G_PROBE_FAIL | G_IN_RANGE,
        {{V(38, 0), V(44, 1), FAM_ANY, NULL, "depth/stencil/cull/stride"},
         {V(48, 0), V(54, 0), FAM_ANY, NULL, "EDS3 conservative raster, line mode, sample mask"},
         {V(46, 0), V(51, 0), FAM_JM, NULL, "job manager GPUs"}}},
    [VKB_ERRATUM_VB_SPLIT] = {"vb_split", "MK-05", "vb_strides", 0, G_ALWAYS, G_ALWAYS,
        {{V(42, 0), V(48, 0), FAM_ANY, NULL, NULL}}},
    [VKB_ERRATUM_VIEW_QUARANTINE] = {"view_quarantine", "MK-06", "rendering_churn", 0, G_IN_RANGE | G_VERSION_UNKNOWN, G_IN_RANGE,
        {{V(49, 1), V(49, 3), FAM_ANY, NULL, NULL}, {V(50, 0), V(51, 1), FAM_ANY, NULL, NULL}},
        vkb_errata_views_install, vkb_errata_views_install},
    [VKB_ERRATUM_VIEW_COUNTS] = {"view_counts", "MK-07", "view3d_layers", 0, G_ALWAYS, G_ALWAYS,
        {{V(19, 0), V(54, 1), FAM_ANY, NULL, NULL}}, vkb_errata_views_install, vkb_errata_views_install},
    [VKB_ERRATUM_EXAMPLE] = {"example", NULL, NULL, 0, 0, 0, {{0}}, example_install, example_imitate},
};

/* The driver, as the version gates see it */
typedef struct {
    int arm;           /* an Arm GPU (or a faked version) */
    int known;         /* r and p decoded */
    uint32_t r, p;
    int beta;
    int family;        /* FAM_JM / FAM_CSF */
    const char *from;  /* where the version came from */
    char build[VK_MAX_DRIVER_INFO_SIZE];
} drv_version;

static int (*probe_source)(const char *probe);
static vkb_errata_dev *last_dev;

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

static void driver_version(vkb_srv_table *t, drv_version *d, char *name, size_t name_cap)
{
    memset(d, 0, sizeof(*d));
    VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
    if (t->real.vkGetPhysicalDeviceProperties2 && t->api_version >= VK_API_VERSION_1_2)
        t->real.vkGetPhysicalDeviceProperties2(t->physical, &p2);
    else {
        p2.pNext = NULL;
        t->real.vkGetPhysicalDeviceProperties(t->physical, &p2.properties);
    }
    const VkPhysicalDeviceProperties *pr = &p2.properties;
    snprintf(name, name_cap, "%s", pr->deviceName);
    d->family = pr->limits.maxDrawIndirectCount <= 1 ? FAM_JM : FAM_CSF;
    d->arm = pr->vendorID == ARM_VENDOR;
    snprintf(d->build, sizeof(d->build), "%s", dp.driverInfo);

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
    int ib = 0, info = vkb_errata_parse_version(dp.driverInfo, &ir, &ip, &ib);
    if (info) {
        d->known = 1, d->r = ir, d->p = ip, d->beta = ib;
        d->from = "driverInfo";
        if (ir != vr || ip != vp)
            VKB_WARN("errata: driverInfo \"%s\" says r%up%u, driverVersion 0x%08x says r%up%u; going by driverInfo",
                     dp.driverInfo, ir, ip, pr->driverVersion, vr, vp);
    } else if (vr >= 1 && vr < 100) {
        d->known = 1, d->r = vr, d->p = vp;
        d->from = "driverVersion";
    }
}

static const erratum_range *in_range(const erratum *e, const drv_version *d, const char *gpu)
{
    if (!d->arm || !d->known) return NULL;
    uint32_t v = V(d->r, d->p);
    for (int i = 0; i < 4; i++) {
        const erratum_range *g = &e->ranges[i];
        if (!g->from && !g->fixed) break;
        if (v < g->from || v >= g->fixed) continue;
        if (g->family != FAM_ANY && g->family != d->family) continue;
        if (g->gpu && !strstr(gpu, g->gpu)) continue;
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

/* VKBRIDGE_ERRATA_PROBE=name:pass|fail,... (host tests), else the probe's record */
static int probe_result(const char *probe)
{
    const char *v = vkb_srv_env("VKBRIDGE_ERRATA_PROBE");
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
    const char *m = vkb_srv_env("VKBRIDGE_MODE");
    if (!m || !*m || !strcmp(m, "safe")) return 0;
    if (!strcmp(m, "performance") || !strcmp(m, "perf")) return 1;
    VKB_WARN("errata: VKBRIDGE_MODE=%s is neither safe nor performance; safe", m);
    return 0;
}

#define CAT(...) (o += o < sizeof(why) ? (size_t)snprintf(why + o, sizeof(why) - o, __VA_ARGS__) : 0)

void vkb_errata_device_init(vkb_srv_table *t)
{
    vkb_errata_dev *e = calloc(1, sizeof(*e));
    if (!e) return;
    t->errata = e;
    char gpu[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    drv_version d;
    driver_version(t, &d, gpu, sizeof(gpu));
    int perf = performance_mode();
    const char *mode = perf ? "performance mode" : "safe mode";

    uint32_t plus = 0, minus = 0, imitate = 0;
    parse_names("VKBRIDGE_ERRATA", vkb_srv_env("VKBRIDGE_ERRATA"), &plus, &minus);
    const char *imi = vkb_srv_env("VKBRIDGE_ERRATA_IMITATE");
#ifdef __ANDROID__
    if (imi && *imi) VKB_WARN("errata: VKBRIDGE_ERRATA_IMITATE is for host tests; ignored");
#else
    parse_names("VKBRIDGE_ERRATA_IMITATE", imi, &imitate, NULL);
#endif

    char drv[400];
    if (!d.arm)
        snprintf(drv, sizeof(drv), "not an Arm GPU");
    else if (!d.known)
        snprintf(drv, sizeof(drv), "Arm driver, version unknown (\"%s\")", d.build);
    else
        snprintf(drv, sizeof(drv), "driver r%up%u%s (%s%s%s%s), %s", d.r, d.p, d.beta ? " beta" : "", d.from,
                 *d.build ? " \"" : "", d.build, *d.build ? "\"" : "", d.family == FAM_JM ? "job manager" : "CSF");

    char on_list[256] = "", off_list[256] = "";
    for (int i = 0; i < VKB_ERRATA_COUNT; i++) {
        const erratum *x = &errata[i];
        unsigned gates = perf ? x->perf : x->safe;
        char why[320];
        size_t o = 0;
        why[0] = 0;
        int pr = x->probe ? probe_result(x->probe) : VKB_ERRATA_PROBE_NOT_RUN;
        const erratum_range *g = in_range(x, &d, gpu);
        int want = 0;
        if (gates & G_ALWAYS) want = 1, CAT("always");
        if ((gates & G_PROBE_FAIL) && x->probe_gates && pr == VKB_ERRATA_PROBE_FAIL) want = 1, CAT("%sprobe %s failed", o ? "; " : "", x->probe);
        if ((gates & G_IN_RANGE) && g) {
            char rs[128];
            range_str(g, rs, sizeof(rs));
            want = 1, CAT("%sr%up%u in %s", o ? "; " : "", d.r, d.p, rs);
        }
        if ((gates & G_VERSION_UNKNOWN) && d.arm && !d.known) want = 1, CAT("%sdriver version unknown", o ? "; " : "");
        if (want) CAT("; %s", mode);
        int on = want;
        if (plus >> i & 1) {
            if (!want) o = 0, why[0] = 0;
            CAT("%sVKBRIDGE_ERRATA=+%s", o ? "; " : "", x->name);
            on = 1;
        }
        if (minus >> i & 1) {
            if (want) VKB_INFO("errata: %s OFF (VKBRIDGE_ERRATA=-%s; it would be on: %s)", x->name, x->name, why);
            on = 0;
        }
        if (x->probe && pr != VKB_ERRATA_PROBE_NOT_RUN && on)
            CAT("; probe %s %s%s", x->probe, pr == VKB_ERRATA_PROBE_FAIL ? "failed" : "passed", x->probe_gates ? "" : ", recorded only");
        char *l = on ? on_list : off_list;
        size_t n = strlen(l);
        snprintf(l + n, 256 - n, "%s%s", n ? "," : "", x->name);
        if (on) {
            e->on |= 1u << i;
            if (x->install)
                VKB_INFO("errata: %s ON (%s)%s%s", x->name, why, x->mk ? " - " : "", x->mk ? x->mk : "");
            else if (x->elsewhere)
                VKB_INFO("errata: %s ON (%s) - %s, fixed in %s", x->name, why, x->mk, x->elsewhere);
            else
                VKB_INFO("errata: %s ON (%s) - %s, no fix in this build yet", x->name, why, x->mk);
        }
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
    for (int i = 0; i < VKB_ERRATA_COUNT; i++)
        if ((e->on >> i & 1) && errata[i].install) errata[i].install(t, e);
    __atomic_store_n(&last_dev, e, __ATOMIC_RELEASE);
    VKB_INFO("errata: device %u, %s, %s, %s: on %s; off %s", t->id, gpu, drv, mode, *on_list ? on_list : "none",
             *off_list ? off_list : "none");
}

void vkb_errata_device_destroy(vkb_srv_table *t)
{
    vkb_errata_dev *e = t->errata;
    if (!e) return;
    vkb_errata_views_destroy(t, e);
    if ((e->on | e->imitated) >> VKB_ERRATUM_EXAMPLE & 1)
        VKB_INFO("errata: example: %llu vkCreateImageView call(s) through the fix, %llu through the imitation beneath it",
                 (unsigned long long)e->example_calls, (unsigned long long)e->example_imitated);
    vkb_errata_dev *exp = e;
    __atomic_compare_exchange_n(&last_dev, &exp, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    t->errata = NULL;
    free(e);
}

vkb_errata_dev *vkb_errata_cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->errata ? t->errata : __atomic_load_n(&last_dev, __ATOMIC_ACQUIRE);
}

/* example: what a fix looks like. Its wrapper calls e->next_fix; an imitation calls e->next_imi. */
static VKAPI_ATTR VkResult VKAPI_CALL ex_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                         VkImageView *out)
{
    vkb_errata_dev *e = vkb_errata_cur();
    __atomic_add_fetch(&e->example_calls, 1, __ATOMIC_RELAXED);
    return e->next_fix.vkCreateImageView(d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL ex_imi_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                             VkImageView *out)
{
    vkb_errata_dev *e = vkb_errata_cur();
    __atomic_add_fetch(&e->example_imitated, 1, __ATOMIC_RELAXED);
    return e->next_imi.vkCreateImageView(d, ci, a, out);
}

static void example_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    (void)e;
    ERRATA_HOOK(t, vkCreateImageView, ex_CreateImageView);
}

static void example_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    (void)e;
    ERRATA_HOOK(t, vkCreateImageView, ex_imi_CreateImageView);
}
