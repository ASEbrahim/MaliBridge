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

void vkb_log(int level, const char *fmt, ...)
{
    if (level > vkb_log_level()) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
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

/* ------------------------------------------------------------------ connection */

typedef struct vkb_conn {
    int sock;
    int dead;
    uint32_t epoch;
    vkb_enc enc;            /* reused request buffer */
    uint8_t *in;
    size_t incap;
    vkb_arena arena;
    int busy;
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

static void conn_thread_exit(void *p)
{
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
 * Off unless VKBRIDGE_DESC_BATCH=1 (vkb_desc_batch): otherwise one message per request, as before. */
static pthread_mutex_t aq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *aq_buf;
static size_t aq_len, aq_cap;
#define AQ_FLUSH (256u << 10)

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

/* The flusher: asynchronous requests do not wait for the next synchronous one forever - another
 * thread may be blocked in the server on work they would start (a submit that signals what it is
 * waiting for). It sends what is queued ~100 us after it was queued, batching what follows. */
static pthread_cond_t aq_cond = PTHREAD_COND_INITIALIZER;
static int aq_flusher_started;
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
    vkb_conn *conn = get_conn();
    if (!conn) return NULL;
    pthread_mutex_lock(&aq_lock);
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
        if (aq_flush_locked(conn) < 0) {
            conn->dead = 1;
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
    pthread_mutex_unlock(&aq_lock);
}

static void atfork_child(void)
{
    aq_len = 0;
    aq_flusher_started = 0;
    pthread_cond_init(&aq_cond, NULL);
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

void vkb_call_begin(vkb_call *c, uint32_t cmd, uint32_t table)
{
    memset(c, 0, sizeof(*c));
    c->cmd = cmd;
    c->table = table;
    c->conn = get_conn();
    if (c->conn && !c->conn->busy) {
        c->conn->busy = 1;
        c->e = c->conn->enc;
        vkb_enc_reset(&c->e);
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
        c->conn->enc = c->e;
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

int vkb_call_exec(vkb_call *c)
{
    vkb_conn *conn = c->conn;
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
    vkb_msg_hdr h = {VKB_MAGIC, (uint32_t)c->e.len, c->cmd, c->table, (uint32_t)c->e.nfds, c->flags & VKB_F_NOREPLY, 0, 0};
    vkb_stats_count(c->cmd, (c->flags & VKB_F_NOREPLY) != 0, c->e.len);
    pthread_mutex_lock(&aq_lock);
    if ((c->flags & VKB_F_NOREPLY) && !c->e.nfds && vkb_async_framed()) {
        /* Appended to the open vkbBatch (or opening it) under the lock that orders the seqs. */
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
        uint32_t sub[2] = {c->cmd | c->table << 16, (uint32_t)c->e.len};
        memcpy(aq_buf + aq_len, sub, 8);
        memcpy(aq_buf + aq_len + 8, c->e.buf, c->e.len);
        memset(aq_buf + aq_len + 8 + c->e.len, 0, pad);
        aq_len = need;
        stat_wire_add(0, 8 + c->e.len + pad);
        ((vkb_msg_hdr *)aq_buf)->size = (uint32_t)(aq_len - sizeof(h));
        aq_start_flusher();
        if (opened) pthread_cond_signal(&aq_cond); /* the flusher waits only on an empty queue */
        int r = aq_len >= AQ_FLUSH ? aq_flush_locked(conn) : 0;
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
        h.seq = ++async_seq;
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
        int r = aq_len >= AQ_FLUSH ? aq_flush_locked(conn) : 0;
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
    /* Everything queued goes first, on this connection. */
    int qr = aq_flush_locked(conn);
    stat_wire_add(1, sizeof(h) + c->e.len);
    if (c->flags & VKB_F_NOREPLY) {
        h.seq = ++async_seq;
        h.barrier = h.seq - 1;
    } else {
        h.barrier = async_seq;
    }
    pthread_mutex_unlock(&aq_lock);
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

int vkb_desc_batch(void)
{
    static int v = -1;
    if (v < 0) {
        /* Off by default in both modes: Risk of Rain 2 showed black flickering with it on
         * (2026-10-14, Mali-G720, v0.2) and no game measured faster. Performance mode turns it on
         * here once that is fixed. VKBRIDGE_DESC_BATCH=1 turns it on, =0 off; VKBRIDGE_NO_DESC_BATCH=1
         * still wins. */
        static const int by_mode[2] = {[VKB_CLIENT_MODE_SAFE] = 0, [VKB_CLIENT_MODE_PERFORMANCE] = 0};
        const char *on = getenv("VKBRIDGE_DESC_BATCH"), *off = getenv("VKBRIDGE_NO_DESC_BATCH");
        v = on && *on ? strcmp(on, "0") != 0 : by_mode[vkb_client_mode()];
        if (off && *off && strcmp(off, "0")) v = 0;
    }
    return v;
}

static int env_set(const char *name)
{
    const char *e = getenv(name);
    return e && *e && strcmp(e, "0");
}

/* The two halves of VKBRIDGE_DESC_BATCH=1, to bisect the Risk of Rain 2 flicker on the device:
 * VKBRIDGE_DESC_BATCH_NOFRAME=1 keeps the packed vkUpdateDescriptorSets but sends every
 * asynchronous request as a message of its own; VKBRIDGE_DESC_BATCH_NOPACK=1 keeps the vkbBatch
 * framing but sends vkUpdateDescriptorSets the generic way. */
int vkb_async_framed(void)
{
    static int v = -1;
    if (v < 0) v = vkb_desc_batch() && !env_set("VKBRIDGE_DESC_BATCH_NOFRAME");
    return v;
}

int vkb_desc_packed(void)
{
    static int v = -1;
    if (v < 0) v = vkb_desc_batch() && !env_set("VKBRIDGE_DESC_BATCH_NOPACK");
    return v;
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
static uint64_t stat_last;

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
