/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The frame pacer (VKBRIDGE_PACER=1 in the guest's environment, or from the server: <cache dir>/env, a game's preset; off by
 * default). Input lag in a GPU-bound
 * DXVK game is mostly frames waiting in line for the GPU: DXVK lets the game run maxFrameLatency (3) frames
 * ahead of the GPU, so the S10+ lagprobe (35 fps, GPU-bound) showed 4 frames between the bridge and the
 * GPU's done, 145 ms from a press to the screen. The pacer keeps about one frame in line,
 * started just early enough that the GPU never waits for it.
 *
 * The lever. DXVK calls the bridge from its own threads (dxvk-queue submits, presents and acquires), never
 * from the game's; a wait there would only move the queue into DXVK's. What holds the game's thread is
 * DXVK's frame latency gate: Present returns once frame N-M has ended, and with present wait offered
 * (FIFO) a frame ends when DXVK's dxvk-frame thread's vkWaitForPresentKHR for it returns (DXVK
 * a78a467: d3d11_swapchain.cpp SyncFrameLatency, dxvk_presenter.cpp runFrameThread). That call is
 * answered here, in the client, so the bridge decides when the game starts its next frame.
 *
 * With the switch on, present wait is offered to DXVK too (wsi.c), and a DXVK swapchain's present waits
 * are answered by the pacer instead of by the compositor's "shown" (which follows gamescope's vblanks:
 * VKBRIDGE_PRESENT_WAIT=1 alone cost Portal's menu 25-35% of its frames that way):
 *   - not engaged (the default state): a present id counts as done as soon as it was presented, so DXVK's
 *     gate is exactly what it is without present wait (its own finish thread's "GPU work done");
 *   - engaged: vkWaitForPresentKHR(j) returns at D(j) + H, D(j) when the swapchain's presenting thread saw
 *     frame j's GPU work done, H the pacer's hold.
 * H comes from one closed loop on slack(k) = D(k-1) - P(k), how long before the GPU finished the previous
 * frame the whole of frame k was handed to the bridge (P(k): vkQueuePresentKHR): the target is a transit
 * allowance A (the wire and the server's stream, 1.3 ms median, 2.8 ms p90 on the S10+; the client cannot
 * see it per frame) plus twice the slack's spread (the GPU's frame time and the game's CPU time vary).
 * Holding more shrinks the slack one for one, so H += 0.1 x (slack - target) a frame; a frame that may have
 * reached the GPU after it went idle (slack < A) takes the hold back at once.
 *
 * Engaged only when the GPU is the limit and the line is deep, judged over one-second windows (two in a row):
 * the app waits for presents (the lever exists: DXVK does only in FIFO), the presenting thread waited for
 * the GPU on >= 90% of frames (not for the compositor: a light or refresh-bound game, like Portal's menu,
 * fails here), the next frame was queued >= max(4 ms, half a frame) ahead even at the 10th percentile, a
 * steady frame time of >= 4 ms, and no DXVK latency sleep or frame limit in DXVK_CONFIG. (Acquires that wait
 * for an image say nothing here: a GPU-bound game with a deep line holds every image too; the presenting
 * thread's fence waits are what tell the GPU from the compositor.) It lets go (H = 0 at once, then a backoff that doubles) when frames reach the GPU
 * after it went idle for > 3% of two windows, when frames get > 8% slower than when it engaged for two
 * windows (and if they speed up again without the hold, it was the GPU's clock: a long backoff), when the
 * hold falls to nothing (the CPU became the limit), when the compositor becomes the limit, or when the app
 * stops waiting for presents. Each reason is said at most once a window; a summary line every 1200 frames
 * and at the swapchain's end.
 *
 * Off: present wait is not offered to DXVK, and nothing here runs. On: a clock read at each present and
 * ~40 arithmetic operations a frame on the presenting thread, a sort of <= 256 numbers a second; no thread,
 * no message, no allocation per frame. Other apps (vkd3d-proton, native Vulkan) are not paced.
 */
#define _GNU_SOURCE
#include "wsi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_NS 1000000000ull     /* a window: at least 1 s ... */
#define WIN_MIN 30u              /* ... and 30 frames ... */
#define WIN_MAX_NS 4000000000ull /* ... or 4 s, whichever comes first after both */
#define NS 256u                  /* samples kept per window */
#define A_MIN 3.0                /* ms: transit allowance, start and floor */
#define A_MAX 8.0
#define H_MAX 250.0              /* ms */

/* no libm in the client: the few needed here */
static double dmax(double a, double b) { return a > b ? a : b; }
static double dmin(double a, double b) { return a < b ? a : b; }
static double sqrt_pos(double x) /* x >= 0; Newton, a few ulps is plenty for a margin */
{
    if (x <= 0) return 0;
    double r = x > 1 ? x : 1;
    for (int i = 0; i < 40; i++) {
        double n = 0.5 * (r + x / r);
        if (n >= r) break;
        r = n;
    }
    return r;
}

enum { R_IDLE, R_SLOWER, R_NOTGPU, R_REFRESH, R_NOWAITS, R_CLOCK, R_COUNT };
static const char *const r_name[R_COUNT] = {"GPU idle", "slower frames", "not GPU-bound", "compositor-bound", "no present waits",
                                            "GPU clock"};

struct vkb_pacer {
    vkb_swapchain *sc;
    uint64_t hold_ns;  /* atomic: the hold, 0 while not engaged */
    uint64_t pw_calls; /* atomic: vkWaitForPresentKHR calls on the swapchain */
    /* the presenting thread's from here on */
    int engaged;
    uint64_t prev_d;
    int prev_real;
    double H, A, mean_s, var_s, dref, pref;
    int sd_init, freeze;
    uint64_t w_start, w_pw0, w_acq0;
    uint32_t w_n, w_real, w_late, w_hlow, w_k;
    double w_excess, w_hsum;
    float w_slack[NS], w_d[NS], w_wait[NS];
    int streak;
    double elig_d[2];
    int backoff, mult, idle_streak, slow_streak, clean, check_windows;
    double check_pref;
    uint64_t engaged_at;
    uint64_t frames, frames_engaged, late_total, last_summary;
    double hold_ms_sum;
    uint32_t why[R_COUNT];
    uint32_t armed_said; /* bits: a reason for not engaging said once */
};

static int wanted = -1, verbose;

/* The switch without a line: 1 on, 0 off; *from 0 the guest's environment, 1 the server's switch (vkb_server_pd_info
 * .client_knobs: <cache dir>/env, a game's preset, the server's environment), 2 the default; *final 0 while the server's
 * switches are not in yet (asked again then). The guest's own VKBRIDGE_PACER wins. */
int vkb_pacer_setting(int *from, int *final)
{
    const char *e = getenv("VKBRIDGE_PACER");
    *final = 1;
    if (e && *e) {
        *from = 0;
        return strcmp(e, "0") != 0;
    }
    int known, k = vkb_client_knob(VKB_KNOB_PACER_SHIFT, &known);
    *final = known;
    *from = k >= 0 ? 1 : 2;
    return k == 1;
}

int vkb_pacer_wanted(void)
{
    int w = __atomic_load_n(&wanted, __ATOMIC_RELAXED);
    if (w >= 0) return w;
    int from, final;
    w = vkb_pacer_setting(&from, &final);
    if (!final) return w; /* nothing from the server yet: off for now, asked again */
    const char *e = getenv("VKBRIDGE_PACER");
    verbose = w && from == 0 && !strcmp(e, "2"); /* VKBRIDGE_PACER=2: also a line per window (what each rule saw) */
    __atomic_store_n(&wanted, w, __ATOMIC_RELAXED);
    if (w && from == 0)
        VKB_INFO("pacer: on (VKBRIDGE_PACER=1): present wait offered to DXVK; its swapchains' frames are paced while the GPU is the limit");
    else if (w)
        VKB_INFO("pacer: on (VKBRIDGE_PACER=1 from the server: <cache dir>/env or the game's preset): present wait offered to DXVK; "
                 "its swapchains' frames are paced while the GPU is the limit");
    return w;
}

vkb_pacer *vkb_pacer_create(vkb_swapchain *sc)
{
    if (!vkb_pacer_wanted()) return NULL;
    const vkb_physdev *pd = sc->dev ? sc->dev->pd : NULL;
    if (!pd || !pd->instance || !pd->instance->dxvk) {
        VKB_ONCE(VKB_LOG_INFO, "pacer: not used: the app is not DXVK (only DXVK's frame latency gate is paced)");
        return NULL;
    }
    if (sc->copy_present || !sc->dmabuf) {
        VKB_ONCE(VKB_LOG_INFO, "pacer: not used on a swapchain presented by copy (no presenting thread to see its frames' GPU work)");
        return NULL;
    }
    const char *why = vkb_dxvkcfg_pacer_conflict();
    if (why) {
        VKB_ONCE(VKB_LOG_INFO, "pacer: stands aside: DXVK_CONFIG sets %s (DXVK paces the frames itself)", why);
        return NULL;
    }
    vkb_pacer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->sc = sc;
    p->A = A_MIN;
    p->mult = 1;
    return p;
}

static void summary(vkb_pacer *p, const char *when)
{
    VKB_INFO("pacer%s: %llu frames, engaged for %.0f%%, hold %.1f ms on average there, %.1f%% of them maybe late, transit allowance "
             "%.1f ms; let go for: GPU idle %u, slower frames %u, not GPU-bound %u, compositor-bound %u, no present waits %u, GPU clock %u",
             when, (unsigned long long)p->frames, p->frames ? 100.0 * p->frames_engaged / p->frames : 0.0,
             p->frames_engaged ? p->hold_ms_sum / p->frames_engaged : 0.0,
             p->frames_engaged ? 100.0 * p->late_total / p->frames_engaged : 0.0, p->A, p->why[R_IDLE], p->why[R_SLOWER],
             p->why[R_NOTGPU], p->why[R_REFRESH], p->why[R_NOWAITS], p->why[R_CLOCK]);
}

void vkb_pacer_destroy(vkb_pacer *p)
{
    if (!p) return;
    if (p->frames) summary(p, " at the swapchain's end");
    free(p);
}

uint64_t vkb_pacer_hold_ns(vkb_pacer *p) { return p ? __atomic_load_n(&p->hold_ns, __ATOMIC_ACQUIRE) : 0; }

void vkb_pacer_pw_call(vkb_pacer *p)
{
    if (p) __atomic_fetch_add(&p->pw_calls, 1, __ATOMIC_RELAXED);
}

static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static double pctf(float *v, uint32_t n, double q)
{
    if (!n) return 0;
    qsort(v, n, sizeof(float), cmp_f);
    uint32_t i = (uint32_t)(q * n);
    return v[i >= n ? n - 1 : i];
}

static void set_hold(vkb_pacer *p, double h_ms)
{
    p->H = h_ms;
    __atomic_store_n(&p->hold_ns, h_ms > 0 ? (uint64_t)(h_ms * 1e6) : 0, __ATOMIC_RELEASE);
}

static void let_go(vkb_pacer *p, int reason, uint64_t now, const char *detail)
{
    p->engaged = 0;
    set_hold(p, 0);
    p->why[reason]++;
    if (now - p->engaged_at < 30000000000ull) p->mult = p->mult >= 32 ? 64 : p->mult * 2;
    else if (now - p->engaged_at >= 60000000000ull) p->mult = 1;
    p->backoff = 2 * p->mult;
    p->streak = 0;
    p->idle_streak = p->slow_streak = 0;
    VKB_INFO("pacer: let go (%s: %s); tried again after %d s", r_name[reason], detail, p->backoff);
}

/* engaged: one frame's slack and interval (ms) */
static void control(vkb_pacer *p, double slack, double d)
{
    if (d > 1.5 * p->dref) { /* a hitch (a load, a compile): nothing learned from it */
        p->freeze = 2;
        return;
    }
    if (!p->sd_init) {
        p->sd_init = 1;
        p->mean_s = slack;
        p->var_s = 1.0;
    } else {
        double e = slack - p->mean_s;
        p->mean_s += e / 16;
        p->var_s += (e * e - p->var_s) / 16;
    }
    double target = p->A + dmax(1.0, 2.0 * sqrt_pos(p->var_s));
    double h = p->H;
    if (slack < p->A) { /* frame k may have reached the GPU after it went idle */
        p->w_late++;
        p->w_excess += dmax(0.0, d - p->dref);
        if (!p->freeze) {
            h -= target - slack;
            p->freeze = 4; /* the frames already let go with the old hold come late too: one cut */
        }
    } else {
        if (slack >= p->A + 2.0) p->dref += (d - p->dref) / 16;
        if (p->freeze) p->freeze--;
        else {
            double err = slack - target;
            if (err > p->dref) err = p->dref;
            if (err < -p->dref) err = -p->dref;
            h += 0.1 * err;
        }
    }
    double hmax = dmin(H_MAX, 8.0 * p->dref);
    if (h > hmax) h = hmax;
    if (h < 0) h = 0;
    set_hold(p, h);
}

static void armed_say(vkb_pacer *p, int bit, const char *fmt, double a, double b)
{
    if (p->armed_said & (1u << bit)) return;
    p->armed_said |= 1u << bit;
    char msg[256];
    snprintf(msg, sizeof(msg), fmt, a, b);
    VKB_INFO("pacer: not engaged yet: %s", msg);
}

static void window_end(vkb_pacer *p, uint64_t now, uint64_t acq_total)
{
    uint32_t n = p->w_n, k = p->w_k;
    if (!n) {
        p->w_start = now;
        return;
    }
    float tmp[NS];
    memcpy(tmp, p->w_d, k * sizeof(float));
    double med_d = pctf(tmp, k, 0.5), p90_d = pctf(tmp, k, 0.9);
    memcpy(tmp, p->w_slack, k * sizeof(float));
    double p10_s = pctf(tmp, k, 0.1);
    uint64_t pw = __atomic_load_n(&p->pw_calls, __ATOMIC_RELAXED) - p->w_pw0;
    double pw_frac = (double)pw / n, acq_frac = (double)(acq_total - p->w_acq0) / n, real_frac = (double)p->w_real / n;
    double win_ms = (now - p->w_start) / 1e6;
    memcpy(tmp, p->w_wait, k * sizeof(float));
    double med_w = pctf(tmp, k, 0.5), p10_w = pctf(tmp, k, 0.1);
    if (verbose)
        VKB_INFO("pacer window: %u frames in %.0f ms, %s, frame %.1f ms (p90 %.1f), slack p10 %.1f, fence waits p10 %.1f p50 %.1f ms (%.0f%% real), "
                 "present waits %.0f%%, acquire waits %.0f%%, hold %.1f ms, late %u, transit allowance %.1f", n, win_ms,
                 p->engaged ? "engaged" : "not engaged", med_d, p90_d, p10_s, p10_w, med_w, 100 * real_frac, 100 * pw_frac, 100 * acq_frac,
                 p->H, p->w_late, p->A);
    if (!p->engaged) {
        if (p->check_windows > 0) {
            p->check_windows--;
            if (med_d <= 1.03 * p->check_pref) {
                p->why[R_CLOCK]++;
                if (p->backoff < 16 * p->mult) p->backoff = 16 * p->mult;
                p->mult = p->mult >= 32 ? 64 : p->mult * 2;
                p->check_windows = 0;
                VKB_INFO("pacer: frames took %.1f ms again without the hold (%.1f ms engaged): the hold slowed the GPU (its clock); "
                         "tried again after %d s", med_d, p->check_pref * 1.08, p->backoff);
            }
        }
        int ok = 1;
        if (p->backoff > 0) p->backoff--, ok = 0;
        else if (pw_frac < 0.5) ok = 0, armed_say(p, 0, "the app waits for %.0f%% of its presents (DXVK does in FIFO only)%.0s", 100 * pw_frac, 0);
        else if (real_frac < 0.9)
            ok = 0, armed_say(p, 1, "the presenting thread waited for the GPU on %.0f%% of frames (the compositor or the CPU is the limit)%.0s",
                              100 * real_frac, 0);
        else if (p10_s < dmax(4.0, med_d / 2))
            ok = 0, armed_say(p, 2, "the next frame was queued only %.1f ms ahead at the 10th percentile (%.1f ms a frame)", p10_s, med_d);
        else if (med_d < 4.0 || p90_d >= 1.5 * med_d)
            ok = 0, armed_say(p, 3, "frame times %.1f ms (p90 %.1f): too short or not steady", med_d, p90_d);
        if (ok) {
            p->elig_d[p->streak & 1] = med_d;
            if (++p->streak >= 2) {
                p->engaged = 1;
                p->pref = (p->elig_d[0] + p->elig_d[1]) / 2;
                p->dref = p->pref;
                p->sd_init = 0;
                p->freeze = 0;
                p->idle_streak = p->slow_streak = p->clean = 0;
                p->engaged_at = now;
                set_hold(p, 0);
                VKB_INFO("pacer: engaged: the GPU is the limit (%.1f ms a frame, the next frame queued %.1f ms ahead at the 10th "
                         "percentile, the presenting thread waited for the GPU on %.0f%% of frames)", med_d, p10_s, 100 * real_frac);
            }
        } else {
            p->streak = 0;
        }
    } else {
        p->frames_engaged += n;
        p->late_total += p->w_late;
        p->hold_ms_sum += p->w_hsum;
        double idle = p->w_excess / win_ms, late_frac = (double)p->w_late / n;
        char detail[160];
        int reason = -1;
        if (pw_frac < 0.5) {
            reason = R_NOWAITS;
            snprintf(detail, sizeof(detail), "the app waited for %.0f%% of its presents", 100 * pw_frac);
        } else if (real_frac < 0.75) {
            reason = R_REFRESH;
            snprintf(detail, sizeof(detail), "the presenting thread waited for the GPU on %.0f%% of frames (acquires waited on %.0f%%)",
                     100 * real_frac, 100 * acq_frac);
        } else if (idle > 0.03 && ++p->idle_streak >= 2) {
            reason = R_IDLE;
            snprintf(detail, sizeof(detail), "the GPU waited for frames %.1f%% of the time, %.0f%% of frames late", 100 * idle, 100 * late_frac);
        } else if (med_d > 1.08 * p->pref && ++p->slow_streak >= 2) {
            reason = R_SLOWER;
            snprintf(detail, sizeof(detail), "%.1f ms a frame, %.1f when it engaged", med_d, p->pref);
            p->check_windows = 2;
            p->check_pref = p->pref;
        } else if (p->w_hlow >= n) {
            reason = R_NOTGPU;
            snprintf(detail, sizeof(detail), "the hold stayed under 0.5 ms all window");
        }
        if (idle <= 0.03) p->idle_streak = 0;
        if (med_d <= 1.08 * p->pref) p->slow_streak = 0;
        if (reason < 0) {
            if (idle > 0.01 || late_frac > 0.08) {
                double a = dmin(A_MAX, p->A + 0.5);
                if (a > p->A)
                    VKB_INFO("pacer: frames reached the GPU after it went idle (%.1f%% of the time, %.0f%% of frames): transit allowance "
                             "%.1f ms", 100 * idle, 100 * late_frac, a);
                p->A = a;
                p->clean = 0;
            } else {
                if (++p->clean >= 10) {
                    p->A = dmax(A_MIN, p->A - 0.25);
                    p->clean = 0;
                }
                if (med_d < 1.03 * p->pref) p->pref = 0.9 * p->pref + 0.1 * med_d;
            }
        } else {
            let_go(p, reason, now, detail);
        }
    }
    if (p->frames - p->last_summary >= 1200) {
        p->last_summary = p->frames;
        summary(p, "");
    }
    p->w_start = now;
    p->w_pw0 = __atomic_load_n(&p->pw_calls, __ATOMIC_RELAXED);
    p->w_acq0 = acq_total;
    p->w_n = p->w_real = p->w_late = p->w_hlow = p->w_k = 0;
    p->w_excess = p->w_hsum = 0;
}

void vkb_pacer_frame(vkb_pacer *p, uint64_t present_ns, uint64_t done_ns, uint64_t waited_ns, uint64_t acq_total)
{
    if (!p || !present_ns || !done_ns) return;
    int real = waited_ns >= 1000000ull; /* the fence was not signalled yet when the presenting thread began to wait */
    if (!p->prev_d || done_ns <= p->prev_d) {
        if (!p->w_start) {
            p->w_start = done_ns;
            p->w_pw0 = __atomic_load_n(&p->pw_calls, __ATOMIC_RELAXED);
            p->w_acq0 = acq_total;
        }
        p->prev_d = done_ns;
        p->prev_real = real;
        return;
    }
    double slack = (double)(int64_t)(p->prev_d - present_ns) / 1e6;
    double d = (done_ns - p->prev_d) / 1e6;
    int prev_real = p->prev_real;
    p->prev_d = done_ns;
    p->prev_real = real;
    p->frames++;
    p->w_n++;
    p->w_real += real;
    if (p->w_k < NS) {
        p->w_slack[p->w_k] = (float)slack;
        p->w_d[p->w_k] = (float)d;
        p->w_wait[p->w_k] = (float)(waited_ns / 1e6);
        p->w_k++;
    }
    if (p->engaged) {
        /* the previous frame's done is a GPU time only when the presenting thread was waiting for it */
        if (prev_real) control(p, slack, d);
        p->w_hsum += p->H;
        if (p->H < 0.5) p->w_hlow++;
    }
    if ((done_ns - p->w_start >= WIN_NS && p->w_n >= WIN_MIN) || done_ns - p->w_start >= WIN_MAX_NS) window_end(p, done_ns, acq_total);
}
