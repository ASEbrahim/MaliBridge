/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * vkbridge server entry point.
 *
 *   libvkbridge_server.so --socket <path> [--log <file>] [--vulkan <libvulkan>] [--cache-dir <dir>] [--selftest]
 *
 * Packaged as a "library" so Android extracts it into the native library directory, from where
 * the app executes it like proot. It loads the system Vulkan loader (on the device: the vendor's
 * Mali driver behind /system/lib64/libvulkan.so), runs a self-test that decides how mapped memory
 * is shared with the Linux side, then serves the session's Vulkan calls until it is killed or its
 * parent dies.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#elif defined(__GLIBC__)
#include <execinfo.h>
#endif

PFN_vkGetInstanceProcAddr vkb_gipa;
vkb_dispatch vkb_global_dt;
int vkb_verbose;

static FILE *log_file;
static int log_level = VKB_LOG_INFO;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

int vkb_log_level(void) { return log_level; }

void vkb_log(int level, const char *fmt, ...)
{
    if (level > log_level) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    static const char *const tags[] = {"E", "W", "I", "D"};
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    pthread_mutex_lock(&log_lock);
    FILE *f = log_file ? log_file : stderr;
    fprintf(f, "%02d:%02d:%02d.%03ld vkbridge-server %s %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000, tags[level & 3], msg);
    fflush(f);
    pthread_mutex_unlock(&log_lock);
    vkb_report_log(level, fmt, msg); /* errors and warnings by call site, for the session report */
#ifdef __ANDROID__
    static const int prio[] = {ANDROID_LOG_ERROR, ANDROID_LOG_WARN, ANDROID_LOG_INFO, ANDROID_LOG_DEBUG};
    __android_log_write(prio[level & 3], "vkbridge", msg);
#endif
}

extern __thread const char *vkb_srv_current_cmd;

/* A crash is almost always the driver's: say in which call, then die as we would have. On Android
 * the fault then goes to the handler that was there before (debuggerd), whose tombstone in logcat
 * carries the backtrace; our own exit would lose it (tablet run 7: a crash in
 * vkCreateGraphicsPipelines with nothing to go on). */
static struct sigaction crash_prev[32];

static void on_crash(int sig, siginfo_t *si, void *uc)
{
    /* Inside a guarded shader/pipeline compile (server_guard.c): that call fails instead. */
    if (sig != SIGABRT) vkb_guard_fault(sig, si ? si->si_addr : NULL);
    vkb_probe_mark_end(sig, 1); /* a startup probe in flight died of it (server_probe.c) */
    char buf[1024];
    const char *cmd = vkb_srv_current_cmd ? vkb_srv_current_cmd : "(no Vulkan call)";
    /* Where: the faulting pc as library + offset (logcat may not keep debuggerd's tombstone), and
     * inside a command buffer replay its position and the commands before it (after The412Banner's
     * crash detail on the DroidDeck test branch). dladdr is not async-signal-safe; the process is
     * going down anyway. */
    uintptr_t pc = 0;
#if defined(__aarch64__)
    if (uc) pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc;
#elif defined(__x86_64__) && defined(REG_RIP)
    if (uc) pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#endif
    Dl_info di;
    memset(&di, 0, sizeof(di));
    int found = pc && dladdr((void *)pc, &di) && di.dli_fname;
    const char *lib = found ? strrchr(di.dli_fname, '/') : NULL;
    int n = snprintf(buf, sizeof(buf), "vkbridge-server E crashed with signal %d (code %d, address %p) in %s, pc %p (%s+0x%lx)\n", sig,
                     si ? si->si_code : 0, si ? si->si_addr : NULL, cmd, (void *)pc,
                     found ? (lib ? lib + 1 : di.dli_fname) : "?", found ? (unsigned long)(pc - (uintptr_t)di.dli_fbase) : 0ul);
    if (vkb_srv_replay_cb && n > 0 && n < (int)sizeof(buf)) {
        uint32_t done = vkb_srv_replay_n, first = done > VKB_REPLAY_RING ? done - VKB_REPLAY_RING : 0;
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "vkbridge-server E   replaying command buffer %p, command %u of its stream; the last ones:",
                      (void *)vkb_srv_replay_cb, done);
        for (uint32_t i = first; i < done && n > 0 && n < (int)sizeof(buf) - 2; i++) {
            const char *name = vkb_srv_replay_ring[i % VKB_REPLAY_RING];
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s", name ? name : "?");
        }
        if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
        buf[n++] = '\n';
        buf[n] = 0;
    }
    if (vkb_srv_crash_detail[0] && n > 0 && n < (int)sizeof(buf))
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "vkbridge-server E   %s\n", vkb_srv_crash_detail);
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    if (log_file) {
        if (write(fileno(log_file), buf, (size_t)n) < 0) {}
    }
    if (write(2, buf, (size_t)n) < 0) {}
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_FATAL, "vkbridge", buf);
    if (sig > 0 && sig < 32) {
        const struct sigaction *p = &crash_prev[sig];
        int chained = (p->sa_flags & SA_SIGINFO) ? p->sa_sigaction != NULL : (p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN);
        if (chained && sigaction(sig, p, NULL) == 0) return; /* the fault repeats under the previous handler */
    }
#elif defined(__GLIBC__)
    void *frames[32];
    int nf = backtrace(frames, 32);
    backtrace_symbols_fd(frames, nf, 2);
    if (log_file) backtrace_symbols_fd(frames, nf, fileno(log_file));
#endif
    signal(sig, SIG_DFL);
    raise(sig);
    (void)uc;
}

/* In the app's process, ART's libsigchain interposes sigaction: this becomes the "user" handler,
 * run only after ART's own fault handler has declined the fault (not managed code), and what
 * crash_prev gets is the handler before it (debuggerd's), which on_crash hands unarmed faults
 * back to. sigchain calls the user handler outside its own bookkeeping, so jumping out of it
 * leaves no sigchain state behind; bionic gives every thread an alternate signal stack. */
static void catch_crash(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sig > 0 && sig < 32) sigaction(sig, &sa, &crash_prev[sig]);
}

static void *load_vulkan(const char *override)
{
    const char *names[] = {
        override,
#ifdef __ANDROID__
        "libvulkan.so",
#else
        "libvulkan.so.1",
        "libvulkan.so",
#endif
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!names[i]) continue;
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) {
            VKB_INFO("Vulkan loader: %s", names[i]);
            return h;
        }
        VKB_WARN("cannot open %s: %s", names[i], dlerror());
    }
    return NULL;
}

static int listen_on(const char *path)
{
    struct sockaddr_un a;
    if (strlen(path) >= sizeof(a.sun_path)) {
        VKB_ERR("socket path too long (%zu bytes): %s", strlen(path), path);
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        VKB_ERR("socket: %s", strerror(errno));
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        VKB_ERR("bind %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    if (listen(fd, 128) < 0) {
        VKB_ERR("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Opens (or switches to) the log file; NULL keeps logging to stderr/logcat only. */
static void open_log(const char *path)
{
    FILE *f = path ? fopen(path, "ae") : NULL;
    if (path && !f) fprintf(stderr, "vkbridge-server: cannot open log %s: %s\n", path, strerror(errno));
    pthread_mutex_lock(&log_lock);
    FILE *old = log_file;
    log_file = f;
    pthread_mutex_unlock(&log_lock);
    if (old) fclose(old);
    vkb_report_session(path); /* the report goes beside the log */
}

/* Loads the Vulkan loader and runs the self-test; 0 when there is nothing to serve. */
static int server_init(const char *vk_lib, const char *cache_dir)
{
    VKB_INFO("starting (pid %d, protocol %016llx)", getpid(), (unsigned long long)VKB_PROTOCOL_HASH);
    void *lib = load_vulkan(vk_lib);
    if (!lib) {
        VKB_ERR("no Vulkan loader; nothing to serve");
        return 0;
    }
    vkb_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!vkb_gipa) {
        VKB_ERR("the Vulkan loader exports no vkGetInstanceProcAddr");
        return 0;
    }
    vkb_dispatch_load_instance(&vkb_global_dt, vkb_gipa, VK_NULL_HANDLE);
    vkb_pcache_set_dir(cache_dir);
    vkb_srv_env_reload(); /* <cache dir>/env: VKBRIDGE_FAKE_MISSING and the like reach the self-test */
    vkb_gipa = vkb_mem_imitate_gipa(vkb_gipa); /* host tests: another Mali's memory table (server_memory.c) */
    vkb_gipa = vkb_emu_imitate_gipa(vkb_gipa); /* host tests: the G715 r54p3's robustness2 (server_emu.c) */
    vkb_mode_refresh();   /* logged with its source; the probe decides for both modes */
    vkb_selftest_all();
    return 1;
}

#ifdef VKB_JNI
/*
 * In the app's own process (the default on the device): Android chooses the GPU driver, Samsung's
 * updatable one included, while the app starts up, and only for the app's process - a separate
 * executable gets the plain /vendor driver, which on the Tab S10+ lacked VK_EXT_robustness2 and
 * more that the app itself sees. Winlator's Vortek runs its server in-process for the same reason.
 * Started once and kept for the app's life; each session only points the log at its own folder.
 */
#include <jni.h>

static pthread_mutex_t jni_lock = PTHREAD_MUTEX_INITIALIZER;
static char jni_socket[256];
static int jni_state; /* 0 not started, 1 serving, -1 failed */

static void *serve_thread(void *arg)
{
    vkb_srv_serve((int)(intptr_t)arg);
    VKB_ERR("in-app server stopped serving");
    return NULL;
}

static char *jstr(JNIEnv *env, jstring s)
{
    if (!s) return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    char *r = c ? strdup(c) : NULL;
    if (c) (*env)->ReleaseStringUTFChars(env, s, c);
    return r;
}

JNIEXPORT jint JNICALL Java_com_droiddeck_launcher_gpu_VkBridgeNative_nativeStart(JNIEnv *env, jclass cls, jstring jsocket,
                                                                                jstring jlog, jstring jcache)
{
    (void)cls;
    char *sock = jstr(env, jsocket), *logp = jstr(env, jlog), *cache = jstr(env, jcache);
    /* Opt-in (a file "inapp" beside the socket): a driver crash in-process takes the app down
     * (tablet run 5), and the separate server got the same driver. */
    if (jni_state == 0 && sock) {
        char flag[300];
        snprintf(flag, sizeof(flag), "%s", sock);
        char *slash = strrchr(flag, '/');
        if (slash) snprintf(slash + 1, sizeof(flag) - (size_t)(slash + 1 - flag), "inapp");
        if (access(flag, F_OK) != 0) {
            free(sock);
            free(logp);
            free(cache);
            return 0;
        }
    }
    pthread_mutex_lock(&jni_lock);
    open_log(logp);
    if (getenv("VKBRIDGE_DEBUG")) {
        vkb_verbose = 1;
        log_level = VKB_LOG_DEBUG;
    }
    int ok = 0;
    if (jni_state == 1 && sock && !strcmp(sock, jni_socket) && access(sock, F_OK) == 0) {
        VKB_INFO("in-app server already serving at %s (pid %d); this session's log continues here", sock, getpid());
        vkb_srv_env_reload();
        int mode = vkb_pds_select_mode(); /* Setup's switch may have changed: <cache dir>/mode */
        VKB_INFO("mode: %s (%s) for this session", vkb_mode_name(mode), vkb_mode_source_name());
        vkb_log_summary();
        ok = 1;
    } else if (jni_state == 1) {
        VKB_ERR("in-app server serves %s, asked for %s (or its socket is gone)", jni_socket, sock ? sock : "(none)");
    } else if (jni_state == 0 && sock) {
        jni_state = -1;
        /* Before the self-test: the startup probe's faults are contained too (server_probe.c).
         * SIGABRT is left alone: an abort is not a compiler fault worth unwinding, and debuggerd's
         * tombstone of it is what helps. */
        catch_crash(SIGSEGV);
        catch_crash(SIGBUS);
        catch_crash(SIGILL);
        catch_crash(SIGFPE);
        if (server_init(NULL, cache)) {
            int lfd = listen_on(sock);
            pthread_t t;
            if (lfd >= 0 && pthread_create(&t, NULL, serve_thread, (void *)(intptr_t)lfd) == 0) {
                pthread_detach(t);
                snprintf(jni_socket, sizeof(jni_socket), "%s", sock);
                jni_state = 1;
                ok = 1;
                VKB_INFO("listening on %s (in the app's process)", sock);
            }
        }
    }
    pthread_mutex_unlock(&jni_lock);
    free(sock);
    free(logp);
    free(cache);
    return ok;
}
#else
static void usage(void)
{
    fprintf(stderr, "usage: vkbridge-server --socket PATH [--log FILE] [--vulkan LIB] [--cache-dir DIR] [--selftest] [--verbose]\n");
}

static pid_t parent_pid;

static void *parent_watch(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        if (getppid() != parent_pid) {
            VKB_INFO("the app (pid %d) is gone; exiting", (int)parent_pid);
            vkb_probe_mark_end(0, 0);
            _exit(0);
        }
    }
    return NULL;
}

/* A stop (the session's end, a shell's ^C): a startup probe in flight is run again at the next
 * start rather than taken for a crash (server_probe.c). */
static void on_stop(int sig)
{
    vkb_probe_mark_end(sig, 0);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void start_parent_watch(void)
{
    parent_pid = getppid();
    if (parent_pid <= 1) return; /* started by init or a shell that already left: nothing to watch */
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, parent_watch, NULL);
    pthread_attr_destroy(&a);
}

int main(int argc, char **argv)
{
    const char *sock_path = getenv("VKBRIDGE_SOCKET");
    const char *log_path = getenv("VKBRIDGE_SERVER_LOG");
    const char *vk_lib = getenv("VKBRIDGE_SERVER_VULKAN");
    const char *cache_dir = getenv("VKBRIDGE_CACHE_DIR");
    int selftest_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock_path = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--vulkan") && i + 1 < argc) vk_lib = argv[++i];
        else if (!strcmp(argv[i], "--cache-dir") && i + 1 < argc) cache_dir = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest_only = 1;
        else if (!strcmp(argv[i], "--verbose")) vkb_verbose = 1;
        else {
            usage();
            return 2;
        }
    }
    if (getenv("VKBRIDGE_DEBUG")) vkb_verbose = 1;
    if (vkb_verbose) log_level = VKB_LOG_DEBUG;
    open_log(log_path);
    signal(SIGPIPE, SIG_IGN);
    catch_crash(SIGSEGV);
    catch_crash(SIGBUS);
    catch_crash(SIGABRT);
    catch_crash(SIGILL);
    catch_crash(SIGFPE);
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGHUP, on_stop);
    vkb_probe_crash_marks(1);
    /* The server is the app's child: when the app dies, so does the session's GPU. Not with
     * PR_SET_PDEATHSIG: that fires when the parent THREAD exits, and Android's ProcessBuilder forks
     * from the session's start-up thread, which ends right after starting the guest (the server
     * was killed 6 ms into the first device run). The parent process is watched instead. */
    start_parent_watch();

    if (!server_init(vk_lib, cache_dir)) return 1;
    if (selftest_only) return vkb_npds > 0 ? 0 : 1;

    if (!sock_path) {
        usage();
        return 2;
    }
    int lfd = listen_on(sock_path);
    if (lfd < 0) return 1;
    VKB_INFO("listening on %s", sock_path);
    vkb_srv_serve(lfd);
    return 0;
}
#endif
