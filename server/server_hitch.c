/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Per-hitch attribution. With VKBRIDGE_HITCH_MS=<ms> (the game's preset, <cache dir>/env, the
 * server's or the guest's environment; HITCH_DEFAULT_MS when none says anything, 0 or off: off) a
 * frame of the process that takes longer than that gets one line in this log naming what the frame
 * spent its time on:
 *
 *   hitch: frame=812 ms=187 cause=compile compile_ms=152 compiles=3 bc_ms=0 bc_mtex=0.0 bc_recent_mtex=0.0
 *          alloc_ms=0 allocs=0 alloc_mb=0 pcache_ms=0 wait_ms=20 wait_n=4 wait_max_ms=18
 *          wait_max=vkWaitForFences@dxvk-submit present_ms=5 rt_ms=4 rt_n=88 pid=4242 comm=DarkSoulsIII.ex
 *
 * (one line). The client (client/hitch.c) times its presents; a frame over the threshold is sent
 * here (vkbHitch, no reply) with its window and what the process waited for in it: the app's fence,
 * semaphore and idle waits (wait_*), the swapchain's own waits for the GPU and for a free image
 * (present_*), the other round trips (rt_*, which include the compiles and allocations below as
 * the app saw them). The server keeps, per process, a ring of what it did that can take long -
 * pipeline and shader-module compiles (server_guard.c, every compile there is timed already, the
 * BC decoders' own builds included), BC texture conversions recorded (server_emu_bcn.c),
 * vkAllocateMemory (server_memory.c), pipeline-cache saves and loads (server_pcache.c) - and sums
 * what of it overlaps the window. bc_recent_mtex counts the frame before too: a conversion
 * recorded in one frame runs on the GPU when it is submitted, and the frame that waits for it is
 * often the next. bc_n / bc_recent_n count the conversions (one per BC copy the app recorded), and
 * bc_gpu_est_ms is the most GPU time the recent ones can take on this GPU (server_emu_bcn.c
 * gpu_est_ns: texels x the family's weight x the search's cost, from the G720's measured rate).
 *
 * cause= is the largest of compile, bc (the conversions' recording, or, when the waits are the
 * largest, the GPU time of the textures converted in this frame or the one before - only when that
 * estimate reaches a fifth of the wait), alloc, pcache and wait (the longer of the app's and the
 * swapchain's waits; GPU-bound frames and a server busy elsewhere land here); other when none
 * reaches a fifth of the frame - the game's own work, FEX translating, CPU clocks (S6-S8 in the
 * stutter plan). Each class is capped at the frame's length: compiles on several threads can sum
 * past it. Until 2026-11-05 any texel converted in the two frames made a wait "bc": GTA V,
 * G615 had 394 ms waits after 0.1 Mtex called bc; the summary now
 * counts the waits that came after conversions too small to explain them.
 *
 * At the process's end one line counts the hitches by cause, and while it runs a "so far" line
 * every SOFAR_S when that changed (a log sent with the game still running has them too). Hitch
 * lines are rate-limited (HITCH_RATE a second on average, HITCH_BURST at once; the ones skipped are
 * counted in the frames line) and at most HITCH_LINES per process (counted after that).
 *
 * Suspend: the app stops the whole session (SIGSTOP, SessionSuspendController) when its screen goes
 * away and continues it later; CLOCK_MONOTONIC runs on meanwhile, so the frame that spans it is as
 * long as the suspend (GTA5.exe 2812 ms and 45016 ms, 2026-11-05).
 * The server notices its own stop: a beat thread ticks every BEAT_NS and a tick, or a hitch/window
 * message arriving before one, that finds more than SUSP_GAP_NS since the last records the interval as
 * suspended (from one beat after the last tick, so a real hitch is never hidden by the estimate).
 * The suspended time of a frame is taken off it: a frame that is under the threshold without it
 * gets a "suspended:" line instead of a hitch line (counted in the summary as suspended, not in
 * the hitches, the worst or the fps), one that is still over it is a hitch of its net length with
 * suspended_ms=; the frames lines' s=, fps and over= leave the suspended time out and say
 * suspended_ms=. This needs the server to be stopped with the game (a helper process in the app's
 * list); a server left running would need the app to tell it (nothing in the protocol yet).
 *
 * The frames line: about once a second the client sends what its presents did in that window
 * (VKB_HITCH_K_WINDOW: frames, the longest, how many over the threshold, the window's waits) and the
 * server writes it with what it did for the process in the same window:
 *
 *   frames: pid=4242 comm=GTA5.exe s=1.0 windows=1 n=15 fps=15.0 worst_ms=187 over=3 hitches=bc:2,wait:1
 *           wait_ms=420 wait_n=30 wait_max_ms=160 wait_max=vkWaitForFences@dxvk-queue present_ms=12 rt_ms=30
 *           rt_n=88 compile_ms=0 compiles=0 bc_ms=12 bc_mtex=3.2 alloc_ms=0 allocs=0 alloc_mb=0 pcache_ms=0
 *
 * (one line; wait_ms sums every thread's waits, so it can pass the window's length: DXVK's queue
 * thread waiting for the GPU all the time is a GPU-bound window; bc_mtex is the texels converted,
 * GPU work that lands in the waits). A window with a frame over the threshold or under
 * NOTABLE_FPS gets a line of its own (WINDOW_LINES of them a process, then they are merged per
 * 10 s); smooth windows are merged into one line per SMOOTH_S (windows=N), so a smooth game writes
 * about a line a minute. Off, nothing is kept: the ring is made when the client turns it on, and
 * every note is one pointer test.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

#define HITCH_RING 2048
#define HITCH_LINES 1000
#define HITCH_DEFAULT_MS 100 /* on by default since 2026-11-04 */
#define HITCH_RATE 1.0       /* hitch lines a second on average ... */
#define HITCH_BURST 30.0     /* ... and at once */
#define MERGE_NS 2000000ull /* events of one class closer than this are one entry (a burst of uploads) */
#define WINDOW_LINES 1200    /* frames lines of their own a process (20 minutes of bad seconds) */
#define NOTABLE_FPS 25.0
#define SMOOTH_S 60  /* smooth windows merged into one line this long */
#define CAPPED_NS 10000000000ull  /* past WINDOW_LINES, every window merged this long */
#define SOFAR_S 60
#define BEAT_NS 200000000ull       /* the suspend detector's tick */
#define SUSP_GAP_NS 1000000000ull  /* no tick for this long: the process was stopped */
#define SUSP_RING 64
#define SUSP_LINES 100

/* VKBRIDGE_TEST_HITCH_PERIODS=<smooth s>,<so far s>,<burst> (host tests, the server's environment
 * only): shorter periods and a smaller burst of hitch lines. */
static struct { uint64_t smooth_ns, sofar_ns; double burst; } per;
static pthread_once_t per_once = PTHREAD_ONCE_INIT;
static void per_init(void)
{
    per.smooth_ns = SMOOTH_S * 1000000000ull, per.sofar_ns = SOFAR_S * 1000000000ull, per.burst = HITCH_BURST;
    const char *v = getenv("VKBRIDGE_TEST_HITCH_PERIODS");
    double a = 0, b = 0, c = 0;
    if (v && sscanf(v, "%lf,%lf,%lf", &a, &b, &c) >= 1) {
        if (a > 0) per.smooth_ns = (uint64_t)(a * 1e9);
        if (b > 0) per.sofar_ns = (uint64_t)(b * 1e9);
        if (c > 0) per.burst = c;
    }
}

/* The server's own stops (a suspended session): process-wide, under susp_lock. */
static pthread_mutex_t susp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t t0, t1; } susp_ring[SUSP_RING];
static uint64_t susp_n, susp_last;
static pthread_once_t beat_once = PTHREAD_ONCE_INIT;

static uint64_t susp_gap_ns(void)
{
    static uint64_t v;
    if (!v) {
        v = SUSP_GAP_NS;
        const char *e = getenv("VKBRIDGE_TEST_SUSPEND_GAP_MS"); /* host tests: a shorter gap */
        if (e && atoi(e) > 0) v = (uint64_t)atoi(e) * 1000000ull;
    }
    return v;
}

static void susp_tick(uint64_t now)
{
    pthread_mutex_lock(&susp_lock);
    if (susp_last && now > susp_last + susp_gap_ns()) {
        uint64_t t0 = susp_last + BEAT_NS;
        if (t0 > now) t0 = now;
        susp_ring[susp_n % SUSP_RING].t0 = t0;
        susp_ring[susp_n % SUSP_RING].t1 = now;
        susp_n++;
    }
    susp_last = now;
    pthread_mutex_unlock(&susp_lock);
}

static void *susp_beat(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = {0, BEAT_NS};
        nanosleep(&ts, NULL);
        susp_tick(vkb_hitch_now());
    }
    return NULL;
}

static void susp_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    susp_tick(vkb_hitch_now());
    if (pthread_create(&t, &a, susp_beat, NULL) != 0) VKB_WARN("hitch: no suspend detector thread; a suspended session counts as hitches");
    pthread_attr_destroy(&a);
}

/* How much of [t0, t1] the process was stopped (ns). Ticks first, so a message that arrives before
 * the beat thread has woken up from the stop still finds it. */
static uint64_t susp_overlap(uint64_t t0, uint64_t t1)
{
    susp_tick(vkb_hitch_now());
    uint64_t ns = 0;
    pthread_mutex_lock(&susp_lock);
    uint64_t have = susp_n < SUSP_RING ? susp_n : SUSP_RING;
    for (uint64_t i = 0; i < have; i++) {
        uint64_t a = susp_ring[(susp_n - 1 - i) % SUSP_RING].t0, z = susp_ring[(susp_n - 1 - i) % SUSP_RING].t1;
        if (z <= t0 || a >= t1) continue;
        ns += (z < t1 ? z : t1) - (a > t0 ? a : t0);
    }
    pthread_mutex_unlock(&susp_lock);
    return ns > t1 - t0 ? t1 - t0 : ns;
}

typedef struct hev {
    uint64_t t0, t1, ns, bytes, gpu_ns; /* gpu_ns: BC conversions' estimated GPU time (vkb_hitch_note_bc) */
    uint32_t n;
    uint32_t cls;
} hev;

enum { C_COMPILE, C_BC, C_ALLOC, C_PCACHE, C_WAIT, C_OTHER, C_COUNT };
static const char *const cause_name[C_COUNT] = {"compile", "bc", "alloc", "pcache", "wait", "other"};

/* A frames line's numbers: one window, or several merged. */
typedef struct win_acc {
    uint64_t t0, t1, windows, frames, over, worst_ns, susp_ns;
    uint64_t wns[VKB_HITCH_W_COUNT], wn[VKB_HITCH_W_COUNT], wmax_ns;
    uint32_t wmax_cmd;
    char wmax_thread[16];
    double cms, bms, ams, pms;
    uint64_t cn, btex, an, abytes;
    uint64_t causes[C_COUNT], skipped;
    int notable;
} win_acc;

struct vkb_hitch {
    pthread_mutex_t lock;
    hev ev[HITCH_RING];
    uint64_t written;  /* entries ever written (ring index: written % HITCH_RING) */
    uint32_t threshold_ms;
    uint32_t lines, compile_lines;
    uint64_t hitches, causes[C_COUNT];
    uint64_t wait_bc_small; /* wait hitches after BC conversions whose GPU estimate cannot explain them */
    double worst_ms;
    /* under lock: the hitch lines' bucket, the hitches since the last window, the merged windows */
    double tokens;
    uint64_t tok_ns;
    uint64_t win_causes[C_COUNT], win_skipped;
    win_acc pend;
    uint32_t window_lines;
    uint64_t sofar_ns, sofar_hitches;
    uint64_t skipped_total; /* hitch lines the rate limit left out */
    uint64_t susp_frames, susp_total_ns, win_susp_frames, susp_lines; /* frames that spanned a suspend (see the header) */
    int presented; /* a window or a hitch came: the process shows frames (else no summary at its end) */
};

uint64_t vkb_hitch_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* VKBRIDGE_HITCH_MS as the server has it for the client on this thread: the game's preset file,
 * <cache dir>/env, the server's environment, the game's built-in preset. 0: off. */
uint32_t vkb_hitch_ms_setting(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_HITCH_MS");
    if (!v || !*v) return HITCH_DEFAULT_MS;
    if (!strcmp(v, "off")) return 0;
    char *end;
    unsigned long ms = strtoul(v, &end, 10);
    if (*end || *v == '-') {
        VKB_ONCE(VKB_LOG_WARN, "VKBRIDGE_HITCH_MS=%.40s is not a number of milliseconds; the default %u ms", v, HITCH_DEFAULT_MS);
        return HITCH_DEFAULT_MS;
    }
    return ms > 60000 ? 60000 : (uint32_t)ms;
}

void vkb_hitch_enable(vkb_srv_proc *p, uint32_t ms)
{
    if (!p || !ms) return;
    if (__atomic_load_n(&p->hitch, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&p->hitch->threshold_ms, ms, __ATOMIC_RELAXED);
        return;
    }
    struct vkb_hitch *h = calloc(1, sizeof(*h));
    if (!h) return;
    pthread_mutex_init(&h->lock, NULL);
    h->threshold_ms = ms;
    pthread_once(&per_once, per_init);
    pthread_once(&beat_once, susp_start);
    h->tokens = per.burst;
    h->tok_ns = vkb_hitch_now();
    struct vkb_hitch *none = NULL;
    if (!__atomic_compare_exchange_n(&p->hitch, &none, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pthread_mutex_destroy(&h->lock);
        free(h);
        return;
    }
    VKB_INFO("hitch attribution on for process %d (%s): frames over %u ms are logged with what they spent their time on", p->pid,
             p->comm[0] ? p->comm : "?", ms);
}

static void note(vkb_srv_proc *p, int cls, uint64_t t0, uint64_t t1, uint32_t n, uint64_t bytes, uint64_t gpu_ns)
{
    struct vkb_hitch *h = p ? __atomic_load_n(&p->hitch, __ATOMIC_ACQUIRE) : NULL;
    if (!h || cls < 0 || cls >= VKB_HITCH_CLASSES) return;
    if (t1 < t0) t1 = t0;
    pthread_mutex_lock(&h->lock);
    hev *last = h->written ? &h->ev[(h->written - 1) % HITCH_RING] : NULL;
    if (last && last->cls == (uint32_t)cls && t0 <= last->t1 + MERGE_NS && t1 >= last->t0) {
        if (t0 < last->t0) last->t0 = t0;
        if (t1 > last->t1) last->t1 = t1;
        last->ns += t1 - t0;
        last->n += n;
        last->bytes += bytes;
        last->gpu_ns += gpu_ns;
    } else {
        h->ev[h->written % HITCH_RING] = (hev){t0, t1, t1 - t0, bytes, gpu_ns, n, (uint32_t)cls};
        h->written++;
    }
    pthread_mutex_unlock(&h->lock);
}

void vkb_hitch_note(vkb_srv_proc *p, int cls, uint64_t t0, uint64_t t1, uint32_t n, uint64_t bytes)
{
    note(p, cls, t0, t1, n, bytes, 0);
}

void vkb_hitch_note_bc(vkb_srv_proc *p, uint64_t t0, uint64_t t1, uint64_t texels, uint64_t gpu_est_ns)
{
    note(p, VKB_HITCH_BC, t0, t1, 1, texels, gpu_est_ns);
}

/* A compile taking at least half the threshold gets a line of its own (the hitch line has only
 * the sum); HITCH_LINES of them per process. */
void vkb_hitch_compile(vkb_srv_proc *p, const char *what, uint32_t n, uint64_t t0, uint64_t t1, int result)
{
    struct vkb_hitch *h = p ? __atomic_load_n(&p->hitch, __ATOMIC_ACQUIRE) : NULL;
    if (!h) return;
    vkb_hitch_note(p, VKB_HITCH_COMPILE, t0, t1, n ? n : 1, 0);
    double ms = (t1 - t0) / 1e6;
    if (ms * 2 < __atomic_load_n(&h->threshold_ms, __ATOMIC_RELAXED)) return;
    if (__atomic_fetch_add(&h->compile_lines, 1, __ATOMIC_RELAXED) >= HITCH_LINES) return;
    char name[17] = "";
    prctl(PR_GET_NAME, name, 0, 0, 0); /* this thread: Android before API 26 has no pthread_getname_np */
    VKB_INFO("compile: %s (%u) in %.1f ms (thread %s)%s", what, n, ms, name[0] ? name : "?", result < 0 ? ", failed" : "");
}

/* What of class cls overlaps [ws, we]: its time (each entry's share of its own span), how many and
 * what size, and the GPU time estimated for it (whole entries); *lost: the ring no longer reaches
 * back to ws. */
static void sum_gpu(struct vkb_hitch *h, int cls, uint64_t ws, uint64_t we, double *ms, uint64_t *n, uint64_t *bytes, double *gpu_ms,
                    int *lost)
{
    double ns = 0, gns = 0;
    uint64_t cnt = 0, b = 0;
    uint64_t have = h->written < HITCH_RING ? h->written : HITCH_RING;
    uint64_t i = 0;
    for (; i < have; i++) {
        const hev *e = &h->ev[(h->written - 1 - i) % HITCH_RING];
        /* entries are written in the order they end, near enough: one that ended a merge window
         * before ws means none older counts (MERGE_NS of slack for a long merged entry) */
        if (e->t1 + MERGE_NS < ws) break;
        if (e->cls != (uint32_t)cls || e->t1 < ws || e->t0 > we) continue;
        uint64_t a = e->t0 > ws ? e->t0 : ws, z = e->t1 < we ? e->t1 : we;
        uint64_t span = e->t1 - e->t0;
        ns += span ? (double)e->ns * (double)(z - a) / (double)span : (double)e->ns;
        cnt += e->n;
        b += e->bytes;
        gns += (double)e->gpu_ns;
    }
    if (i == have && h->written > HITCH_RING) *lost = 1;
    *ms = ns / 1e6;
    if (n) *n = cnt;
    if (bytes) *bytes = b;
    if (gpu_ms) *gpu_ms = gns / 1e6;
}

static void sum(struct vkb_hitch *h, int cls, uint64_t ws, uint64_t we, double *ms, uint64_t *n, uint64_t *bytes, int *lost)
{
    sum_gpu(h, cls, ws, we, ms, n, bytes, NULL, lost);
}

static double cap(double v, double lim) { return v > lim ? lim : v; }

static const char *wait_name(uint32_t cmd) { return cmd < VKB_CMD_COUNT ? vkb_cmd_names[cmd] : "acquire"; }

/* "bc:2,wait:1" or "none" */
static void causes_text(const uint64_t *causes, uint64_t skipped, char *out, size_t cap_)
{
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < C_COUNT && o < (int)cap_ - 24; i++)
        if (causes[i]) o += snprintf(out + o, cap_ - (size_t)o, "%s%s:%llu", o ? "," : "", cause_name[i], (unsigned long long)causes[i]);
    if (!o) o = snprintf(out, cap_, "none");
    if (skipped && o < (int)cap_ - 24) snprintf(out + o, cap_ - (size_t)o, " hitch_lines_skipped=%llu", (unsigned long long)skipped);
}

/* The frames line of w (formatted under h->lock, written by the caller after it). */
static void window_text(const vkb_srv_proc *p, const win_acc *w, uint32_t threshold_ms, char *line, size_t n)
{
    double secs = (double)(w->t1 - w->t0 - w->susp_ns) / 1e9;
    char hc[192], wmax[64] = "-", sus[48] = "";
    if (w->susp_ns) snprintf(sus, sizeof(sus), " suspended_ms=%.0f", w->susp_ns / 1e6);
    causes_text(w->causes, w->skipped, hc, sizeof(hc));
    if (w->wmax_ns) snprintf(wmax, sizeof(wmax), "%s@%s", wait_name(w->wmax_cmd), w->wmax_thread[0] ? w->wmax_thread : "?");
    snprintf(line, n,
             "frames: pid=%d comm=%s s=%.1f windows=%llu n=%llu fps=%.1f worst_ms=%.0f over=%llu (%u ms) hitches=%s wait_ms=%.0f wait_n=%llu "
             "wait_max_ms=%.0f wait_max=%s present_ms=%.0f rt_ms=%.0f rt_n=%llu compile_ms=%.0f compiles=%llu bc_ms=%.0f bc_mtex=%.1f "
             "alloc_ms=%.0f allocs=%llu alloc_mb=%llu pcache_ms=%.0f%s",
             p->pid, p->comm[0] ? p->comm : "?", secs, (unsigned long long)w->windows, (unsigned long long)w->frames,
             secs > 0 ? w->frames / secs : 0.0, w->worst_ns / 1e6, (unsigned long long)w->over, threshold_ms, hc,
             w->wns[VKB_HITCH_W_APP] / 1e6, (unsigned long long)w->wn[VKB_HITCH_W_APP], w->wmax_ns / 1e6, wmax,
             w->wns[VKB_HITCH_W_PRESENT] / 1e6, w->wns[VKB_HITCH_W_RT] / 1e6, (unsigned long long)w->wn[VKB_HITCH_W_RT], w->cms,
             (unsigned long long)w->cn, w->bms, w->btex / 1e6, w->ams, (unsigned long long)w->an, (unsigned long long)(w->abytes >> 20),
             w->pms, sus);
}

static void window_merge(win_acc *into, const win_acc *w)
{
    if (!into->windows) {
        *into = *w;
        return;
    }
    if (w->t0 < into->t0) into->t0 = w->t0;
    if (w->t1 > into->t1) into->t1 = w->t1;
    into->susp_ns += w->susp_ns;
    into->windows += w->windows, into->frames += w->frames, into->over += w->over;
    if (w->worst_ns > into->worst_ns) into->worst_ns = w->worst_ns;
    for (int i = 0; i < VKB_HITCH_W_COUNT; i++) into->wns[i] += w->wns[i], into->wn[i] += w->wn[i];
    if (w->wmax_ns > into->wmax_ns) {
        into->wmax_ns = w->wmax_ns, into->wmax_cmd = w->wmax_cmd;
        memcpy(into->wmax_thread, w->wmax_thread, sizeof(into->wmax_thread));
    }
    into->cms += w->cms, into->bms += w->bms, into->ams += w->ams, into->pms += w->pms;
    into->cn += w->cn, into->btex += w->btex, into->an += w->an, into->abytes += w->abytes;
    for (int i = 0; i < C_COUNT; i++) into->causes[i] += w->causes[i];
    into->skipped += w->skipped;
    into->notable |= w->notable;
}

/* "; N of the waits came after BC conversions too small to explain them" on a summary, when N > 0 */
/* "; N frames spanned a session suspend ..." when the session was suspended in a frame */
static void susp_text(struct vkb_hitch *h, char *line, size_t n)
{
    size_t len = strlen(line);
    if (h->susp_frames && len < n)
        snprintf(line + len, n - len, "; %llu frame%s spanned a session suspend (%.0f ms stopped), not counted as hitches",
                 (unsigned long long)h->susp_frames, h->susp_frames == 1 ? "" : "s", h->susp_total_ns / 1e6);
}

static void small_bc_text(struct vkb_hitch *h, char *line, size_t n)
{
    uint64_t k = __atomic_load_n(&h->wait_bc_small, __ATOMIC_RELAXED);
    size_t len = strlen(line);
    if (k && len < n)
        snprintf(line + len, n - len, "; %llu of the waits came after BC conversions too small to explain them (bc_gpu_est_ms in their lines)",
                 (unsigned long long)k);
}

/* The "so far" counts (under h->lock): 1 and the text when due. */
static int sofar_text(const vkb_srv_proc *p, struct vkb_hitch *h, uint64_t now, char *line, size_t n)
{
    if (now - h->sofar_ns < per.sofar_ns) return 0;
    uint64_t hit = __atomic_load_n(&h->hitches, __ATOMIC_RELAXED);
    if (!h->sofar_ns) {
        h->sofar_ns = now; /* the first minute starts at the first window */
        return 0;
    }
    h->sofar_ns = now;
    if (hit == h->sofar_hitches) return 0;
    h->sofar_hitches = hit;
    snprintf(line, n,
             "hitches over %u ms in process %d (%s) so far: %llu (compile %llu, bc %llu, alloc %llu, pcache %llu, wait %llu, other %llu); "
             "worst %.0f ms",
             h->threshold_ms, p->pid, p->comm[0] ? p->comm : "?", (unsigned long long)hit, (unsigned long long)h->causes[C_COMPILE],
             (unsigned long long)h->causes[C_BC], (unsigned long long)h->causes[C_ALLOC], (unsigned long long)h->causes[C_PCACHE],
             (unsigned long long)h->causes[C_WAIT], (unsigned long long)h->causes[C_OTHER], h->worst_ms);
    small_bc_text(h, line, n);
    susp_text(h, line, n);
    return 1;
}

static void window(vkb_srv_proc *p, struct vkb_hitch *h, const vkb_hitch_msg *m)
{
    if (m->t_end < m->t_start) return;
    win_acc w;
    memset(&w, 0, sizeof(w));
    w.t0 = m->t_start, w.t1 = m->t_end, w.windows = 1, w.frames = m->frames, w.over = m->over, w.worst_ns = m->worst_ns;
    for (int i = 0; i < VKB_HITCH_W_COUNT; i++) {
        w.wns[i] = m->w[i].ns, w.wn[i] = m->w[i].n;
        if (i != VKB_HITCH_W_RT && m->w[i].max_ns > w.wmax_ns) {
            w.wmax_ns = m->w[i].max_ns, w.wmax_cmd = m->w[i].max_cmd;
            memcpy(w.wmax_thread, m->w[i].max_thread, sizeof(w.wmax_thread));
            w.wmax_thread[sizeof(w.wmax_thread) - 1] = 0;
        }
    }
    uint64_t susp = susp_overlap(m->t_start, m->t_end);
    w.susp_ns = susp;
    double secs = (double)(m->t_end - m->t_start - susp) / 1e9;
    char l1[1024], l2[1024], l3[512];
    int have1 = 0, have2 = 0, have3 = 0;
    int lost = 0;
    pthread_mutex_lock(&h->lock);
    /* the frames that spanned a suspend were reported apart (frame messages come first): not over, and
     * the suspend's own long frame is not the window's worst */
    uint64_t sf = h->win_susp_frames;
    h->win_susp_frames = 0;
    w.over = w.over > sf ? w.over - sf : 0;
    if (susp && sf) w.worst_ns = w.worst_ns > susp ? w.worst_ns - susp : 0;
    sum(h, C_COMPILE, w.t0, w.t1, &w.cms, &w.cn, NULL, &lost);
    sum(h, C_BC, w.t0, w.t1, &w.bms, NULL, &w.btex, &lost);
    sum(h, C_ALLOC, w.t0, w.t1, &w.ams, &w.an, &w.abytes, &lost);
    sum(h, C_PCACHE, w.t0, w.t1, &w.pms, NULL, NULL, &lost);
    memcpy(w.causes, h->win_causes, sizeof(w.causes));
    w.skipped = h->win_skipped;
    memset(h->win_causes, 0, sizeof(h->win_causes));
    h->win_skipped = 0;
    w.notable = w.over || (secs > 0 && m->frames / secs < NOTABLE_FPS);
    for (int i = 0; i < C_COUNT; i++) w.notable |= w.causes[i] != 0;
    if (w.notable && h->window_lines < WINDOW_LINES) {
        if (h->pend.windows) {
            window_text(p, &h->pend, h->threshold_ms, l1, sizeof(l1)), have1 = 1;
            memset(&h->pend, 0, sizeof(h->pend));
        }
        h->window_lines++;
        window_text(p, &w, h->threshold_ms, l2, sizeof(l2)), have2 = 1;
    } else {
        window_merge(&h->pend, &w);
        if (h->pend.t1 - h->pend.t0 >= (h->pend.notable ? CAPPED_NS : per.smooth_ns)) {
            window_text(p, &h->pend, h->threshold_ms, l1, sizeof(l1)), have1 = 1;
            memset(&h->pend, 0, sizeof(h->pend));
        }
    }
    have3 = sofar_text(p, h, m->t_end, l3, sizeof(l3));
    pthread_mutex_unlock(&h->lock);
    if (have1) VKB_INFO("%s", l1);
    if (have2) VKB_INFO("%s", l2);
    if (have3) VKB_INFO("%s", l3);
}

void vkb_sv_vkbHitch(vkb_srv_call *c)
{
    vkb_hitch_msg m;
    memset(&m, 0, sizeof(m));
    vkb_dec_raw_tail(&c->d, &m, sizeof(m));
    vkb_srv_proc *p = c->conn ? c->conn->proc : NULL;
    if (c->d.err || !p) {
        vkb_srv_bad_message(c);
        return;
    }
    if (!m.frame && m.kind != VKB_HITCH_K_WINDOW) { /* the client turned it on (its own environment, or what vkbQueryServer said) */
        vkb_hitch_enable(p, m.threshold_ms ? m.threshold_ms : 1);
        return;
    }
    struct vkb_hitch *h = __atomic_load_n(&p->hitch, __ATOMIC_ACQUIRE);
    if (!h) {
        vkb_hitch_enable(p, m.threshold_ms ? m.threshold_ms : 1);
        if (!(h = __atomic_load_n(&p->hitch, __ATOMIC_ACQUIRE))) return;
    }
    for (int i = 0; i < VKB_HITCH_W_COUNT; i++) m.w[i].max_thread[sizeof(m.w[i].max_thread) - 1] = 0;
    __atomic_store_n(&h->presented, 1, __ATOMIC_RELAXED);
    if (m.kind == VKB_HITCH_K_WINDOW) {
        window(p, h, &m);
        return;
    }
    if (m.t_end < m.t_start) m.t_end = m.t_start;
    if (!m.t_prev || m.t_prev > m.t_start) m.t_prev = m.t_start;
    uint64_t susp = susp_overlap(m.t_start, m.t_end);
    double frame_ms = (double)(m.t_end - m.t_start - susp) / 1e6;
    if (susp && frame_ms < (double)__atomic_load_n(&h->threshold_ms, __ATOMIC_RELAXED)) {
        /* a frame that is not a hitch without the suspend: its own line, not a hitch, never silent */
        uint64_t k;
        pthread_mutex_lock(&h->lock);
        h->susp_frames++, h->win_susp_frames++, h->susp_total_ns += susp;
        k = h->susp_lines++;
        pthread_mutex_unlock(&h->lock);
        if (k < SUSP_LINES)
            VKB_INFO("suspended: frame=%llu ms=%.0f suspended_ms=%.0f net_ms=%.0f (the session was suspended: not a hitch) pid=%d comm=%s",
                     (unsigned long long)m.frame, (m.t_end - m.t_start) / 1e6, susp / 1e6, frame_ms, p->pid, p->comm[0] ? p->comm : "?");
        return;
    }
    double cms, bms, ams, pms, recent_ms, rgpu_ms;
    uint64_t cn, bn, btex, an, abytes, rtex, rn;
    int lost = 0;
    pthread_mutex_lock(&h->lock);
    sum(h, C_COMPILE, m.t_start, m.t_end, &cms, &cn, NULL, &lost);
    sum(h, C_BC, m.t_start, m.t_end, &bms, &bn, &btex, &lost);
    sum_gpu(h, C_BC, m.t_prev, m.t_end, &recent_ms, &rn, &rtex, &rgpu_ms, &lost);
    sum(h, C_ALLOC, m.t_start, m.t_end, &ams, &an, &abytes, &lost);
    sum(h, C_PCACHE, m.t_start, m.t_end, &pms, NULL, NULL, &lost);
    pthread_mutex_unlock(&h->lock);
    const vkb_hitch_wait *wa = &m.w[VKB_HITCH_W_APP], *wp = &m.w[VKB_HITCH_W_PRESENT], *wr = &m.w[VKB_HITCH_W_RT];
    double wait_ms = wa->ns / 1e6, present_ms = wp->ns / 1e6;
    double v[C_COUNT] = {cap(cms, frame_ms), cap(bms, frame_ms), cap(ams, frame_ms), cap(pms, frame_ms),
                         cap(wait_ms > present_ms ? wait_ms : present_ms, frame_ms), 0};
    int top = 0;
    for (int i = 1; i < C_OTHER; i++)
        if (v[i] > v[top]) top = i;
    /* the GPU converting textures is what was waited for - when their GPU time can be a fifth of the
     * wait (the most they can take on this GPU); a wait after a few small ones stays a wait */
    int small_bc = 0;
    if (top == C_WAIT && rtex) {
        if (rgpu_ms * 5 >= v[C_WAIT]) top = C_BC;
        else small_bc = 1;
    }
    double topv = top == C_BC ? (v[C_BC] > v[C_WAIT] ? v[C_BC] : v[C_WAIT]) : v[top];
    if (topv * 5 < frame_ms) top = C_OTHER;
    uint32_t line_no = __atomic_fetch_add(&h->lines, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->hitches, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->causes[top], 1, __ATOMIC_RELAXED);
    if (small_bc && top == C_WAIT) __atomic_add_fetch(&h->wait_bc_small, 1, __ATOMIC_RELAXED);
    /* the rate limit: a token per line, HITCH_RATE a second back, HITCH_BURST at most */
    int skip = 0;
    pthread_mutex_lock(&h->lock);
    if (frame_ms > h->worst_ms) h->worst_ms = frame_ms;
    h->win_causes[top]++;
    uint64_t now = vkb_hitch_now();
    h->tokens += HITCH_RATE * (double)(now - h->tok_ns) / 1e9;
    if (h->tokens > per.burst) h->tokens = per.burst;
    h->tok_ns = now;
    if (line_no < HITCH_LINES) {
        if (h->tokens >= 1.0) h->tokens -= 1.0;
        else skip = 1, h->win_skipped++, h->skipped_total++;
    }
    pthread_mutex_unlock(&h->lock);
    if (skip) {
        __atomic_sub_fetch(&h->lines, 1, __ATOMIC_RELAXED); /* not written: the cap counts written lines */
        return;
    }
    if (line_no >= HITCH_LINES) {
        if (line_no == HITCH_LINES)
            VKB_INFO("hitch: process %d has had %u hitch lines; further hitches are only counted (the frames lines and the summary at its end)",
                     p->pid, HITCH_LINES);
        return;
    }
    char wmax[64] = "-", sus[48] = "";
    if (susp) snprintf(sus, sizeof(sus), " suspended_ms=%.0f", susp / 1e6);
    const vkb_hitch_wait *lw = wa->max_ns >= wp->max_ns ? wa : wp;
    if (lw->max_ns) snprintf(wmax, sizeof(wmax), "%s@%s", wait_name(lw->max_cmd), lw->max_thread[0] ? lw->max_thread : "?");
    VKB_INFO("hitch: frame=%llu ms=%.0f cause=%s compile_ms=%.0f compiles=%llu bc_ms=%.0f bc_mtex=%.1f bc_recent_mtex=%.1f bc_n=%llu "
             "bc_recent_n=%llu bc_gpu_est_ms=%.0f alloc_ms=%.0f "
             "allocs=%llu alloc_mb=%llu pcache_ms=%.0f wait_ms=%.0f wait_n=%llu wait_max_ms=%.0f wait_max=%s present_ms=%.0f "
             "rt_ms=%.0f rt_n=%llu pid=%d comm=%s%s%s",
             (unsigned long long)m.frame, frame_ms, cause_name[top], cms, (unsigned long long)cn, bms, btex / 1e6, rtex / 1e6,
             (unsigned long long)bn, (unsigned long long)rn, rgpu_ms, ams,
             (unsigned long long)an, (unsigned long long)(abytes >> 20), pms, wait_ms, (unsigned long long)wa->n,
             (lw->max_ns) / 1e6, wmax, present_ms, wr->ns / 1e6, (unsigned long long)wr->n, p->pid, p->comm[0] ? p->comm : "?",
             lost ? " lost=1" : "", sus);
}

void vkb_hitch_proc_gone(vkb_srv_proc *p)
{
    struct vkb_hitch *h = p ? __atomic_exchange_n(&p->hitch, NULL, __ATOMIC_ACQ_REL) : NULL;
    if (!h) return;
    if (h->pend.windows) { /* the merged windows not written yet */
        char l[1024];
        window_text(p, &h->pend, h->threshold_ms, l, sizeof(l));
        VKB_INFO("%s", l);
    }
    if (h->hitches) {
        char l[768];
        snprintf(l, sizeof(l),
                 "hitches over %u ms in process %d (%s): %llu (compile %llu, bc %llu, alloc %llu, pcache %llu, wait %llu, other %llu); "
                 "worst %.0f ms; %llu hitch lines left out by the rate limit",
                 h->threshold_ms, p->pid, p->comm[0] ? p->comm : "?", (unsigned long long)h->hitches,
                 (unsigned long long)h->causes[C_COMPILE], (unsigned long long)h->causes[C_BC], (unsigned long long)h->causes[C_ALLOC],
                 (unsigned long long)h->causes[C_PCACHE], (unsigned long long)h->causes[C_WAIT], (unsigned long long)h->causes[C_OTHER],
                 h->worst_ms, (unsigned long long)h->skipped_total);
        small_bc_text(h, l, sizeof(l));
        susp_text(h, l, sizeof(l));
        VKB_INFO("%s", l);
    } else if (h->presented) {
        char l[512];
        snprintf(l, sizeof(l), "hitches over %u ms in process %d (%s): none", h->threshold_ms, p->pid, p->comm[0] ? p->comm : "?");
        susp_text(h, l, sizeof(l));
        VKB_INFO("%s", l);
    }
    pthread_mutex_destroy(&h->lock);
    free(h);
}
