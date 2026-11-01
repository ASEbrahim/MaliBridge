/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The client's half of the latency log (server/server_latency.c writes the lines).
 * VKBRIDGE_LATLOG in the guest's environment (anything but 0: on), else the server's value
 * (vkbQueryServer's latlog: the game's preset, <cache dir>/env, the server's environment); off by
 * default.
 *
 * On, each vkQueuePresentKHR gets a number (n, from 1 in the process) and a record of when:
 *   queued     the app called vkQueuePresentKHR (with DXVK: its presenting thread, after its own queue)
 *   gpu_done   the client saw the frame's rendering done (the frame's fence: wsi.c, wsi_wayland.c)
 *   committed  the frame was handed to the compositor (wl_surface.commit, the X11 present)
 *   shown      the compositor said it was shown (wp_presentation_feedback.presented; on DroidDeck,
 *              the scene that drew it; under nested gamescope, gamescope's own)
 * At each present a vkbLatency message (no reply, in the asynchronous queue, so in order before the
 * present's own submit) numbers it and carries the records completed since the last one; the server
 * adds when it reached the message and writes one line a frame. A record waiting for an answer from
 * the compositor that never comes is sent 64 presents later, marked so.
 *
 * Off, a present costs one load of vkb_lat_active. On: a few clock reads and one small message a
 * frame, and three short lock holds.
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "vkb_proto.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int vkb_lat_active;

#define RING 256  /* records by n % RING */
#define LATE 64   /* presents after which a record still waiting for the compositor is sent as it is */
#define OUTQ 64   /* completed records waiting for the next present */

enum { ST_FREE, ST_OPEN };
#define I_PRESENTED 0x10000u /* internal: the present call is over (commit stamped or failed) */
#define I_ANSWERED 0x20000u  /* internal: the compositor answered (shown or discarded) */

typedef struct rec {
    vkb_lat_frame f;
    int state;
    uint32_t iflags;
} rec;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static rec ring[RING];
static vkb_lat_frame outq[OUTQ];
static uint32_t nout, dropped, next_n;
static int decided, env_set, said;

uint64_t vkb_lat_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void decide_env(void)
{
    if (__atomic_load_n(&decided, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&lock);
    if (!decided) {
        const char *e = getenv("VKBRIDGE_LATLOG");
        if (e && *e) {
            env_set = 1;
            if (strcmp(e, "0")) __atomic_store_n(&vkb_lat_active, 1, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&decided, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
}

int vkb_lat_on(void)
{
    decide_env();
    return __atomic_load_n(&vkb_lat_active, __ATOMIC_ACQUIRE);
}

/* What the server said at vkbQueryServer (physdev.c); the guest's environment wins. */
void vkb_lat_server(uint32_t on)
{
    decide_env();
    if (env_set || !on) return;
    __atomic_store_n(&vkb_lat_active, 1, __ATOMIC_RELEASE);
}

/* lock held: r is complete when the present call is over and the compositor, if asked, answered */
static void maybe_complete(rec *r)
{
    if (r->state != ST_OPEN || !(r->iflags & I_PRESENTED)) return;
    if ((r->f.flags & VKB_LAT_F_FEEDBACK) && !(r->iflags & I_ANSWERED)) return;
    if (nout == OUTQ) {
        memmove(outq, outq + 1, (OUTQ - 1) * sizeof(*outq));
        nout--;
        dropped++;
    }
    outq[nout++] = r->f;
    r->state = ST_FREE;
}

static rec *find(uint32_t n)
{
    rec *r = &ring[n % RING];
    return n && r->state == ST_OPEN && r->f.n == n ? r : NULL;
}

static void send_msg(const vkb_lat_msg *m)
{
    vkb_call c;
    vkb_call_begin(&c, VKB_CMD_vkbLatency, 0);
    vkb_enc_raw(&c.e, m, offsetof(vkb_lat_msg, f) + m->count * sizeof(vkb_lat_frame));
    vkb_call_exec_async(&c);
}

/* lock held, released on return: sends m (this present, or none) with the completed records, in as
 * many messages as they need. Returns the records dropped since the last time. */
static uint32_t drain(vkb_lat_msg *m)
{
    uint32_t lost = dropped;
    dropped = 0;
    for (;;) {
        uint32_t k = nout < VKB_LAT_MAX_FRAMES ? nout : VKB_LAT_MAX_FRAMES;
        memcpy(m->f, outq, k * sizeof(*outq));
        memmove(outq, outq + k, (nout - k) * sizeof(*outq));
        nout -= k;
        m->count = k;
        pthread_mutex_unlock(&lock);
        if (m->n || k) send_msg(m);
        pthread_mutex_lock(&lock);
        if (!nout) break;
        m->n = 0;
        m->queued = 0;
    }
    pthread_mutex_unlock(&lock);
    return lost;
}

/* At each vkQueuePresentKHR, before anything of it goes to the server (wsi.c): the present's
 * number, 0 while off. */
uint32_t vkb_lat_present(void)
{
    if (!vkb_lat_on()) return 0;
    uint64_t now = vkb_lat_now();
    vkb_lat_msg m;
    pthread_mutex_lock(&lock);
    if (!said) {
        said = 1;
        VKB_INFO("latency log: each frame's stages (presented by the app, at the server, drawn, committed, shown) go to the "
                 "server's vkbridge-latency.log (VKBRIDGE_LATLOG)");
    }
    uint32_t n = ++next_n;
    if (!n) n = ++next_n; /* 0 means none */
    if (n > LATE) {
        rec *old = find(n - LATE);
        if (old) {
            old->f.flags |= VKB_LAT_F_NO_ANSWER;
            old->iflags |= I_PRESENTED | I_ANSWERED;
            maybe_complete(old);
        }
    }
    rec *r = &ring[n % RING];
    memset(r, 0, sizeof(*r));
    r->state = ST_OPEN;
    r->f.n = n;
    r->f.queued = now;
    m.n = n;
    m.queued = now;
    uint32_t lost = drain(&m);
    if (lost) VKB_WARN("latency log: %u frame records dropped (the app presented faster than they were sent)", lost);
    return n;
}

/* The frame's rendering seen done (VKB_LAT_DONE) or the frame shown (VKB_LAT_SHOWN, the
 * compositor's time); t 0: now. */
void vkb_lat_stamp(uint32_t n, int what, uint64_t t)
{
    if (!n) return;
    if (!t) t = vkb_lat_now();
    pthread_mutex_lock(&lock);
    rec *r = find(n);
    if (r) {
        if (what == VKB_LAT_DONE) r->f.gpu_done = t;
        else if (what == VKB_LAT_SHOWN) {
            r->f.shown = t;
            r->iflags |= I_ANSWERED;
            maybe_complete(r);
        }
    }
    pthread_mutex_unlock(&lock);
}

/* VKB_LAT_F_* for frame n: FEEDBACK when the compositor was asked (the record then waits for its
 * answer); DISCARDED or OTHER_CLOCK when it answered (never shown; shown, no usable time). */
void vkb_lat_flag(uint32_t n, uint32_t flags)
{
    if (!n) return;
    pthread_mutex_lock(&lock);
    rec *r = find(n);
    if (r) {
        r->f.flags |= flags;
        if (flags & (VKB_LAT_F_DISCARDED | VKB_LAT_F_OTHER_CLOCK | VKB_LAT_F_NO_ANSWER)) {
            r->iflags |= I_ANSWERED;
            maybe_complete(r);
        }
    }
    pthread_mutex_unlock(&lock);
}

/* The present call for frame n is over (wsi.c, the presenting thread): committed now when ok, else
 * marked failed. */
void vkb_lat_presented(uint32_t n, int ok)
{
    if (!n) return;
    uint64_t t = vkb_lat_now();
    pthread_mutex_lock(&lock);
    rec *r = find(n);
    if (r) {
        if (ok) r->f.committed = t;
        else r->f.flags |= VKB_LAT_F_FAILED;
        if (!ok) r->iflags |= I_ANSWERED; /* nothing will come */
        r->iflags |= I_PRESENTED;
        maybe_complete(r);
    }
    pthread_mutex_unlock(&lock);
}

/* A swapchain is gone (wsi.c, after its backend answered or gave up its frames): the completed
 * records go now, not at a present that may never come. */
void vkb_lat_flush(void)
{
    if (!__atomic_load_n(&vkb_lat_active, __ATOMIC_ACQUIRE)) return;
    vkb_lat_msg m;
    memset(&m, 0, offsetof(vkb_lat_msg, f));
    pthread_mutex_lock(&lock);
    drain(&m);
}
