/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The session report: one file a developer reads to know every issue a session hit, with what is
 * needed to reproduce each - <log dir>/malibridge-report.md for people and malibridge-report.json
 * for tools (tools/malibridge_report_diff.py compares two). DroidDeck's "Share logs" zips the
 * session folder, the server log's directory, so the report goes with it.
 *
 * Until now one tester session showed the first problem and little else: the in-app server
 * printed its self-test only into the first session's log, warnings were scattered over 100k
 * lines, clocks and memory were in other files, and the frame rate came from the tester's eyes
 * (docs: failure-catalogue F46). Here, continuously (rewritten every 5 s while anything changes,
 * atomically, so a crash leaves the last state; and when a process exits or the session changes):
 *   - a summary: "N issues", one line each;
 *   - device, Android, RAM, storage, bridge build and protocol, environment and overrides;
 *   - the startup probe's results and what it switched on (server_probe.c), the emulation masks;
 *   - per client process (Steam app id from its pipeline-cache key, exe name, engine - DXVK,
 *     vkd3d-proton, Zink - from vkCreateInstance): start, first device, first frame, exit; frames
 *     and frame-time distribution (from the client's vkbridge-frames-<pid>.txt in the same
 *     folder: the server never sees presents); compiles (count, time histogram, the slow ones
 *     with their stage hashes); device-extensions and features it asked for that the driver lacks;
 *     device losses and waits longer than 2 s; its GPU memory peak; clocks, thermal state and
 *     available memory while it ran;
 *   - every error and warning (and every notable info line) as a class: the log call site, per
 *     process, with its count and first occurrence - the count-and-first-text of all of them is
 *     what makes one log enough for several fixes;
 *   - artefacts written this session (stuck-spirv/, failed-*.spv, stuck-pipelines, dumps).
 * VKBRIDGE_REPORT=0 turns it off; without a server log (host tests) VKBRIDGE_REPORT_DIR names
 * the folder.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#ifndef VKB_GIT
#define VKB_GIT "unknown"
#endif

#define RP_PROCS 64
#define RP_CLASSES 768
#define RP_SLOW 8
#define FIRST 320

extern char **environ;

typedef struct rp_class {
    const char *fmt; /* the call site (its format string) is the class */
    int level;
    int pid;         /* 0: the server's own threads */
    uint32_t count;
    double first_s, last_s;
    char first[FIRST];
} rp_class;

typedef struct rp_proc {
    int pid;
    char comm[32], key[48];
    char app[64], engine[64];
    uint32_t engine_version, api;
    double t_hello, t_device, t_gone; /* session seconds */
    int devices, devices_failed, alive;
    /* compiles */
    uint64_t compiles, compile_fail;
    double compile_ms, compile_max_ms;
    uint32_t hist[8];
    char slow[RP_SLOW][220];
    uint32_t nslow;
    char features[400], exts[600];
    /* device losses and long waits */
    uint32_t lost, long_waits;
    double long_wait_max_ms;
    char lost_first[200];
    /* while it ran */
    int64_t gpu_mem_peak;
    long avail_min_kb;
    unsigned cpu_top_min, cpu_top_max, cpu_cap_min, gpu_min, gpu_max;
    int thermal_max, samples;
    /* frames: the client's own line */
    char frames[700];
    double first_frame_s;
    double fps_p50;
} rp_proc;

static pthread_mutex_t rlock = PTHREAD_MUTEX_INITIALIZER;
static __thread int in_report; /* our own log lines are not fed back in */
static char rdir[600];
static char clocks_log[700];
static double t0_mono;
static time_t t0_wall;
static rp_class classes[RP_CLASSES];
static int nclasses, dropped_classes;
static rp_proc procs[RP_PROCS];
static int nprocs;
static int dirty, started_thread;
static long avail_min_kb = -1, mem_total_kb;
static int64_t rss_peak_kb;
static unsigned cpu_cap_min = 100, gpu_min_all, gpu_max_all;
static int thermal_max_all = -1;
static char gpu_src[200];

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double now_s(void) { return mono_s() - t0_mono; }

static int off(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_REPORT");
    return (v && !strcmp(v, "0")) || !rdir[0];
}

static rp_proc *proc_find(int pid, int make)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].pid == pid) return &procs[i];
    if (!make) return NULL;
    if (nprocs == RP_PROCS) {
        /* the oldest gone process makes room */
        for (int i = 0; i < nprocs; i++)
            if (!procs[i].alive) {
                memmove(&procs[i], &procs[i + 1], (size_t)(nprocs - i - 1) * sizeof(procs[0]));
                nprocs--;
                break;
            }
        if (nprocs == RP_PROCS) return NULL;
    }
    rp_proc *p = &procs[nprocs++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->avail_min_kb = -1;
    p->first_frame_s = -1;
    return p;
}

static int cur_pid(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->proc ? t->proc->pid : 0;
}

/* ------------------------------------------------------------------ log classes */

static int notable(const char *msg)
{
    static const char *const words[] = {"refused", "fail", "lost", "not emulated", "approximat", "dropped", "skipped", "cut to", "TIMED OUT",
                                        "stuck", "crash", "unsupported", "not supported", "lacks", "fallback", "cannot",
                                        "unknown", "never", "too ", "exceed", "no ordinary", "emulated instead", "replaced", "denied"};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strcasestr(msg, words[i])) return 1;
    return 0;
}

void vkb_report_log(int level, const char *fmt, const char *msg)
{
    if (in_report || level > VKB_LOG_INFO || !rdir[0]) return;
    if (level == VKB_LOG_INFO && !notable(msg)) return;
    int pid = cur_pid();
    pthread_mutex_lock(&rlock);
    rp_class *c = NULL;
    for (int i = nclasses - 1; i >= 0 && !c; i--)
        if (classes[i].fmt == fmt && classes[i].pid == pid) c = &classes[i];
    if (!c && nclasses < RP_CLASSES) {
        c = &classes[nclasses++];
        memset(c, 0, sizeof(*c));
        c->fmt = fmt;
        c->pid = pid;
        c->level = level;
        c->first_s = now_s();
        snprintf(c->first, sizeof(c->first), "%s", msg);
        dirty = 1;
    } else if (!c) {
        dropped_classes++;
    }
    if (c) {
        c->count++;
        c->last_s = now_s();
        if (level < VKB_LOG_INFO) dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
}

/* ------------------------------------------------------------------ events */

void vkb_report_hello(vkb_srv_proc *pr)
{
    pthread_mutex_lock(&rlock);
    rp_proc *p = proc_find(pr->pid, 1);
    if (p) {
        snprintf(p->comm, sizeof(p->comm), "%s", pr->comm);
        snprintf(p->key, sizeof(p->key), "%s", pr->pkey);
        if (!p->alive) p->t_hello = now_s();
        p->alive = 1;
        p->t_gone = 0;
        dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
}

static void write_report(int final);
static void read_frames(void);

void vkb_report_gone(vkb_srv_proc *pr)
{
    pthread_mutex_lock(&rlock);
    rp_proc *p = proc_find(pr->pid, 0);
    if (p) {
        p->alive = 0;
        p->t_gone = now_s();
        dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
    if (p && !off()) {
        read_frames(); /* its last numbers */
        write_report(0);
    }
}

void vkb_report_instance(vkb_srv_table *t, const VkInstanceCreateInfo *ci)
{
    if (!t->proc || !ci->pApplicationInfo) return;
    const VkApplicationInfo *a = ci->pApplicationInfo;
    pthread_mutex_lock(&rlock);
    rp_proc *p = proc_find(t->proc->pid, 1);
    if (p) {
        if (a->pApplicationName && *a->pApplicationName) snprintf(p->app, sizeof(p->app), "%s", a->pApplicationName);
        if (a->pEngineName && *a->pEngineName) {
            snprintf(p->engine, sizeof(p->engine), "%s", a->pEngineName);
            p->engine_version = a->engineVersion;
        }
        if (a->apiVersion > p->api) p->api = a->apiVersion;
        dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
}

void vkb_report_features(const VkPhysicalDeviceFeatures *want, const VkPhysicalDeviceFeatures *have, char *out, size_t cap)
{
    static const char *const names[] = {
        "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader", "tessellationShader",
        "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp",
        "fillModeNonSolid", "depthBounds", "wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy",
        "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
        "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended",
        "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat",
        "shaderStorageImageWriteWithoutFormat", "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
        "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance", "shaderCullDistance",
        "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency", "shaderResourceMinLod", "sparseBinding",
        "sparseResidencyBuffer", "sparseResidencyImage2D", "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples",
        "sparseResidency8Samples", "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries"};
    const VkBool32 *w = (const VkBool32 *)want, *h = (const VkBool32 *)have;
    size_t len = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && len < cap; i++)
        if (w[i] && !h[i]) len += (size_t)snprintf(out + len, cap - len, "%s%s", len ? " " : "", names[i]);
}

void vkb_report_device(vkb_srv_table *inst, const VkDeviceCreateInfo *ci, VkResult r, const char *dropped, const char *not_passed)
{
    (void)ci;
    if (!inst->proc) return;
    pthread_mutex_lock(&rlock);
    rp_proc *p = proc_find(inst->proc->pid, 1);
    if (p) {
        if (r == VK_SUCCESS) {
            if (!p->devices) p->t_device = now_s();
            p->devices++;
        } else {
            p->devices_failed++;
        }
        if (dropped && *dropped && !strstr(p->features, dropped))
            snprintf(p->features + strlen(p->features), sizeof(p->features) - strlen(p->features), "%s%s", p->features[0] ? " " : "", dropped);
        /* each name once */
        char buf[600];
        snprintf(buf, sizeof(buf), "%s", not_passed ? not_passed : "");
        for (char *s = strtok(buf, " "); s; s = strtok(NULL, " ")) {
            size_t n = strlen(s), have = 0;
            for (const char *q = p->exts; (q = strstr(q, s)); q += n)
                if ((q == p->exts || q[-1] == ' ') && (q[n] == ' ' || !q[n])) have = 1;
            size_t len = strlen(p->exts);
            if (!have && len + n + 2 < sizeof(p->exts)) snprintf(p->exts + len, sizeof(p->exts) - len, "%s%s", len ? " " : "", s);
        }
        dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
}

void vkb_report_compile(vkb_srv_table *t, int compute, uint32_t n, double ms, VkResult r, const char *slow)
{
    if (!t || !t->proc || !rdir[0]) return;
    static const double edges[7] = {10, 50, 200, 1000, 2000, 5000, 10000};
    int b = 0;
    while (b < 7 && ms >= edges[b]) b++;
    pthread_mutex_lock(&rlock);
    rp_proc *p = proc_find(t->proc->pid, 1);
    if (p) {
        p->compiles += n ? n : 1;
        p->compile_ms += ms;
        if (ms > p->compile_max_ms) p->compile_max_ms = ms;
        p->hist[b]++;
        if (r != VK_SUCCESS) p->compile_fail++;
        if (slow && *slow && p->nslow < RP_SLOW) {
            snprintf(p->slow[p->nslow++], sizeof(p->slow[0]), "%s %.0f ms at %.0f s, %s: %s", compute ? "compute" : "graphics", ms, now_s(),
                     r == VK_SUCCESS ? "built" : "failed", slow);
            dirty = 1;
        }
    }
    pthread_mutex_unlock(&rlock);
}

/* ------------------------------------------------------------------ device-lost watch */

typedef struct vkb_report_dev {
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
    PFN_vkWaitForFences wait_fences;
    PFN_vkWaitSemaphores wait_sems;
    PFN_vkDeviceWaitIdle device_idle;
    PFN_vkQueueWaitIdle queue_idle;
    int lost_logged;
} vkb_report_dev;

static void watch_result(const char *call, VkResult r, double ms)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (r != VK_ERROR_DEVICE_LOST && ms < 2000.0) return;
    int pid = t && t->proc ? t->proc->pid : 0;
    if (r == VK_ERROR_DEVICE_LOST && t && t->report && !t->report->lost_logged) {
        t->report->lost_logged = 1;
        VKB_ERR("device %u LOST: %s returned VK_ERROR_DEVICE_LOST (process %d) - VKBRIDGE_HANG_DETECT=1 names the job", t->id, call, pid);
    }
    pthread_mutex_lock(&rlock);
    rp_proc *p = proc_find(pid, pid != 0);
    if (p) {
        if (r == VK_ERROR_DEVICE_LOST) {
            if (!p->lost) snprintf(p->lost_first, sizeof(p->lost_first), "%s at %.1f s", call, now_s());
            p->lost++;
        } else {
            p->long_waits++;
            if (ms > p->long_wait_max_ms) p->long_wait_max_ms = ms;
        }
        dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
}

static vkb_report_dev *rdev(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->report : NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL rp_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
    vkb_report_dev *d = rdev();
    VkResult r = d ? d->submit(q, n, s, f) : VK_ERROR_DEVICE_LOST;
    if (r == VK_ERROR_DEVICE_LOST) watch_result("vkQueueSubmit", r, 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL rp_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{
    vkb_report_dev *d = rdev();
    VkResult r = d ? d->submit2(q, n, s, f) : VK_ERROR_DEVICE_LOST;
    if (r == VK_ERROR_DEVICE_LOST) watch_result("vkQueueSubmit2", r, 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL rp_WaitForFences(VkDevice dev, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{
    vkb_report_dev *d = rdev();
    double t = mono_s();
    VkResult r = d ? d->wait_fences(dev, n, f, all, timeout) : VK_ERROR_DEVICE_LOST;
    watch_result("vkWaitForFences", r, r == VK_SUCCESS ? (mono_s() - t) * 1000.0 : 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL rp_WaitSemaphores(VkDevice dev, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    vkb_report_dev *d = rdev();
    double t = mono_s();
    VkResult r = d ? d->wait_sems(dev, wi, timeout) : VK_ERROR_DEVICE_LOST;
    watch_result("vkWaitSemaphores", r, r == VK_SUCCESS ? (mono_s() - t) * 1000.0 : 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL rp_DeviceWaitIdle(VkDevice dev)
{
    vkb_report_dev *d = rdev();
    VkResult r = d ? d->device_idle(dev) : VK_ERROR_DEVICE_LOST;
    watch_result("vkDeviceWaitIdle", r, 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL rp_QueueWaitIdle(VkQueue q)
{
    vkb_report_dev *d = rdev();
    VkResult r = d ? d->queue_idle(q) : VK_ERROR_DEVICE_LOST;
    watch_result("vkQueueWaitIdle", r, 0);
    return r;
}

void vkb_report_device_init(vkb_srv_table *t)
{
    if (off()) return;
    vkb_report_dev *d = calloc(1, sizeof(*d));
    if (!d) return;
    vkb_dispatch *dt = &t->dt;
    d->submit = dt->vkQueueSubmit;
    d->submit2 = dt->vkQueueSubmit2;
    d->wait_fences = dt->vkWaitForFences;
    d->wait_sems = dt->vkWaitSemaphores;
    d->device_idle = dt->vkDeviceWaitIdle;
    d->queue_idle = dt->vkQueueWaitIdle;
    t->report = d;
    if (d->submit) dt->vkQueueSubmit = rp_QueueSubmit;
    if (d->submit2) dt->vkQueueSubmit2 = rp_QueueSubmit2;
    if (d->wait_fences) dt->vkWaitForFences = rp_WaitForFences;
    if (d->wait_sems) dt->vkWaitSemaphores = rp_WaitSemaphores;
    if (d->device_idle) dt->vkDeviceWaitIdle = rp_DeviceWaitIdle;
    if (d->queue_idle) dt->vkQueueWaitIdle = rp_QueueWaitIdle;
}

void vkb_report_device_destroy(vkb_srv_table *t)
{
    free(t->report);
    t->report = NULL;
}

/* ------------------------------------------------------------------ sampling */

static long read_num(const char *path)
{
    char b[64];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *e;
    long v = strtol(b, &e, 10);
    return e == b ? -1 : v;
}

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f) return -1;
    char line[160];
    long v = -1;
    size_t n = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, n) && line[n] == ':') {
            v = strtol(line + n + 1, NULL, 10);
            break;
        }
    fclose(f);
    return v;
}

static long self_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "re");
    if (!f) return -1;
    char line[160];
    long v = -1;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "VmRSS:", 6)) v = strtol(line + 6, NULL, 10);
    fclose(f);
    return v;
}

/* The GPU clock in MHz from the first source the app may read (ClockLogComponent's list). */
static long gpu_mhz(void)
{
    static const char *const fixed[] = {"/sys/kernel/gpu/gpu_clock", "/sys/class/misc/mali0/device/cur_freq", "/sys/class/misc/mali0/device/clock",
                                        "/sys/kernel/ged/hal/current_freqency", "/sys/kernel/ged/hal/current_frequency"};
    if (!gpu_src[0]) {
        for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]) && !gpu_src[0]; i++)
            if (read_num(fixed[i]) > 0) snprintf(gpu_src, sizeof(gpu_src), "%s", fixed[i]);
        DIR *d = gpu_src[0] ? NULL : opendir("/sys/class/devfreq");
        struct dirent *e;
        while (d && (e = readdir(d)) && !gpu_src[0]) {
            if (!strcasestr(e->d_name, "mali") && !strcasestr(e->d_name, "gpu")) continue;
            char p[300];
            snprintf(p, sizeof(p), "/sys/class/devfreq/%s/cur_freq", e->d_name);
            if (read_num(p) > 0) snprintf(gpu_src, sizeof(gpu_src), "%.199s", p);
        }
        if (d) closedir(d);
        if (!gpu_src[0]) snprintf(gpu_src, sizeof(gpu_src), "-");
    }
    if (gpu_src[0] == '-') return -1;
    long v = read_num(gpu_src);
    if (v <= 0) return -1;
    return v > 10000000 ? v / 1000000 : v > 10000 ? v / 1000 : v;
}

/* CPU: the fastest cluster's current clock (MHz), and the lowest cap any cluster has (scaling max
 * against hardware max, %: Samsung's SSRM lowers scaling_max_freq when the device warms). */
static void cpu_sample(unsigned *top, unsigned *cap)
{
    *top = 0;
    *cap = 100;
    DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "policy", 6)) continue;
        char p[300];
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpufreq/%.200s/scaling_cur_freq", e->d_name);
        long cur = read_num(p);
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpufreq/%.200s/scaling_max_freq", e->d_name);
        long max = read_num(p);
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpufreq/%.200s/cpuinfo_max_freq", e->d_name);
        long hw = read_num(p);
        if (cur > 0 && (unsigned)(cur / 1000) > *top) *top = (unsigned)(cur / 1000);
        if (max > 0 && hw > 0 && (unsigned)(max * 100 / hw) < *cap) *cap = (unsigned)(max * 100 / hw);
    }
    closedir(d);
}

/* Android's thermal status from the app's clock log (vkbridge-clocks.log, "thermal N"), else -1. */
static int thermal_status(void)
{
    if (!clocks_log[0]) return -1;
    int fd = open(clocks_log, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char b[400];
    off_t end = lseek(fd, 0, SEEK_END);
    ssize_t n = pread(fd, b, sizeof(b) - 1, end > (off_t)sizeof(b) - 1 ? end - (off_t)sizeof(b) + 1 : 0);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *p = NULL, *q = b;
    while ((q = strstr(q, "thermal "))) p = q++;
    return p ? atoi(p + 8) : -1;
}

/* The client's frame statistics (client/frames.c): one line in <dir>/vkbridge-frames-<pid>.txt. */
static void read_frames(void)
{
    DIR *d = opendir(rdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        int pid = 0;
        if (sscanf(e->d_name, "vkbridge-frames-%d.txt", &pid) != 1) continue;
        char p[900], line[700];
        snprintf(p, sizeof(p), "%s/%s", rdir, e->d_name);
        FILE *f = fopen(p, "re");
        if (!f) continue;
        int got = fgets(line, sizeof(line), f) != NULL;
        fclose(f);
        if (!got) continue;
        line[strcspn(line, "\n")] = 0;
        pthread_mutex_lock(&rlock);
        rp_proc *pr = proc_find(pid, 1);
        if (pr && strcmp(pr->frames, line)) {
            snprintf(pr->frames, sizeof(pr->frames), "%s", line);
            const char *s = strstr(line, "t_first=");
            double tf = s ? atof(s + 8) : 0;
            if (tf > 0) pr->first_frame_s = tf - t0_mono;
            s = strstr(line, "fps_p50=");
            pr->fps_p50 = s ? atof(s + 8) : 0;
            if (!pr->comm[0] && (s = strstr(line, "comm="))) sscanf(s + 5, "%31s", pr->comm);
            dirty = 1;
        }
        pthread_mutex_unlock(&rlock);
    }
    closedir(d);
}

static void sample(void)
{
    long avail = meminfo_kb("MemAvailable");
    long rss = self_rss_kb();
    unsigned top = 0, cap = 100;
    cpu_sample(&top, &cap);
    long gpu = gpu_mhz();
    int th = thermal_status();
    /* each live process's memory first, outside our lock (the accounting has its own) */
    int pids[RP_PROCS], n = 0;
    int64_t mem[RP_PROCS];
    pthread_mutex_lock(&rlock);
    for (int i = 0; i < nprocs; i++)
        if (procs[i].alive) pids[n++] = procs[i].pid;
    pthread_mutex_unlock(&rlock);
    for (int i = 0; i < n; i++) {
        int64_t tot, im, bc, src;
        vkb_memstat_query(pids[i], &mem[i], &tot, &im, &bc, &src);
    }
    pthread_mutex_lock(&rlock);
    if (avail >= 0 && (avail_min_kb < 0 || avail < avail_min_kb)) avail_min_kb = avail;
    if (rss > rss_peak_kb) rss_peak_kb = rss;
    if (cap < cpu_cap_min) cpu_cap_min = cap;
    if (gpu > 0 && (!gpu_min_all || (unsigned)gpu < gpu_min_all)) gpu_min_all = (unsigned)gpu;
    if (gpu > 0 && (unsigned)gpu > gpu_max_all) gpu_max_all = (unsigned)gpu;
    if (th > thermal_max_all) thermal_max_all = th;
    for (int i = 0; i < nprocs; i++) {
        rp_proc *p = &procs[i];
        if (!p->alive) continue;
        for (int k = 0; k < n; k++)
            if (pids[k] == p->pid && mem[k] > p->gpu_mem_peak) p->gpu_mem_peak = mem[k];
        if (avail >= 0 && (p->avail_min_kb < 0 || avail < p->avail_min_kb)) p->avail_min_kb = avail;
        if (!p->samples || top < p->cpu_top_min) p->cpu_top_min = top;
        if (top > p->cpu_top_max) p->cpu_top_max = top;
        if (!p->samples || cap < p->cpu_cap_min) p->cpu_cap_min = cap;
        if (gpu > 0 && (!p->gpu_min || (unsigned)gpu < p->gpu_min)) p->gpu_min = (unsigned)gpu;
        if (gpu > 0 && (unsigned)gpu > p->gpu_max) p->gpu_max = (unsigned)gpu;
        if (!p->samples || th > p->thermal_max) p->thermal_max = th;
        p->samples++;
        dirty = 1;
    }
    pthread_mutex_unlock(&rlock);
}

/* ------------------------------------------------------------------ writing */

static void js(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

static void md_text(FILE *f, const char *s)
{
    for (; s && *s; s++) fputc(*s == '|' ? '/' : *s == '\n' ? ' ' : *s, f);
}

static const char *prop(const char *name, char *buf)
{
    buf[0] = 0;
#ifdef __ANDROID__
    __system_property_get(name, buf);
#else
    /* a desktop (host tests): the distribution in place of the Android release */
    FILE *f = !strcmp(name, "ro.build.version.release") ? fopen("/etc/os-release", "re") : NULL;
    char line[200];
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, "PRETTY_NAME=", 12)) {
            snprintf(buf, 92, "%.80s", line + 12);
            buf[strcspn(buf, "\"\n")] = 0;
            if (buf[0] == '"') memmove(buf, buf + 1, strlen(buf));
            buf[strcspn(buf, "\"")] = 0;
        }
    if (f) fclose(f);
#endif
    return buf;
}

static const char *proc_name(const rp_proc *p, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s%s%s%s", p->comm[0] ? p->comm : "?", p->key[0] ? " (" : "", p->key, p->key[0] ? ")" : "");
    return buf;
}

static const char *engine_text(const rp_proc *p, char *buf, size_t cap)
{
    if (!p->engine[0]) snprintf(buf, cap, "-");
    else
        snprintf(buf, cap, "%s %u.%u.%u", p->engine, VK_API_VERSION_MAJOR(p->engine_version), VK_API_VERSION_MINOR(p->engine_version),
                 VK_API_VERSION_PATCH(p->engine_version));
    return buf;
}

typedef struct issues {
    FILE *f;
    int n;
    int json;
} issues;

static void issue(issues *is, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void issue(issues *is, const char *fmt, ...)
{
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    is->n++;
    if (!is->f) return;
    if (is->json) {
        fprintf(is->f, "%s\n    ", is->n > 1 ? "," : "");
        js(is->f, line);
    } else {
        fprintf(is->f, "%d. ", is->n);
        md_text(is->f, line);
        fputc('\n', is->f);
    }
}

static void probe_issue(void *arg, const char *line) { issue(arg, "%s", line); }

static const rp_proc *by_pid(int pid)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].pid == pid) return &procs[i];
    return NULL;
}

/* Test switches left on, which change what the session does (and costs): SPIR-V evidence mode
 * (<cache dir>/dump-spirv exists: every driver module saved there - 249 MB once on a tablet that
 * nobody remembered setting it on) and the <cache dir>/env overrides. Each a line into cb. */
static double dump_scan_s = -1e9;
static long long dump_bytes;
static long dump_files;

static void test_switches(void (*cb)(void *arg, const char *line), void *arg)
{
    const char *dir = vkb_pcache_dir();
    char line[1700], path[700];
    if (dir) {
        snprintf(path, sizeof(path), "%s/dump-spirv", dir);
        if (access(path, F_OK) == 0) {
            if (now_s() - dump_scan_s > 60) { /* thousands of files: not every 5 s */
                dump_scan_s = now_s();
                dump_bytes = 0;
                dump_files = 0;
                DIR *d = opendir(path);
                struct dirent *e;
                while (d && (e = readdir(d)) && dump_files < 200000) {
                    struct stat st;
                    if (e->d_name[0] == '.' || fstatat(dirfd(d), e->d_name, &st, 0) != 0) continue;
                    dump_bytes += st.st_size;
                    dump_files++;
                }
                if (d) closedir(d);
            }
            snprintf(line, sizeof(line),
                     "test switch on: SPIR-V evidence mode (%s exists: every shader module the driver gets is saved there; %ld files, %lld MB) - delete the directory unless collecting evidence",
                     path, dump_files, dump_bytes >> 20);
            cb(arg, line);
        }
    }
    char env[1500];
    if (*vkb_srv_env_list(env, sizeof(env))) {
        snprintf(line, sizeof(line), "test switch on: overrides from %s/env in effect: %s", dir ? dir : "<cache dir>", env);
        cb(arg, line);
    }
}

static void md_switch(void *arg, const char *line)
{
    fprintf(arg, "> **Warning, %s**\n\n", line);
}

/* Everything that should be looked at, one line each (the report's head). */
static int list_issues(FILE *f, int json)
{
    issues is = {f, 0, json};
    test_switches(probe_issue, &is);
    vkb_probe_issues(probe_issue, &is);
    char nm[120];
    for (int lv = VKB_LOG_ERROR; lv <= VKB_LOG_WARN; lv++)
        for (int i = 0; i < nclasses; i++) {
            const rp_class *c = &classes[i];
            if (c->level != lv) continue;
            if (!strncmp(c->first, "probe: ", 7)) continue; /* the probe's own lines are listed above */
            const rp_proc *p = by_pid(c->pid);
            issue(&is, "%s %s: %s (x%u, first at %.1f s)", lv == VKB_LOG_ERROR ? "error" : "warning",
                  c->pid ? (p ? proc_name(p, nm, sizeof(nm)) : "process") : "server", c->first, c->count, c->first_s);
        }
    for (int i = 0; i < nprocs; i++) {
        const rp_proc *p = &procs[i];
        proc_name(p, nm, sizeof(nm));
        if (p->lost) issue(&is, "%s: device lost %u time(s), first in %s", nm, p->lost, p->lost_first);
        if (p->long_waits) issue(&is, "%s: %u GPU wait(s) longer than 2 s (longest %.1f s)", nm, p->long_waits, p->long_wait_max_ms / 1000.0);
        if (p->compile_fail) issue(&is, "%s: %llu pipeline(s) failed to build", nm, (unsigned long long)p->compile_fail);
        if (p->fps_p50 > 0 && p->fps_p50 < 20) issue(&is, "%s: low frame rate, median %.1f fps", nm, p->fps_p50);
        if (p->devices && p->first_frame_s < 0 && p->t_device > 0 && (p->alive ? now_s() : p->t_gone) - p->t_device > 60)
            issue(&is, "%s: a device for %.0f s and no frame presented", nm, (p->alive ? now_s() : p->t_gone) - p->t_device);
        if (p->samples && p->cpu_cap_min < 70) issue(&is, "%s: CPU clocks capped to %u%% of their maximum while it ran (thermal)", nm, p->cpu_cap_min);
        if (p->thermal_max >= 3) issue(&is, "%s: Android thermal status reached %d", nm, p->thermal_max);
        if (p->avail_min_kb >= 0 && p->avail_min_kb < 600 * 1024)
            issue(&is, "%s: system memory ran low (%ld MB available at the lowest)", nm, p->avail_min_kb / 1024);
    }
    if (dropped_classes) issue(&is, "report: %d further log classes not recorded (table full)", dropped_classes);
    return is.n;
}

typedef struct artefact {
    char path[400];
    long long size;
    int fresh;
} artefact;

static int artefacts(artefact *a, int max)
{
    const char *dir = vkb_pcache_dir();
    if (!dir) return 0;
    int n = 0;
    static const char *const subs[] = {"", "stuck-spirv", "dump-spirv"};
    for (size_t k = 0; k < 3 && n < max; k++) {
        char d[700];
        snprintf(d, sizeof(d), "%s%s%s", dir, *subs[k] ? "/" : "", subs[k]);
        DIR *dd = opendir(d);
        if (!dd) continue;
        struct dirent *e;
        int many = 0;
        while ((e = readdir(dd)) && n < max) {
            if (e->d_name[0] == '.') continue;
            if (!*subs[k] && strncmp(e->d_name, "failed-", 7) && strcmp(e->d_name, "stuck-pipelines") && strncmp(e->d_name, "probe-", 6)) continue;
            char p[1000];
            snprintf(p, sizeof(p), "%s/%s", d, e->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            int fresh = st.st_mtime >= t0_wall;
            if (!fresh && *subs[k]) continue; /* the dump folders: this session's files only */
            if (++many > 40) continue;
            snprintf(a[n].path, sizeof(a[n].path), "%.399s", p);
            a[n].size = (long long)st.st_size;
            a[n].fresh = fresh;
            n++;
        }
        closedir(dd);
    }
    return n;
}

static void write_md(FILE *f, int final)
{
    char b1[200], b2[200], b3[200], nm[120];
    struct tm tm;
    time_t now = time(NULL);
    localtime_r(&now, &tm);
    char ts[64], ts0[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    localtime_r(&t0_wall, &tm);
    strftime(ts0, sizeof(ts0), "%Y-%m-%d %H:%M:%S", &tm);
    /* line 1: the mode, so two reports are compared like for like (malibridge_report_diff.py) */
    int mode = vkb_mode();
    const char *tw;
    double to = vkb_guard_timeout_s(mode, &tw);
    fprintf(f, "Mode: %s (%s) - %s; compile watchdog %.0f s (%s).\n\n", vkb_mode_name(mode), vkb_mode_source_name(), vkb_mode_summary(mode), to, tw);
    fprintf(f, "# MaliBridge session report\n\n");
    fprintf(f, "Session started %s, report written %s (%.0f s in)%s. Machine-readable: `malibridge-report.json`.\n\n", ts0, ts, now_s(),
            final ? ", final" : "; rewritten every 5 s");
    test_switches(md_switch, f);
    int n = list_issues(NULL, 0);
    fprintf(f, "## Summary: %d issue%s\n\n", n, n == 1 ? "" : "s");
    if (n) list_issues(f, 0);
    else fprintf(f, "Nothing to report: every probe passed and nothing was logged as an error or warning.\n");
    fprintf(f, "\n## Device\n\n| | |\n|---|---|\n");
    struct utsname u;
    uname(&u);
#ifdef __ANDROID__
    fprintf(f, "| model | %s %s (%s) |\n", prop("ro.product.manufacturer", b1), prop("ro.product.model", b2), u.machine);
    fprintf(f, "| SoC | %s |\n", prop("ro.soc.model", b1));
    fprintf(f, "| Android | %s (SDK %s), build %s |\n", prop("ro.build.version.release", b1), prop("ro.build.version.sdk", b2),
            prop("ro.build.display.id", b3));
#else
    (void)b2;
    (void)b3;
    fprintf(f, "| OS | %s (%s), not Android |\n", prop("ro.build.version.release", b1), u.machine);
#endif
    fprintf(f, "| kernel | %s %s |\n", u.sysname, u.release);
    fprintf(f, "| memory | %ld MB total, %ld MB available at the lowest |\n", mem_total_kb / 1024, avail_min_kb / 1024);
    struct statvfs sv;
    if (vkb_pcache_dir() && statvfs(vkb_pcache_dir(), &sv) == 0)
        fprintf(f, "| storage | %llu MB free where the caches live |\n", (unsigned long long)sv.f_bavail * sv.f_frsize >> 20);
    fprintf(f, "| clocks | CPU clusters capped to %u%% at the lowest; ", cpu_cap_min);
    if (gpu_max_all) fprintf(f, "GPU %u-%u MHz (from %s)", gpu_min_all, gpu_max_all, gpu_src);
    else fprintf(f, "GPU clock not readable");
    if (thermal_max_all >= 0) fprintf(f, "; Android thermal status max %d (vkbridge-clocks.log)", thermal_max_all);
    fprintf(f, " |\n");
    fprintf(f, "| server | pid %d, %s, RSS peak %lld MB |\n", getpid(),
#ifdef VKB_JNI
            "in the app's process",
#else
            "its own process",
#endif
            (long long)rss_peak_kb / 1024);
    for (int i = 0; i < vkb_npds; i++) {
        const vkb_pd_knowledge *k = &vkb_pds[i];
        if (!k->valid) continue;
        fprintf(f, "| GPU %d | %s, Vulkan %u.%u.%u to the server; memory sharing %s; missing 0x%x, emulating 0x%x |\n", i, k->name,
                VK_API_VERSION_MAJOR(k->info.real_api_version), VK_API_VERSION_MINOR(k->info.real_api_version),
                VK_API_VERSION_PATCH(k->info.real_api_version), vkb_mem_strategy_name(k->info.strategy), k->info.missing, k->info.emu);
    }
    fprintf(f, "\n## Bridge\n\nBuild `%s`, protocol `%016llx`. Cache directory `%s`.\n\n", VKB_GIT, (unsigned long long)VKB_PROTOCOL_HASH,
            vkb_pcache_dir() ? vkb_pcache_dir() : "(none)");
    char env[1500];
    fprintf(f, "Overrides from `<cache dir>/env`: `%s`\n\n", *vkb_srv_env_list(env, sizeof(env)) ? env : "(none)");
    fprintf(f, "Server environment:");
    int any = 0;
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, "VKBRIDGE_", 9) || !strncmp(*e, "VK_", 3)) fprintf(f, "%s `%s`", any++ ? "," : "", *e);
    fprintf(f, "%s\n\n## Startup probe\n\n", any ? "" : " (no VKBRIDGE_/VK_ variables)");
    vkb_probe_write_md(f);
    fprintf(f, "## Processes\n\n");
    if (!nprocs) fprintf(f, "No client connected yet.\n\n");
    for (int i = 0; i < nprocs; i++) {
        const rp_proc *p = &procs[i];
        fprintf(f, "### %s - pid %d%s\n\n", proc_name(p, nm, sizeof(nm)), p->pid, p->alive ? " (running)" : "");
        fprintf(f, "- application `%s`, engine %s, Vulkan %u.%u requested\n", p->app[0] ? p->app : "-", engine_text(p, b1, sizeof(b1)),
                VK_API_VERSION_MAJOR(p->api), VK_API_VERSION_MINOR(p->api));
        fprintf(f, "- connected at %.1f s", p->t_hello);
        if (p->devices) fprintf(f, ", first device +%.1f s", p->t_device - p->t_hello);
        if (p->first_frame_s >= 0) fprintf(f, ", first frame +%.1f s", p->first_frame_s - p->t_hello);
        if (!p->alive && p->t_gone > 0) fprintf(f, ", exited at %.1f s (ran %.1f s)", p->t_gone, p->t_gone - p->t_hello);
        fprintf(f, "; %d device(s)%s\n", p->devices, p->devices_failed ? " (and failed creations)" : "");
        if (p->frames[0]) {
            fprintf(f, "- frames: `");
            md_text(f, p->frames);
            fprintf(f, "`\n");
        } else if (p->devices) {
            fprintf(f, "- frames: none reported by the client\n");
        }
        if (p->compiles) {
            fprintf(f, "- pipelines: %llu built in %.1f s of compile time, longest %.0f ms, %llu failed; by time: <10 ms %u, <50 %u, <200 %u, <1 s %u, "
                       "<2 s %u, <5 s %u, <10 s %u, more %u\n",
                    (unsigned long long)p->compiles, p->compile_ms / 1000.0, p->compile_max_ms, (unsigned long long)p->compile_fail, p->hist[0],
                    p->hist[1], p->hist[2], p->hist[3], p->hist[4], p->hist[5], p->hist[6], p->hist[7]);
            for (uint32_t s = 0; s < p->nslow; s++) fprintf(f, "  - slow: %s\n", p->slow[s]);
        }
        if (p->exts[0]) fprintf(f, "- device extensions it asked for that the driver lacks (emulated, aliased or client-side): %s\n", p->exts);
        if (p->features[0]) fprintf(f, "- features it enabled that the GPU lacks (emulated): %s\n", p->features);
        if (p->lost || p->long_waits)
            fprintf(f, "- GPU: device lost %u time(s)%s%s; %u wait(s) over 2 s (longest %.1f s)\n", p->lost, p->lost ? ", first in " : "", p->lost_first,
                    p->long_waits, p->long_wait_max_ms / 1000.0);
        if (p->samples) {
            fprintf(f, "- while it ran: GPU memory peak %lld MB; system memory available >= %ld MB; fastest CPU cluster %u-%u MHz, capped to %u%%",
                    (long long)(p->gpu_mem_peak >> 20), p->avail_min_kb / 1024, p->cpu_top_min, p->cpu_top_max, p->cpu_cap_min);
            if (p->gpu_max) fprintf(f, "; GPU %u-%u MHz", p->gpu_min, p->gpu_max);
            if (p->thermal_max >= 0) fprintf(f, "; thermal status max %d", p->thermal_max);
            fprintf(f, "\n");
        }
        int header = 0;
        for (int c = 0; c < nclasses; c++) {
            const rp_class *k = &classes[c];
            if (k->pid != p->pid) continue;
            if (!header++) fprintf(f, "\n| level | count | first at | first occurrence |\n|---|---:|---:|---|\n");
            fprintf(f, "| %s | %u | %.1f s | ", k->level == VKB_LOG_ERROR ? "**error**" : k->level == VKB_LOG_WARN ? "warning" : "info", k->count,
                    k->first_s);
            md_text(f, k->first);
            fprintf(f, " |\n");
        }
        fprintf(f, "\n");
    }
    fprintf(f, "## Server (not tied to a process)\n\n");
    int header = 0;
    for (int c = 0; c < nclasses; c++) {
        const rp_class *k = &classes[c];
        if (k->pid && by_pid(k->pid)) continue;
        if (!header++) fprintf(f, "| level | count | first at | first occurrence |\n|---|---:|---:|---|\n");
        fprintf(f, "| %s | %u | %.1f s | ", k->level == VKB_LOG_ERROR ? "**error**" : k->level == VKB_LOG_WARN ? "warning" : "info", k->count, k->first_s);
        md_text(f, k->first);
        fprintf(f, " |\n");
    }
    if (!header) fprintf(f, "Nothing logged.\n");
    artefact a[64];
    int na = artefacts(a, 64);
    fprintf(f, "\n## Artefacts\n\n");
    if (!na) fprintf(f, "None (no saved SPIR-V, stuck pipelines or failed modules).\n");
    for (int i = 0; i < na; i++) fprintf(f, "- `%s` (%lld bytes%s)\n", a[i].path, a[i].size, a[i].fresh ? ", this session" : "");
    if (na) fprintf(f, "\nPull them with `adb shell run-as com.droiddeck.launcher cat <path>`; `spirv-dis` reads the .spv files.\n");
}

static void write_json(FILE *f, int final)
{
    char nm[120], b1[200];
    fprintf(f, "{\n  \"malibridge_report\": 1,\n  \"final\": %s,\n  \"seconds\": %.1f,\n  \"started\": %lld,\n", final ? "true" : "false", now_s(),
            (long long)t0_wall);
    int mode = vkb_mode();
    fprintf(f, "  \"mode\": {\"value\": \"%s\", \"source\": ", vkb_mode_name(mode));
    js(f, vkb_mode_source_name());
    fprintf(f, ", \"compile_timeout_s\": %.1f},\n", vkb_guard_timeout_s(mode, NULL));
    fprintf(f, "  \"issue_count\": %d,\n  \"issues\": [", list_issues(NULL, 1));
    list_issues(f, 1);
    struct utsname u;
    uname(&u);
    fprintf(f, "\n  ],\n  \"device\": {\"model\": ");
    js(f, prop("ro.product.model", b1));
    fprintf(f, ", \"manufacturer\": ");
    js(f, prop("ro.product.manufacturer", b1));
    fprintf(f, ", \"soc\": ");
    js(f, prop("ro.soc.model", b1));
    fprintf(f, ", \"android\": ");
    js(f, prop("ro.build.version.release", b1));
    fprintf(f, ", \"kernel\": ");
    js(f, u.release);
    fprintf(f, ", \"mem_total_mb\": %ld, \"mem_available_min_mb\": %ld, \"cpu_cap_min_pct\": %u, \"gpu_mhz_min\": %u, \"gpu_mhz_max\": %u, "
               "\"thermal_max\": %d, \"server_rss_peak_mb\": %lld},\n",
            mem_total_kb / 1024, avail_min_kb / 1024, cpu_cap_min, gpu_min_all, gpu_max_all, thermal_max_all, (long long)rss_peak_kb / 1024);
    fprintf(f, "  \"bridge\": {\"build\": ");
    js(f, VKB_GIT);
    fprintf(f, ", \"protocol\": \"%016llx\", \"cache_dir\": ", (unsigned long long)VKB_PROTOCOL_HASH);
    js(f, vkb_pcache_dir() ? vkb_pcache_dir() : "");
    char env[1500];
    fprintf(f, ", \"env_overrides\": ");
    js(f, vkb_srv_env_list(env, sizeof(env)));
    fprintf(f, "},\n  \"gpus\": [");
    for (int i = 0; i < vkb_npds; i++) {
        const vkb_pd_knowledge *k = &vkb_pds[i];
        fprintf(f, "%s{\"name\": ", i ? ", " : "");
        js(f, k->name);
        fprintf(f, ", \"api\": \"%u.%u.%u\", \"memory_sharing\": ", VK_API_VERSION_MAJOR(k->info.real_api_version), VK_API_VERSION_MINOR(k->info.real_api_version),
                VK_API_VERSION_PATCH(k->info.real_api_version));
        js(f, vkb_mem_strategy_name(k->info.strategy));
        fprintf(f, ", \"missing\": \"0x%x\", \"emulating\": \"0x%x\"}", k->info.missing, k->info.emu);
    }
    fprintf(f, "],\n  \"probe\": ");
    vkb_probe_write_json(f);
    fprintf(f, ",\n  \"processes\": [");
    for (int i = 0; i < nprocs; i++) {
        const rp_proc *p = &procs[i];
        fprintf(f, "%s\n    {\"pid\": %d, \"name\": ", i ? "," : "", p->pid);
        js(f, proc_name(p, nm, sizeof(nm)));
        fprintf(f, ", \"comm\": ");
        js(f, p->comm);
        fprintf(f, ", \"key\": ");
        js(f, p->key);
        fprintf(f, ", \"app\": ");
        js(f, p->app);
        fprintf(f, ", \"engine\": ");
        js(f, engine_text(p, b1, sizeof(b1)));
        fprintf(f, ", \"alive\": %s, \"t_connect\": %.1f, \"t_device\": %.1f, \"t_first_frame\": %.1f, \"t_exit\": %.1f, \"devices\": %d, "
                   "\"devices_failed\": %d,\n     \"frames\": ",
                p->alive ? "true" : "false", p->t_hello, p->devices ? p->t_device : -1.0, p->first_frame_s, p->alive ? -1.0 : p->t_gone, p->devices,
                p->devices_failed);
        js(f, p->frames);
        fprintf(f, ", \"fps_p50\": %.1f,\n     \"compiles\": %llu, \"compile_ms\": %.1f, \"compile_max_ms\": %.1f, \"compile_failed\": %llu, "
                   "\"compile_hist\": [%u, %u, %u, %u, %u, %u, %u, %u], \"slow_compiles\": [",
                p->fps_p50, (unsigned long long)p->compiles, p->compile_ms, p->compile_max_ms, (unsigned long long)p->compile_fail, p->hist[0],
                p->hist[1], p->hist[2], p->hist[3], p->hist[4], p->hist[5], p->hist[6], p->hist[7]);
        for (uint32_t s = 0; s < p->nslow; s++) {
            if (s) fprintf(f, ", ");
            js(f, p->slow[s]);
        }
        fprintf(f, "],\n     \"missing_extensions\": ");
        js(f, p->exts);
        fprintf(f, ", \"emulated_features\": ");
        js(f, p->features);
        fprintf(f, ", \"device_lost\": %u, \"long_waits\": %u, \"long_wait_max_ms\": %.0f, \"gpu_mem_peak_mb\": %lld, \"mem_available_min_mb\": %ld, "
                   "\"cpu_mhz\": [%u, %u], \"cpu_cap_min_pct\": %u, \"gpu_mhz\": [%u, %u], \"thermal_max\": %d,\n     \"log\": [",
                p->lost, p->long_waits, p->long_wait_max_ms, (long long)(p->gpu_mem_peak >> 20), p->avail_min_kb / 1024, p->cpu_top_min, p->cpu_top_max,
                p->cpu_cap_min, p->gpu_min, p->gpu_max, p->thermal_max);
        int k = 0;
        for (int c = 0; c < nclasses; c++) {
            if (classes[c].pid != p->pid) continue;
            fprintf(f, "%s\n       {\"level\": \"%s\", \"count\": %u, \"first_s\": %.1f, \"text\": ", k++ ? "," : "",
                    classes[c].level == VKB_LOG_ERROR ? "error" : classes[c].level == VKB_LOG_WARN ? "warning" : "info", classes[c].count,
                    classes[c].first_s);
            js(f, classes[c].first);
            fprintf(f, "}");
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n  ],\n  \"server_log\": [");
    int k = 0;
    for (int c = 0; c < nclasses; c++) {
        if (classes[c].pid && by_pid(classes[c].pid)) continue;
        fprintf(f, "%s\n    {\"level\": \"%s\", \"count\": %u, \"first_s\": %.1f, \"text\": ", k++ ? "," : "",
                classes[c].level == VKB_LOG_ERROR ? "error" : classes[c].level == VKB_LOG_WARN ? "warning" : "info", classes[c].count,
                classes[c].first_s);
        js(f, classes[c].first);
        fprintf(f, "}");
    }
    artefact a[64];
    int na = artefacts(a, 64);
    fprintf(f, "\n  ],\n  \"artefacts\": [");
    for (int i = 0; i < na; i++) {
        fprintf(f, "%s{\"path\": ", i ? ", " : "");
        js(f, a[i].path);
        fprintf(f, ", \"bytes\": %lld, \"this_session\": %s}", a[i].size, a[i].fresh ? "true" : "false");
    }
    fprintf(f, "]\n}\n");
}

static void write_file(const char *name, void (*fn)(FILE *, int), int final)
{
    char path[700], tmp[720];
    snprintf(path, sizeof(path), "%s/%s", rdir, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f && errno == ENOENT && vkb_mkdirs(rdir) == 0) {
        /* its directory went (on Android it can be the app's cache directory, emptied when storage runs out) */
        static int told;
        if (!told++) VKB_WARN("report directory %s was removed; recreated", rdir);
        f = fopen(tmp, "we");
    }
    if (!f) return;
    fn(f, final);
    int ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok) rename(tmp, path);
    else unlink(tmp); /* a full storage: the last whole report stays */
}

static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;

static void write_report(int final)
{
    if (off()) return;
    int was = in_report;
    in_report = 1;
    pthread_mutex_lock(&wlock);
    pthread_mutex_lock(&rlock);
    dirty = 0;
    write_file("malibridge-report.md", write_md, final);
    write_file("malibridge-report.json", write_json, final);
    pthread_mutex_unlock(&rlock);
    pthread_mutex_unlock(&wlock);
    in_report = was;
}

static void *report_main(void *arg)
{
    (void)arg;
    in_report = 1;
    for (unsigned tick = 0;; tick++) {
        sleep(1);
        if (off()) continue;
        if (tick % 2 == 0) sample();
        if (tick % 5 == 0) {
            read_frames();
            if (dirty) write_report(0);
        }
    }
    return NULL;
}

void vkb_report_session(const char *log_path)
{
    const char *dir_env = vkb_srv_env("VKBRIDGE_REPORT_DIR");
    char dir[600] = "";
    if (dir_env && *dir_env) snprintf(dir, sizeof(dir), "%s", dir_env);
    else if (log_path) {
        snprintf(dir, sizeof(dir), "%s", log_path);
        char *s = strrchr(dir, '/');
        if (s) *s = 0;
        else snprintf(dir, sizeof(dir), ".");
    }
    if (rdir[0]) write_report(1); /* the previous session's, final */
    pthread_mutex_lock(&rlock);
    snprintf(rdir, sizeof(rdir), "%s", dir);
    snprintf(clocks_log, sizeof(clocks_log), "%s/vkbridge-clocks.log", dir);
    t0_mono = mono_s();
    t0_wall = time(NULL);
    nclasses = 0;
    dropped_classes = 0;
    /* processes of the previous session are gone with it (the in-app server outlives sessions) */
    int keep = 0;
    for (int i = 0; i < nprocs; i++)
        if (procs[i].alive) {
            procs[keep] = procs[i];
            procs[keep].t_hello = 0;
            keep++;
        }
    nprocs = keep;
    avail_min_kb = -1;
    rss_peak_kb = 0;
    cpu_cap_min = 100;
    gpu_min_all = gpu_max_all = 0;
    thermal_max_all = -1;
    mem_total_kb = meminfo_kb("MemTotal");
    dirty = 1;
    pthread_mutex_unlock(&rlock);
    if (off()) return;
    if (!started_thread) {
        started_thread = 1;
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, report_main, NULL) != 0) started_thread = 0;
        pthread_attr_destroy(&a);
    }
    in_report = 1;
    VKB_INFO("session report: %s/malibridge-report.md (and .json), rewritten every 5 s", rdir);
    in_report = 0;
}
