/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * cores_unit: the fast cores for the threads a game waits on (server/server_cores.c) on its own,
 * the server's environment and log stubbed:
 *   1  the fast set from cpu capacities, a lone fastest core left to the game (on the S10+ a server
 *      thread on it woke the throttled prime cluster and Portal lost fps): Tab S10+ (Dimensity 9300+,
 *      4 A720 + 3 X4 + 1 X4 prime) = 4-6, Dimensity 8300 (4 A510 + 3 A715 + 1 A715) = 4-6,
 *      Dimensity 9400 (4 A720 + 3 X4 + 1 X925) = 4-6, Exynos 1380 (4 A78 + 4 A55, big first) = 0-3,
 *      8 Gen 2 (3 A510 + 2 A715 + 2 A710 + 1 X3) = 3-6; nothing for every core alike (8 Elite),
 *      2 big + 6 small (Helio G99), an unknown capacity, one core;
 *   2  VKBRIDGE_HOT_CORES: 1/on both, unset/empty/0/off/unknown nothing (OFF BY DEFAULT since 11-07:
 *      Portal in-game lost ~8% with it on the Tab S10+), aff, nice;
 *   3  on this machine with a made-up sysfs (cpu0 small, the rest fast): after vkb_cores_device_made
 *      the calling thread may run only on the fast cores; a thread it starts gets every core back
 *      with vkb_cores_plain_thread; with VKBRIDGE_HOT_CORES=0 nothing changes;
 *      every connection thread at its start too (VKBRIDGE_HOT_CORES=0: not);
 *   4  nice -4 on the calling thread (only where this machine allows a lower nice; else said);
 *   5  the default (VKBRIDGE_HOT_CORES unset) changes nothing, on a device or a connection thread, and
 *      says why once per process (not again at the next device).
 * Prints "OK: ..." / "FAILED: ..." last. host/Makefile builds it; host/run_tests.sh runs it.
 */
#include "../server/server_cores.c"

#include <stdarg.h>
#include <sys/stat.h>

static const char *fake_mode;
const char *vkb_srv_env_src(const char *name, int *src)
{
    if (src) *src = fake_mode ? VKB_ENV_SRC_CACHE_ENV : VKB_ENV_SRC_NONE;
    return strcmp(name, "VKBRIDGE_HOT_CORES") ? NULL : fake_mode;
}
void vkb_srv_env_refresh(void) {}
const char *vkb_env_src_name(int src) { return src ? "<cache dir>/env" : "the default"; }
static char last_log[512];
void vkb_log(int level, const char *fmt, ...)
{
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    fprintf(stderr, "  log: %s\n", last_log);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint64_t my_mask(void)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    sched_getaffinity(0, sizeof(s), &s);
    uint64_t m = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &s)) m |= 1ull << i;
    return m;
}

static uint64_t child_mask;
static int child_nice;
static void *child(void *arg)
{
    (void)arg;
    vkb_cores_plain_thread();
    child_mask = my_mask();
    child_nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    return NULL;
}

static uint64_t conn_mask;
static void *conn_child(void *arg)
{
    (void)arg;
    vkb_cores_conn_thread();
    conn_mask = my_mask();
    return NULL;
}

int main(void)
{
    /* 1 */
    unsigned s10[] = {450, 450, 450, 450, 871, 871, 871, 1024};
    CHECK(vkb_cores_fast_mask(s10, 8) == 0x70, "S10+ %llx", (unsigned long long)vkb_cores_fast_mask(s10, 8));
    unsigned d8300[] = {280, 280, 280, 280, 880, 880, 880, 1024};
    CHECK(vkb_cores_fast_mask(d8300, 8) == 0x70, "D8300");
    unsigned d9400[] = {520, 520, 520, 520, 890, 890, 890, 1024};
    CHECK(vkb_cores_fast_mask(d9400, 8) == 0x70, "D9400");
    unsigned twoplus[] = {300, 300, 300, 300, 300, 300, 800, 1024};
    CHECK(vkb_cores_fast_mask(twoplus, 8) == 0, "1 + 1 + 6 small: one core left after the prime -> nothing");
    unsigned ex1380[] = {1024, 1024, 1024, 1024, 380, 380, 380, 380};
    CHECK(vkb_cores_fast_mask(ex1380, 8) == 0x0f, "Exynos 1380");
    unsigned g2[] = {280, 280, 280, 780, 780, 750, 750, 1024};
    CHECK(vkb_cores_fast_mask(g2, 8) == 0x78, "8 Gen 2 %llx", (unsigned long long)vkb_cores_fast_mask(g2, 8));
    unsigned elite[] = {1024, 1024, 830, 830, 830, 830, 830, 830};
    CHECK(vkb_cores_fast_mask(elite, 8) == 0, "8 Elite: all fast -> nothing");
    unsigned g99[] = {400, 400, 400, 400, 400, 400, 1024, 1024};
    CHECK(vkb_cores_fast_mask(g99, 8) == 0, "2 big + 6 small -> nothing");
    unsigned unk[] = {450, 450, 0, 450, 871, 871, 871, 1024};
    CHECK(vkb_cores_fast_mask(unk, 8) == 0, "unknown capacity -> nothing");
    unsigned one[] = {1024};
    CHECK(vkb_cores_fast_mask(one, 1) == 0, "one core");
    char b[64];
    vkb_cores_mask_str(0xf0, b, sizeof(b));
    CHECK(!strcmp(b, "4-7"), "mask_str %s", b);
    vkb_cores_mask_str(0x2d, b, sizeof(b));
    CHECK(!strcmp(b, "0,2-3,5"), "mask_str %s", b);

    /* 2 */
    CHECK(vkb_cores_mode(NULL) == 0 && vkb_cores_mode("") == 0, "default must be off");
    CHECK(vkb_cores_mode("1") == (VKB_HOT_AFF | VKB_HOT_NICE) && vkb_cores_mode("on") == (VKB_HOT_AFF | VKB_HOT_NICE), "1/on");
    CHECK(vkb_cores_mode("yes") == 0, "an unknown value is the default (off)");
    CHECK(vkb_cores_mode("0") == 0 && vkb_cores_mode("off") == 0, "0/off");
    CHECK(vkb_cores_mode("aff") == VKB_HOT_AFF, "aff");
    CHECK(vkb_cores_mode("nice") == VKB_HOT_NICE, "nice");

    /* 3: a made-up sysfs for this machine's cores */
    long n = sysconf(_SC_NPROCESSORS_CONF);
    uint64_t start = my_mask();
    int start_nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (n < 4 || n > 64 || (start & 0xf) != 0xf) {
        printf("OK: topology and modes (%d checks, default off); placement skipped: %ld cores here, affinity %llx\n", 19, n, (unsigned long long)start);
        return fails ? 1 : 0;
    }
    char root[] = "/tmp/cores_unit.XXXXXX";
    if (!mkdtemp(root)) return 2;
    for (long i = 0; i < n; i++) {
        char p[128];
        snprintf(p, sizeof(p), "%s/cpu%ld", root, i);
        mkdir(p, 0755);
        snprintf(p, sizeof(p), "%s/cpu%ld/cpu_capacity", root, i);
        FILE *f = fopen(p, "w");
        fprintf(f, "%u\n", i == 0 ? 300u : 1024u);
        fclose(f);
    }
    vkb_cores_test_root(root);
    uint64_t all_m = n == 64 ? ~0ull : (1ull << n) - 1, fast_m = all_m & ~1ull;

    /* 5: the default, before anything else moved this thread */
    fake_mode = NULL;
    last_log[0] = 0;
    vkb_cores_device_made();
    CHECK(my_mask() == start, "the default changed the affinity to %llx", (unsigned long long)my_mask());
    CHECK(getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid)) == start_nice, "the default changed nice");
    CHECK(strstr(last_log, "off by default") != NULL, "the default's reason not said: %s", last_log);
    last_log[0] = 0;
    vkb_cores_device_made();
    CHECK(last_log[0] == 0, "the default's reason said twice: %s", last_log);
    pthread_t dt;
    pthread_create(&dt, NULL, conn_child, NULL);
    pthread_join(dt, NULL);
    CHECK(conn_mask == start, "connection thread with the default: %llx", (unsigned long long)conn_mask);

    fake_mode = "0";
    vkb_cores_device_made();
    CHECK(my_mask() == start, "VKBRIDGE_HOT_CORES=0 changed the affinity to %llx", (unsigned long long)my_mask());
    CHECK(getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid)) == start_nice, "VKBRIDGE_HOT_CORES=0 changed nice");

    /* a connection that never makes a device (Balatro: zink's queue thread and the present thread
     * each have a connection of their own) gets the fast cores at its start */
    fake_mode = "aff";
    pthread_t ct;
    pthread_create(&ct, NULL, conn_child, NULL);
    pthread_join(ct, NULL);
    CHECK(conn_mask == fast_m, "connection thread: %llx, want %llx", (unsigned long long)conn_mask, (unsigned long long)fast_m);
    fake_mode = "0";
    pthread_create(&ct, NULL, conn_child, NULL);
    pthread_join(ct, NULL);
    CHECK(conn_mask == start, "connection thread with 0: %llx", (unsigned long long)conn_mask);
    fake_mode = "aff";
    vkb_cores_device_made();
    CHECK(my_mask() == fast_m, "after aff: %llx, want %llx", (unsigned long long)my_mask(), (unsigned long long)fast_m);
    CHECK(strstr(last_log, "on the fast cores 1-") != NULL, "log: %s", last_log);
    pthread_t th;
    pthread_create(&th, NULL, child, NULL);
    pthread_join(th, NULL);
    CHECK(child_mask == all_m, "plain thread: %llx, want %llx", (unsigned long long)child_mask, (unsigned long long)all_m);

    /* 4 (on: VKBRIDGE_HOT_CORES=1) */
    fake_mode = "1";
    vkb_cores_device_made();
    int nv = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    const char *nice_note = "nice -4 checked";
    if (nv == NICE_HOT) {
        pthread_create(&th, NULL, child, NULL);
        pthread_join(th, NULL);
        CHECK(child_nice == 0, "plain thread nice %d", child_nice);
    } else {
        CHECK(strstr(last_log, "kept their placement") != NULL || strstr(last_log, "refused") != NULL, "refused nice not said: %s", last_log);
        nice_note = "nice -4 not allowed here (said in the log)";
    }
    CHECK(my_mask() == fast_m, "with 1: %llx", (unsigned long long)my_mask());

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd)) {}
    if (fails) printf("FAILED: %d check(s)\n", fails);
    else printf("OK: topology, modes (default off) and placement on %ld cores (fast %llx); %s\n", n, (unsigned long long)fast_m, nice_note);
    return fails ? 1 : 0;
}
