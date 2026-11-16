/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Always-on performance diagnostics: for each process that presents frames,
 * every PF_WINDOW_S seconds, two lines in this log and a summary in the session report, so one Send
 * logs says what limited a game without a switch or a second log:
 *
 *   perf: pid=4242 comm=GTA5.exe s=10.0 verdict=GPU n=598 fps=59.8 ft_p50=16.6 ft_p90=18.1 worst_ms=40
 *         over=0 repeats=3 gpu_busy=93 gpu_mhz=848 gpu_mhz_max=1100 present_ms=12.1 driver_ms=11.4
 *         queue_ms=0.1 ipc_ms=0.6 local_ms=0.0 app_wait_ms=1.0 top_cpu=62 game_cpu=245 compile_ms=0
 *         upload_ms=0 rpasses=610 b2i_copies=12 buf_creates=4 submits=600 hitches=none thermal=1 cap=100
 *         mem_avail_mb=2100 swap_used_mb=900
 *   perf-threads: pid=4242 comm=GTA5.exe s=10.0 cpu=245 top=GTA5.exe:92@p4:70/p7:30,dxvk-submit:41@p7,...
 *         others=steamwebhelper:12@p0,gamescope-wl:8@p4,Xwayland:3@p0,server:15@p4
 *
 * (each one line). Where the numbers come from, none of it in a game's frame path:
 *   - frames: the client's once-a-second window message (client/hitch.c, VKB_HITCH_K_WINDOW, on by default):
 *     frames, the longest, those over the hitch threshold, the frame times at 50/90 % and the repeated
 *     frames of that second (each frame over 1.5x the median counts round(frame / median) - 1 refreshes
 *     on which the picture before stayed), the app's waits; the server's hitch ring adds the compile,
 *     BC-conversion and allocation time of the same second (server_hitch.c window). p50/p90 of a
 *     window: the per-second values weighted by their frames;
 *   - the present wait split (per frame, ms): present_ms is every wait of the swapchain's own (the
 *     frame's fence, a free image), as the client timed it; the fence waits that were round trips carry
 *     VKB_F_PRESENT_WAIT and the server times them (server_conn.c): queue_ms = waiting for the process's
 *     earlier asynchronous requests (our order), driver_ms = the request's handler, the driver's wait
 *     for the GPU's work; ipc_ms = the rest of the round trip (the client's queue flush, the socket, the
 *     threads waking); local_ms = the waits that were no round trip (an image, the frame queue);
 *   - upload_ms is the bridge's own work only (BC conversions + vkAllocateMemory, the hitch ring): a game's own copies in its
 *     command buffers are not in it. rpasses / b2i_copies / buf_creates / submits are what the process sent in the window
 *     (server_conn.c vkb_srv_count_work, counted as each request runs): render pass instances begun, buffer-to-image copies
 *     recorded, vkCreateBuffer, queue submits. Per frame: divide by n. A GL game through Zink that writes a texture its frame
 *     already drew with shows one more render pass, one copy and two buffers per write (Half-Life's lightmaps with the
 *     flashlight on);
 *   - a thread of its own (vkb-perf), every PF_SAMPLE_S: the GPU busy % and clock (the first readable
 *     of the sysfs files below: the clocks the app's vkbridge-clocks.log reads, and the vendors'
 *     utilisation files; gpu_busy=? when none answers: retail MediaTek and Exynos refuse some),
 *     MemAvailable and swap, the CPU clusters' caps, Android's thermal status (the clock log), and each
 *     presenting process's threads from /proc/<pid>/task/<tid>/stat (CPU time, and the core it is on,
 *     named by its cpufreq policy: p4 = policy4); every window the background processes (Steam, its
 *     web helper, gamescope, Xwayland, wineserver, FEX's server, this server's own process: "server",
 *     which in the app is the app's process) the same way;
 *   - the verdict, the first of (thresholds below): COMPILE (compiles >= 15 % of the window, or half its
 *     hitches), UPLOAD (BC conversions + allocations, the same), MEMORY (MemAvailable < 500 MB), THERMAL
 *     (thermal status >= 3, or a cluster capped below 70 %), GPU (busy >= 85 %), GPU-or-latency (busy unreadable
 *     and the driver's fence wait >= 60 % of the window: the GPU's work or a serialised CPU+GPU round trip, not
 *     measured), CPU (a game thread >= 85 % of a core), PRESENT (our part of the present wait, queue + IPC,
 *     >= 2 ms and >= 15 % of the median frame; or repeated frames >= 10 % of the frames with no hitch, neither
 *     GPU nor CPU over its threshold), else NONE (a frame cap, vsync, a light scene). Every input is on the line
 *     for another reading offline.
 *   - "cpu-threads:" (server_cpuprof.h), a third line a window: for the game's busiest threads (PF_XTOP by CPU so far, one
 *     sample a second from this thread: /proc/<tid>/schedstat, state, wchan) how long each ran, waited for a core
 *     (rq) and sat idle, and what the idle samples waited in. cost_us_s is what this sampling cost, a second.
 *   - "cpu-cap:" (P4), per cpufreq policy the highest clock seen while a process presented against cpuinfo_max_freq
 *     ("p4=1300/2000MHz(65%):held top=88%": never above 65 %, at that top 88 % of the samples), after two windows
 *     of samples, then when the held set changes (at most once a minute) and every 10 minutes; the report keeps the last;
 *   - "perf-collapse:" (P5), a window's fps under half the run's median window (after 3 windows, median >= 20 fps):
 *     state=start, then state=end once back over three quarters; vkbridge-perf-state.txt in the session folder says the
 *     same for the app (perf_state_write).
 * Nothing is read while no process presents. Needs the client's window messages (hitch attribution, on by default;
 * VKBRIDGE_HITCH_MS=0 turns those and so these off too).
 * Lines: PF_LINES of each kind per process, then every PF_THIN-th window. The report's Performance
 * section (server_report.c) sums the windows per process: verdict counts, medians, the busiest threads
 * over the run. VKBRIDGE_PERF_DIAG=0 turns it off. Host tests: VKBRIDGE_TEST_PERF_PERIODS=<sample s>,<window s>,
 * VKBRIDGE_GPU_SYSFS_ROOT (a prefix for the GPU's files), VKBRIDGE_CPUFREQ_DIR, VKBRIDGE_TEST_MEMINFO.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_cpuprof.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define PF_SAMPLE_S 2.0
#define PF_WINDOW_S 10.0
#define PF_PROCS 16
#define PF_THREADS 512
#define PF_BG 1024      /* background tasks followed */
#define PF_BGNAMES 16
#define PF_SLOTS 16     /* clusters (or CPUs without a cpufreq map) */
#define PF_CPUS 256
#define PF_SECS 64      /* per-second p50/p90 kept a window */
#define PF_TOP 6
#define PF_XTOP 8       /* threads followed by schedstat / state / wchan (cpu-threads line) */
#define PF_XLINE 5      /* threads on the cpu-threads line */
#define PF_LINES 1080   /* 3 hours of windows a process, then every PF_THIN-th */
#define PF_THIN 6
#define PF_ACTIVE_NS 3000000000ull /* a window message this recent: the process is presenting */

/* verdict thresholds */
#define T_WORK_SHARE 0.15
#define T_MEM_MB 500
#define T_THERMAL 3
#define T_CAP_PCT 70
#define T_GPU_BUSY 85
#define T_GPU_WAIT_SHARE 0.60
#define T_CPU_THREAD 85
#define T_OURS_MS 2.0
#define T_OURS_SHARE 0.15
#define T_REPEAT_SHARE 0.10

enum { V_CPU, V_GPU, V_PRESENT, V_COMPILE, V_UPLOAD, V_THERMAL, V_MEMORY, V_GPULAT, V_NONE, V_COUNT };
/* GPU-or-latency: GPU busy unreadable and the driver's fence wait >= T_GPU_WAIT_SHARE of the window - the wait is the
 * GPU's work or a serialised CPU+GPU round trip, not told apart (the report adds "unmeasured") */
static const char *const vname[V_COUNT] = {"CPU", "GPU", "PRESENT", "COMPILE", "UPLOAD", "THERMAL", "MEMORY", "GPU-or-latency", "NONE"};
static const char *const cause_names[VKB_PERF_CAUSES] = {"compile", "bc", "alloc", "pcache", "wait", "other"};

typedef struct pf_thread {
    int tid, seen;
    char name[16];
    uint64_t last, win, total;          /* clock ticks: at the last sample, this window, the whole run */
    uint32_t slot_win[PF_SLOTS], slot_total[PF_SLOTS];
    cpuprof_acc cp;                     /* schedstat / state / wchan this window (the busiest threads only) */
} pf_thread;

typedef struct pf_proc {
    int pid, used, gone;
    char comm[32];
    uint64_t last_msg_ns;
    /* the window being filled */
    uint64_t t0, t1, susp_ns, frames, over, repeats, worst_ns, hitches[VKB_PERF_CAUSES];
    double app_ms, present_ms, prt_ms, compile_ms, upload_ms, driver_ms, queue_ms;
    uint32_t nsec, sec_p50[PF_SECS], sec_p90[PF_SECS], sec_n[PF_SECS];
    uint64_t pw_order, pw_srv, pw_n; /* the server's present-wait sums at the last message */
    uint64_t wk_last[4], wk[4];      /* the process's work counts (vkb_srv_proc wk_*) at the last message, and this window's */
    int pw_init;
    double gpu_busy_sum, gpu_mhz_sum;
    int gpu_busy_n, gpu_mhz_n;
    unsigned gpu_mhz_max_w;
    long avail_min_kb, swap_used_kb;
    int thermal_max;
    unsigned cap_min;
    /* threads */
    pf_thread *th;
    int nth;
    double cpu_s, cpu_total_s; /* sampled seconds: this window, the whole run */
    uint64_t xcost_ns;         /* the cpu-threads sampling's own time, this window */
    uint32_t xsamples;         /* its samples, this window */
    struct {
        int tid, fd[3];        /* a followed thread and its schedstat, stat, wchan (open) */
    } xs[PF_XTOP];
    uint64_t sample_ns;        /* the last sample */
    uint32_t lines, windows_seen;
    /* the run (the report) */
    uint32_t windows, verdicts[V_COUNT];
    uint32_t fps_hist[241];
    float w_p50[256], w_p90[256];
    uint32_t nw;
    uint64_t s_frames, s_repeats;
    double s_gpu_busy_sum, s_gpu_mhz_sum;
    int s_gpu_busy_n, s_gpu_mhz_n;
    unsigned s_gpu_busy_max, s_gpu_mhz_max;
    double s_present, s_driver, s_queue, s_ipc, s_local;
    /* fps collapse (P5): the window's fps under T_COLLAPSE of the run's median window */
    int collapsed;
    uint64_t collapse_ns;      /* since when (now_ns) */
    double last_fps, last_med;
    char ev[256];              /* a perf-collapse line for the caller to log after lk (empty: none) */
} pf_proc;

typedef struct pf_bg {
    int tid, pid, seen, name;
    uint64_t last;
} pf_bg;

typedef struct pf_bgname {
    char name[16];
    uint64_t win, total;
    uint32_t slot_win[PF_SLOTS], slot_total[PF_SLOTS];
} pf_bgname;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pf_proc procs[PF_PROCS];
static pf_bg bg[PF_BG];
static int nbg;
static pf_bgname bgn[PF_BGNAMES];
static int nbgn;
static double bg_win_s, bg_total_s;
static uint64_t bg_sample_ns;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int state = -1; /* -1 not decided, 0 off, 1 on */
static double sample_s = PF_SAMPLE_S, window_s = PF_WINDOW_S;
static long clk_tck = 100;
/* the cluster map: CPU -> slot, and each slot's name */
static int cpu_slot[PF_CPUS];
static char slot_name[PF_SLOTS][12];
static int nslots;
/* CPU cap inferred (clocks at 60-67 % of the ceilings with the cap file unreadable): per
 * cpufreq policy, the highest scaling_cur_freq seen while a process presented vs cpuinfo_max_freq, and the share of
 * samples at that highest clock (within 5 %): a cluster that never passed 90 % of its maximum is "held", and a high share
 * at its own top says the hold, not the load, set the clock. Reset at each session. */
static int slot_policy[PF_SLOTS];          /* the policy number of a pN slot, -1 for a cpuN slot */
static long cc_hw[PF_SLOTS], cc_max[PF_SLOTS], cc_last[PF_SLOTS];
static uint32_t cc_n[PF_SLOTS], cc_top[PF_SLOTS];
static uint32_t cc_samples, cc_held_said = ~0u;
static uint64_t cc_said_ns;
#define T_HELD_PCT 90
/* fps collapse (P5) */
#define T_COLLAPSE 0.5     /* a window under half the run's median fps: collapsed */
#define T_RECOVER 0.75     /* back over three quarters: over */
#define COLLAPSE_MIN_WINDOWS 3
#define COLLAPSE_MIN_FPS 20
/* GPU sources: chosen once (again a minute later while none answers) */
static char busy_src[300], mhz_src[300];
static uint64_t src_ns;
static double gpu_mhz_max_seen;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int vkb_perf_on(void)
{
    if (state < 0) {
        const char *v = vkb_srv_env_global("VKBRIDGE_PERF_DIAG");
        state = !(v && (!strcmp(v, "0") || !strcasecmp(v, "off")));
    }
    return state;
}

/* ------------------------------------------------------------------ readers */

static long read_first_num(const char *path, int *ok)
{
    *ok = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char b[96];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *p = b;
    while (*p && (*p < '0' || *p > '9')) p++;
    if (!*p) return -1;
    *ok = 1;
    return strtol(p, NULL, 10);
}

static void gpu_path(char *out, size_t cap, const char *path)
{
    const char *root = vkb_srv_env_global("VKBRIDGE_GPU_SYSFS_ROOT");
    snprintf(out, cap, "%s%s", root && *root ? root : "", path);
}

/* The GPU's busy % and clock sources: the clock as ClockLogComponent.kt and server_report.c read it, the busy %
 * from the vendors' utilisation files (MediaTek ged, Samsung/Exynos, kbase builds that have one, devfreq load). */
static void pick_sources(void)
{
    static const char *const busy[] = {"/sys/kernel/gpu/gpu_busy", "/sys/kernel/ged/hal/gpu_utilization",
                                       "/sys/class/misc/mali0/device/utilization", "/sys/class/misc/mali0/device/utilisation",
                                       "/proc/mali/utilization", "/sys/module/ged/parameters/gpu_loading"};
    static const char *const mhz[] = {"/sys/kernel/gpu/gpu_clock", "/sys/class/misc/mali0/device/cur_freq", "/sys/class/misc/mali0/device/clock",
                                      "/sys/kernel/ged/hal/current_freqency", "/sys/kernel/ged/hal/current_frequency"};
    char p[300];
    int ok;
    if (!busy_src[0])
        for (size_t i = 0; i < sizeof(busy) / sizeof(busy[0]) && !busy_src[0]; i++) {
            gpu_path(p, sizeof(p), busy[i]);
            long v = read_first_num(p, &ok);
            if (ok && v >= 0 && v <= 100) snprintf(busy_src, sizeof(busy_src), "%s", p);
        }
    if (!mhz_src[0])
        for (size_t i = 0; i < sizeof(mhz) / sizeof(mhz[0]) && !mhz_src[0]; i++) {
            gpu_path(p, sizeof(p), mhz[i]);
            if (read_first_num(p, &ok) > 0) snprintf(mhz_src, sizeof(mhz_src), "%s", p);
        }
    if (!busy_src[0] || !mhz_src[0]) {
        char d[300];
        gpu_path(d, sizeof(d), "/sys/class/devfreq");
        DIR *dir = opendir(d);
        struct dirent *e;
        while (dir && (e = readdir(dir))) {
            if (!strcasestr(e->d_name, "mali") && !strcasestr(e->d_name, "gpu")) continue;
            if (!busy_src[0]) {
                snprintf(p, sizeof(p), "%.200s/%.80s/load", d, e->d_name);
                long v = read_first_num(p, &ok);
                if (ok && v >= 0 && v <= 100) snprintf(busy_src, sizeof(busy_src), "%s", p);
            }
            if (!mhz_src[0]) {
                snprintf(p, sizeof(p), "%.200s/%.80s/cur_freq", d, e->d_name);
                if (read_first_num(p, &ok) > 0) snprintf(mhz_src, sizeof(mhz_src), "%s", p);
            }
        }
        if (dir) closedir(dir);
    }
    if (busy_src[0] || mhz_src[0])
        VKB_INFO("perf: GPU busy from %s, clock from %s", busy_src[0] ? busy_src : "nothing readable", mhz_src[0] ? mhz_src : "nothing readable");
}

static void gpu_read(int *busy, double *mhz)
{
    uint64_t t = now_ns();
    if ((!busy_src[0] || !mhz_src[0]) && (!src_ns || t - src_ns > 60000000000ull)) {
        src_ns = t;
        pick_sources();
    }
    int ok;
    *busy = -1;
    *mhz = -1;
    if (busy_src[0]) {
        long v = read_first_num(busy_src, &ok);
        if (ok && v >= 0 && v <= 100) *busy = (int)v;
    }
    if (mhz_src[0]) {
        long v = read_first_num(mhz_src, &ok);
        if (ok && v > 0) *mhz = v > 10000000 ? v / 1e6 : v > 10000 ? v / 1e3 : (double)v;
    }
}

static void mem_read(long *avail_kb, long *swap_used_kb)
{
    const char *t = vkb_srv_env_global("VKBRIDGE_TEST_MEMINFO");
    FILE *f = fopen(t && *t ? t : "/proc/meminfo", "re");
    *avail_kb = *swap_used_kb = -1;
    if (!f) return;
    char line[160];
    long st = -1, sf = -1;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemAvailable:", 13)) *avail_kb = strtol(line + 13, NULL, 10);
        else if (!strncmp(line, "SwapTotal:", 10)) st = strtol(line + 10, NULL, 10);
        else if (!strncmp(line, "SwapFree:", 9)) sf = strtol(line + 9, NULL, 10);
    }
    fclose(f);
    if (st >= 0 && sf >= 0) *swap_used_kb = st - sf;
}

static const char *cpufreq_dir(void)
{
    const char *v = vkb_srv_env_global("VKBRIDGE_CPUFREQ_DIR");
    return v && *v ? v : "/sys/devices/system/cpu/cpufreq";
}

/* "0-3" / "0 1 2 3" / "4-6,7" into cpu_slot */
static void map_cpus(const char *list, int slot)
{
    const char *p = list;
    while (*p) {
        while (*p && (*p < '0' || *p > '9')) p++;
        if (!*p) break;
        char *e;
        long a = strtol(p, &e, 10), b = a;
        if (*e == '-') b = strtol(e + 1, &e, 10);
        for (long c = a; c <= b && c < PF_CPUS; c++)
            if (c >= 0) cpu_slot[c] = slot;
        p = e;
    }
}

/* The clusters: one slot per cpufreq policy (named pN), CPUs it does not name get a slot each (cpuN). */
static void map_clusters(void)
{
    for (int i = 0; i < PF_CPUS; i++) cpu_slot[i] = -1;
    nslots = 0;
    DIR *d = opendir(cpufreq_dir());
    struct dirent *e;
    int ids[PF_SLOTS], n = 0;
    while (d && (e = readdir(d)))
        if (!strncmp(e->d_name, "policy", 6) && n < PF_SLOTS) ids[n++] = atoi(e->d_name + 6);
    if (d) closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && ids[j - 1] > ids[j]; j--) {
            int t = ids[j];
            ids[j] = ids[j - 1], ids[j - 1] = t;
        }
    for (int i = 0; i < n; i++) {
        char p[400], b[256];
        snprintf(p, sizeof(p), "%.300s/policy%d/related_cpus", cpufreq_dir(), ids[i]);
        FILE *f = fopen(p, "re");
        if (!f) {
            snprintf(p, sizeof(p), "%.300s/policy%d/affected_cpus", cpufreq_dir(), ids[i]);
            f = fopen(p, "re");
        }
        if (!f) continue;
        if (fgets(b, sizeof(b), f)) {
            snprintf(slot_name[nslots], sizeof(slot_name[0]), "p%d", ids[i]);
            slot_policy[nslots] = ids[i];
            map_cpus(b, nslots++);
        }
        fclose(f);
    }
}

static int slot_of(int cpu)
{
    if (cpu < 0 || cpu >= PF_CPUS) return -1;
    if (cpu_slot[cpu] < 0 && nslots < PF_SLOTS) {
        snprintf(slot_name[nslots], sizeof(slot_name[0]), "cpu%d", cpu);
        slot_policy[nslots] = -1;
        cpu_slot[cpu] = nslots++;
    }
    return cpu_slot[cpu];
}

/* The lowest cap of any cluster (scaling_max_freq of cpuinfo_max_freq, %), 0: none readable. */
static unsigned cap_read(void)
{
    DIR *d = opendir(cpufreq_dir());
    struct dirent *e;
    unsigned cap = 0;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "policy", 6)) continue;
        char p[600];
        int ok1, ok2;
        snprintf(p, sizeof(p), "%.300s/%.200s/scaling_max_freq", cpufreq_dir(), e->d_name);
        long mx = read_first_num(p, &ok1);
        snprintf(p, sizeof(p), "%.300s/%.200s/cpuinfo_max_freq", cpufreq_dir(), e->d_name);
        long hw = read_first_num(p, &ok2);
        if (ok1 && ok2 && mx > 0 && hw > 0) {
            unsigned c = (unsigned)(mx * 100 / hw);
            if (!cap || c < cap) cap = c;
        }
    }
    if (d) closedir(d);
    return cap;
}

/* P4: each policy's scaling_cur_freq (and once its cpuinfo_max_freq) into the session's per-cluster figures (takes lk) */
static void clocks_sample(void)
{
    long cur[PF_SLOTS];
    for (int s = 0; s < nslots; s++) {
        cur[s] = -1;
        if (slot_policy[s] < 0) continue;
        char p[400];
        int ok;
        if (!cc_hw[s]) {
            snprintf(p, sizeof(p), "%.300s/policy%d/cpuinfo_max_freq", cpufreq_dir(), slot_policy[s]);
            long hw = read_first_num(p, &ok);
            cc_hw[s] = ok && hw > 0 ? hw : -1;
        }
        if (cc_hw[s] <= 0) continue;
        snprintf(p, sizeof(p), "%.300s/policy%d/scaling_cur_freq", cpufreq_dir(), slot_policy[s]);
        long v = read_first_num(p, &ok);
        if (ok && v > 0) cur[s] = v;
    }
    pthread_mutex_lock(&lk);
    int any = 0;
    for (int s = 0; s < nslots; s++) {
        if (cur[s] <= 0) continue;
        any = 1;
        if (cur[s] > cc_max[s]) cc_max[s] = cur[s];
        cc_last[s] = cur[s];
        cc_n[s]++;
    }
    /* the share at the top is counted against the highest so far: a later, higher clock lowers it from then on */
    for (int s = 0; s < nslots; s++)
        if (cur[s] > 0 && cur[s] * 100 >= cc_max[s] * 95) cc_top[s]++;
    cc_samples += any;
    pthread_mutex_unlock(&lk);
}

/* P4: the "cpu-cap:" line (under lk); 0 when there is nothing to say yet. force: the report's copy */
static int clocks_text(char *out, size_t cap, int force)
{
    uint32_t min = (uint32_t)(2 * window_s / sample_s); /* two windows of samples */
    if (cc_samples < (min ? min : 1)) return 0;
    uint32_t held = 0;
    int n = snprintf(out, cap, "cpu-cap: samples=%u", cc_samples);
    for (int s = 0; s < nslots && n < (int)cap; s++) {
        if (slot_policy[s] < 0 || cc_hw[s] <= 0 || !cc_n[s]) continue;
        unsigned pct = (unsigned)(cc_max[s] * 100 / cc_hw[s]);
        int h = pct < T_HELD_PCT;
        if (h) held |= 1u << s;
        n += snprintf(out + n, cap - (size_t)n, " %s=%ld/%ldMHz(%u%%)%s top=%u%%", slot_name[s], cc_max[s] / 1000, cc_hw[s] / 1000, pct,
                      h ? ":held" : "", cc_n[s] ? (unsigned)(cc_top[s] * 100 / cc_n[s]) : 0);
    }
    if (n < (int)cap) {
        n += snprintf(out + n, cap - (size_t)n, " held=");
        int k = 0;
        for (int s = 0; s < nslots && n < (int)cap; s++)
            if (held & (1u << s)) n += snprintf(out + n, cap - (size_t)n, "%s%s", k++ ? "," : "", slot_name[s]);
        if (!k && n < (int)cap) n += snprintf(out + n, cap - (size_t)n, "none");
    }
    if (force) return n > 0;
    /* in the log: the first time, when the held set changes (at most once a minute), and every 10 minutes */
    uint64_t t = now_ns();
    int due = cc_held_said == ~0u || (held != cc_held_said && t - cc_said_ns >= 60000000000ull) || t - cc_said_ns >= 600000000000ull;
    if (!due) return 0;
    cc_held_said = held;
    cc_said_ns = t;
    return n > 0;
}

/* One task's stat: its name, utime + stime and the CPU it last ran on. */
static int task_stat(const char *path, char *name, uint64_t *ticks, int *cpu)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char b[1024];
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    char *l = strchr(b, '('), *r = strrchr(b, ')');
    if (!l || !r || r < l) return 0;
    size_t len = (size_t)(r - l - 1);
    if (len > 15) len = 15;
    memcpy(name, l + 1, len);
    name[len] = 0;
    for (char *c = name; *c; c++)
        if (*c == ' ' || *c == ',' || *c == ':' || *c == '@' || *c == '=' || *c == '|' || (unsigned char)*c < 0x20) *c = '_';
    /* after ")": state(3) ... utime(14) stime(15) ... processor(39) */
    char *p = r + 2;
    unsigned long long ut = 0, st = 0;
    int c = -1;
    for (int field = 3; *p && field <= 39; field++) {
        if (field == 14) ut = strtoull(p, NULL, 10);
        else if (field == 15) st = strtoull(p, NULL, 10);
        else if (field == 39) c = atoi(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    *ticks = ut + st;
    *cpu = c;
    return 1;
}

/* ------------------------------------------------------------------ from the window messages */

static void xs_close_all(pf_proc *p);

static pf_proc *find(int pid, int make)
{
    pf_proc *free_slot = NULL, *oldest_gone = NULL;
    for (int i = 0; i < PF_PROCS; i++) {
        if (procs[i].used && procs[i].pid == pid && !procs[i].gone) return &procs[i];
        if (!procs[i].used && !free_slot) free_slot = &procs[i];
        if (procs[i].used && procs[i].gone && (!oldest_gone || procs[i].last_msg_ns < oldest_gone->last_msg_ns)) oldest_gone = &procs[i];
    }
    if (!make) return NULL;
    pf_proc *p = free_slot ? free_slot : oldest_gone;
    if (!p) return NULL;
    pf_thread *th = p->th;
    xs_close_all(p);
    memset(p, 0, sizeof(*p));
    p->th = th ? th : calloc(PF_THREADS, sizeof(pf_thread));
    if (!p->th) return NULL;
    memset(p->th, 0, PF_THREADS * sizeof(pf_thread));
    p->used = 1;
    p->pid = pid;
    p->avail_min_kb = p->swap_used_kb = -1;
    p->thermal_max = -1;
    return p;
}

static void win_reset(pf_proc *p)
{
    p->t0 = p->t1 = p->susp_ns = p->frames = p->over = p->repeats = p->worst_ns = 0;
    memset(p->hitches, 0, sizeof(p->hitches));
    p->app_ms = p->present_ms = p->prt_ms = p->compile_ms = p->upload_ms = p->driver_ms = p->queue_ms = 0;
    memset(p->wk, 0, sizeof(p->wk));
    p->nsec = 0;
    p->gpu_busy_sum = p->gpu_mhz_sum = 0;
    p->gpu_busy_n = p->gpu_mhz_n = 0;
    p->gpu_mhz_max_w = 0;
    p->avail_min_kb = p->swap_used_kb = -1;
    p->thermal_max = -1;
    p->cap_min = 0;
    p->cpu_s = 0;
    p->xcost_ns = p->xsamples = 0;
    for (int i = 0; i < p->nth; i++) {
        cpuprof_acc_reset(&p->th[i].cp);
        p->th[i].win = 0;
        memset(p->th[i].slot_win, 0, sizeof(p->th[i].slot_win));
    }
}

static void *perf_main(void *arg);

static void start_thread(void)
{
    clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    const char *v = getenv("VKBRIDGE_TEST_PERF_PERIODS");
    double a = 0, b = 0;
    if (v && sscanf(v, "%lf,%lf", &a, &b) >= 1) {
        if (a > 0) sample_s = a;
        if (b > 0) window_s = b;
    }
    map_clusters();
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, perf_main, NULL) != 0) VKB_WARN("perf: no sampler thread; no perf lines this session");
    pthread_attr_destroy(&at);
}

void vkb_perf_window(vkb_srv_proc *sp, const vkb_perf_win *w)
{
    if (!sp || !w || !vkb_perf_on()) return;
    pthread_once(&once, start_thread);
    uint64_t po = __atomic_load_n(&sp->pw_order_ns, __ATOMIC_RELAXED), ps = __atomic_load_n(&sp->pw_srv_ns, __ATOMIC_RELAXED),
             pn = __atomic_load_n(&sp->pw_n, __ATOMIC_RELAXED);
    uint64_t wk[4] = {__atomic_load_n(&sp->wk_rpass, __ATOMIC_RELAXED), __atomic_load_n(&sp->wk_b2i, __ATOMIC_RELAXED),
                      __atomic_load_n(&sp->wk_bufs, __ATOMIC_RELAXED), __atomic_load_n(&sp->wk_submits, __ATOMIC_RELAXED)};
    pthread_mutex_lock(&lk);
    pf_proc *p = find(sp->pid, 1);
    if (p) {
        if (!p->comm[0]) snprintf(p->comm, sizeof(p->comm), "%s", sp->comm[0] ? sp->comm : "?");
        if (!p->t0) p->t0 = w->t0;
        if (w->t1 > p->t1) p->t1 = w->t1;
        p->last_msg_ns = now_ns();
        p->susp_ns += w->susp_ns;
        p->frames += w->frames;
        p->over += w->over;
        p->repeats += w->repeats;
        if (w->worst_ns > p->worst_ns) p->worst_ns = w->worst_ns;
        for (int i = 0; i < VKB_PERF_CAUSES; i++) p->hitches[i] += w->causes[i];
        p->app_ms += w->app_ms, p->present_ms += w->present_ms, p->prt_ms += w->present_rt_ms;
        p->compile_ms += w->compile_ms, p->upload_ms += w->bc_ms + w->alloc_ms;
        if (w->frames && w->ft_p50_us && p->nsec < PF_SECS) {
            p->sec_p50[p->nsec] = w->ft_p50_us, p->sec_p90[p->nsec] = w->ft_p90_us, p->sec_n[p->nsec] = (uint32_t)w->frames;
            p->nsec++;
        }
        if (p->pw_init) {
            p->queue_ms += (po - p->pw_order) / 1e6;
            p->driver_ms += (ps - p->pw_srv) / 1e6;
        }
        p->pw_order = po, p->pw_srv = ps, p->pw_n = pn, p->pw_init = 1;
        /* the work since the last message (the first message counts from the process's start: its loading) */
        for (int i = 0; i < 4; i++) p->wk[i] += wk[i] - p->wk_last[i], p->wk_last[i] = wk[i];
    }
    pthread_mutex_unlock(&lk);
}

/* ------------------------------------------------------------------ sampling (the vkb-perf thread) */

/* The tasks of pid into p's table (deltas since the last sample). Reads outside the lock into `got`. */
typedef struct task_read {
    int tid, cpu;
    uint64_t ticks;
    char name[16];
} task_read;

static int read_tasks(int pid, task_read *out, int max)
{
    char d[64];
    snprintf(d, sizeof(d), "/proc/%d/task", pid);
    DIR *dir = opendir(d);
    if (!dir) return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(dir)) && n < max) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[128];
        snprintf(p, sizeof(p), "/proc/%d/task/%.20s/stat", pid, e->d_name);
        if (task_stat(p, out[n].name, &out[n].ticks, &out[n].cpu)) out[n].tid = atoi(e->d_name), n++;
    }
    closedir(dir);
    return n;
}

static void merge_tasks(pf_proc *p, const task_read *t, int n, double dt)
{
    for (int i = 0; i < p->nth; i++) p->th[i].seen = 0;
    for (int k = 0; k < n; k++) {
        pf_thread *th = NULL;
        for (int i = 0; i < p->nth && !th; i++)
            if (p->th[i].tid == t[k].tid) th = &p->th[i];
        int fresh = 0;
        if (!th) {
            if (p->nth < PF_THREADS) th = &p->th[p->nth++];
            else
                for (int i = 0; i < p->nth && !th; i++) /* a thread gone since the last sample makes room */
                    if (!p->th[i].seen && p->th[i].tid != t[k].tid && !p->th[i].win) th = &p->th[i];
            if (!th) continue;
            memset(th, 0, sizeof(*th));
            th->tid = t[k].tid;
            fresh = 1;
        }
        memcpy(th->name, t[k].name, sizeof(th->name));
        th->seen = 1;
        /* a thread first seen now: its time before this sample is not this window's (except at the
         * process's first sample: nothing known before) */
        uint64_t d = fresh ? 0 : t[k].ticks >= th->last ? t[k].ticks - th->last : 0;
        th->last = t[k].ticks;
        th->win += d, th->total += d;
        int s = slot_of(t[k].cpu);
        if (s >= 0 && d) th->slot_win[s] += (uint32_t)d, th->slot_total[s] += (uint32_t)d;
    }
    p->cpu_s += dt, p->cpu_total_s += dt;
}

static void slots_text(const uint32_t *sl, char *out, size_t cap);

/* The files of the followed threads stay open (pread at offset 0 re-reads a /proc file): a sample is one syscall a file, not an open,
 * a read and a close (the path walk of an open was most of the cost). Entries belong to p->xs; closed when the thread leaves the
 * busiest PF_XTOP, and when the process slot is reused or the process is gone (under lk). */
static void xs_close(pf_proc *p, int slot)
{
    for (int f = 0; f < 3; f++)
        if (p->xs[slot].fd[f] > 0) close(p->xs[slot].fd[f]);
    memset(&p->xs[slot], 0, sizeof(p->xs[slot]));
}

static void xs_close_all(pf_proc *p)
{
    for (int i = 0; i < PF_XTOP; i++) xs_close(p, i);
}

/* One of a followed thread's files into b (NUL-terminated): 0 = unreadable. */
static int xs_read(int fd, char *b, size_t cap)
{
    if (fd <= 0) return 0;
    ssize_t n = pread(fd, b, cap - 1, 0);
    if (n <= 0) return 0;
    b[n] = 0;
    return 1;
}

static uint64_t thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The cpu-threads sample of pid: the PF_XTOP threads with the most CPU so far, each one's schedstat, state letter and wchan; a thread
 * with no baseline of its own yet only sets its base. Under lk throughout (the files are the table's). cost: this thread's own CPU time. */
static void cpu_extras(int pid)
{
    int tids[PF_XTOP], n = 0;
    uint64_t best[PF_XTOP];
    pthread_mutex_lock(&lk);
    pf_proc *p = find(pid, 0);
    if (!p || !p->sample_ns) {
        pthread_mutex_unlock(&lk);
        return;
    }
    uint64_t c0 = thread_cpu_ns();
    for (int i = 0; i < p->nth; i++) {
        uint64_t v = p->th[i].total;
        if (!p->th[i].seen) continue; /* a thread with no CPU yet is followed too, when there is room (a main thread that sleeps) */
        int pos = n;
        if (n < PF_XTOP) n++;
        else if (v > best[PF_XTOP - 1]) pos = PF_XTOP - 1;
        else continue;
        tids[pos] = p->th[i].tid, best[pos] = v;
        for (int j = pos; j > 0 && best[j] > best[j - 1]; j--) {
            uint64_t tv = best[j];
            best[j] = best[j - 1], best[j - 1] = tv;
            int tt = tids[j];
            tids[j] = tids[j - 1], tids[j - 1] = tt;
        }
    }
    /* entries of threads that left the busiest set are closed first, so a new one can take the room */
    for (int e = 0; e < PF_XTOP; e++) {
        int keep = 0;
        for (int k = 0; k < n && p->xs[e].tid; k++) keep |= p->xs[e].tid == tids[k];
        if (p->xs[e].tid && !keep) xs_close(p, e);
    }
    for (int k = 0; k < n; k++) {
        int e = -1, freeslot = -1;
        for (int j = 0; j < PF_XTOP; j++) {
            if (p->xs[j].tid == tids[k]) e = j;
            else if (!p->xs[j].tid && freeslot < 0) freeslot = j;
        }
        if (e >= 0 || freeslot < 0) continue;
        static const char *const files[3] = {"schedstat", "stat", "wchan"};
        for (int f = 0; f < 3; f++) {
            char path[96];
            snprintf(path, sizeof(path), "/proc/%d/task/%d/%s", pid, tids[k], files[f]);
            p->xs[freeslot].fd[f] = open(path, O_RDONLY | O_CLOEXEC);
        }
        p->xs[freeslot].tid = tids[k];
    }
    for (int k = 0; k < n; k++) {
        int e = -1;
        for (int j = 0; j < PF_XTOP; j++)
            if (p->xs[j].tid == tids[k]) e = j;
        if (e < 0) continue;
        char b[256];
        cpuprof_sched sc;
        if (!xs_read(p->xs[e].fd[0], b, sizeof(b)) || !cpuprof_parse_schedstat(b, &sc)) continue;
        char st = '?';
        int wc = CPUPROF_WC_NONE;
        if (xs_read(p->xs[e].fd[1], b, sizeof(b))) {
            char *r = strrchr(b, ')');
            if (r && r[1] == ' ') st = r[2];
        }
        if (st != 'R' && xs_read(p->xs[e].fd[2], b, sizeof(b))) wc = cpuprof_wchan_class(b);
        for (int i = 0; i < p->nth; i++)
            if (p->th[i].tid == tids[k]) {
                cpuprof_acc_sample(&p->th[i].cp, &sc, st, wc);
                break;
            }
    }
    p->xcost_ns += thread_cpu_ns() - c0;
    p->xsamples++;
    pthread_mutex_unlock(&lk);
}

/* The window's cpu-threads line (under lk): the PF_XLINE threads that ran most, "" when none was followed. */
static void cpu_threads_text(const pf_proc *p, double secs, char *out, size_t cap)
{
    int idx[PF_XLINE], n = 0;
    for (int i = 0; i < p->nth; i++) {
        const cpuprof_acc *a = &p->th[i].cp;
        if (!a->have || !(a->st_r + a->st_s + a->st_d + a->st_o)) continue;
        int pos = n;
        if (n < PF_XLINE) n++;
        else if (a->run_ns > p->th[idx[PF_XLINE - 1]].cp.run_ns) pos = PF_XLINE - 1;
        else continue;
        idx[pos] = i;
        for (int j = pos; j > 0 && p->th[idx[j]].cp.run_ns > p->th[idx[j - 1]].cp.run_ns; j--) {
            int t = idx[j];
            idx[j] = idx[j - 1], idx[j - 1] = t;
        }
    }
    out[0] = 0;
    if (!n) return;
    size_t o = (size_t)snprintf(out, cap, "cpu-threads: pid=%d comm=%s s=%.1f samples=%u cost_us_s=%.0f top=", p->pid, p->comm, p->cpu_s, p->xsamples,
                                p->cpu_s > 0 ? p->xcost_ns / 1e3 / p->cpu_s : 0.0);
    for (int k = 0; k < n && o < cap; k++) {
        const pf_thread *th = &p->th[idx[k]];
        char sl[64] = "", one[400];
        slots_text(th->slot_win, sl, sizeof(sl));
        cpuprof_acc_text(&th->cp, th->name[0] ? th->name : "?", secs, sl[0] ? sl + (sl[0] == '@' ? 1 : 0) : "-", one, sizeof(one));
        o += (size_t)snprintf(out + o, cap - o, "%s%s", k ? " " : "", one);
    }
}

static const char *const bg_names[] = {"steamwebhelper", "steam", "gamescope", "Xwayland", "wineserver", "FEXServer"};

static int bg_name_slot(const char *name)
{
    for (int i = 0; i < nbgn; i++)
        if (!strcmp(bgn[i].name, name)) return i;
    if (nbgn == PF_BGNAMES) return -1;
    memset(&bgn[nbgn], 0, sizeof(bgn[0]));
    snprintf(bgn[nbgn].name, sizeof(bgn[0].name), "%s", name);
    return nbgn++;
}

/* The background processes by name (their tasks), deltas since the last scan. Not under lk while reading. */
static void sample_bg(void)
{
    static task_read tr[2048];
    static int tr_pid[2048], tr_name[2048];
    int n = 0;
    int self = getpid();
    DIR *d = opendir("/proc");
    struct dirent *e;
    pthread_mutex_lock(&lk);
    int game[PF_PROCS], ng = 0;
    for (int i = 0; i < PF_PROCS; i++)
        if (procs[i].used && !procs[i].gone) game[ng++] = procs[i].pid;
    pthread_mutex_unlock(&lk);
    while (d && (e = readdir(d)) && n < 2048) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name), isgame = 0;
        for (int i = 0; i < ng; i++) isgame |= game[i] == pid;
        if (isgame) continue;
        const char *label = NULL;
        if (pid == self) label = "server";
        else {
            char p[64], comm[32] = "";
            snprintf(p, sizeof(p), "/proc/%d/comm", pid);
            int fd = open(p, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            ssize_t k = read(fd, comm, sizeof(comm) - 1);
            close(fd);
            if (k <= 0) continue;
            comm[k] = 0;
            comm[strcspn(comm, "\n")] = 0;
            for (size_t i = 0; i < sizeof(bg_names) / sizeof(bg_names[0]) && !label; i++)
                if (!strncmp(comm, bg_names[i], strlen(bg_names[i]))) label = bg_names[i];
            if (!label) continue;
        }
        int m = read_tasks(pid, tr + n, 2048 - n);
        if (m <= 0) continue;
        pthread_mutex_lock(&lk);
        int slot = bg_name_slot(label);
        pthread_mutex_unlock(&lk);
        for (int i = 0; i < m; i++) tr_pid[n + i] = pid, tr_name[n + i] = slot;
        n += m;
    }
    if (d) closedir(d);
    uint64_t t = now_ns();
    pthread_mutex_lock(&lk);
    double dt = bg_sample_ns ? (t - bg_sample_ns) / 1e9 : 0;
    bg_sample_ns = t;
    for (int i = 0; i < nbg; i++) bg[i].seen = 0;
    for (int k = 0; k < n; k++) {
        if (tr_name[k] < 0) continue;
        pf_bg *b = NULL;
        for (int i = 0; i < nbg && !b; i++)
            if (bg[i].tid == tr[k].tid && bg[i].pid == tr_pid[k]) b = &bg[i];
        int fresh = 0;
        if (!b) {
            if (nbg < PF_BG) b = &bg[nbg++];
            else continue;
            b->tid = tr[k].tid, b->pid = tr_pid[k], b->last = tr[k].ticks;
            fresh = 1;
        }
        b->seen = 1;
        b->name = tr_name[k];
        uint64_t dd = fresh || tr[k].ticks < b->last ? 0 : tr[k].ticks - b->last;
        b->last = tr[k].ticks;
        pf_bgname *bn = &bgn[b->name];
        bn->win += dd, bn->total += dd;
        int s = slot_of(tr[k].cpu);
        if (s >= 0 && dd) bn->slot_win[s] += (uint32_t)dd, bn->slot_total[s] += (uint32_t)dd;
    }
    /* tasks gone: their entries go (the table stays small) */
    int keep = 0;
    for (int i = 0; i < nbg; i++)
        if (bg[i].seen) bg[keep++] = bg[i];
    nbg = keep;
    bg_win_s += dt, bg_total_s += dt;
    pthread_mutex_unlock(&lk);
}

/* "@p4" (one cluster >= 90 %), else "@p4:70/p7:30" (clusters with >= 10 %) */
static void slots_text(const uint32_t *sl, char *out, size_t cap)
{
    uint64_t tot = 0;
    for (int s = 0; s < nslots; s++) tot += sl[s];
    out[0] = 0;
    if (!tot) {
        snprintf(out, cap, "@-");
        return;
    }
    for (int s = 0; s < nslots; s++)
        if (sl[s] * 10 >= tot * 9) {
            snprintf(out, cap, "@%s", slot_name[s]);
            return;
        }
    size_t o = 0;
    o += (size_t)snprintf(out, cap, "@");
    int k = 0;
    for (int s = 0; s < nslots && o < cap; s++)
        if (sl[s] * 10 >= tot) o += (size_t)snprintf(out + o, cap - o, "%s%s:%u", k++ ? "/" : "", slot_name[s], (unsigned)(sl[s] * 100 / tot));
}

/* the threads' text, busiest first: "name:pct@clusters,..." (total: whole run, else this window); returns the busiest's % */
static unsigned threads_text(const pf_proc *p, int total, char *out, size_t cap, unsigned *sum_pct)
{
    double secs = total ? p->cpu_total_s : p->cpu_s;
    int idx[PF_TOP];
    int n = 0;
    uint64_t sum = 0;
#define TV(i) (total ? p->th[i].total : p->th[i].win)
    for (int i = 0; i < p->nth; i++) {
        uint64_t v = TV(i);
        sum += v;
        if (!v) continue;
        int pos = n;
        if (n < PF_TOP) n++;
        else if (v > TV(idx[PF_TOP - 1])) pos = PF_TOP - 1;
        else continue;
        idx[pos] = i;
        for (int j = pos; j > 0 && TV(idx[j]) > TV(idx[j - 1]); j--) {
            int t = idx[j];
            idx[j] = idx[j - 1], idx[j - 1] = t;
        }
    }
#undef TV
    size_t o = 0;
    out[0] = 0;
    unsigned top = 0;
    for (int k = 0; k < n && o < cap; k++) {
        const pf_thread *th = &p->th[idx[k]];
        uint64_t v = total ? th->total : th->win;
        unsigned pct = secs > 0 ? (unsigned)(v * 100.0 / clk_tck / secs + 0.5) : 0;
        if (!k) top = pct;
        char sl[64];
        slots_text(total ? th->slot_total : th->slot_win, sl, sizeof(sl));
        o += (size_t)snprintf(out + o, cap - o, "%s%s:%u%s", k ? "," : "", th->name[0] ? th->name : "?", pct, sl);
    }
    if (!n) snprintf(out, cap, "-");
    if (sum_pct) *sum_pct = secs > 0 ? (unsigned)(sum * 100.0 / clk_tck / secs + 0.5) : 0;
    return top;
}

static void bg_text(int total, char *out, size_t cap)
{
    double secs = total ? bg_total_s : bg_win_s;
    size_t o = 0;
    out[0] = 0;
    int k = 0;
    for (int i = 0; i < nbgn && o < cap; i++) {
        uint64_t v = total ? bgn[i].total : bgn[i].win;
        unsigned pct = secs > 0 ? (unsigned)(v * 100.0 / clk_tck / secs + 0.5) : 0;
        char sl[64];
        slots_text(total ? bgn[i].slot_total : bgn[i].slot_win, sl, sizeof(sl));
        o += (size_t)snprintf(out + o, cap - o, "%s%s:%u%s", k++ ? "," : "", bgn[i].name, pct, pct ? sl : "");
    }
    if (!k) snprintf(out, cap, "-");
}

/* weighted quantile of per-second values */
static double wq(const uint32_t *v, const uint32_t *w, uint32_t n, double q)
{
    uint32_t idx[PF_SECS];
    uint64_t tot = 0;
    for (uint32_t i = 0; i < n; i++) idx[i] = i, tot += w[i];
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && v[idx[j - 1]] > v[idx[j]]; j--) {
            uint32_t t = idx[j];
            idx[j] = idx[j - 1], idx[j - 1] = t;
        }
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += w[idx[i]];
        if (acc * 1.0 > q * tot) return v[idx[i]] / 1000.0;
    }
    return n ? v[idx[n - 1]] / 1000.0 : 0;
}

static double medianf(const float *v, uint32_t n)
{
    float t[256];
    if (!n) return 0;
    memcpy(t, v, n * sizeof(float));
    for (uint32_t i = 1; i < n; i++)
        for (uint32_t j = i; j > 0 && t[j - 1] > t[j]; j--) {
            float x = t[j];
            t[j] = t[j - 1], t[j - 1] = x;
        }
    return n % 2 ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2;
}

/* The window's lines (under lk), then it starts again. */
static int finish(pf_proc *p, char *l1, size_t c1, char *l2, size_t c2, char *l3, size_t c3)
{
    l3[0] = 0;
    double secs = (double)(p->t1 - p->t0 - (p->susp_ns < p->t1 - p->t0 ? p->susp_ns : 0)) / 1e9;
    int out = 0;
    if (p->frames && secs > 0) {
        double n = (double)p->frames, fps = n / secs;
        double p50 = wq(p->sec_p50, p->sec_n, p->nsec, 0.5), p90 = wq(p->sec_p90, p->sec_n, p->nsec, 0.9);
        double present = p->present_ms / n, driver = p->driver_ms / n, queue = p->queue_ms / n;
        double ipc = (p->prt_ms - p->driver_ms - p->queue_ms) / n, local = (p->present_ms - p->prt_ms) / n;
        if (ipc < 0) ipc = 0;
        if (local < 0) local = 0;
        int busy = p->gpu_busy_n ? (int)(p->gpu_busy_sum / p->gpu_busy_n + 0.5) : -1;
        double mhz = p->gpu_mhz_n ? p->gpu_mhz_sum / p->gpu_mhz_n : -1;
        unsigned game_cpu = 0;
        char top[600];
        unsigned top_cpu = threads_text(p, 0, top, sizeof(top), &game_cpu);
        uint64_t hit = 0, compile_h = p->hitches[0], upload_h = p->hitches[1] + p->hitches[2];
        for (int i = 0; i < VKB_PERF_CAUSES; i++) hit += p->hitches[i];
        double wall_ms = secs * 1000;
        long avail_mb = p->avail_min_kb >= 0 ? p->avail_min_kb / 1024 : -1, swap_mb = p->swap_used_kb >= 0 ? p->swap_used_kb / 1024 : -1;
        int v;
        if (p->compile_ms >= T_WORK_SHARE * wall_ms || (hit >= 2 && compile_h * 2 >= hit)) v = V_COMPILE;
        else if (p->upload_ms >= T_WORK_SHARE * wall_ms || (hit >= 2 && upload_h * 2 >= hit)) v = V_UPLOAD;
        else if (avail_mb >= 0 && avail_mb < T_MEM_MB) v = V_MEMORY;
        else if (p->thermal_max >= T_THERMAL || (p->cap_min && p->cap_min < T_CAP_PCT)) v = V_THERMAL;
        else if (busy >= T_GPU_BUSY) v = V_GPU;
        else if (busy < 0 && p->driver_ms >= T_GPU_WAIT_SHARE * wall_ms) v = V_GPULAT;
        else if (top_cpu >= T_CPU_THREAD) v = V_CPU;
        else if ((queue + ipc >= T_OURS_MS && queue + ipc >= T_OURS_SHARE * p50) || (p->repeats >= T_REPEAT_SHARE * n && !p->over))
            v = V_PRESENT;
        else v = V_NONE;
        char hc[160] = "";
        size_t o = 0;
        for (int i = 0; i < VKB_PERF_CAUSES; i++)
            if (p->hitches[i]) o += (size_t)snprintf(hc + o, sizeof(hc) - o, "%s%s:%llu", o ? "," : "", cause_names[i], (unsigned long long)p->hitches[i]);
        if (!o) snprintf(hc, sizeof(hc), "none");
        char gb[16], gm[16], th[16], cp[16];
        if (busy >= 0) snprintf(gb, sizeof(gb), "%d", busy);
        else snprintf(gb, sizeof(gb), "?");
        if (mhz > 0) snprintf(gm, sizeof(gm), "%.0f", mhz);
        else snprintf(gm, sizeof(gm), "?");
        if (p->thermal_max >= 0) snprintf(th, sizeof(th), "%d", p->thermal_max);
        else snprintf(th, sizeof(th), "?");
        if (p->cap_min) snprintf(cp, sizeof(cp), "%u", p->cap_min);
        else snprintf(cp, sizeof(cp), "?");
        p->windows_seen++;
        int write = p->lines < PF_LINES || p->windows_seen % PF_THIN == 0;
        if (write) {
            p->lines++;
            snprintf(l1, c1,
                     "perf: pid=%d comm=%s s=%.1f verdict=%s n=%llu fps=%.1f ft_p50=%.1f ft_p90=%.1f worst_ms=%.0f over=%llu repeats=%llu "
                     "gpu_busy=%s gpu_mhz=%s gpu_mhz_max=%.0f present_ms=%.2f driver_ms=%.2f queue_ms=%.2f ipc_ms=%.2f local_ms=%.2f "
                     "app_wait_ms=%.2f top_cpu=%u game_cpu=%u compile_ms=%.0f upload_ms=%.0f rpasses=%llu b2i_copies=%llu buf_creates=%llu "
                     "submits=%llu hitches=%s thermal=%s cap=%s mem_avail_mb=%ld "
                     "swap_used_mb=%ld",
                     p->pid, p->comm, secs, vname[v], (unsigned long long)p->frames, fps, p50, p90, p->worst_ns / 1e6,
                     (unsigned long long)p->over, (unsigned long long)p->repeats, gb, gm, gpu_mhz_max_seen, present, driver, queue, ipc, local,
                     p->app_ms / n, top_cpu, game_cpu, p->compile_ms, p->upload_ms, (unsigned long long)p->wk[0],
                     (unsigned long long)p->wk[1], (unsigned long long)p->wk[2], (unsigned long long)p->wk[3], hc, th, cp, avail_mb, swap_mb);
            char others[600];
            bg_text(0, others, sizeof(others));
            snprintf(l2, c2, "perf-threads: pid=%d comm=%s s=%.1f cpu=%u top=%s others=%s", p->pid, p->comm, p->cpu_s, game_cpu, top, others);
            cpu_threads_text(p, p->cpu_s, l3, c3);
            out = 1;
        }
        /* the run's sums */
        p->windows++;
        p->verdicts[v]++;
        p->fps_hist[fps >= 240 ? 240 : (int)fps]++;
        /* P5: the fps collapse signal against the run's median window (this one included) */
        {
            uint32_t half = (p->windows + 1) / 2, acc = 0;
            double med = 0;
            for (int i = 0; i <= 240; i++)
                if ((acc += p->fps_hist[i]) >= half) {
                    med = i;
                    break;
                }
            p->last_fps = fps, p->last_med = med;
            p->ev[0] = 0;
            uint64_t tn = now_ns();
            if (!p->collapsed && p->windows > COLLAPSE_MIN_WINDOWS && med >= COLLAPSE_MIN_FPS && fps < T_COLLAPSE * med) {
                p->collapsed = 1;
                p->collapse_ns = tn;
                snprintf(p->ev, sizeof(p->ev), "perf-collapse: pid=%d comm=%s state=start fps=%.1f fps_med=%.0f ratio=%.2f window=%u", p->pid,
                         p->comm, fps, med, med > 0 ? fps / med : 0, p->windows);
            } else if (p->collapsed && fps >= T_RECOVER * med) {
                p->collapsed = 0;
                snprintf(p->ev, sizeof(p->ev), "perf-collapse: pid=%d comm=%s state=end fps=%.1f fps_med=%.0f ratio=%.2f window=%u lasted_s=%.0f",
                         p->pid, p->comm, fps, med, med > 0 ? fps / med : 0, p->windows, (tn - p->collapse_ns) / 1e9);
            }
        }
        if (p->nw < 256) p->w_p50[p->nw] = (float)p50, p->w_p90[p->nw] = (float)p90, p->nw++;
        p->s_frames += p->frames, p->s_repeats += p->repeats;
        if (busy >= 0) {
            p->s_gpu_busy_sum += busy, p->s_gpu_busy_n++;
            if ((unsigned)busy > p->s_gpu_busy_max) p->s_gpu_busy_max = (unsigned)busy;
        }
        if (mhz > 0) {
            p->s_gpu_mhz_sum += mhz, p->s_gpu_mhz_n++;
            if (p->gpu_mhz_max_w > p->s_gpu_mhz_max) p->s_gpu_mhz_max = p->gpu_mhz_max_w;
        }
        p->s_present += p->present_ms, p->s_driver += p->driver_ms, p->s_queue += p->queue_ms;
        p->s_ipc += ipc * n, p->s_local += local * n;
    }
    win_reset(p);
    return out;
}

/*
 * P5: <session folder>/vkbridge-perf-state.txt, rewritten (temporary file + rename) after each window, one line per
 * presenting process - the channel the app already reads for the bridge's numbers is files in the session folder
 * (vkbridge-frames-<pid>.txt, the report); an app-side trigger (screenshots at a collapse) polls it:
 *   pid=4242 comm=GTA5.exe t=812.4 fps=11.0 fps_med=58 collapse=1 since=790.2 windows=81
 * t/since: CLOCK_MONOTONIC seconds (as t_first/t_last in vkbridge-frames-<pid>.txt). The start and end of each
 * collapse are also "perf-collapse:" lines in the log. No frame-path work: the vkb-perf thread writes it.
 */
static void perf_state_write(void)
{
    char dir[600];
    if (!vkb_report_dir(dir, sizeof(dir)) || !dir[0]) return;
    char buf[2048];
    int n = 0;
    pthread_mutex_lock(&lk);
    uint64_t t = now_ns();
    for (int i = 0; i < PF_PROCS && n < (int)sizeof(buf) - 200; i++) {
        const pf_proc *p = &procs[i];
        if (!p->used || p->gone || !p->windows || !p->last_msg_ns || t - p->last_msg_ns > PF_ACTIVE_NS) continue;
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "pid=%d comm=%s t=%.1f fps=%.1f fps_med=%.0f collapse=%d since=%.1f windows=%u\n", p->pid,
                      p->comm, t / 1e9, p->last_fps, p->last_med, p->collapsed, p->collapsed ? p->collapse_ns / 1e9 : 0.0, p->windows);
    }
    pthread_mutex_unlock(&lk);
    char path[700], tmp[720];
    snprintf(path, sizeof(path), "%s/vkbridge-perf-state.txt", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    int ok = write(fd, buf, (size_t)n) == n;
    close(fd);
    if (ok) rename(tmp, path);
    else unlink(tmp);
}

static void *perf_main(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "vkb-perf");
    static task_read tr[PF_THREADS];
    uint64_t last_bg = 0;
    unsigned tick = 0;
    for (;;) {
        /* a tick is half a sample: the full sample (every thread's CPU time, the GPU, memory, ...) on every second tick, the
         * cpu-threads sample of the busiest threads on all of them (once a second at the default 2 s sample) */
        double tick_s = sample_s / 2;
        struct timespec ts = {(time_t)tick_s, (long)((tick_s - (time_t)tick_s) * 1e9)};
        nanosleep(&ts, NULL);
        int full = (++tick & 1) == 0;
        /* nothing presenting (Steam's menus through gamescope, an idle session): nothing read */
        int active = 0;
        uint64_t t_now = now_ns();
        pthread_mutex_lock(&lk);
        for (int i = 0; i < PF_PROCS && !active; i++)
            active = procs[i].used && !procs[i].gone && procs[i].last_msg_ns && t_now - procs[i].last_msg_ns <= PF_ACTIVE_NS;
        pthread_mutex_unlock(&lk);
        if (!active) continue;
        if (!full) {
            int xp[PF_PROCS], nx = 0;
            pthread_mutex_lock(&lk);
            for (int i = 0; i < PF_PROCS; i++) {
                pf_proc *p = &procs[i];
                if (p->used && !p->gone && p->last_msg_ns && t_now - p->last_msg_ns <= PF_ACTIVE_NS) xp[nx++] = p->pid;
            }
            pthread_mutex_unlock(&lk);
            for (int k = 0; k < nx; k++) cpu_extras(xp[k]);
            continue;
        }
        int busy;
        double mhz;
        long avail, swap;
        gpu_read(&busy, &mhz);
        mem_read(&avail, &swap);
        int th = vkb_report_thermal();
        unsigned cap = cap_read();
        clocks_sample();
        uint64_t t = now_ns();
        /* the presenting processes' threads */
        int pids[PF_PROCS], np = 0;
        pthread_mutex_lock(&lk);
        if (mhz > gpu_mhz_max_seen) gpu_mhz_max_seen = mhz;
        for (int i = 0; i < PF_PROCS; i++) {
            pf_proc *p = &procs[i];
            if (!p->used || p->gone || !p->last_msg_ns || t - p->last_msg_ns > PF_ACTIVE_NS) continue;
            pids[np++] = p->pid;
            if (busy >= 0) p->gpu_busy_sum += busy, p->gpu_busy_n++;
            if (mhz > 0) {
                p->gpu_mhz_sum += mhz, p->gpu_mhz_n++;
                if (mhz > p->gpu_mhz_max_w) p->gpu_mhz_max_w = (unsigned)mhz;
            }
            if (avail >= 0 && (p->avail_min_kb < 0 || avail < p->avail_min_kb)) p->avail_min_kb = avail;
            if (swap > p->swap_used_kb) p->swap_used_kb = swap;
            if (th > p->thermal_max) p->thermal_max = th;
            if (cap && (!p->cap_min || cap < p->cap_min)) p->cap_min = cap;
        }
        pthread_mutex_unlock(&lk);
        for (int k = 0; k < np; k++) {
            int n = read_tasks(pids[k], tr, PF_THREADS);
            uint64_t now = now_ns();
            pthread_mutex_lock(&lk);
            pf_proc *p = find(pids[k], 0);
            if (p && n >= 0) {
                double dt = p->sample_ns ? (now - p->sample_ns) / 1e9 : 0;
                if (!p->sample_ns) { /* the first look: the baseline only */
                    merge_tasks(p, tr, n, 0);
                    for (int i = 0; i < p->nth; i++) p->th[i].win = p->th[i].total = 0, memset(p->th[i].slot_win, 0, sizeof(p->th[i].slot_win)),
                                                     memset(p->th[i].slot_total, 0, sizeof(p->th[i].slot_total));
                } else {
                    merge_tasks(p, tr, n, dt);
                }
                p->sample_ns = now;
            }
            pthread_mutex_unlock(&lk);
            cpu_extras(pids[k]);
        }
        /* windows due: the background processes first (their window), then the lines */
        int due = 0;
        pthread_mutex_lock(&lk);
        for (int i = 0; i < PF_PROCS; i++) {
            pf_proc *p = &procs[i];
            if (p->used && !p->gone && p->t0 && p->t1 - p->t0 >= (uint64_t)(window_s * 1e9)) due = 1;
        }
        pthread_mutex_unlock(&lk);
        if (np && (due || !last_bg || t - last_bg >= (uint64_t)(window_s * 1e9))) {
            sample_bg();
            last_bg = t;
        }
        if (!due) continue;
        char l1[1400], l2[1600], l3[1800], ev[256];
        for (int i = 0; i < PF_PROCS; i++) {
            int w = 0;
            ev[0] = 0;
            pthread_mutex_lock(&lk);
            pf_proc *p = &procs[i];
            if (p->used && !p->gone && p->t0 && p->t1 - p->t0 >= (uint64_t)(window_s * 1e9)) {
                w = finish(p, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3));
                snprintf(ev, sizeof(ev), "%s", p->ev);
                p->ev[0] = 0;
            }
            pthread_mutex_unlock(&lk);
            if (w) {
                VKB_INFO("%s", l1);
                VKB_INFO("%s", l2);
                if (l3[0]) VKB_INFO("%s", l3);
            }
            if (ev[0]) VKB_INFO("%s", ev);
        }
        perf_state_write();
        {
            char cl[600];
            pthread_mutex_lock(&lk);
            int c = clocks_text(cl, sizeof(cl), 0);
            pthread_mutex_unlock(&lk);
            if (c) VKB_INFO("%s", cl);
        }
        pthread_mutex_lock(&lk);
        for (int i = 0; i < nbgn; i++) bgn[i].win = 0, memset(bgn[i].slot_win, 0, sizeof(bgn[i].slot_win));
        bg_win_s = 0;
        pthread_mutex_unlock(&lk);
    }
    return NULL;
}

void vkb_perf_proc_gone(vkb_srv_proc *sp)
{
    if (!sp || state != 1) return;
    char l1[1400], l2[1600], l3[1800], ev[256] = "";
    int w = 0;
    pthread_mutex_lock(&lk);
    pf_proc *p = find(sp->pid, 0);
    if (p) {
        /* the last part of a window, when it is at least a fifth of one */
        if (p->t0 && p->t1 - p->t0 >= (uint64_t)(window_s * 0.2e9)) w = finish(p, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3));
        snprintf(ev, sizeof(ev), "%s", p->ev);
        p->ev[0] = 0;
        p->gone = 1;
        xs_close_all(p);
    }
    pthread_mutex_unlock(&lk);
    if (w) {
        VKB_INFO("%s", l1);
        VKB_INFO("%s", l2);
        if (l3[0]) VKB_INFO("%s", l3);
    }
    if (ev[0]) VKB_INFO("%s", ev);
}

void vkb_perf_session(void)
{
    pthread_mutex_lock(&lk);
    for (int i = 0; i < PF_PROCS; i++)
        if (procs[i].used && procs[i].gone) procs[i].used = 0;
        else if (procs[i].used) {
            pf_proc *p = &procs[i];
            p->windows = 0, p->nw = 0, p->s_frames = p->s_repeats = 0;
            memset(p->verdicts, 0, sizeof(p->verdicts));
            memset(p->fps_hist, 0, sizeof(p->fps_hist));
            p->s_gpu_busy_sum = p->s_gpu_mhz_sum = 0, p->s_gpu_busy_n = p->s_gpu_mhz_n = 0, p->s_gpu_busy_max = p->s_gpu_mhz_max = 0;
            p->s_present = p->s_driver = p->s_queue = p->s_ipc = p->s_local = 0;
            p->cpu_total_s = 0;
            for (int k = 0; k < p->nth; k++) p->th[k].total = 0, memset(p->th[k].slot_total, 0, sizeof(p->th[k].slot_total));
        }
    for (int i = 0; i < nbgn; i++) bgn[i].total = 0, memset(bgn[i].slot_total, 0, sizeof(bgn[i].slot_total));
    bg_total_s = 0;
    memset(cc_max, 0, sizeof(cc_max)), memset(cc_n, 0, sizeof(cc_n)), memset(cc_top, 0, sizeof(cc_top));
    cc_samples = 0, cc_held_said = ~0u, cc_said_ns = 0;
    for (int i = 0; i < PF_PROCS; i++) procs[i].collapsed = 0;
    pthread_mutex_unlock(&lk);
}

/* ------------------------------------------------------------------ the report's section */

static int summary(const pf_proc *p, char *out, size_t cap, double *fps_med, double *p50m, double *p90m)
{
    uint32_t half = (p->windows + 1) / 2, acc = 0;
    *fps_med = 0;
    for (int i = 0; i <= 240; i++) {
        acc += p->fps_hist[i];
        if (acc >= half) {
            *fps_med = i;
            break;
        }
    }
    *p50m = medianf(p->w_p50, p->nw), *p90m = medianf(p->w_p90, p->nw);
    char vs[200] = "";
    size_t o = 0;
    for (int v = 0; v < V_COUNT; v++)
        if (p->verdicts[v]) o += (size_t)snprintf(vs + o, sizeof(vs) - o, "%s%s:%u", o ? "," : "", vname[v], p->verdicts[v]);
    double n = p->s_frames ? (double)p->s_frames : 1;
    char top[600], others[600], gb[16], gbm[16];
    threads_text(p, 1, top, sizeof(top), NULL);
    bg_text(1, others, sizeof(others));
    if (p->s_gpu_busy_n) snprintf(gb, sizeof(gb), "%.0f", p->s_gpu_busy_sum / p->s_gpu_busy_n), snprintf(gbm, sizeof(gbm), "%u", p->s_gpu_busy_max);
    else snprintf(gb, sizeof(gb), "?"), snprintf(gbm, sizeof(gbm), "?");
    return snprintf(out, cap,
                    "perf-summary: pid=%d comm=%s windows=%u verdicts=%s fps_med=%.0f ft_p50_med=%.1f ft_p90_med=%.1f frames=%llu repeats=%llu "
                    "gpu_busy_avg=%s gpu_busy_max=%s gpu_mhz_avg=%.0f gpu_mhz_max=%u present_ms=%.2f driver_ms=%.2f queue_ms=%.2f ipc_ms=%.2f "
                    "local_ms=%.2f top=%s others=%s",
                    p->pid, p->comm, p->windows, o ? vs : "none", *fps_med, *p50m, *p90m, (unsigned long long)p->s_frames,
                    (unsigned long long)p->s_repeats, gb, gbm, p->s_gpu_mhz_n ? p->s_gpu_mhz_sum / p->s_gpu_mhz_n : 0, p->s_gpu_mhz_max,
                    p->s_present / n, p->s_driver / n, p->s_queue / n, p->s_ipc / n, p->s_local / n, top, others);
}

static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++)
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    fputc('"', f);
}

void vkb_perf_report(FILE *f, int json)
{
    if (state != 1) {
        if (json) fprintf(f, "  \"performance\": [],\n");
        return;
    }
    pthread_mutex_lock(&lk);
    int any = 0;
    if (json) fprintf(f, "  \"performance\": [");
    for (int i = 0; i < PF_PROCS; i++) {
        const pf_proc *p = &procs[i];
        if (!p->used || !p->windows) continue;
        char line[2000];
        double fm, p50, p90;
        summary(p, line, sizeof(line), &fm, &p50, &p90);
        if (json) {
            double n = p->s_frames ? (double)p->s_frames : 1;
            fprintf(f, "%s\n    {\"pid\": %d, \"comm\": ", any ? "," : "", p->pid);
            jstr(f, p->comm);
            fprintf(f, ", \"windows\": %u, \"verdicts\": {", p->windows);
            int k = 0;
            for (int v = 0; v < V_COUNT; v++)
                if (p->verdicts[v]) fprintf(f, "%s\"%s\": %u", k++ ? ", " : "", vname[v], p->verdicts[v]);
            fprintf(f, "}, \"fps_median\": %.0f, \"ft_p50_median_ms\": %.1f, \"ft_p90_median_ms\": %.1f, \"frames\": %llu, \"repeats\": %llu, ", fm,
                    p50, p90, (unsigned long long)p->s_frames, (unsigned long long)p->s_repeats);
            if (p->s_gpu_busy_n) fprintf(f, "\"gpu_busy_avg\": %.0f, \"gpu_busy_max\": %u, ", p->s_gpu_busy_sum / p->s_gpu_busy_n, p->s_gpu_busy_max);
            else fprintf(f, "\"gpu_busy_avg\": null, \"gpu_busy_max\": null, ");
            fprintf(f, "\"gpu_mhz_avg\": %.0f, \"gpu_mhz_max\": %u, \"present_ms\": %.2f, \"driver_ms\": %.2f, \"queue_ms\": %.2f, \"ipc_ms\": %.2f, "
                       "\"local_ms\": %.2f, \"line\": ",
                    p->s_gpu_mhz_n ? p->s_gpu_mhz_sum / p->s_gpu_mhz_n : 0, p->s_gpu_mhz_max, p->s_present / n, p->s_driver / n, p->s_queue / n,
                    p->s_ipc / n, p->s_local / n);
            jstr(f, line);
            fprintf(f, "}");
        } else {
            if (!any)
                fprintf(f, "## Performance\n\nPer %.0f-s window while a process presented frames (`perf:` and `perf-threads:` lines in "
                           "vkbridge-server.log); the verdict names what limited the window most. Present wait per frame: driver (the GPU's "
                           "work) / queue (our order) / IPC / local.\n\n"
                           "| process | windows | verdicts | fps (median window) | frame ms p50 / p90 | repeated frames | GPU busy avg / max %% | "
                           "GPU MHz avg / max | present ms: driver / queue / IPC / local |\n|---|---:|---|---:|---|---:|---|---|---|\n",
                        window_s);
            char vs[200] = "";
            size_t o = 0;
            for (int v = 0; v < V_COUNT; v++)
                if (p->verdicts[v])
                    o += (size_t)snprintf(vs + o, sizeof(vs) - o, "%s%s%s %u", o ? ", " : "", vname[v], v == V_GPULAT ? " (unmeasured)" : "",
                                          p->verdicts[v]);
            double n = p->s_frames ? (double)p->s_frames : 1;
            char gb[24];
            if (p->s_gpu_busy_n) snprintf(gb, sizeof(gb), "%.0f / %u", p->s_gpu_busy_sum / p->s_gpu_busy_n, p->s_gpu_busy_max);
            else snprintf(gb, sizeof(gb), "not readable");
            fprintf(f, "| %s (%d) | %u | %s | %.0f | %.1f / %.1f | %llu of %llu | %s | %.0f / %u | %.2f: %.2f / %.2f / %.2f / %.2f |\n", p->comm, p->pid,
                    p->windows, vs, fm, p50, p90, (unsigned long long)p->s_repeats, (unsigned long long)p->s_frames, gb,
                    p->s_gpu_mhz_n ? p->s_gpu_mhz_sum / p->s_gpu_mhz_n : 0, p->s_gpu_mhz_max, p->s_present / n, p->s_driver / n,
                    p->s_queue / n, p->s_ipc / n, p->s_local / n);
        }
        any++;
    }
    if (json) fprintf(f, "%s],\n", any ? "\n  " : "");
    else if (any) {
        fprintf(f, "\nBusiest threads over the run (%% of a core @ cluster) and the summary lines tools read:\n\n```\n");
        for (int i = 0; i < PF_PROCS; i++) {
            const pf_proc *p = &procs[i];
            if (!p->used || !p->windows) continue;
            char line[2000];
            double fm, p50, p90;
            summary(p, line, sizeof(line), &fm, &p50, &p90);
            fprintf(f, "%s\n", line);
        }
        char cl[600];
        if (clocks_text(cl, sizeof(cl), 1)) fprintf(f, "%s\n", cl);
        fprintf(f, "```\n\n");
    }
    pthread_mutex_unlock(&lk);
}
