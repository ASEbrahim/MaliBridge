/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Frame statistics for the server's session report (server/server_report.c). Presentation is the
 * client's own (wsi.c): the server never sees a present, and until now a session's frame rate was
 * whatever the tester saw. Each process counts its presents and the time between them, and at most
 * once a second rewrites one line into <dir>/vkbridge-frames-<pid>.txt - <dir> being the folder of
 * VKBRIDGE_CLIENT_LOG (DroidDeck's session folder, where the server's log and report are) or
 * VKBRIDGE_FRAMES_DIR. VKBRIDGE_FRAMES=0 turns it off. Times are CLOCK_MONOTONIC seconds, which the
 * server shares (one kernel), so it can place the first frame against the process's start.
 *
 * Not every process shows its frames through a swapchain of the bridge's: nested gamescope hands
 * its output to the outer compositor as dma-bufs, Xwayland and Zink under X (steamwebhelper)
 * pass theirs on as dma-bufs too. The line also counts the swapchains made and the memory the app
 * exported or imported (fds), so the report can tell such a process from one that hangs.
 */
#define _GNU_SOURCE
#include "vkb_client.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FT_BUCKETS 201 /* 1 ms each; the last: 200 ms and more */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int state = -1; /* -1 not looked at, 0 off, 1 on */
static char path[640];
static uint64_t presents, external, all_presents; /* all_presents: counted with the statistics off too */
static uint32_t swapchains;
static double t_first, t_prev, t_write, sec_start, ft_max;
static uint32_t ft[FT_BUCKETS], fps_s[6], sec_frames, hitch100, hitch1000;

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void setup(void)
{
    state = 0;
    const char *v = getenv("VKBRIDGE_FRAMES");
    if (v && !strcmp(v, "0")) return;
    char dir[512] = "";
    const char *d = getenv("VKBRIDGE_FRAMES_DIR");
    if (d && *d) snprintf(dir, sizeof(dir), "%s", d);
    else if ((d = getenv("VKBRIDGE_CLIENT_LOG")) && *d) {
        snprintf(dir, sizeof(dir), "%s", d);
        char *s = strrchr(dir, '/');
        if (!s) return;
        *s = 0;
    }
    if (!dir[0]) return;
    snprintf(path, sizeof(path), "%s/vkbridge-frames-%d.txt", dir, (int)getpid());
    state = 1;
}

/* The frame time (ms) at fraction q of all frames. */
static unsigned pct(double q)
{
    uint64_t n = 0, want;
    for (int i = 0; i < FT_BUCKETS; i++) n += ft[i];
    if (!n) return 0;
    want = (uint64_t)(q * (double)n);
    uint64_t acc = 0;
    for (int i = 0; i < FT_BUCKETS; i++) {
        acc += ft[i];
        if (acc > want) return (unsigned)i + 1;
    }
    return FT_BUCKETS;
}

static void write_line(void)
{
    char comm[32] = "?";
    int fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        comm[n > 0 ? n : 0] = 0;
        comm[strcspn(comm, "\n ")] = 0;
        close(fd);
    }
    unsigned p50 = pct(0.5);
    double span = t_prev - t_first;
    char line[600];
    int len = snprintf(line, sizeof(line),
                       "pid=%d comm=%s presents=%llu t_first=%.3f t_last=%.3f fps_avg=%.1f fps_p50=%.1f ft_p50=%u ft_p90=%u ft_p99=%u "
                       "ft_max=%.0f hitch100=%u hitch1000=%u fps_seconds=<10:%u,10-20:%u,20-30:%u,30-45:%u,45-60:%u,60+:%u swapchains=%u external=%llu\n",
                       (int)getpid(), comm, (unsigned long long)presents, t_first, t_prev, span > 0 ? (double)(presents - 1) / span : 0.0,
                       p50 ? 1000.0 / p50 : 0.0, p50, pct(0.9), pct(0.99), ft_max, hitch100, hitch1000, fps_s[0], fps_s[1], fps_s[2], fps_s[3],
                       fps_s[4], fps_s[5], swapchains, (unsigned long long)external);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        state = 0; /* a folder the process may not write: no more tries */
        return;
    }
    if (write(fd, line, (size_t)len) < 0) {}
    close(fd);
}

void vkb_frames_present(void)
{
    __atomic_add_fetch(&all_presents, 1, __ATOMIC_RELAXED);
    double now = mono();
    pthread_mutex_lock(&lock);
    if (state < 0) setup();
    if (!state) {
        pthread_mutex_unlock(&lock);
        return;
    }
    if (!presents) {
        t_first = sec_start = now;
    } else {
        double ms = (now - t_prev) * 1000.0;
        ft[ms >= FT_BUCKETS - 1 ? FT_BUCKETS - 1 : (int)ms]++;
        if (ms > ft_max) ft_max = ms;
        hitch100 += ms >= 100.0;
        hitch1000 += ms >= 1000.0;
    }
    t_prev = now;
    presents++;
    sec_frames++;
    if (now - sec_start >= 1.0) {
        double fps = sec_frames / (now - sec_start);
        fps_s[fps < 10 ? 0 : fps < 20 ? 1 : fps < 30 ? 2 : fps < 45 ? 3 : fps < 60 ? 4 : 5]++;
        sec_frames = 0;
        sec_start = now;
    }
    if (now - t_write >= 1.0 || presents == 1) {
        t_write = now;
        write_line();
    }
    pthread_mutex_unlock(&lock);
}

/* A swapchain made (wsi.c), or memory the app exported or imported as an fd (memory.c): counted,
 * and the line written at the first of each, then at most once a second. */
static void count(uint32_t *sc, uint64_t *ext)
{
    double now = mono();
    pthread_mutex_lock(&lock);
    if (state < 0) setup();
    if (state) {
        int first = sc ? !(*sc)++ : !(*ext)++;
        if (first || now - t_write >= 1.0) {
            t_write = now;
            write_line();
        }
    }
    pthread_mutex_unlock(&lock);
}

void vkb_frames_swapchain(void) { count(&swapchains, NULL); }

void vkb_frames_external(void) { count(NULL, &external); }

uint64_t vkb_frames_presented(void) { return __atomic_load_n(&all_presents, __ATOMIC_RELAXED); }
