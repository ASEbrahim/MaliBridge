/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * VKBRIDGE_FRAME_TRACE=1: a per-frame event trace of the presenting path, for finding where frames are lost (default off: one
 * load of vkb_ft_on at each site). Events go into a ring (one atomic add and four stores each) and a thread writes them out every
 * 250 ms as log lines "FT <pid> <events>", each event "<code><us since the epoch>,<a>,<b>", with the epoch printed in the first line
 * "FT0 <pid> epoch_ns=<CLOCK_MONOTONIC ns>" (the clock of the latency log, VKBRIDGE_LATLOG). The lines are always logged (as the
 * wait statistics are), so they are in the guest process's log. Codes: see the sites (wsi.c, wsi_wayland.c); the
 * code letters are listed in the comment above vkb_ft_event.
 */
#define _GNU_SOURCE
#include "vkb_client.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int vkb_ft_on;

#define RINGN (1u << 16)
typedef struct ft_ev {
    uint64_t ns;
    int64_t b;
    int32_t a;
    uint32_t code;
    uint32_t seq; /* written last: index + 1 */
} ft_ev;

static ft_ev ring[RINGN];
static uint64_t wr, rd, epoch;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Codes. wsi.c: a acquire entered, d/D acquire fence wait start/end, A acquired, f no free image, Q/q present entered/left, w/W frame-queue
 * wait start/end. wsi_wayland.c: m swapchain created, P present thread took a frame, g its fence wait ended, b/c/M wl_present: callback
 * outstanding, commit started, commit sent; F frame callback, L buffer release, S presentation feedback (seq, ns), X frame discarded. */
void vkb_ft_event(char code, int32_t a, int64_t b)
{
    uint64_t i = __atomic_fetch_add(&wr, 1, __ATOMIC_RELAXED);
    ft_ev *e = &ring[i % RINGN];
    e->ns = now_ns();
    e->a = a;
    e->b = b;
    e->code = (uint32_t)code;
    __atomic_store_n(&e->seq, (uint32_t)(i + 1), __ATOMIC_RELEASE);
}

static void *writer(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "vkb-ftrace");
    char line[1900];
    for (;;) {
        struct timespec ts = {0, 250000000};
        nanosleep(&ts, NULL);
        uint64_t end = __atomic_load_n(&wr, __ATOMIC_ACQUIRE);
        if (end - rd > RINGN - 64) {
            VKB_INFO_ALWAYS("FT lost %llu events", (unsigned long long)(end - rd - (RINGN - 64)));
            rd = end - (RINGN - 64);
        }
        size_t n = 0;
        while (rd < end) {
            ft_ev *e = &ring[rd % RINGN];
            if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != (uint32_t)(rd + 1)) break; /* not complete yet: next time */
            if (n > sizeof(line) - 64) {
                VKB_INFO_ALWAYS("FT %d%s", (int)getpid(), line);
                n = 0;
            }
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %c%lld,%d,%lld", (char)e->code, (long long)((e->ns - epoch) / 1000), e->a,
                                  (long long)e->b);
            rd++;
        }
        if (n) VKB_INFO_ALWAYS("FT %d%s", (int)getpid(), line);
    }
    return NULL;
}

static void start(void)
{
    const char *e = getenv("VKBRIDGE_FRAME_TRACE");
    if (!e || !*e || !strcmp(e, "0")) return;
    epoch = now_ns();
    VKB_INFO_ALWAYS("FT0 %d epoch_ns=%llu", (int)getpid(), (unsigned long long)epoch);
    pthread_t t;
    if (pthread_create(&t, NULL, writer, NULL) == 0) {
        pthread_detach(t);
        __atomic_store_n(&vkb_ft_on, 1, __ATOMIC_RELEASE);
    }
}

void vkb_ft_init(void) { pthread_once(&once, start); }
