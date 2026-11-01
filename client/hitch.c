/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The client's half of the per-hitch line (server/server_hitch.c has the line and the rest).
 * VKBRIDGE_HITCH_MS=<ms> in the guest's environment, else the server's value (vkbQueryServer's
 * hitch_ms: the game's preset, <cache dir>/env, the server's environment); off by default.
 *
 * On: each present is timed against the one before (one clock read per present), and the process's
 * round trips are timed (two clock reads each) into three sums - the app's waits (vkWaitForFences,
 * vkWaitSemaphores, vkQueueWaitIdle, vkDeviceWaitIdle), the swapchain's own waits (the frame's fence
 * in wsi.c, a free image at acquire) and every other round trip - each with its longest and the
 * thread that made it. A frame longer than the threshold goes to the server as vkbHitch (no reply,
 * in the asynchronous queue), with its window; the sums start again at every present. Off, a round
 * trip and a present cost one load of vkb_hitch_active each.
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "vkb_proto.h"

#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

int vkb_hitch_active;
__thread int vkb_hitch_in_present;

static uint32_t threshold_ms;
static int decided; /* the guest's environment read */
static int env_set; /* ... and it said something (the server's value is then ignored) */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; /* the present state and the longest waits */
static uint64_t t_prev, t_start, frames;
static vkb_hitch_wait acc[VKB_HITCH_W_COUNT];
static int enable_sent;

uint64_t vkb_hitch_now(void)
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
        const char *e = getenv("VKBRIDGE_HITCH_MS");
        if (e && *e) {
            char *end;
            unsigned long v = strtoul(e, &end, 10);
            if (*end || *e == '-') {
                VKB_WARN("VKBRIDGE_HITCH_MS=%.40s is not a number of milliseconds; not applied", e);
            } else {
                env_set = 1;
                threshold_ms = v > 60000 ? 60000 : (uint32_t)v;
            }
        }
        __atomic_store_n(&decided, 1, __ATOMIC_RELEASE);
        if (threshold_ms) __atomic_store_n(&vkb_hitch_active, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&lock);
}

/* What the server said at vkbQueryServer (physdev.c); the guest's environment wins. */
void vkb_hitch_server_ms(uint32_t ms)
{
    decide_env();
    if (env_set || !ms) return;
    pthread_mutex_lock(&lock);
    if (!threshold_ms) threshold_ms = ms > 60000 ? 60000 : ms;
    pthread_mutex_unlock(&lock);
    __atomic_store_n(&vkb_hitch_active, 1, __ATOMIC_RELEASE);
}

void vkb_hitch_waited(int kind, uint32_t cmd, uint64_t ns)
{
    vkb_hitch_wait *a = &acc[kind];
    __atomic_add_fetch(&a->ns, ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&a->n, 1, __ATOMIC_RELAXED);
    if (ns <= __atomic_load_n(&a->max_ns, __ATOMIC_RELAXED)) return;
    char name[16] = "";
    prctl(PR_GET_NAME, name, 0, 0, 0);
    pthread_mutex_lock(&lock);
    if (ns > a->max_ns) {
        __atomic_store_n(&a->max_ns, ns, __ATOMIC_RELAXED);
        a->max_cmd = cmd;
        memcpy(a->max_thread, name, sizeof(a->max_thread));
        a->max_thread[sizeof(a->max_thread) - 1] = 0;
    }
    pthread_mutex_unlock(&lock);
}

/* A round trip of cmd that took ns (conn.c). */
void vkb_hitch_rt(uint32_t cmd, uint64_t ns)
{
    int kind = VKB_HITCH_W_RT;
    if (cmd == VKB_CMD_vkWaitForFences || cmd == VKB_CMD_vkWaitSemaphores || cmd == VKB_CMD_vkQueueWaitIdle || cmd == VKB_CMD_vkDeviceWaitIdle)
        kind = vkb_hitch_in_present ? VKB_HITCH_W_PRESENT : VKB_HITCH_W_APP;
    vkb_hitch_waited(kind, cmd, ns);
}

static void send_msg(const vkb_hitch_msg *m)
{
    vkb_call c;
    vkb_call_begin(&c, VKB_CMD_vkbHitch, 0);
    vkb_enc_raw(&c.e, m, sizeof(*m));
    vkb_call_exec_async(&c);
}

/* At each vkQueuePresentKHR, before anything of it runs (wsi.c). */
void vkb_hitch_present(void)
{
    decide_env();
    if (!__atomic_load_n(&vkb_hitch_active, __ATOMIC_ACQUIRE)) return;
    uint64_t now = vkb_hitch_now();
    vkb_hitch_msg m;
    int send = 0;
    pthread_mutex_lock(&lock);
    if (!enable_sent) {
        /* the server keeps what it does for this process from now on (if it did not already) */
        enable_sent = 1;
        memset(&m, 0, sizeof(m));
        m.threshold_ms = threshold_ms;
        VKB_INFO("hitch attribution: frames over %u ms are reported to the server, whose log names what they spent their time on",
                 threshold_ms);
        pthread_mutex_unlock(&lock);
        send_msg(&m);
        pthread_mutex_lock(&lock);
    }
    frames++;
    if (t_start && now - t_start >= (uint64_t)threshold_ms * 1000000ull) {
        memset(&m, 0, sizeof(m));
        m.frame = frames;
        m.t_prev = t_prev;
        m.t_start = t_start;
        m.t_end = now;
        m.threshold_ms = threshold_ms;
        for (int i = 0; i < VKB_HITCH_W_COUNT; i++) {
            m.w[i].ns = __atomic_load_n(&acc[i].ns, __ATOMIC_RELAXED);
            m.w[i].n = __atomic_load_n(&acc[i].n, __ATOMIC_RELAXED);
            m.w[i].max_ns = acc[i].max_ns;
            m.w[i].max_cmd = acc[i].max_cmd;
            memcpy(m.w[i].max_thread, acc[i].max_thread, sizeof(m.w[i].max_thread));
        }
        send = 1;
    }
    for (int i = 0; i < VKB_HITCH_W_COUNT; i++) {
        __atomic_store_n(&acc[i].ns, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&acc[i].n, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&acc[i].max_ns, 0, __ATOMIC_RELAXED);
        acc[i].max_thread[0] = 0;
    }
    t_prev = t_start;
    t_start = now;
    pthread_mutex_unlock(&lock);
    if (send) send_msg(&m);
}
