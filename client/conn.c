/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Client transport: one Unix-socket connection per thread (the server mirrors it with one thread),
 * so calls from different threads run in parallel exactly as they would on a local driver.
 */
#define _GNU_SOURCE
#include "vkb_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ logging */

static int log_level_v = -1;
static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

int vkb_log_level(void)
{
    if (log_level_v < 0) {
        const char *d = getenv("VKBRIDGE_DEBUG");
        log_level_v = d && *d && strcmp(d, "0") ? VKB_LOG_DEBUG : VKB_LOG_WARN;
        const char *v = getenv("VKBRIDGE_LOG_LEVEL");
        if (v) log_level_v = atoi(v);
        const char *f = getenv("VKBRIDGE_CLIENT_LOG");
        if (f && *f) log_file = fopen(f, "ae");
    }
    return log_level_v;
}

static void log_msg(int level, const char *msg);

void vkb_log(int level, const char *fmt, ...)
{
    if (level > vkb_log_level()) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log_msg(level, msg);
}

/* A line that is informational but always wanted (the default-on statistics: VKBRIDGE_WAIT_STATS):
 * tagged info, written at any log level but VKBRIDGE_LOG_LEVEL below 0 (nothing at all). */
void vkb_log_info_always(const char *fmt, ...)
{
    if (vkb_log_level() < 0) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log_msg(VKB_LOG_INFO, msg);
}

static void log_msg(int level, const char *msg)
{
    static const char *const tags[] = {"error", "warning", "info", "debug"};
    char comm[32] = "?";
    FILE *cf = fopen("/proc/self/comm", "re");
    if (cf) {
        if (fgets(comm, sizeof(comm), cf)) comm[strcspn(comm, "\n")] = 0;
        fclose(cf);
    }
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "vkbridge %s [%s %d]: %s\n", tags[level & 3], comm, getpid(), msg);
    if (log_file) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        fprintf(log_file, "%02d:%02d:%02d.%03ld vkbridge-client %s [%s %d]: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                ts.tv_nsec / 1000000, tags[level & 3], comm, getpid(), msg);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_lock);
}

/* The first time each call returns each error, one warning: games often stop on a failed submit,
 * wait or present with a message of their own (Stray's DXGI_ERROR_DEVICE_RESET, #21) or none at
 * all, and until now neither log said which call failed. The line names the call, the result, the
 * frames presented so far and the CLOCK_MONOTONIC time (as in vkbridge-frames-<pid>.txt), so it
 * can be placed against the server's log. Callers test the result first (vkb_note_error); this
 * runs only on an error. */
static const char *result_name(VkResult r)
{
    switch (r) {
#define R(x) case x: return #x;
    R(VK_TIMEOUT) R(VK_NOT_READY) R(VK_ERROR_OUT_OF_HOST_MEMORY) R(VK_ERROR_OUT_OF_DEVICE_MEMORY) R(VK_ERROR_INITIALIZATION_FAILED)
    R(VK_ERROR_DEVICE_LOST) R(VK_ERROR_MEMORY_MAP_FAILED) R(VK_ERROR_LAYER_NOT_PRESENT) R(VK_ERROR_EXTENSION_NOT_PRESENT)
    R(VK_ERROR_FEATURE_NOT_PRESENT) R(VK_ERROR_INCOMPATIBLE_DRIVER) R(VK_ERROR_TOO_MANY_OBJECTS) R(VK_ERROR_FORMAT_NOT_SUPPORTED)
    R(VK_ERROR_FRAGMENTED_POOL) R(VK_ERROR_UNKNOWN) R(VK_ERROR_OUT_OF_POOL_MEMORY) R(VK_ERROR_INVALID_EXTERNAL_HANDLE)
    R(VK_ERROR_FRAGMENTATION) R(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS) R(VK_ERROR_SURFACE_LOST_KHR)
    R(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) R(VK_ERROR_OUT_OF_DATE_KHR) R(VK_ERROR_VALIDATION_FAILED_EXT)
    R(VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
#undef R
    default: return "an error";
    }
}

#define FIRST_ERRORS 64
static struct {
    const char *call;
    VkResult r;
} first_errors[FIRST_ERRORS];
static uint32_t first_error_count;
static pthread_mutex_t first_error_lock = PTHREAD_MUTEX_INITIALIZER;

void vkb_first_error(const char *call, VkResult r)
{
    pthread_mutex_lock(&first_error_lock);
    for (uint32_t i = 0; i < first_error_count; i++)
        if (first_errors[i].r == r && !strcmp(first_errors[i].call, call)) {
            pthread_mutex_unlock(&first_error_lock);
            return;
        }
    uint32_t n = first_error_count;
    if (n <= FIRST_ERRORS) first_error_count++;
    if (n < FIRST_ERRORS) {
        first_errors[n].call = call;
        first_errors[n].r = r;
    }
    pthread_mutex_unlock(&first_error_lock);
    if (n == FIRST_ERRORS) VKB_WARN("more Vulkan errors returned to the game; not logged");
    if (n >= FIRST_ERRORS) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    VKB_WARN("%s returned %s (%d) to the game, the first time; %llu frames presented, t=%.3f%s", call, result_name(r), (int)r,
             (unsigned long long)vkb_frames_presented(), ts.tv_sec + ts.tv_nsec / 1e9,
             r == VK_ERROR_DEVICE_LOST ? " (games report this as a lost or reset device)"
             : r > 0 ? " (the driver may not return this here; DXVK reports it as a reset device)" : "");
}

/* ------------------------------------------------------------------ connection */

/* The async-send timer's counts of one thread (see vkb_call_exec): kept in the thread's own connection, so that counting a call
 * touches no thread-local of its own (in the ICD library each costs a __tls_get_addr call). */
typedef struct async_timer {
    uint64_t n;                  /* async requests sent */
    uint64_t tn, tns;            /* the sampled ones and their time, the lock and flush time inside them taken out (the plain append) */
    uint64_t lock_ns, lock_n;    /* waits for the queue's lock (another thread, the flusher, held it): every one timed */
    uint64_t flush_ns, flush_n;  /* flushes this thread did (queue full): every one timed */
    uint64_t max_ns, start;      /* the longest lock wait or flush; the window's start */
} async_timer;

typedef struct vkb_conn {
    int sock;
    int dead;
    uint32_t epoch;
    vkb_enc enc;            /* reused request buffer */
    uint8_t *in;
    size_t incap;
    vkb_arena arena;
    int busy;
    async_timer at;
} vkb_conn;

static uint64_t process_token;
/* VKBRIDGE_STATS (vkb_stats_count): messages written to the socket and their bytes, headers
 * included - what each side pays a syscall, a header and an ordering step for. */
static int stat_period = -1;
static uint64_t stat_msgs, stat_wire, stat_msgs_total, stat_wire_total;
static void stat_wire_add(uint64_t msgs, uint64_t bytes)
{
    if (stat_period <= 0) return;
    __atomic_add_fetch(&stat_msgs, msgs, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stat_wire, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stat_msgs_total, msgs, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stat_wire_total, bytes, __ATOMIC_RELAXED);
}
/* The last asynchronous request's sequence number in this process (see vkb_msg_hdr). */
static uint64_t async_seq;
static uint32_t fork_epoch;
static pthread_key_t conn_key;
static pthread_once_t conn_once = PTHREAD_ONCE_INIT;
static __thread vkb_conn *tls_conn;

static void conn_free(vkb_conn *c)
{
    if (!c) return;
    if (c->sock >= 0) close(c->sock);
    vkb_enc_free(&c->enc);
    free(c->in);
    vkb_arena_free(&c->arena);
    free(c);
}

static void async_note_flush(vkb_conn *c);

static void conn_thread_exit(void *p)
{
    async_note_flush(p); /* the thread's last async-send line */
    conn_free(p);
}

/* Asynchronous requests of the whole process wait here and are written out in one go, ahead of
 * the next synchronous request any thread makes (or when the queue grows large). They carry their
 * sequence numbers, so whichever connection carries them, the server runs them in order.
 *
 * Normally the queue is one vkbBatch message (server_conn.c run_batch): the first request opens
 * it with the next sequence number and every request until the flush is appended to it as a u32
 * (command | table << 16), a u32 size and its payload padded to 8 bytes. DXVK sends ~100k small
 * requests a second (a descriptor set allocation and an update per draw, Subnautica 2026-10-12);
 * as messages of their own, each cost both sides a header, an ordering wait and a wake-up.
 * On by default (vkb_desc_batch); VKBRIDGE_DESC_BATCH=0, VKBRIDGE_NO_DESC_BATCH=1 or
 * VKBRIDGE_DESC_BATCH_NOFRAME=1 send one message per request, as before v0.5.5. */
static pthread_mutex_t aq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *aq_buf;
static size_t aq_len, aq_cap;
#define AQ_FLUSH (256u << 10)
/* The queue's socket writes happen OUTSIDE aq_lock (2026-11-10, RE3 / Tomb Raider / BL2: dxvk-cs waited 1.1-1.4 s of every 10 s
 * on this lock, ~91 us a wait, max 3.8-7.2 ms, while its holder - mostly the flusher, else a synchronous caller - was inside
 * send(), which blocks when the server's thread is behind and keeps the lock while its thread is preempted). Under the lock a
 * writer only takes what is queued (aq_take_locked: the buffer swapped for a spare) and reads the sequence numbers; it writes
 * after unlocking. Two batches can then reach the server on two connections in either order: the server runs requests by
 * sequence number whatever connection carries them (server_conn.c order_wait), as it does for any two threads' messages.
 * At AQ_FLUSH the appending thread hands the write to the flusher instead of doing it itself; only at AQ_HARD (the flusher
 * behind too) does it write itself: the old backpressure, without holding the lock. VKBRIDGE_AQ_LOCKED_WRITES=1: every write
 * under the lock as before (the A/B arm). */
#define AQ_HARD (4u * AQ_FLUSH)
#define AQ_SPARES 2
static uint8_t *aq_spare[AQ_SPARES];
static size_t aq_spare_cap[AQ_SPARES];
static int aq_inflight; /* batches taken out of the queue and not yet written (under aq_lock) */
static pthread_cond_t aq_idle = PTHREAD_COND_INITIALIZER;
typedef struct aq_taken {
    uint8_t *buf;
    size_t len, cap;
} aq_taken;

static int aq_locked_writes(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("VKBRIDGE_AQ_LOCKED_WRITES");
        v = !(e && *e) || strcmp(e, "0") != 0; /* v0.5.5.4: the locked writes by default; VKBRIDGE_AQ_LOCKED_WRITES=0 */
    }
    return v;
}

static int write_all(int sock, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t r = send(sock, b, n, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static int aq_flush_locked(vkb_conn *conn)
{
    if (!aq_len) return 0;
    int r = write_all(conn->sock, aq_buf, aq_len);
    aq_len = 0;
    if (aq_cap > (4u << 20)) {
        free(aq_buf);
        aq_buf = NULL;
        aq_cap = 0;
    }
    return r;
}

/* Under aq_lock: what is queued leaves the queue (a spare buffer, or none, takes its place); the caller writes it with
 * aq_write_taken after unlocking, then gives the buffer back with aq_give_back. Nothing queued: t->len 0, nothing to do. */
static void aq_take_locked(aq_taken *t)
{
    t->buf = aq_buf;
    t->len = aq_len;
    t->cap = aq_cap;
    if (!aq_len) return;
    aq_buf = NULL;
    aq_cap = 0;
    aq_len = 0;
    for (int i = 0; i < AQ_SPARES; i++)
        if (aq_spare[i]) {
            aq_buf = aq_spare[i];
            aq_cap = aq_spare_cap[i];
            aq_spare[i] = NULL;
            break;
        }
    aq_inflight++;
}

/* Not under aq_lock. */
static int aq_write_taken(vkb_conn *conn, const aq_taken *t)
{
    return t->len ? write_all(conn->sock, t->buf, t->len) : 0;
}

/* Under aq_lock: the written buffer back to the spares (one above 4 MB, or with the spares full, is freed). */
static void aq_give_back_locked(aq_taken *t)
{
    if (!t->len) return;
    int kept = 0;
    for (int i = 0; i < AQ_SPARES && !kept && t->cap <= (4u << 20); i++)
        if (!aq_spare[i]) {
            aq_spare[i] = t->buf;
            aq_spare_cap[i] = t->cap;
            kept = 1;
        }
    if (!kept) free(t->buf);
    t->buf = NULL;
    t->len = 0;
    if (--aq_inflight == 0) pthread_cond_broadcast(&aq_idle);
}

/* The flusher's own counts ("async flusher" line, every WAIT_STATS period when its writes took >= 1 % of it): the time it spent
 * writing the queue to the socket - before 2026-11-10 with aq_lock held, i.e. what the other threads' "lock wait" waited for. */
static struct {
    uint64_t start, n, bytes, ns, max_ns;
} aq_fstat;
static void aq_flusher_note(uint64_t t0, uint64_t t1, size_t bytes);

/* The flusher: asynchronous requests do not wait for the next synchronous one forever - another
 * thread may be blocked in the server on work they would start (a submit that signals what it is
 * waiting for). It sends what is queued ~100 us after it was queued, batching what follows. */
static pthread_cond_t aq_cond = PTHREAD_COND_INITIALIZER;
static int aq_flusher_started;
static int aq_flusher_gone; /* it could not connect, or lost its connection */
static uint64_t aq_first_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static vkb_conn *get_conn(void);

static void *aq_flusher(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "vkb-flush", 0, 0, 0); /* else it carries the name of the thread that started it (often dxvk-cs) */
    vkb_conn *conn = get_conn();
    pthread_mutex_lock(&aq_lock);
    if (!conn) {
        aq_flusher_gone = 1;
        pthread_mutex_unlock(&aq_lock);
        return NULL;
    }
    for (;;) {
        while (!aq_len) pthread_cond_wait(&aq_cond, &aq_lock);
        uint64_t due = aq_first_ns + 100000, now = now_ns();
        if (now < due && aq_len < AQ_FLUSH / 4) {
            pthread_mutex_unlock(&aq_lock);
            struct timespec ts = {0, (long)(due - now)};
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&aq_lock);
            continue;
        }
        size_t bytes = aq_len;
        uint64_t t0 = now_ns();
        int r;
        if (aq_locked_writes()) {
            r = aq_flush_locked(conn);
        } else {
            aq_taken t;
            aq_take_locked(&t);
            pthread_mutex_unlock(&aq_lock);
            r = aq_write_taken(conn, &t);
            pthread_mutex_lock(&aq_lock);
            aq_give_back_locked(&t);
        }
        aq_flusher_note(t0, now_ns(), bytes);
        if (r < 0) {
            conn->dead = 1;
            aq_flusher_gone = 1;
            break;
        }
    }
    pthread_mutex_unlock(&aq_lock);
    return NULL;
}

/* Under aq_lock. */
static int aq_grow(size_t need)
{
    size_t nc = aq_cap ? aq_cap : 65536;
    while (nc < need) nc *= 2;
    uint8_t *nb = realloc(aq_buf, nc);
    if (!nb) return -1;
    aq_buf = nb;
    aq_cap = nc;
    return 0;
}

/* Under aq_lock. */
static void aq_start_flusher(void)
{
    if (aq_flusher_started) return;
    aq_flusher_started = 1;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, aq_flusher, NULL) != 0) aq_flusher_started = 0;
    pthread_attr_destroy(&a);
}

static void aq_exit_flush(void)
{
    vkb_conn *c = tls_conn;
    if (!c || c->dead) return;
    pthread_mutex_lock(&aq_lock);
    aq_flush_locked(c);
    /* batches other threads took out of the queue and are still writing (at most 200 ms): exit must not cut one */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 200000000L;
    if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    while (aq_inflight > 0 && pthread_cond_timedwait(&aq_idle, &aq_lock, &until) == 0) {}
    pthread_mutex_unlock(&aq_lock);
}

static void atfork_child(void)
{
    aq_len = 0;
    aq_flusher_started = 0;
    aq_flusher_gone = 0;
    aq_inflight = 0; /* the parent's writers are not in the child; their buffers stay with the parent */
    for (int i = 0; i < AQ_SPARES; i++) aq_spare[i] = NULL;
    memset(&aq_fstat, 0, sizeof(aq_fstat));
    pthread_cond_init(&aq_cond, NULL);
    pthread_cond_init(&aq_idle, NULL);
    pthread_mutex_init(&aq_lock, NULL);
    /* The child must not talk on the parent's sockets: new token, new connections. */
    fork_epoch++;
    process_token = 0;
    async_seq = 0;
    tls_conn = NULL; /* the old one is leaked deliberately: its socket belongs to the parent too */
}

static void conn_init_once(void)
{
    pthread_key_create(&conn_key, conn_thread_exit);
    pthread_atfork(NULL, NULL, atfork_child);
    atexit(aq_exit_flush);
}

void vkb_conn_after_fork(void) { atfork_child(); }

static uint64_t make_token(void)
{
    uint64_t t = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &t, sizeof(t)) != sizeof(t)) t = 0;
        close(fd);
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t ^= ((uint64_t)getpid() << 32) ^ (uint64_t)ts.tv_nsec ^ (uint64_t)ts.tv_sec * 1000003u;
    return t ? t : 1;
}

static const char *socket_path(void)
{
    const char *p = getenv(VKB_DEFAULT_SOCKET_ENV);
    if (p && *p) return p;
    static char buf[256];
    const char *rd = getenv("XDG_RUNTIME_DIR");
    snprintf(buf, sizeof(buf), "%s/vkbridge.sock", rd && *rd ? rd : "/tmp");
    return buf;
}

/* Which persistent pipeline cache the server should give this process, appended to the process
 * name in the hello ("name@key"; servers without the convention just see a longer name).
 *   steam<id>       a Steam game (SteamAppId, else SteamGameId, as Steam and Proton set them);
 *   steam<id>.run   Steam's shader pre-caching (fossilize_replay) replaying what this device
 *                   recorded of the game (an archive under shadercache/<id>/steamapprun_...);
 *   steam<id>.dl    the same for the collections Steam downloaded (shadercache/<id>/...).
 * The game merges the two pre-caching files into its own when it starts, so "Processing Vulkan
 * shaders" warms the cache the game loads (it used to fill a fossilize_repla cache nothing read).
 * VKBRIDGE_PCACHE_KEY=<name> overrides (letters, digits, '-', '_', '.'). Empty: none known. */
static int all_digits(const char *s, size_t n)
{
    if (!n || n > 20) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return !(n == 1 && s[0] == '0');
}

static void pcache_key(const char *comm, char *out, size_t cap)
{
    out[0] = 0;
    const char *k = getenv("VKBRIDGE_PCACHE_KEY");
    if (k && *k) {
        snprintf(out, cap, "%s", k);
        return;
    }
    char id[24] = "";
    int fossilize = !strncmp(comm, "fossilize", 9), run = 0;
    if (fossilize) {
        /* Steam runs it on the archives in steamapps/shadercache/<appid>/fozpipelinesv6/; the
         * replayer's own child processes get the same arguments. */
        enum { CMD_MAX = 65536 };
        char *cmd = malloc(CMD_MAX);
        int fd = cmd ? open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC) : -1;
        ssize_t n = 0;
        while (fd >= 0 && n < CMD_MAX - 1) {
            ssize_t r = read(fd, cmd + n, (size_t)(CMD_MAX - 1 - n));
            if (r <= 0) break;
            n += r;
        }
        if (fd >= 0) close(fd);
        if (cmd) cmd[n] = 0;
        for (ssize_t i = 0; n > 0 && i < n;) {
            const char *arg = cmd + i;
            size_t len = strnlen(arg, (size_t)(n - i));
            const char *sc = memmem(arg, len, "shadercache/", 12);
            if (sc) {
                const char *d = sc + 12;
                size_t dn = strspn(d, "0123456789");
                if (d[dn] == '/' && all_digits(d, dn) && !id[0]) snprintf(id, sizeof(id), "%.*s", (int)dn, d);
                if (strstr(arg, "/steamapprun_pipeline_cache")) run = 1;
            }
            i += (ssize_t)len + 1;
        }
        free(cmd);
    }
    if (!id[0]) {
        const char *names[2] = {"SteamAppId", "SteamGameId"};
        for (int i = 0; i < 2 && !id[0]; i++) {
            const char *v = getenv(names[i]);
            if (v && all_digits(v, strlen(v))) snprintf(id, sizeof(id), "%s", v);
        }
    }
    if (id[0]) snprintf(out, cap, "steam%s%s", id, fossilize ? (run ? ".run" : ".dl") : "");
}

static int do_connect(vkb_conn *c)
{
    const char *path = socket_path();
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) {
        VKB_ERR("socket path too long: %s", path);
        return -1;
    }
    strcpy(a.sun_path, path);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        VKB_ONCE(VKB_LOG_ERROR, "cannot reach the Vulkan bridge server at %s: %s", path, strerror(errno));
        close(s);
        return -1;
    }
    c->sock = s;
    /* Hello: protocol check and which process this thread belongs to. */
    /* Two threads making their first call at once must not each pick a token: the server would
     * take them for two processes, with separate objects and separate async orders. */
    uint64_t token = __atomic_load_n(&process_token, __ATOMIC_ACQUIRE);
    if (!token) {
        uint64_t mine = make_token();
        token = __atomic_compare_exchange_n(&process_token, &token, mine, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? mine : token;
    }
    vkb_enc e;
    vkb_enc_init(&e);
    vkb_enc_u64(&e, VKB_PROTOCOL_HASH);
    vkb_enc_u64(&e, token);
    vkb_enc_u32(&e, (uint32_t)getpid());
    char comm[64] = "?";
    FILE *f = fopen("/proc/self/comm", "re");
    if (f) {
        if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
        fclose(f);
    }
    char key[64], hello[160];
    pcache_key(comm, key, sizeof(key));
    snprintf(hello, sizeof(hello), "%s%s%s", comm, key[0] ? "@" : "", key);
    vkb_enc_str(&e, hello);
    vkb_msg_hdr h = {VKB_MAGIC, (uint32_t)e.len, VKB_CMD_vkbHello, 0, 0, 0, 0, 0};
    int ok = vkb_send_msg(s, &h, e.buf, NULL, 0) == 0;
    vkb_enc_free(&e);
    int fds[VKB_MAX_FDS], nfds = 0;
    vkb_msg_hdr rh;
    if (ok) ok = vkb_recv_msg(s, &rh, &c->in, &c->incap, fds, &nfds) == 0 && rh.cmd == VKB_ST_OK && rh.size >= 12;
    if (ok) {
        uint64_t sh;
        uint32_t accepted;
        memcpy(&sh, c->in, 8);
        memcpy(&accepted, c->in + 8, 4);
        if (!accepted) {
            VKB_ONCE(VKB_LOG_ERROR, "the bridge server speaks protocol %016llx, this driver %016llx; update the client ICD",
                     (unsigned long long)sh, (unsigned long long)VKB_PROTOCOL_HASH);
            ok = 0;
        }
    }
    if (!ok) {
        close(s);
        c->sock = -1;
        return -1;
    }
    return 0;
}

static vkb_conn *get_conn(void)
{
    pthread_once(&conn_once, conn_init_once);
    vkb_conn *c = tls_conn;
    if (c && c->epoch != fork_epoch) c = tls_conn = NULL;
    if (c) return c->dead ? NULL : c;
    c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->sock = -1;
    c->epoch = fork_epoch;
    vkb_enc_init(&c->enc);
    if (do_connect(c) < 0) {
        c->dead = 1;
    }
    tls_conn = c;
    pthread_setspecific(conn_key, c);
    return c->dead ? NULL : c;
}

int vkb_connected(void)
{
    return get_conn() != NULL;
}

/* Status polls (vkb_call_poll_next): sent with no barrier and without flushing the queue first.
 * synced_seq: the highest barrier a finished synchronous call carried (every async request up to
 * it has run); last_async_seq: the seq this thread's last async request went out under. */
static __thread int poll_next;
static __thread uint64_t last_async_seq;
static uint64_t synced_seq;

void vkb_call_poll_next(void) { poll_next = 1; }

/* VKB_F_FAST_CREATE / VKB_F_CREATE_REQS on this thread's next call (vkb_call_fast_next), and the requirements its vkCreateImage
 * reply carried */
static __thread uint32_t fast_next;
static __thread vkb_fast_reqs fast_got;
void vkb_call_fast_next(uint32_t flags) { fast_next = flags; }
void vkb_fast_reqs_take(vkb_fast_reqs *q)
{
    *q = fast_got;
    fast_got.have = 0;
}
uint64_t vkb_async_seq_last(void) { return last_async_seq; }
int vkb_async_ran(uint64_t seq) { return __atomic_load_n(&synced_seq, __ATOMIC_ACQUIRE) >= seq; }

void vkb_call_begin(vkb_call *c, uint32_t cmd, uint32_t table)
{
    /* Not a memset of the whole call plus a copy of the connection's encoder in and out (176 + 2 x 96 bytes per
     * request; ~5% of the client's own CPU in a DXVK frame on the host): the decoder is cleared, and of the encoder
     * only what lives between calls (the buffer and its capacity) moves; its fds[] is read only below nfds. */
    c->d = (vkb_dec){0};
    c->cmd = cmd;
    c->table = table;
    c->flags = 0;
    if (poll_next) c->flags |= VKB_CALL_POLL, poll_next = 0;
    if (fast_next) c->flags |= fast_next, fast_next = 0;
    c->conn = get_conn();
    if (c->conn && !c->conn->busy) {
        c->conn->busy = 1;
        c->e.buf = c->conn->enc.buf;
        c->e.cap = c->conn->enc.cap;
        c->e.len = 0;
        c->e.nfds = 0;
        c->e.oom = 0;
    } else {
        /* No connection, or a nested call: encode into a private buffer. */
        if (c->conn && c->conn->busy) VKB_ERR("internal: nested bridge call (%s)", vkb_cmd_names[cmd]);
        vkb_enc_init(&c->e);
        c->flags |= 0x80000000u;
    }
}

static void release_enc(vkb_call *c)
{
    if (c->flags & 0x80000000u) {
        vkb_enc_free(&c->e);
    } else if (c->conn) {
        c->conn->enc.buf = c->e.buf;
        c->conn->enc.cap = c->e.cap;
        c->conn->busy = 0;
        /* Don't keep a huge buffer around after one big message. */
        if (c->conn->enc.cap > (16u << 20)) {
            vkb_enc_free(&c->conn->enc);
            vkb_enc_init(&c->conn->enc);
        }
    }
}

static int trace_on(void)
{
    static int v = -1;
    if (v < 0) {
        v = getenv("VKBRIDGE_TRACE") && strcmp(getenv("VKBRIDGE_TRACE"), "0");
        /* VKBRIDGE_TRACE_COMM=name: only processes whose name starts so (the whole session shares
         * one environment; Steam's browser GPU process was the one to watch, not gamescope). */
        const char *want = getenv("VKBRIDGE_TRACE_COMM");
        if (v && want && *want) {
            char comm[32] = "";
            FILE *cf = fopen("/proc/self/comm", "re");
            if (cf) {
                if (!fgets(comm, sizeof(comm), cf)) comm[0] = 0;
                fclose(cf);
            }
            v = strncmp(comm, want, strlen(want)) == 0;
        }
    }
    return v;
}

static int knob(int shift, int *known);

/* The server's VKB_KNOB_SYNCFD_SIDE_SHIFT; VKBRIDGE_SYNCFD_SIDE in the guest wins (0 off, else on). */
static int syncfd_side(void)
{
    static int env = -2;
    if (env == -2) {
        const char *v = getenv("VKBRIDGE_SYNCFD_SIDE");
        env = v && *v ? strcmp(v, "0") != 0 : -1;
    }
    if (env >= 0) return env;
    int known;
    return knob(VKB_KNOB_SYNCFD_SIDE_SHIFT, &known) == 1;
}

/* VKBRIDGE_SLOW_MS=<ms>: a round trip that kept its thread longer than that is logged with the
 * command and the thread, for finding what a game waits on when its frames stall. */
static int slow_ms = -1;

static int exec(vkb_call *c);

/* VKBRIDGE_WAIT_STATS=<seconds>: each thread logs, that often, the time it spent in round trips
 * (waiting for the server's reply, ordering included) by command, under its own name (DXVK names
 * its threads), to tell which thread pays for the bridge's waits. On by default since 2026-11-04,
 * every WAIT_STATS_DEFAULT s (0 turns it off): a player's log then says it without a switch. A
 * thread's line is written only when its round trips took at least WAIT_STATS_MIN_PERMILLE of the
 * period (a thread that hardly waits says nothing), at most WAIT_STATS_LINES lines a process; the
 * lines are informational (tagged info at any log level). The round trip is timed once for this
 * and the per-hitch sums (hitch.c): two clock reads. */
#define WAIT_STATS_DEFAULT 10
#define WAIT_STATS_MIN_PERMILLE 10 /* 1 %: 100 ms in 10 s */
#define WAIT_STATS_LINES 2000
static int wait_period = -1, wait_explicit;
static uint32_t wait_lines;
static __thread struct {
    uint64_t ns[VKB_CMD_COUNT];
    uint32_t n[VKB_CMD_COUNT];
    uint64_t start;
} *tw;

static void wait_note(uint32_t cmd, uint64_t ns, uint64_t now)
{
    if (!tw && !(tw = calloc(1, sizeof(*tw)))) return;
    if (!tw->start) tw->start = now;
    tw->ns[cmd] += ns;
    tw->n[cmd]++;
    uint64_t period = (uint64_t)wait_period * 1000000000ull;
    if (now - tw->start < period) return;
    uint64_t total = 0, n = 0;
    for (uint32_t i = 0; i < VKB_CMD_COUNT; i++) total += tw->ns[i], n += tw->n[i];
    /* explicitly asked for (VKBRIDGE_WAIT_STATS set): every thread's line, as before */
    int quiet = !wait_explicit && total * 1000 < (now - tw->start) * WAIT_STATS_MIN_PERMILLE;
    uint32_t no = quiet ? 0 : __atomic_fetch_add(&wait_lines, 1, __ATOMIC_RELAXED);
    if (!quiet && no < WAIT_STATS_LINES) {
        char name[17] = "";
        prctl(PR_GET_NAME, name, 0, 0, 0);
        char line[512];
        int o = snprintf(line, sizeof(line), "waits over %.1fs, thread %s (%ld): %.1f ms in %llu round trips;", (now - tw->start) / 1e9,
                         name, (long)syscall(SYS_gettid), total / 1e6, (unsigned long long)n);
        for (int k = 0; k < 5; k++) {
            uint32_t best = 0;
            for (uint32_t i = 1; i < VKB_CMD_COUNT; i++)
                if (tw->ns[i] > tw->ns[best]) best = i;
            if (!tw->ns[best] || o >= (int)sizeof(line) - 64) break;
            o += snprintf(line + o, sizeof(line) - (size_t)o, " %s %.1f ms (%u)", vkb_cmd_names[best], tw->ns[best] / 1e6, tw->n[best]);
            tw->ns[best] = 0;
        }
        if (no + 1 == WAIT_STATS_LINES)
            snprintf(line + o, sizeof(line) - (size_t)o, " (the last of %u wait lines for this process)", WAIT_STATS_LINES);
        vkb_log_info_always("%s", line);
    }
    memset(tw, 0, sizeof(*tw));
    tw->start = now;
}

/* VKBRIDGE_ASYNC_TIMER: the time each thread spends queuing asynchronous requests (the NOREPLY path of exec(): the wait for the queue's
 * lock, the copy into the batch, a flush when the queue is full) - not covered by "waits over" (those are the round trips), so a
 * thread's idle time could not be told from time spent in the bridge's queue. Every call is counted; one in ASYNC_SAMPLE is timed (two
 * clock reads) and scaled (the plain append), while every wait for the lock and every flush this thread does is timed exactly (rare,
 * and what stalls a thread: a sampled call could miss them). Cost to a game thread: a counter, ~1 ns a call. VKBRIDGE_ASYNC_TIMER=1
 * times every call, 0 turns it off, any other number: sampled, and every thread says its line. A line a WAIT_STATS period for a
 * thread with >= ASYNC_MIN_CALLS calls whose queuing took >= 1 % of the period (no line: less), and one when the thread ends (a
 * window of a second or more):
 *   async sends over 10.0s, thread NAME (tid): N calls, queuing ~X ms (append ~A ms sampled, lock wait L ms in LN, flush F ms in FN, max_us=M)
 * X = A + L + F; max_us is the longest single lock wait or flush, i.e. a stall in the queue shows there. */
#define ASYNC_SAMPLE 64
#define ASYNC_MIN_CALLS 100
#define ASYNC_LINES 1000
static int async_explicit; /* VKBRIDGE_ASYNC_TIMER set (not 0): every thread with calls gets its line */
static int async_mask = -1; /* -1 off; else a call is timed when (count & mask) == 0 (63: one in 64) */
static uint32_t async_lines;
static void async_note_line(async_timer *ta, uint64_t now)
{
    if (!ta->n) return;
    /* what queuing cost: the plain appends (sampled, scaled to all calls) + every lock wait + every flush */
    double append_ms = ta->tn ? ta->tns / 1e6 * (double)ta->n / (double)ta->tn : 0;
    double est_ms = append_ms + (ta->lock_ns + ta->flush_ns) / 1e6;
    /* a thread says something only when queuing took >= 1 % of a window of at least a second (as the waits-over lines do), unless asked for all */
    uint64_t win = ta->start && now > ta->start ? now - ta->start : 0;
    int quiet = !async_explicit && (win < 1000000000ull || est_ms * 1e6 * 100 < (double)win);
    if (!quiet && ta->n >= ASYNC_MIN_CALLS && __atomic_fetch_add(&async_lines, 1, __ATOMIC_RELAXED) < ASYNC_LINES) {
        char name[17] = "";
        prctl(PR_GET_NAME, name, 0, 0, 0);
        vkb_log_info_always("async sends over %.1fs, thread %s (%ld): %llu calls, queuing ~%.1f ms (append ~%.1f ms sampled, lock wait %.1f ms in %llu, "
                            "flush %.1f ms in %llu, max_us=%llu)",
                            win / 1e9, name, (long)syscall(SYS_gettid), (unsigned long long)ta->n, est_ms, append_ms, ta->lock_ns / 1e6,
                            (unsigned long long)ta->lock_n, ta->flush_ns / 1e6, (unsigned long long)ta->flush_n, (unsigned long long)(ta->max_ns / 1000));
    }
    memset(ta, 0, sizeof(*ta));
    ta->start = now;
}

static void async_note_flush(vkb_conn *c)
{
    if (async_mask >= 0 && c) async_note_line(&c->at, now_ns());
}

/* Under aq_lock (the flusher's loop holds it here). */
static void aq_flusher_note(uint64_t t0, uint64_t t1, size_t bytes)
{
    if (async_mask < 0) return;
    if (!aq_fstat.start) aq_fstat.start = t0;
    uint64_t d = t1 - t0;
    aq_fstat.n++, aq_fstat.bytes += bytes, aq_fstat.ns += d;
    if (d > aq_fstat.max_ns) aq_fstat.max_ns = d;
    uint64_t win = t1 - aq_fstat.start;
    if (wait_period <= 0 || win < (uint64_t)wait_period * 1000000000ull) return;
    if ((async_explicit || aq_fstat.ns * 100 >= win) && __atomic_fetch_add(&async_lines, 1, __ATOMIC_RELAXED) < ASYNC_LINES)
        vkb_log_info_always("async flusher over %.1fs, thread vkb-flush (%ld): %llu writes, %.1f KB, writing %.1f ms (max_us=%llu), %s",
                            win / 1e9, (long)syscall(SYS_gettid), (unsigned long long)aq_fstat.n, aq_fstat.bytes / 1024.0, aq_fstat.ns / 1e6,
                            (unsigned long long)(aq_fstat.max_ns / 1000),
                            aq_locked_writes() ? "the queue's lock held while writing (VKBRIDGE_AQ_LOCKED_WRITES=1)" : "the queue's lock not held while writing");
    memset(&aq_fstat, 0, sizeof(aq_fstat));
    aq_fstat.start = t1;
}

/* an async request through exec(), timed when it is the thread's turn; wait_period (the WAIT_STATS period) paces the lines */
static int async_exec(vkb_call *c)
{
    async_timer *ta = &c->conn->at;
    if ((++ta->n & (uint64_t)async_mask) != 0) return exec(c);
    uint64_t t0 = now_ns(), stall0 = ta->lock_ns + ta->flush_ns;
    if (!ta->start) ta->start = t0;
    int r = exec(c);
    uint64_t t1 = now_ns(), d = t1 - t0, stall = ta->lock_ns + ta->flush_ns - stall0;
    ta->tn++, ta->tns += d > stall ? d - stall : 0;
    if (wait_period > 0 && t1 - ta->start >= (uint64_t)wait_period * 1000000000ull) async_note_line(ta, t1);
    return r;
}

int vkb_call_exec(vkb_call *c)
{
    if (slow_ms < 0) {
        const char *v = getenv("VKBRIDGE_SLOW_MS");
        slow_ms = v ? atoi(v) : 0;
        v = getenv("VKBRIDGE_WAIT_STATS");
        wait_explicit = v && *v;
        wait_period = wait_explicit ? atoi(v) : WAIT_STATS_DEFAULT;
        v = getenv("VKBRIDGE_ASYNC_TIMER");
        async_mask = v && *v ? (atoi(v) == 0 ? -1 : atoi(v) == 1 ? 0 : ASYNC_SAMPLE - 1) : ASYNC_SAMPLE - 1;
        async_explicit = v && *v && atoi(v) != 0;
    }
    int hitch = __atomic_load_n(&vkb_hitch_active, __ATOMIC_RELAXED);
    if (c->flags & VKB_F_NOREPLY) return async_mask >= 0 && c->conn ? async_exec(c) : exec(c);
    if (!slow_ms && wait_period <= 0 && !hitch) return exec(c);
    if (hitch && vkb_hitch_in_present) c->flags |= VKB_F_PRESENT_WAIT; /* the server times it for the present wait split */
    uint64_t t0 = now_ns();
    int r = exec(c);
    uint64_t t1 = now_ns(), ns = t1 - t0, us = ns / 1000;
    if (hitch && r) vkb_hitch_rt(c->cmd, ns); /* the per-hitch sums (hitch.c) */
    if (wait_period > 0) wait_note(c->cmd, ns, t1);
    if (slow_ms && us >= (uint64_t)slow_ms * 1000) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        vkb_log(VKB_LOG_ERROR, "slow %ld.%03ld: %s %llu us (thread %ld)", (long)ts.tv_sec, ts.tv_nsec / 1000000, vkb_cmd_names[c->cmd],
                (unsigned long long)us, (long)syscall(SYS_gettid));
    }
    return r;
}

/* VKBRIDGE_TEST_DROP_ASYNC=<command> (host tests only, asyncgap_test.sh): the first asynchronous request of that
 * command takes its sequence number and is never sent, as one a thread took with it when it ended would be: the
 * server's order wait then has a request that never arrives. */
static int test_drop(uint32_t cmd)
{
    static int want = -2;
    if (want == -2) {
        const char *n = getenv("VKBRIDGE_TEST_DROP_ASYNC");
        int w = -1;
        for (uint32_t i = 0; n && *n && i < VKB_CMD_COUNT; i++)
            if (vkb_cmd_names[i] && !strcmp(vkb_cmd_names[i], n)) w = (int)i;
        want = w;
    }
    if (want < 0 || cmd != (uint32_t)want) return 0;
    want = -1; /* once (under aq_lock) */
    return 1;
}

/* The queue's lock for an async request: a wait is timed (the try costs what the lock does), and a flush this thread does under
 * it (the queue is full, or the socket is) - the parts of queuing that stall a thread; the append itself is sampled (async_exec). */
static void aq_lock_timed(async_timer *at)
{
    if (!pthread_mutex_trylock(&aq_lock)) return;
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&aq_lock);
    uint64_t d = now_ns() - t0;
    at->lock_ns += d, at->lock_n++;
    if (d > at->max_ns) at->max_ns = d;
}

/* Called with aq_lock held, returns with it held: the full queue written by this thread (outside the lock unless
 * VKBRIDGE_AQ_LOCKED_WRITES=1), timed for the async-send line. */
static int aq_flush_timed(vkb_conn *conn, async_timer *at)
{
    uint64_t t0 = at ? now_ns() : 0;
    int r;
    if (aq_locked_writes()) {
        r = aq_flush_locked(conn);
    } else {
        aq_taken t;
        aq_take_locked(&t);
        pthread_mutex_unlock(&aq_lock);
        r = aq_write_taken(conn, &t);
        pthread_mutex_lock(&aq_lock);
        aq_give_back_locked(&t);
    }
    if (at) {
        uint64_t d = now_ns() - t0;
        at->flush_ns += d, at->flush_n++;
        if (d > at->max_ns) at->max_ns = d;
    }
    return r;
}

/* Under aq_lock, after an append: a queue at AQ_FLUSH goes to the flusher (due now); this thread writes it itself only at
 * AQ_HARD, or when there is no flusher, or with VKBRIDGE_AQ_LOCKED_WRITES=1 (then at AQ_FLUSH, as before). */
static int aq_full(vkb_conn *conn, async_timer *at)
{
    if (aq_len < AQ_FLUSH) return 0;
    if (!aq_locked_writes() && aq_len < AQ_HARD && aq_flusher_started && !aq_flusher_gone) {
        aq_first_ns = 0; /* due now */
        pthread_cond_signal(&aq_cond);
        return 0;
    }
    return aq_flush_timed(conn, at);
}

static int exec(vkb_call *c)
{
    vkb_conn *conn = c->conn;
    async_timer *at = (c->flags & VKB_F_NOREPLY) && async_mask >= 0 && conn ? &conn->at : NULL;
    if (trace_on()) {
        /* VkResult-returning calls put it first in the reply; peeked after the round trip. */
        vkb_log(VKB_LOG_ERROR, "trace: %s", vkb_cmd_names[c->cmd]);
    }
    if (!conn || conn->dead || (c->flags & 0x80000000u)) {
        release_enc(c);
        c->conn = NULL;
        return 0;
    }
    if (c->e.oom) {
        VKB_ERR("%s: request could not be encoded", vkb_cmd_names[c->cmd]);
        release_enc(c);
        c->conn = NULL;
        return 0;
    }
    vkb_msg_hdr h = {VKB_MAGIC, (uint32_t)c->e.len, c->cmd, c->table, (uint32_t)c->e.nfds,
                     c->flags & (VKB_F_NOREPLY | VKB_F_PRESENT_WAIT | VKB_F_FAST_CREATE | VKB_F_CREATE_REQS), 0, 0};
    vkb_stats_count(c->cmd, (c->flags & VKB_F_NOREPLY) != 0, c->e.len);
    if (at) aq_lock_timed(at);
    else pthread_mutex_lock(&aq_lock);
    if ((c->flags & VKB_F_NOREPLY) && test_drop(c->cmd)) {
        /* what is queued goes out first, so that the dropped one's number is a gap of its own */
        int r = aq_flush_locked(conn);
        last_async_seq = ++async_seq;
        pthread_mutex_unlock(&aq_lock);
        VKB_WARN("VKBRIDGE_TEST_DROP_ASYNC: %s (request %llu) not sent", vkb_cmd_names[c->cmd], (unsigned long long)last_async_seq);
        release_enc(c);
        c->conn = NULL;
        return r < 0 ? 0 : 1;
    }
    if ((c->flags & VKB_F_NOREPLY) && !c->e.nfds && vkb_async_framed()) {
        /* Appended to the open vkbBatch (or opening it) under the lock that orders the seqs. Requests
         * queued as messages of their own before the server's switches came in (the first
         * vkbQueryServer) go out first: appending to them would rewrite the first one's header. */
        if (aq_len && ((const vkb_msg_hdr *)aq_buf)->cmd != VKB_CMD_vkbBatch && aq_flush_locked(conn) < 0) {
            pthread_mutex_unlock(&aq_lock);
            VKB_ERR("lost the bridge server (%s)", strerror(errno));
            conn->dead = 1;
            release_enc(c);
            c->conn = NULL;
            return 0;
        }
        size_t pad = (8 - (c->e.len & 7)) & 7;
        size_t need = (aq_len ? aq_len : sizeof(h)) + 8 + c->e.len + pad;
        if (need > aq_cap && aq_grow(need) < 0) {
            pthread_mutex_unlock(&aq_lock);
            release_enc(c);
            c->conn = NULL;
            return 0;
        }
        int opened = !aq_len;
        if (opened) {
            vkb_msg_hdr bh = {VKB_MAGIC, 0, VKB_CMD_vkbBatch, 0, 0, VKB_F_NOREPLY, ++async_seq, 0};
            bh.barrier = bh.seq - 1;
            memcpy(aq_buf, &bh, sizeof(bh));
            aq_len = sizeof(bh);
            aq_first_ns = now_ns();
            stat_wire_add(1, sizeof(bh));
        }
        last_async_seq = ((vkb_msg_hdr *)aq_buf)->seq;
        uint32_t sub[2] = {c->cmd | c->table << 16, (uint32_t)c->e.len};
        memcpy(aq_buf + aq_len, sub, 8);
        memcpy(aq_buf + aq_len + 8, c->e.buf, c->e.len);
        memset(aq_buf + aq_len + 8 + c->e.len, 0, pad);
        aq_len = need;
        stat_wire_add(0, 8 + c->e.len + pad);
        ((vkb_msg_hdr *)aq_buf)->size = (uint32_t)(aq_len - sizeof(h));
        aq_start_flusher();
        if (opened) pthread_cond_signal(&aq_cond); /* the flusher waits only on an empty queue */
        int r = aq_full(conn, at);
        pthread_mutex_unlock(&aq_lock);
        release_enc(c);
        c->conn = NULL;
        if (r < 0) {
            VKB_ERR("lost the bridge server (%s)", strerror(errno));
            conn->dead = 1;
            return 0;
        }
        return 1;
    }
    if ((c->flags & VKB_F_NOREPLY) && !c->e.nfds) {
        /* Queued as a message of its own: header and payload appended. */
        h.seq = last_async_seq = ++async_seq;
        h.barrier = h.seq - 1;
        size_t need = aq_len + sizeof(h) + c->e.len;
        if (need > aq_cap && aq_grow(need) < 0) {
            pthread_mutex_unlock(&aq_lock);
            release_enc(c);
            c->conn = NULL;
            return 0;
        }
        if (!aq_len) aq_first_ns = now_ns();
        stat_wire_add(1, sizeof(h) + c->e.len);
        memcpy(aq_buf + aq_len, &h, sizeof(h));
        memcpy(aq_buf + aq_len + sizeof(h), c->e.buf, c->e.len);
        aq_len = need;
        aq_start_flusher();
        pthread_cond_signal(&aq_cond);
        int r = aq_full(conn, at);
        pthread_mutex_unlock(&aq_lock);
        release_enc(c);
        c->conn = NULL;
        if (r < 0) {
            VKB_ERR("lost the bridge server (%s)", strerror(errno));
            conn->dead = 1;
            return 0;
        }
        return 1;
    }
    /* Everything queued goes first, on this connection; a poll leaves it to the flusher and
     * overtakes it (its answer may be one the GPU had a little earlier). Ahead of a sync-fd export
     * with the server's order deadline on (Samsung's driver), on the flusher's instead: there a
     * submit the driver holds does not keep the export unread behind it (a player log), and the server's
     * deadline applies. (A round trip is timed around this, in vkb_call_exec.) */
    int qr = 0;
    aq_taken taken = {0};
    if (c->flags & VKB_CALL_POLL) {
        /* the poll overtakes the queue */
    } else if ((c->flags & VKB_F_FAST_CREATE) && aq_len && aq_flusher_started && !aq_flusher_gone) {
        /* a create the server may run ahead of the queue (vkb_fast_create): the queue still goes out now, as it would have, but
         * on the flusher's connection - this thread does not write it out under the queue's lock first */
        aq_first_ns = 0; /* due now */
        pthread_cond_signal(&aq_cond);
    } else if (aq_len && aq_flusher_started && !aq_flusher_gone &&
               (c->cmd == VKB_CMD_vkGetSemaphoreFdKHR || c->cmd == VKB_CMD_vkGetFenceFdKHR) && syncfd_side()) {
        aq_first_ns = 0; /* due now */
        pthread_cond_signal(&aq_cond);
    } else if (aq_locked_writes()) {
        qr = aq_flush_locked(conn);
    } else {
        aq_take_locked(&taken); /* written below, after the unlock, ahead of this request on this connection */
    }
    stat_wire_add(1, sizeof(h) + c->e.len);
    if (c->flags & VKB_F_NOREPLY) {
        h.seq = last_async_seq = ++async_seq;
        h.barrier = h.seq - 1;
    } else {
        h.barrier = c->flags & VKB_CALL_POLL ? 0 : async_seq;
    }
    pthread_mutex_unlock(&aq_lock);
    if (taken.len) {
        qr = aq_write_taken(conn, &taken);
        pthread_mutex_lock(&aq_lock);
        aq_give_back_locked(&taken);
        pthread_mutex_unlock(&aq_lock);
    }
    if (qr < 0 || vkb_send_msg(conn->sock, &h, c->e.buf, c->e.fds, c->e.nfds) < 0) {
        VKB_ERR("%s: lost the bridge server (%s)", vkb_cmd_names[c->cmd], strerror(errno));
        conn->dead = 1;
        release_enc(c);
        c->conn = NULL;
        return 0;
    }
    release_enc(c);
    if (c->flags & VKB_F_NOREPLY) {
        c->conn = NULL;
        return 1;
    }
    int fds[VKB_MAX_FDS], nfds = 0;
    vkb_msg_hdr rh;
    if (vkb_recv_msg(conn->sock, &rh, &conn->in, &conn->incap, fds, &nfds) < 0) {
        VKB_ERR("%s: lost the bridge server while waiting for the reply", vkb_cmd_names[c->cmd]);
        conn->dead = 1;
        c->conn = NULL;
        return 0;
    }
    /* (a fast create's barrier proves nothing: the server may have run it ahead of the queue) */
    for (uint64_t o = __atomic_load_n(&synced_seq, __ATOMIC_RELAXED);
         !(c->flags & VKB_F_FAST_CREATE) && o < h.barrier && !__atomic_compare_exchange_n(&synced_seq, &o, h.barrier, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);) {}
    if (rh.cmd != VKB_ST_OK) {
        static const char *const why[] = {"ok", "malformed request", "the driver lacks this command", "unknown object"};
        VKB_ERR("%s: the server refused the call (%s)", vkb_cmd_names[c->cmd], rh.cmd < 4 ? why[rh.cmd] : "?");
        for (int i = 0; i < nfds; i++) close(fds[i]);
        c->conn = NULL;
        return 0;
    }
    /* The decoder keeps fds in a per-call array. */
    static __thread int call_fds[VKB_MAX_FDS];
    memcpy(call_fds, fds, sizeof(int) * (size_t)nfds);
    vkb_dec_init(&c->d, conn->in, rh.size, &conn->arena, call_fds, nfds);
    return 1;
}

void vkb_call_end(vkb_call *c)
{
    if (!c->conn) return;
    if ((c->flags & VKB_F_CREATE_REQS) && c->cmd == VKB_CMD_vkCreateImage) {
        /* the reply's tail after the generated outputs (server_conn.c vkb_srv_fast_create_reply) */
        fast_got.have = 0;
        if (vkb_dec_u8(&c->d)) {
            vkb_dec_bytes(&c->d, &fast_got.req, sizeof(fast_got.req));
            fast_got.prefers = vkb_dec_u32(&c->d);
            fast_got.requires = vkb_dec_u32(&c->d);
            fast_got.have = !c->d.err;
        }
    }
    if (c->d.err) VKB_ERR("%s: malformed reply", vkb_cmd_names[c->cmd]);
    vkb_dec_close_untaken(&c->d);
    vkb_arena_reset(&c->conn->arena);
    c->conn = NULL;
}

/* VkResult commands treated as asynchronous (ASYNC_RESULT in the generator): on unless
 * VKBRIDGE_SYNC is set. */
int vkb_async_results(void)
{
    static int v = -1;
    if (v < 0) v = getenv("VKBRIDGE_SYNC") == NULL;
    return v;
}

/* The mode, from the guest's VKBRIDGE_MODE (DroidDeck passes Setup's "Safe mode" switch;
 * /root/.vkbridge-env can override it): safe (the default) or performance. The server reads its
 * own (server_mode.c); here it only picks the client's defaults by mode. */
int vkb_client_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *m = getenv("VKBRIDGE_MODE");
        v = m && (!strcmp(m, "performance") || !strcmp(m, "perf")) ? VKB_CLIENT_MODE_PERFORMANCE : VKB_CLIENT_MODE_SAFE;
        if (m && *m && strcmp(m, "safe") && v == VKB_CLIENT_MODE_SAFE) VKB_WARN("VKBRIDGE_MODE=%s is neither safe nor performance; safe", m);
        VKB_DBG("mode: %s", v == VKB_CLIENT_MODE_PERFORMANCE ? "performance" : "safe");
    }
    return v;
}

/* The client's switches the server sent (vkb_server_pd_info.client_knobs, at the first
 * vkbQueryServer): the game's preset file, <cache dir>/env, the server's environment, the game's
 * built-in preset. The guest's own environment beats them; an older server sends none. */
static uint32_t knobs_word;
static int knobs_known;

void vkb_client_knobs_set(uint32_t w)
{
    if (__atomic_exchange_n(&knobs_known, 1, __ATOMIC_ACQ_REL)) return;
    __atomic_store_n(&knobs_word, w, __ATOMIC_RELEASE);
    uint32_t db = VKB_KNOB_GET(w, VKB_KNOB_DESC_BATCH_SHIFT), qm = VKB_KNOB_GET(w, VKB_KNOB_QUERY_MIRROR_SHIFT);
    if (db || qm)
        VKB_INFO("switches from the server (the game's preset or <cache dir>/env):%s%s%s%s", db ? " descriptor batching " : "",
                 db ? (db == VKB_KNOB_ON ? "on" : "off") : "", qm ? " query mirror " : "", qm ? (qm == VKB_KNOB_ON ? "on" : "off") : "");
}

/* 1 on, 0 off, -1 nothing from the server (yet: *known 0) */
static int knob(int shift, int *known)
{
    *known = __atomic_load_n(&knobs_known, __ATOMIC_ACQUIRE);
    uint32_t k = *known ? VKB_KNOB_GET(__atomic_load_n(&knobs_word, __ATOMIC_ACQUIRE), shift) : 0;
    return k == VKB_KNOB_ON ? 1 : k == VKB_KNOB_OFF ? 0 : -1;
}

int vkb_client_knob(int shift, int *known)
{
    return knob(shift, known);
}

/* VKBRIDGE_FAST_CREATE: image and buffer creates sent with VKB_F_FAST_CREATE (the server runs them ahead of the process's queued
 * work where nothing queued can change them; server_conn.c). Only to a server that said it takes them (its knob ON: the game's
 * preset, <cache dir>/env or its environment, VKBRIDGE_FAST_CREATE=0 there turns it off); the guest's VKBRIDGE_FAST_CREATE=0 also
 * turns it off. Off: the requests and replies are the ones before it. */
int vkb_fast_create(void)
{
    static int v = -1;
    int c = __atomic_load_n(&v, __ATOMIC_RELAXED);
    if (c >= 0) return c;
    int known, k = knob(VKB_KNOB_FAST_CREATE_SHIFT, &known);
    if (!known) return 0; /* nothing from the server yet: as before, asked again next time */
    const char *e = getenv("VKBRIDGE_FAST_CREATE");
    int on = k == 1 && !(e && !strcmp(e, "0"));
    /* 3: BC images too (no BC rule that weighs bytes alive asked for on the server) */
    if (on && knob(VKB_KNOB_FAST_CREATE_BC_SHIFT, &known) == 1) on = 3;
    /* VKBRIDGE_TEST_FAST_CREATE_ALL=1 (host tests): every image and buffer create sent to run ahead, for the server's own check */
    const char *all = getenv("VKBRIDGE_TEST_FAST_CREATE_ALL");
    if (on && all && !strcmp(all, "1")) on = 7;
    __atomic_store_n(&v, on, __ATOMIC_RELAXED);
    VKB_INFO("fast create: %s%s", on ? "on (image and buffer creates may run ahead of queued work; requirements come with the image)"
                                     : k == 1 ? "off (the guest's VKBRIDGE_FAST_CREATE=0)" : k == 0 ? "off (the server's switch: off by default in this build, or VKBRIDGE_FAST_CREATE=0)"
                                                                                               : "off (the server does not take it)",
             on == 1 ? "; BC images as before (a BC rule weighing bytes alive is on)" : on == 7 ? "; VKBRIDGE_TEST_FAST_CREATE_ALL" : "");
    return on;
}

/* vkb_fast_create's on/off without its line (the client's 'perf-switches:' line): *from 0 the guest's VKBRIDGE_FAST_CREATE=0,
 * 1 the server's switch, 2 nothing from the server */
int vkb_fast_create_setting(int *from)
{
    int known, k = knob(VKB_KNOB_FAST_CREATE_SHIFT, &known);
    const char *e = getenv("VKBRIDGE_FAST_CREATE");
    if (k == 1 && e && !strcmp(e, "0")) return *from = 0, 0;
    *from = k >= 0 ? 1 : 2;
    return k == 1;
}

/* *final: 0 while the server's switches are not in yet (the answer may change once) */
static int desc_batch(int *final)
{
    static int v = -1;
    *final = 1;
    if (v >= 0) return v;
    /* On by default in both modes, for the framing only (vkb_desc_packed keeps the packing off
     * unless asked). Risk of Rain 2's black flickering (2026-10-14, Mali-G720, v0.2) and Elden
     * Ring's garbage textures (2026-10-28, S11 Ultra) followed the packing; the framing alone was
     * clean in Elden Ring, DS3, Balatro, Celeste, Resident Evil 2 and Bright Memory: Infinite
     * (2026-10-28, both tablets) and gave Elden Ring +27%, RE2 +6%, Bright Memory +5%.
     * VKBRIDGE_DESC_BATCH=1 turns it on, =0 off; VKBRIDGE_NO_DESC_BATCH=1 still wins. Neither in
     * the guest: the server's switch (a game's preset, <cache dir>/env), else the mode's. Until
     * the server's switches come in (the first vkbQueryServer) the mode's default is used; a later
     * "off" from the server only sends what follows as messages of their own. */
    static const int by_mode[2] = {[VKB_CLIENT_MODE_SAFE] = 1, [VKB_CLIENT_MODE_PERFORMANCE] = 1};
    const char *on = getenv("VKBRIDGE_DESC_BATCH"), *off = getenv("VKBRIDGE_NO_DESC_BATCH");
    int r, known = 1;
    if (on && *on) r = strcmp(on, "0") != 0;
    else if ((r = knob(VKB_KNOB_DESC_BATCH_SHIFT, &known)) < 0) r = by_mode[vkb_client_mode()];
    if (off && *off && strcmp(off, "0")) r = 0, known = 1;
    if (known) v = r;
    *final = known;
    return r;
}

int vkb_desc_batch(void)
{
    int final;
    return desc_batch(&final);
}

static int env_set(const char *name)
{
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0");
}

/* The two halves of descriptor batching. Framing (vkbBatch) is on with it;
 * VKBRIDGE_DESC_BATCH_NOFRAME=1 sends every asynchronous request as a message of its own.
 * Packing (vkbUpdateDescSets) is off unless VKBRIDGE_DESC_PACK=1 in the guest (it needs batching
 * on too); VKBRIDGE_DESC_BATCH_NOPACK=1 still turns it off. */
int vkb_async_framed(void)
{
    static int v = -1;
    if (v >= 0) return v;
    int final, r = desc_batch(&final) && !env_set("VKBRIDGE_DESC_BATCH_NOFRAME");
    if (final) v = r;
    return r;
}

int vkb_desc_packed(void)
{
    static int v = -1;
    if (v >= 0) return v;
    int final, r = desc_batch(&final) && env_set("VKBRIDGE_DESC_PACK") && !env_set("VKBRIDGE_DESC_BATCH_NOPACK");
    if (final) v = r;
    return r;
}

/* VKBRIDGE_QUERY_MIRROR in the guest, else the server's switch; on by default (query.c) */
int vkb_query_mirror_wanted(void)
{
    const char *e = getenv("VKBRIDGE_QUERY_MIRROR");
    if (e && *e) return strcmp(e, "0") != 0;
    int known, k = knob(VKB_KNOB_QUERY_MIRROR_SHIFT, &known);
    return k < 0 ? 1 : k;
}

int vkb_sync_mirror_wanted(void)
{
    const char *e = getenv("VKBRIDGE_SYNC_MIRROR");
    if (e && *e) return strcmp(e, "0") != 0;
    int known, k = knob(VKB_KNOB_SYNC_MIRROR_SHIFT, &known);
    return k < 0 ? 1 : k;
}

/* Asynchronous variant: no reply is awaited (void commands without outputs). */
int vkb_call_exec_async(vkb_call *c)
{
    if (!vkb_async_results()) return vkb_call_exec(c) ? (vkb_call_end(c), 1) : 0;
    c->flags |= VKB_F_NOREPLY;
    return vkb_call_exec(c);
}

/* VKBRIDGE_STATS=<seconds>: per-command counts of round trips and async sends, logged, and a
 * summary of the whole run at exit (with a long period, only that: tests/desc_traffic_check). */
static uint32_t stat_sync[VKB_CMD_COUNT], stat_async[VKB_CMD_COUNT];
static uint64_t stat_bytes, stat_cmd_bytes[VKB_CMD_COUNT];
static uint64_t stat_total_sync[VKB_CMD_COUNT], stat_total_async[VKB_CMD_COUNT], stat_total_bytes;
uint32_t vkb_stat_refused; /* descriptor.c */
static uint64_t stat_last, stat_total_refused, stat_total_full; /* descriptor counts, the whole run */

static void stats_exit(void)
{
    uint64_t s = 0, a = 0;
    for (uint32_t i = 0; i < VKB_CMD_COUNT; i++) {
        s += stat_total_sync[i];
        a += stat_total_async[i];
    }
    vkb_log(VKB_LOG_ERROR, "stats at exit: %llu round trips, %llu async, %llu KB encoded; %llu messages, %llu KB on the wire",
            (unsigned long long)s, (unsigned long long)a, (unsigned long long)(stat_total_bytes >> 10),
            (unsigned long long)stat_msgs_total, (unsigned long long)(stat_wire_total >> 10));
    uint64_t refused = stat_total_refused + __atomic_exchange_n(&vkb_stat_refused, 0, __ATOMIC_RELAXED);
    uint64_t full = stat_total_full + __atomic_exchange_n(&vkb_stat_pool_full, 0, __ATOMIC_RELAXED);
    if (refused || full)
        vkb_log(VKB_LOG_ERROR, "  descriptor set allocations refused: %llu; descriptor pools handed back full: %llu", (unsigned long long)refused,
                (unsigned long long)full);
    for (uint32_t i = 0; i < VKB_CMD_COUNT; i++)
        if (stat_total_sync[i] + stat_total_async[i] >= 100)
            vkb_log(VKB_LOG_ERROR, "  %-40s %8llu sync %8llu async", vkb_cmd_names[i], (unsigned long long)stat_total_sync[i],
                    (unsigned long long)stat_total_async[i]);
}

void vkb_stats_count(uint32_t cmd, int async, size_t bytes)
{
    if (stat_period < 0) {
        const char *v = getenv("VKBRIDGE_STATS");
        int p = v ? atoi(v) : 0;
        if (p > 0) atexit(stats_exit);
        stat_period = p > 0 ? p : 0;
    }
    int period = stat_period;
    if (!period) return;
    __atomic_add_fetch(async ? &stat_async[cmd] : &stat_sync[cmd], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(async ? &stat_total_async[cmd] : &stat_total_sync[cmd], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stat_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stat_total_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stat_cmd_bytes[cmd], bytes, __ATOMIC_RELAXED);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec;
    uint64_t last = __atomic_load_n(&stat_last, __ATOMIC_RELAXED);
    if (!last) {
        __atomic_store_n(&stat_last, now, __ATOMIC_RELAXED);
        return;
    }
    if (now - last < (uint64_t)period || !__atomic_compare_exchange_n(&stat_last, &last, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    uint32_t tot_s = 0, tot_a = 0;
    for (uint32_t i = 0; i < VKB_CMD_COUNT; i++) {
        tot_s += stat_sync[i];
        tot_a += stat_async[i];
    }
    vkb_log(VKB_LOG_ERROR, "stats over %us: %u round trips, %u async, %llu KB encoded; %llu messages, %llu KB on the wire", period,
            tot_s, tot_a, (unsigned long long)(stat_bytes >> 10),
            (unsigned long long)__atomic_exchange_n(&stat_msgs, 0, __ATOMIC_RELAXED),
            (unsigned long long)(__atomic_exchange_n(&stat_wire, 0, __ATOMIC_RELAXED) >> 10));
    for (int k = 0; k < 8; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < VKB_CMD_COUNT; i++)
            if (stat_sync[i] + stat_async[i] > best) {
                best = stat_sync[i] + stat_async[i];
                bi = i;
            }
        if (!best) break;
        vkb_log(VKB_LOG_ERROR, "  %-40s %6u sync %6u async %7llu KB", vkb_cmd_names[bi], stat_sync[bi], stat_async[bi],
                (unsigned long long)(stat_cmd_bytes[bi] >> 10));
        stat_sync[bi] = stat_async[bi] = 0;
    }
    /* Round trips are what cost: the rest of them too, from one a second. */
    for (uint32_t i = 0; i < VKB_CMD_COUNT; i++)
        if (stat_sync[i] >= (uint32_t)period)
            vkb_log(VKB_LOG_ERROR, "  %-40s %6u sync %6u async", vkb_cmd_names[i], stat_sync[i], stat_async[i]);
    uint32_t refused = __atomic_exchange_n(&vkb_stat_refused, 0, __ATOMIC_RELAXED);
    if (refused) vkb_log(VKB_LOG_ERROR, "  descriptor set allocations refused: %u", refused);
    uint32_t full = __atomic_exchange_n(&vkb_stat_pool_full, 0, __ATOMIC_RELAXED);
    if (full) vkb_log(VKB_LOG_ERROR, "  descriptor pools handed back full: %u", full);
    stat_total_refused += refused, stat_total_full += full;
    memset(stat_sync, 0, sizeof(stat_sync));
    memset(stat_async, 0, sizeof(stat_async));
    memset(stat_cmd_bytes, 0, sizeof(stat_cmd_bytes));
    stat_bytes = 0;
}

uint32_t vkb_table_of(const void *h)
{
    return h ? ((const vkb_object *)h)->table : 0;
}

uint64_t vkb_remote(const void *h)
{
    return h ? ((const vkb_object *)h)->remote : 0;
}

int vkb_memfd(const char *name, size_t size)
{
    int fd = (int)syscall(SYS_memfd_create, name, 1u /* MFD_CLOEXEC */);
    if (fd < 0) {
        /* No memfd (old kernel or a filter): an unlinked file in the runtime dir. */
        char path[256];
        const char *rd = getenv("XDG_RUNTIME_DIR");
        snprintf(path, sizeof(path), "%s/vkbridge-XXXXXX", rd && *rd ? rd : "/tmp");
        fd = mkostemp(path, O_CLOEXEC);
        if (fd < 0) return -1;
        unlink(path);
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ hash map */

void vkb_map_init(vkb_map *m)
{
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->lock, NULL);
}

void vkb_map_destroy(vkb_map *m)
{
    free(m->keys);
    free(m->vals);
    pthread_mutex_destroy(&m->lock);
    memset(m, 0, sizeof(*m));
}

static uint32_t hash64(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return (uint32_t)k;
}

/* Keys are never 0 (VK_NULL_HANDLE); 0 marks an empty slot, ~0 a deleted one. */
#define TOMB (~(uint64_t)0)

static void map_rehash(vkb_map *m, uint32_t ncap)
{
    uint64_t *ok = m->keys;
    void **ov = m->vals;
    uint32_t oc = m->cap;
    m->keys = calloc(ncap, sizeof(uint64_t));
    m->vals = calloc(ncap, sizeof(void *));
    m->cap = ncap;
    m->count = 0;
    for (uint32_t i = 0; i < oc; i++) {
        if (ok[i] && ok[i] != TOMB) {
            uint32_t j = hash64(ok[i]) & (ncap - 1);
            while (m->keys[j]) j = (j + 1) & (ncap - 1);
            m->keys[j] = ok[i];
            m->vals[j] = ov[i];
            m->count++;
        }
    }
    free(ok);
    free(ov);
}

void vkb_map_put(vkb_map *m, uint64_t k, void *v)
{
    if (!k) return;
    pthread_mutex_lock(&m->lock);
    if (!m->cap || (m->count + 1) * 2 > m->cap) map_rehash(m, m->cap ? m->cap * 2 : 64);
    uint32_t j = hash64(k) & (m->cap - 1);
    int64_t tomb = -1;
    while (m->keys[j]) {
        if (m->keys[j] == k) {
            m->vals[j] = v;
            pthread_mutex_unlock(&m->lock);
            return;
        }
        if (m->keys[j] == TOMB && tomb < 0) tomb = j;
        j = (j + 1) & (m->cap - 1);
    }
    if (tomb >= 0) j = (uint32_t)tomb;
    m->keys[j] = k;
    m->vals[j] = v;
    m->count++;
    pthread_mutex_unlock(&m->lock);
}

static int64_t map_find(vkb_map *m, uint64_t k)
{
    if (!m->cap || !k) return -1;
    uint32_t j = hash64(k) & (m->cap - 1);
    for (uint32_t n = 0; n < m->cap && m->keys[j]; n++) {
        if (m->keys[j] == k) return j;
        j = (j + 1) & (m->cap - 1);
    }
    return -1;
}

void *vkb_map_get(vkb_map *m, uint64_t k)
{
    pthread_mutex_lock(&m->lock);
    int64_t j = map_find(m, k);
    void *v = j >= 0 ? m->vals[j] : NULL;
    pthread_mutex_unlock(&m->lock);
    return v;
}

void *vkb_map_remove(vkb_map *m, uint64_t k)
{
    pthread_mutex_lock(&m->lock);
    int64_t j = map_find(m, k);
    void *v = NULL;
    if (j >= 0) {
        v = m->vals[j];
        m->keys[j] = TOMB;
        m->vals[j] = NULL;
        /* count keeps tombstones, so the table rehashes before it fills with them */
    }
    pthread_mutex_unlock(&m->lock);
    return v;
}
