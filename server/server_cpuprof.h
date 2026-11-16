/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Per-thread CPU evidence for the perf diagnostics' "cpu-threads:" line (server_perf.c; tests/cpuprof_unit.c): what a game's
 * busiest threads did with their time. For a thread, from /proc/<pid>/task/<tid>/:
 *   schedstat   "run_ns wait_ns slices": time on a CPU, time runnable but waiting for one (the run queue), times scheduled;
 *   stat        the state letter at the sample (R running or runnable, S sleeping, D disk wait) and the CPU it was on;
 *   wchan       the kernel function a sleeping thread waits in (futex, epoll, pipe, socket, sleep, poll). Android and some
 *               kernels hide it (reads "0"): then the sleeps are not told apart and the line says wc=-.
 * Over a window the line gives run % (on a CPU), rq % (runnable, waiting for a CPU), idle % (the rest: asleep or blocked),
 * slices a second and the samples' states and wchan classes. A game thread at run 77 rq 2 idle 21 with its sleeps in futex
 * waits was waiting for another thread; one at rq 15 was waiting for a CPU the system gave it late.
 * Read-only, header only (the unit test includes it), no allocation.
 */
#ifndef VKB_SERVER_CPUPROF_H
#define VKB_SERVER_CPUPROF_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cpuprof_sched {
    uint64_t run_ns, wait_ns, slices;
} cpuprof_sched;

enum {
    CPUPROF_WC_NONE, /* unreadable or not asleep */
    CPUPROF_WC_FUTEX,
    CPUPROF_WC_EPOLL,
    CPUPROF_WC_PIPE,
    CPUPROF_WC_SOCK,
    CPUPROF_WC_SLEEP,
    CPUPROF_WC_POLL,
    CPUPROF_WC_OTHER,
    CPUPROF_WC_COUNT
};

static const char *const cpuprof_wc_name[CPUPROF_WC_COUNT] = {"-", "futex", "epoll", "pipe", "sock", "sleep", "poll", "other"};

/* "run_ns wait_ns slices": 1 = all three read */
static inline int cpuprof_parse_schedstat(const char *buf, cpuprof_sched *out)
{
    if (!buf) return 0;
    uint64_t v[3];
    const char *p = buf;
    for (int i = 0; i < 3; i++) {
        char *e;
        if (*p < '0' || *p > '9') return 0;
        v[i] = strtoull(p, &e, 10);
        p = e;
        if (i < 2) {
            if (*p != ' ') return 0;
            p++;
        }
    }
    out->run_ns = v[0], out->wait_ns = v[1], out->slices = v[2];
    return 1;
}

static inline int cpuprof_wchan_class(const char *sym)
{
    if (!sym || !*sym || !strcmp(sym, "0")) return CPUPROF_WC_NONE;
    if (strstr(sym, "futex")) return CPUPROF_WC_FUTEX;
    if (strstr(sym, "epoll") || strstr(sym, "ep_poll")) return CPUPROF_WC_EPOLL;
    if (strstr(sym, "pipe")) return CPUPROF_WC_PIPE;
    if (strstr(sym, "unix_") || strstr(sym, "sk_wait") || strstr(sym, "sock") || strstr(sym, "tcp_")) return CPUPROF_WC_SOCK;
    if (strstr(sym, "nanosleep") || strstr(sym, "hrtimer") || strstr(sym, "schedule_timeout") || strstr(sym, "sleep")) return CPUPROF_WC_SLEEP;
    if (strstr(sym, "poll") || strstr(sym, "select")) return CPUPROF_WC_POLL;
    return CPUPROF_WC_OTHER;
}

/* One thread's window */
typedef struct cpuprof_acc {
    cpuprof_sched last;
    int have;
    uint64_t run_ns, wait_ns, slices;
    uint32_t st_r, st_s, st_d, st_o;
    uint32_t wc[CPUPROF_WC_COUNT];
} cpuprof_acc;

static inline void cpuprof_acc_init(cpuprof_acc *a) { memset(a, 0, sizeof(*a)); }

/* The next window from the last counters (nothing jumps) */
static inline void cpuprof_acc_reset(cpuprof_acc *a)
{
    cpuprof_sched last = a->last;
    int have = a->have;
    memset(a, 0, sizeof(*a));
    a->last = last;
    a->have = have;
}

/* One sample: the counters now, the state letter and the wchan class. The first sample only sets the base. A counter that went
 * backwards (a reused thread id) adds nothing and becomes the new base. */
static inline void cpuprof_acc_sample(cpuprof_acc *a, const cpuprof_sched *c, char state, int wc)
{
    if (!a->have) {
        a->last = *c;
        a->have = 1;
        return;
    }
    if (c->run_ns >= a->last.run_ns && c->wait_ns >= a->last.wait_ns && c->slices >= a->last.slices) {
        a->run_ns += c->run_ns - a->last.run_ns;
        a->wait_ns += c->wait_ns - a->last.wait_ns;
        a->slices += c->slices - a->last.slices;
    }
    a->last = *c;
    switch (state) {
    case 'R': a->st_r++; break;
    case 'S': a->st_s++; break;
    case 'D': a->st_d++; break;
    default: a->st_o++; break;
    }
    if (wc > CPUPROF_WC_NONE && wc < CPUPROF_WC_COUNT) a->wc[wc]++;
}

/* "name{run=77 rq=5 idle=18 sl=90 st=R4S6 wc=futex:3,epoll:2 cpu=p7:100}" over secs seconds; cpu is the caller's cluster text. Always
 * ends in a NUL within cap. */
static inline void cpuprof_acc_text(const cpuprof_acc *a, const char *name, double secs, const char *cpu, char *out, size_t cap)
{
    double w = secs * 1e9;
    unsigned run = w > 0 ? (unsigned)(a->run_ns * 100.0 / w + 0.5) : 0, rq = w > 0 ? (unsigned)(a->wait_ns * 100.0 / w + 0.5) : 0;
    if (run > 100) run = 100;
    if (rq > 100) rq = 100;
    unsigned idle = run + rq >= 100 ? 0 : 100 - run - rq;
    unsigned sl = secs > 0 ? (unsigned)(a->slices / secs + 0.5) : 0;
    char st[48] = "";
    size_t o = 0;
    if (a->st_r) o += (size_t)snprintf(st + o, sizeof(st) - o, "R%u", a->st_r);
    if (a->st_s) o += (size_t)snprintf(st + o, sizeof(st) - o, "S%u", a->st_s);
    if (a->st_d) o += (size_t)snprintf(st + o, sizeof(st) - o, "D%u", a->st_d);
    if (a->st_o) o += (size_t)snprintf(st + o, sizeof(st) - o, "O%u", a->st_o);
    if (!o) snprintf(st, sizeof(st), "-");
    char wc[96] = "";
    o = 0;
    for (int i = 1; i < CPUPROF_WC_COUNT; i++)
        if (a->wc[i]) o += (size_t)snprintf(wc + o, sizeof(wc) - o, "%s%s:%u", o ? "," : "", cpuprof_wc_name[i], a->wc[i]);
    if (!o) snprintf(wc, sizeof(wc), "-");
    snprintf(out, cap, "%s{run=%u rq=%u idle=%u sl=%u st=%s wc=%s cpu=%s}", name && *name ? name : "?", run, rq, idle, sl, st, wc,
             cpu && *cpu ? cpu : "-");
}

#endif
