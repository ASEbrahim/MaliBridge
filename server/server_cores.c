/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Fast cores for the threads a game waits on.
 *
 * A game's frame goes back and forth between the game, a connection thread here and the driver's
 * event thread (mali-event-hand: fences, completion). None of them is busy for long (on the Tab S10+,
 * vkmark effect2d at 322 fps: the connection thread 21% of one core, the event thread 14%), so the
 * scheduler sees small tasks and keeps them on the small cores at a low clock, and every hop waits
 * on a slow core. The same server pinned to the fast cores ran effect2d at ~550 fps instead of ~330.
 *
 * What this does, at each connection's start and once per device (vkCreateDevice):
 *   - every connection thread and the driver's event threads may run only on the fast cores (the cores
 *     whose capacity is at least 60% of the fastest one's, less a lone fastest core, which stays the
 *     game's); nothing changes when every core is fast, when fewer than 3 are, or when the kernel
 *     gives no capacities;
 *   - the same threads get nice NICE_HOT (wakes ahead of the game's own threads at nice 0..-2 on
 *     the same core, which wait for them anyway).
 * Background work started from those threads (pipeline compiles, BC transcoder builds, cache saves)
 * goes back to every core at nice 0 (vkb_cores_plain_thread), so it never takes a fast core from the
 * game.
 *
 * VKBRIDGE_HOT_CORES: 1/on = the above, unset/0/off = nothing (the threads as the process made them),
 * aff = affinity only, nice = nice only. Read at each device (<cache dir>/env is read again there).
 * OFF BY DEFAULT (11-07): on the Tab S10+ it lost ~8% in Portal in-game (53.5 fps against 58.2 with it
 * off, balanced ABBA blocks), likely because the game's own threads run on the same fast cores 4-7 and
 * the hot threads at nice -4 took turns from them. Kept for the rework (stay off the game's cores).
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_cores.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NICE_HOT (-4)

static const char *sysfs_root = "/sys/devices/system/cpu";

/* ------------------------------------------------------------------ topology (pure, unit-tested) */

/* cap[i] for cpu i (0 = unknown). The fast set: capacity >= 60% of the fastest. 0 when there is
 * nothing to do (an unknown capacity, every core fast, fewer than VKB_CORES_MIN_FAST fast cores). */
uint64_t vkb_cores_fast_mask(const unsigned *cap, int n)
{
    if (n <= 0 || n > 64) return 0;
    unsigned max = 0;
    for (int i = 0; i < n; i++) {
        if (!cap[i]) return 0;
        if (cap[i] > max) max = cap[i];
    }
    uint64_t m = 0;
    int k = 0;
    for (int i = 0; i < n; i++)
        if ((uint64_t)cap[i] * 10 >= (uint64_t)max * 6) m |= 1ull << i, k++;
    if (k < VKB_CORES_MIN_FAST || k == n) return 0;
    /* A lone fastest core (a prime) is left to the game: on the Tab S10+ a server thread there woke the
     * prime cluster at a throttled clock, the game's main thread followed it, and Portal lost fps. */
    int top = 0, ti = -1;
    for (int i = 0; i < n; i++)
        if (cap[i] == max) top++, ti = i;
    if (top == 1) m &= ~(1ull << ti);
    return m;
}

/* "4-7", "0,2-3" */
void vkb_cores_mask_str(uint64_t m, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < 64 && len < cap; i++) {
        if (!(m >> i & 1)) continue;
        int j = i;
        while (j + 1 < 64 && (m >> (j + 1) & 1)) j++;
        len += (size_t)snprintf(out + len, cap - len, "%s%d", len ? "," : "", i);
        if (j > i && len < cap) len += (size_t)snprintf(out + len, cap - len, "-%d", j);
        i = j;
    }
}

/* VKBRIDGE_HOT_CORES -> VKB_HOT_* */
int vkb_cores_mode(const char *v)
{
    if (!v || !*v) return 0; /* the default: off (see the top of this file) */
    if (!strcmp(v, "1") || !strcmp(v, "on")) return VKB_HOT_AFF | VKB_HOT_NICE;
    if (!strcmp(v, "aff")) return VKB_HOT_AFF;
    if (!strcmp(v, "nice")) return VKB_HOT_NICE;
    return 0; /* 0, off, anything else: as the default */
}

/* ------------------------------------------------------------------ the device */

static pthread_once_t once = PTHREAD_ONCE_INIT;
static uint64_t fast, all;
static int ncpu;

static unsigned read_cap(int cpu)
{
    char p[160];
    snprintf(p, sizeof(p), "%s/cpu%d/cpu_capacity", sysfs_root, cpu);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    unsigned v = 0;
    if (fscanf(f, "%u", &v) != 1) v = 0;
    fclose(f);
    return v;
}

static void topo_init(void)
{
    long n = sysconf(_SC_NPROCESSORS_CONF);
    ncpu = n > 0 && n <= 64 ? (int)n : 0;
    unsigned cap[64] = {0};
    for (int i = 0; i < ncpu; i++) cap[i] = read_cap(i);
    all = ncpu ? (ncpu == 64 ? ~0ull : (1ull << ncpu) - 1) : 0;
    fast = vkb_cores_fast_mask(cap, ncpu);
    char caps[256] = "", fs[64], ss[64];
    size_t len = 0;
    for (int i = 0; i < ncpu && len < sizeof(caps); i++) len += (size_t)snprintf(caps + len, sizeof(caps) - len, "%s%u", i ? " " : "", cap[i]);
    vkb_cores_mask_str(fast, fs, sizeof(fs));
    vkb_cores_mask_str(all & ~fast, ss, sizeof(ss));
    if (fast) VKB_INFO("cores: capacities %s; the threads a game waits on run on cores %s (not on %s: slow cores, or the lone fastest one, left to the game)", caps, fs, ss);
    else VKB_INFO("cores: capacities %s; no separate fast cores, the threads a game waits on run anywhere", caps);
}

static int set_aff(pid_t tid, uint64_t m)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    for (int i = 0; i < ncpu; i++)
        if (m >> i & 1) CPU_SET(i, &s);
    return sched_setaffinity(tid, sizeof(s), &s);
}

static int hot(pid_t tid, int mode)
{
    int ok = 1;
    if ((mode & VKB_HOT_AFF) && fast && set_aff(tid, fast) != 0) ok = 0;
    if ((mode & VKB_HOT_NICE) && setpriority(PRIO_PROCESS, (id_t)tid, NICE_HOT) != 0) ok = 0;
    return ok;
}

/* The driver's per-device event threads, by name (Mali: "mali-event-hand"). */
static int driver_threads(int mode, int *failed)
{
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[300], comm[32] = "";
        snprintf(p, sizeof(p), "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(comm, sizeof(comm), f)) comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        if (strcmp(comm, "mali-event-hand")) continue;
        if (hot((pid_t)atoi(e->d_name), mode)) n++;
        else (*failed)++;
    }
    closedir(d);
    return n;
}

/* Every connection: a game's other threads have connections of their own that never make a device
 * (Balatro under zink: the queue thread and the present thread carry most of the work). Silent; the
 * device's line says what was done. */
void vkb_cores_conn_thread(void)
{
    pthread_once(&once, topo_init);
    vkb_srv_env_refresh(); /* a changed <cache dir>/env before this process's first query */
    int mode = vkb_cores_mode(vkb_srv_env_src("VKBRIDGE_HOT_CORES", NULL));
    if (!fast) mode &= ~VKB_HOT_AFF;
    if (mode) hot((pid_t)syscall(SYS_gettid), mode);
}

void vkb_cores_device_made(void)
{
    pthread_once(&once, topo_init);
    int src = 0;
    const char *v = vkb_srv_env_src("VKBRIDGE_HOT_CORES", &src);
    int mode = vkb_cores_mode(v);
    if (!fast) mode &= ~VKB_HOT_AFF;
    if (!mode) {
        static int said;
        if (v) VKB_INFO("cores: VKBRIDGE_HOT_CORES=%s from %s: this device's threads left as they are", v, vkb_env_src_name(src));
        else if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
            VKB_INFO("cores: the threads a game waits on are left where the system puts them (off by default: on the fast cores they cost the game fps, Portal in-game on the Tab S10+; VKBRIDGE_HOT_CORES=1 places them)");
        return;
    }
    pid_t me = (pid_t)syscall(SYS_gettid);
    int failed = 0;
    if (!hot(me, mode)) failed++;
    int nd = driver_threads(mode, &failed);
    char fs[64];
    vkb_cores_mask_str(fast, fs, sizeof(fs));
    VKB_INFO("cores: connection thread %d and %d driver thread(s)%s%s%s%s%s", (int)me, nd,
             (mode & VKB_HOT_AFF) ? " on the fast cores " : "", (mode & VKB_HOT_AFF) ? fs : "",
             (mode & VKB_HOT_NICE) ? ", nice -4" : "", failed ? " (some refused: " : "", failed ? strerror(errno) : "");
    if (failed) VKB_INFO("cores: %d thread(s) kept their placement", failed);
}

/* Undoes only what vkb_cores_device_made did, inherited by a thread its connection thread started. */
void vkb_cores_plain_thread(void)
{
    pthread_once(&once, topo_init);
    pid_t me = (pid_t)syscall(SYS_gettid);
    if (fast) {
        cpu_set_t s;
        CPU_ZERO(&s);
        if (sched_getaffinity(me, sizeof(s), &s) == 0) {
            uint64_t m = 0;
            for (int i = 0; i < ncpu; i++)
                if (CPU_ISSET(i, &s)) m |= 1ull << i;
            if (m == fast) set_aff(me, all);
        }
    }
    errno = 0;
    if (getpriority(PRIO_PROCESS, (id_t)me) == NICE_HOT && !errno) setpriority(PRIO_PROCESS, (id_t)me, 0);
}

/* host tests */
void vkb_cores_test_root(const char *root)
{
    sysfs_root = root;
}
