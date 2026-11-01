/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Per-hitch attribution. With VKBRIDGE_HITCH_MS=<ms> (the game's preset, <cache dir>/env, the
 * server's or the guest's environment; off by default) a frame of the process that takes longer
 * than that gets one line in this log naming what the frame spent its time on:
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
 * often the next.
 *
 * cause= is the largest of compile, bc (the conversions' recording, or, when the waits are the
 * largest and textures were converted in this frame or the one before, the GPU time they were
 * waited for), alloc, pcache and wait (the longer of the app's and the swapchain's waits; GPU-bound
 * frames and a server busy elsewhere land here); other when none reaches a fifth of the frame -
 * the game's own work, FEX translating, CPU clocks (S6-S8 in the stutter plan). Each class is
 * capped at the frame's length: compiles on several threads can sum past it.
 *
 * At the process's end one line counts the hitches by cause. At most HITCH_LINES lines per process
 * (counted after that). Off, nothing is kept: the ring is made when the client turns it on, and
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
#define MERGE_NS 2000000ull /* events of one class closer than this are one entry (a burst of uploads) */

typedef struct hev {
    uint64_t t0, t1, ns, bytes;
    uint32_t n;
    uint32_t cls;
} hev;

enum { C_COMPILE, C_BC, C_ALLOC, C_PCACHE, C_WAIT, C_OTHER, C_COUNT };
static const char *const cause_name[C_COUNT] = {"compile", "bc", "alloc", "pcache", "wait", "other"};

struct vkb_hitch {
    pthread_mutex_t lock;
    hev ev[HITCH_RING];
    uint64_t written;  /* entries ever written (ring index: written % HITCH_RING) */
    uint32_t threshold_ms;
    uint32_t lines, compile_lines;
    uint64_t hitches, causes[C_COUNT];
    double worst_ms;
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
    if (!v || !*v) return 0;
    char *end;
    unsigned long ms = strtoul(v, &end, 10);
    if (*end || *v == '-') {
        VKB_ONCE(VKB_LOG_WARN, "VKBRIDGE_HITCH_MS=%.40s is not a number of milliseconds; off", v);
        return 0;
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
    struct vkb_hitch *none = NULL;
    if (!__atomic_compare_exchange_n(&p->hitch, &none, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pthread_mutex_destroy(&h->lock);
        free(h);
        return;
    }
    VKB_INFO("hitch attribution on for process %d (%s): frames over %u ms are logged with what they spent their time on", p->pid,
             p->comm[0] ? p->comm : "?", ms);
}

void vkb_hitch_note(vkb_srv_proc *p, int cls, uint64_t t0, uint64_t t1, uint32_t n, uint64_t bytes)
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
    } else {
        h->ev[h->written % HITCH_RING] = (hev){t0, t1, t1 - t0, bytes, n, (uint32_t)cls};
        h->written++;
    }
    pthread_mutex_unlock(&h->lock);
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
 * what size; *lost: the ring no longer reaches back to ws. */
static void sum(struct vkb_hitch *h, int cls, uint64_t ws, uint64_t we, double *ms, uint64_t *n, uint64_t *bytes, int *lost)
{
    double ns = 0;
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
    }
    if (i == have && h->written > HITCH_RING) *lost = 1;
    *ms = ns / 1e6;
    if (n) *n = cnt;
    if (bytes) *bytes = b;
}

static double cap(double v, double lim) { return v > lim ? lim : v; }

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
    if (!m.frame) { /* the client turned it on (its own environment, or what vkbQueryServer said) */
        vkb_hitch_enable(p, m.threshold_ms ? m.threshold_ms : 1);
        return;
    }
    struct vkb_hitch *h = __atomic_load_n(&p->hitch, __ATOMIC_ACQUIRE);
    if (!h) {
        vkb_hitch_enable(p, m.threshold_ms ? m.threshold_ms : 1);
        if (!(h = __atomic_load_n(&p->hitch, __ATOMIC_ACQUIRE))) return;
    }
    for (int i = 0; i < VKB_HITCH_W_COUNT; i++) m.w[i].max_thread[sizeof(m.w[i].max_thread) - 1] = 0;
    if (m.t_end < m.t_start) m.t_end = m.t_start;
    if (!m.t_prev || m.t_prev > m.t_start) m.t_prev = m.t_start;
    double frame_ms = (m.t_end - m.t_start) / 1e6;
    double cms, bms, ams, pms, recent_ms;
    uint64_t cn, bn, btex, an, abytes, rtex;
    int lost = 0;
    pthread_mutex_lock(&h->lock);
    sum(h, C_COMPILE, m.t_start, m.t_end, &cms, &cn, NULL, &lost);
    sum(h, C_BC, m.t_start, m.t_end, &bms, &bn, &btex, &lost);
    sum(h, C_BC, m.t_prev, m.t_end, &recent_ms, NULL, &rtex, &lost);
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
    if (top == C_WAIT && rtex) top = C_BC; /* the GPU converting textures is what was waited for */
    double topv = top == C_BC ? (v[C_BC] > v[C_WAIT] ? v[C_BC] : v[C_WAIT]) : v[top];
    if (topv * 5 < frame_ms) top = C_OTHER;
    uint32_t line_no = __atomic_fetch_add(&h->lines, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->hitches, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&h->causes[top], 1, __ATOMIC_RELAXED);
    if (frame_ms > h->worst_ms) h->worst_ms = frame_ms;
    if (line_no >= HITCH_LINES) {
        if (line_no == HITCH_LINES)
            VKB_INFO("hitch: process %d has had %u hitch lines; further hitches are only counted (the summary at its end)", p->pid,
                     HITCH_LINES);
        return;
    }
    char wmax[64] = "-";
    const vkb_hitch_wait *lw = wa->max_ns >= wp->max_ns ? wa : wp;
    if (lw->max_ns)
        snprintf(wmax, sizeof(wmax), "%s@%s", lw->max_cmd < VKB_CMD_COUNT ? vkb_cmd_names[lw->max_cmd] : "acquire",
                 lw->max_thread[0] ? lw->max_thread : "?");
    VKB_INFO("hitch: frame=%llu ms=%.0f cause=%s compile_ms=%.0f compiles=%llu bc_ms=%.0f bc_mtex=%.1f bc_recent_mtex=%.1f alloc_ms=%.0f "
             "allocs=%llu alloc_mb=%llu pcache_ms=%.0f wait_ms=%.0f wait_n=%llu wait_max_ms=%.0f wait_max=%s present_ms=%.0f "
             "rt_ms=%.0f rt_n=%llu pid=%d comm=%s%s",
             (unsigned long long)m.frame, frame_ms, cause_name[top], cms, (unsigned long long)cn, bms, btex / 1e6, rtex / 1e6, ams,
             (unsigned long long)an, (unsigned long long)(abytes >> 20), pms, wait_ms, (unsigned long long)wa->n,
             (lw->max_ns) / 1e6, wmax, present_ms, wr->ns / 1e6, (unsigned long long)wr->n, p->pid, p->comm[0] ? p->comm : "?",
             lost ? " lost=1" : "");
}

void vkb_hitch_proc_gone(vkb_srv_proc *p)
{
    struct vkb_hitch *h = p ? __atomic_exchange_n(&p->hitch, NULL, __ATOMIC_ACQ_REL) : NULL;
    if (!h) return;
    if (h->hitches)
        VKB_INFO("hitches over %u ms in process %d (%s): %llu (compile %llu, bc %llu, alloc %llu, pcache %llu, wait %llu, other %llu); "
                 "worst %.0f ms",
                 h->threshold_ms, p->pid, p->comm[0] ? p->comm : "?", (unsigned long long)h->hitches,
                 (unsigned long long)h->causes[C_COMPILE], (unsigned long long)h->causes[C_BC], (unsigned long long)h->causes[C_ALLOC],
                 (unsigned long long)h->causes[C_PCACHE], (unsigned long long)h->causes[C_WAIT], (unsigned long long)h->causes[C_OTHER],
                 h->worst_ms);
    else
        VKB_INFO("hitches over %u ms in process %d (%s): none", h->threshold_ms, p->pid, p->comm[0] ? p->comm : "?");
    pthread_mutex_destroy(&h->lock);
    free(h);
}
