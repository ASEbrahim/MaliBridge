/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The latency log. With VKBRIDGE_LATLOG (the game's preset, <cache dir>/env, the server's or the
 * guest's environment; anything but 0: on; off by default) every frame a process presents through
 * the bridge gets one line in vkbridge-latency.log, in the session folder (the server log's, so
 * Send logs carries it; VKBRIDGE_LATLOG=<path with a '/'> names another file):
 *
 *   F <pid> <comm> <n> <queued> <server> <gpu_done> <committed> <shown> <flags>
 *
 * all CLOCK_MONOTONIC ns, 0 when not seen; the clock DroidDeck's input and compositor lines use
 * too (droiddeck-latency.log, same folder), so scripts/latency_report.py joins them offline:
 *   queued     the app called vkQueuePresentKHR (DXVK: its presenting thread)
 *   server     the server reached that present in the process's ordered stream (its vkbLatency,
 *              sent just before the present's own submit): queued -> server is the stream's backlog
 *   gpu_done   the client saw the frame's rendering done (its fence)
 *   committed  the client handed it to the compositor (wl_surface.commit)
 *   shown      the compositor's wp_presentation time for it (DroidDeck: the scene that drew it;
 *              nested gamescope: gamescope's)
 *   flags      VKB_LAT_F_*: 1 shown by the presenting thread, 2 the compositor asked, 4 discarded
 *              by it, 8 no answer from it, 16 the present failed, 32 shown on another clock
 *
 * and, when the file is opened and for each new process, "C <CLOCK_MONOTONIC> <CLOCK_MONOTONIC_RAW>"
 * (ns, read together): Wine's QueryPerformanceCounter runs on the raw clock, so a Windows program's
 * own times (tools/lagprobe.exe's) can be put on this one.
 *
 * The client (client/latency.c) sends each present's number at the present and its record once it
 * is complete, a frame or a few later; this side keeps when it saw each number (a ring per process)
 * and writes the line when the record comes. Off, nothing is kept and no message comes.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RING 256
#define PROCS 32

typedef struct lat_proc {
    uint64_t token;
    uint64_t used;               /* for reuse: the oldest goes */
    uint32_t n[RING];
    uint64_t t[RING];
} lat_proc;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static lat_proc *procs[PROCS];
static uint64_t uses;
static int fd = -1;
static char path[700];
static int said_fail;

static uint64_t clock_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now_ns(void) { return clock_ns(CLOCK_MONOTONIC); }

/* the C line: the raw clock read between two reads of the monotonic one */
static int clock_line(char *buf, size_t cap)
{
    uint64_t a = now_ns(), raw = clock_ns(CLOCK_MONOTONIC_RAW), b = now_ns();
    return snprintf(buf, cap, "C %llu %llu\n", (unsigned long long)(a + (b - a) / 2), (unsigned long long)raw);
}

static int on_value(const char *v) { return v && *v && strcmp(v, "0"); }

/* VKBRIDGE_LATLOG as the server has it for the client on this thread (presets, <cache dir>/env,
 * the server's environment): 1 on. */
uint32_t vkb_lat_setting(void) { return on_value(vkb_srv_env("VKBRIDGE_LATLOG")) ? 1u : 0u; }

/* VKBRIDGE_FRAME_QUEUE the same way: 1-8 (the client caps the frames rendering at once, client/wsi.c);
 * 0, unset or not a number: off. */
uint32_t vkb_frame_queue_setting(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_FRAME_QUEUE");
    if (!v || !*v) return 0;
    char *end;
    unsigned long n = strtoul(v, &end, 10);
    if (*end || *v == '-') {
        VKB_ONCE(VKB_LOG_WARN, "VKBRIDGE_FRAME_QUEUE=%.40s is not a number of frames; off", v);
        return 0;
    }
    return n > 8 ? 8u : (uint32_t)n;
}

/* VKBRIDGE_PRESENT_WAIT the same way: 0, VKB_PRESENT_WAIT_OFF (the client offers no present ids and
 * waits, client/wsi.c); "commit", VKB_PRESENT_WAIT_COMMIT; anything else, VKB_PRESENT_WAIT_ON; unset, 0 (the client's
 * default: on). */
uint32_t vkb_present_wait_setting(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_PRESENT_WAIT");
    if (!v || !*v) return 0;
    if (!strcmp(v, "commit")) return VKB_PRESENT_WAIT_COMMIT;
    return strcmp(v, "0") ? VKB_PRESENT_WAIT_ON : VKB_PRESENT_WAIT_OFF;
}

/* lock held: the file for this session (the session folder can change between sessions: the
 * in-app server outlives them) */
static int log_fd(void)
{
    char want[700] = "";
    const char *v = vkb_srv_env("VKBRIDGE_LATLOG");
    char dir[600];
    if (v && strchr(v, '/')) snprintf(want, sizeof(want), "%s", v);
    else if (vkb_report_dir(dir, sizeof(dir))) snprintf(want, sizeof(want), "%s/vkbridge-latency.log", dir);
    else if (vkb_pcache_dir() && *vkb_pcache_dir()) snprintf(want, sizeof(want), "%s/vkbridge-latency.log", vkb_pcache_dir());
    else snprintf(want, sizeof(want), "vkbridge-latency.log");
    if (fd >= 0 && !strcmp(want, path)) return fd;
    if (fd >= 0) close(fd);
    snprintf(path, sizeof(path), "%s", want);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (!said_fail++) VKB_WARN("latency log: cannot open %s; its lines are not written", path);
        return -1;
    }
    static const char head[] =
        "# vkbridge latency log 1: F pid comm n queued server gpu_done committed shown flags; CLOCK_MONOTONIC ns, 0 not seen;"
        " flags 1 async 2 feedback-asked 4 discarded 8 no-answer 16 failed 32 other-clock\n";
    char cl[64];
    int cn = clock_line(cl, sizeof(cl));
    if (write(fd, head, sizeof(head) - 1) < 0 || write(fd, cl, (size_t)cn) < 0) { /* a full disk: the lines fail too */ }
    VKB_INFO("latency log: one line a frame in %s (VKBRIDGE_LATLOG)", path);
    return fd;
}

/* lock held; *fresh: the process is new here */
static lat_proc *proc_slot(uint64_t token, int *fresh)
{
    *fresh = 0;
    int free_i = -1, old_i = 0;
    for (int i = 0; i < PROCS; i++) {
        if (procs[i] && procs[i]->token == token) {
            procs[i]->used = ++uses;
            return procs[i];
        }
        if (!procs[i] && free_i < 0) free_i = i;
        if (procs[i] && procs[old_i] && procs[i]->used < procs[old_i]->used) old_i = i;
    }
    int i = free_i >= 0 ? free_i : old_i;
    *fresh = 1;
    if (!procs[i] && !(procs[i] = malloc(sizeof(lat_proc)))) return NULL;
    memset(procs[i], 0, sizeof(lat_proc));
    procs[i]->token = token;
    procs[i]->used = ++uses;
    return procs[i];
}

void vkb_sv_vkbLatency(vkb_srv_call *c)
{
    uint64_t t = now_ns();
    vkb_lat_msg m;
    memset(&m, 0, sizeof(m));
    vkb_dec_raw_tail(&c->d, &m, sizeof(m));
    vkb_srv_proc *p = c->conn ? c->conn->proc : NULL;
    if (c->d.err || !p) {
        vkb_srv_bad_message(c);
        return;
    }
    if (m.count > VKB_LAT_MAX_FRAMES) m.count = VKB_LAT_MAX_FRAMES;
    char comm[32];
    snprintf(comm, sizeof(comm), "%s", p->comm[0] ? p->comm : "?");
    for (char *q = comm; *q; q++)
        if (*q == ' ' || *q == '\t' || *q == '\n') *q = '_';
    char buf[64 + VKB_LAT_MAX_FRAMES * 200];
    size_t len = 0;
    pthread_mutex_lock(&lock);
    int fresh;
    lat_proc *lp = proc_slot(p->token, &fresh);
    if (fresh) len += (size_t)clock_line(buf, sizeof(buf));
    if (lp && m.n) {
        lp->n[m.n % RING] = m.n;
        lp->t[m.n % RING] = t;
    }
    for (uint32_t i = 0; i < m.count; i++) {
        const vkb_lat_frame *f = &m.f[i];
        uint64_t srv = lp && f->n && lp->n[f->n % RING] == f->n ? lp->t[f->n % RING] : 0;
        int k = snprintf(buf + len, sizeof(buf) - len, "F %d %s %u %llu %llu %llu %llu %llu %u\n", p->pid, comm, f->n,
                         (unsigned long long)f->queued, (unsigned long long)srv, (unsigned long long)f->gpu_done,
                         (unsigned long long)f->committed, (unsigned long long)f->shown, f->flags);
        if (k > 0 && (size_t)k < sizeof(buf) - len) len += (size_t)k;
    }
    int out = len ? log_fd() : -1;
    if (out >= 0 && write(out, buf, len) < 0) { /* a full disk: the log stops */ }
    pthread_mutex_unlock(&lock);
}
