/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Is the game's DXVK configuration applied? DroidDeck passes options as DXVK_CONFIG ("key = value;
 * ..."), a preset or the player may put a dxvk.conf beside the game or name one in
 * DXVK_CONFIG_FILE, and nothing so far showed whether the game's process got them: Steam starts the
 * game with an environment of its own, Proton adds to it. Once per process, at its first present,
 * the client logs:
 *   - the engine the app named (DXVK, vkd3d) - the options matter only to DXVK;
 *   - DXVK_CONFIG as the process has it (its options), DXVK_CONFIG_FILE, and dxvk.conf in the
 *     current directory (where DXVK looks for it without DXVK_CONFIG_FILE: the game's folder);
 *     VKD3D_CONFIG for vkd3d-proton;
 *   - the proof, from DXVK's own log: "Found config env:" / "Found config file:" and its
 *     "Effective configuration:" list, each option of DXVK_CONFIG and the file looked up in it.
 *     DXVK writes its log to DXVK_LOG_PATH (else the current directory) as <exe>_<dll>.log, and
 *     only at DXVK_LOG_LEVEL info or more: Proton sets none unless PROTON_LOG=1 (or the launch
 *     options set DXVK_LOG_LEVEL=info DXVK_LOG_PATH=<dir>); the line then says it cannot confirm.
 * Nothing is logged for an app that is neither DXVK nor vkd3d and has none of these set.
 * DXVK writes its log as it goes, and often after the first present (on the S10+ Dark Souls III's
 * dxgi log came 12 s after it, the d3d11 one a minute later), so the log part is tried again at
 * 5, 15, 30, 60 and 120 s after the first present until every option is found; it says which log
 * files it skipped and why (another run's, unreadable).
 */
#define _GNU_SOURCE
#include "vkb_client.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OPT_MAX 48
#define LOG_FILES_MAX 8

static char engine[64];
static uint32_t engine_version;

void vkb_dxvkcfg_engine(const VkApplicationInfo *app)
{
    if (!app || !app->pEngineName || !*app->pEngineName || engine[0]) return;
    snprintf(engine, sizeof(engine), "%s", app->pEngineName);
    engine_version = app->engineVersion;
}

/* 1: the app is DXVK and its DXVK_CONFIG sets a maxFrameLatency (dxgi. or d3d9.), so DXVK keeps its
 * own frame queue short (VKBRIDGE_FRAME_QUEUE stands aside for it: wsi.c). */
int vkb_dxvkcfg_own_latency(void)
{
    const char *cfg = getenv("DXVK_CONFIG");
    if (strncasecmp(engine, "DXVK", 4) || !cfg) return 0;
    for (const char *p = cfg; (p = strcasestr(p, "maxFrameLatency")); p += 15) {
        const char *v = p + 15;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '=') continue;
        v++;
        while (*v == ' ' || *v == '\t') v++;
        if (*v >= '1' && *v <= '9') return 1;
    }
    return 0;
}

typedef struct opt {
    char key[64], val[96];
    int confirmed; /* found in DXVK's effective configuration with this value */
} opt;

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

/* "key = value" items split at sep (';' for DXVK_CONFIG, '\n' for a file); '#' starts a comment. */
static int parse_opts(const char *text, char sep, opt *o, int max)
{
    int n = 0;
    while (text && *text && n < max) {
        const char *end = strchr(text, sep);
        size_t len = end ? (size_t)(end - text) : strlen(text);
        char item[200];
        if (len >= sizeof(item)) len = sizeof(item) - 1;
        memcpy(item, text, len);
        item[len] = 0;
        char *hash = strchr(item, '#');
        if (hash) *hash = 0;
        char *eq = strchr(item, '=');
        if (eq) {
            *eq = 0;
            trim(item);
            trim(eq + 1);
            snprintf(o[n].key, sizeof(o[n].key), "%.63s", item);
            snprintf(o[n].val, sizeof(o[n].val), "%.95s", eq + 1);
            /* DXVK strips quotes around values */
            size_t vl = strlen(o[n].val);
            if (vl >= 2 && o[n].val[0] == '"' && o[n].val[vl - 1] == '"') {
                memmove(o[n].val, o[n].val + 1, vl - 2);
                o[n].val[vl - 2] = 0;
            }
            o[n].confirmed = 0;
            if (o[n].key[0]) n++;
        }
        if (!end) break;
        text = end + 1;
    }
    return n;
}

static char *read_file(const char *path, size_t max)
{
    FILE *f = fopen(path, "re");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    return buf;
}

static void opts_text(const opt *o, int n, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len + 4 < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, "%s%s=%s", i ? ", " : "", o[i].key, o[i].val);
}

/* When this process started, in wall-clock seconds (0: unknown): DXVK logs older than that are
 * another run's. */
static time_t process_start(void)
{
    char *st = read_file("/proc/self/stat", 4096), *up = read_file("/proc/uptime", 128);
    time_t r = 0;
    if (st && up) {
        const char *p = strrchr(st, ')'); /* comm may hold spaces */
        unsigned long long start_ticks = 0;
        int field = 2;
        for (p = p ? p + 1 : st; *p && field < 22; p++)
            if (*p == ' ') field++;
        if (field == 22) start_ticks = strtoull(p, NULL, 10);
        double uptime = atof(up);
        long hz = sysconf(_SC_CLK_TCK);
        if (start_ticks && hz > 0) r = time(NULL) - (time_t)(uptime - (double)start_ticks / (double)hz);
    }
    free(st);
    free(up);
    return r;
}

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

/* DXVK's log files of this run in dir (<exe>_dxgi.log, _d3d11.log, ...), the ones named after this
 * process first. */
static int find_logs(const char *dir, char paths[][512], int max, char *skipped, size_t cap)
{
    size_t sl = 0;
    skipped[0] = 0;
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(skipped, cap, "cannot open %.300s: %s", dir, strerror(errno));
        return 0;
    }
    char comm[32] = "";
    char *c = read_file("/proc/self/comm", sizeof(comm) - 1);
    if (c) {
        snprintf(comm, sizeof(comm), "%s", c);
        comm[strcspn(comm, ".\n")] = 0; /* "DarkSoulsIII.ex": DXVK's file is "DarkSoulsIII_dxgi.log" */
        free(c);
    }
    time_t since = process_start();
    int n = 0, named = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        const char *nm = e->d_name;
        if (!has_suffix(nm, "_dxgi.log") && !has_suffix(nm, "_d3d11.log") && !has_suffix(nm, "_d3d9.log") && !has_suffix(nm, "_d3d8.log") &&
            !has_suffix(nm, "_d3d10core.log"))
            continue;
        char p[512];
        snprintf(p, sizeof(p), "%.250s/%.250s", dir, nm);
        struct stat st;
        if (stat(p, &st)) {
            if (sl + 64 < cap) sl += (size_t)snprintf(skipped + sl, cap - sl, "%s%.120s (%s)", sl ? ", " : "", nm, strerror(errno));
            continue;
        }
        if (since && st.st_mtime + 2 < since) {
            /* another run's (a file of this run's name not rewritten yet is too: tried again later) */
            if (sl + 64 < cap)
                sl += (size_t)snprintf(skipped + sl, cap - sl, "%s%.120s (written %lld s before this process started: another run's)", sl ? ", " : "",
                                       nm, (long long)(since - st.st_mtime));
            continue;
        }
        int mine = comm[0] && !strncasecmp(nm, comm, strlen(comm));
        if (mine && named < n) {
            /* this process's own logs go before the others */
            memcpy(paths[n], paths[named], sizeof(paths[0]));
            snprintf(paths[named++], sizeof(paths[0]), "%s", p);
        } else {
            snprintf(paths[n], sizeof(paths[0]), "%s", p);
            named += mine;
        }
        n++;
    }
    closedir(d);
    return named ? named : n;
}

/* What DXVK's own log says: the config it found and its effective options. 1: a log was read. */
static int check_log(const char *path, opt *o, int n, char *out, size_t cap, int *has_effective)
{
    char *text = read_file(path, 1 << 20);
    if (!text) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = (size_t)snprintf(out, cap, "%s:", base);
    int found_env = 0, found_file = 0, effective = 0, in_eff = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *s;
        if ((s = strstr(line, "Found config env:"))) found_env = 1;
        else if ((s = strstr(line, "Found config file:"))) {
            found_file = 1;
            char f[200];
            snprintf(f, sizeof(f), "%s", s + strlen("Found config file:"));
            trim(f);
            if (len + 32 < cap) len += (size_t)snprintf(out + len, cap - len, " found config file %s;", f);
        }
        if (strstr(line, "Effective configuration")) {
            effective = in_eff = 1;
            continue;
        }
        if (!in_eff) continue;
        /* "info:    key = value" until the first line that is not an option */
        char *colon = strchr(line, ':');
        char *item = colon ? colon + 1 : line;
        char *eq = strchr(item, '=');
        if (!eq || !isspace((unsigned char)item[0])) {
            in_eff = 0;
            continue;
        }
        opt eff;
        if (parse_opts(item, '\n', &eff, 1) != 1) continue;
        for (int i = 0; i < n; i++)
            if (!strcmp(o[i].key, eff.key) && !strcasecmp(o[i].val, eff.val)) o[i].confirmed = 1;
    }
    free(text);
    if (found_env && len + 32 < cap) len += (size_t)snprintf(out + len, cap - len, " found DXVK_CONFIG;");
    if (!found_env && !found_file && len + 48 < cap) len += (size_t)snprintf(out + len, cap - len, " no config found by DXVK;");
    if (!effective && len + 64 < cap) len += (size_t)snprintf(out + len, cap - len, " no \"Effective configuration\" list;");
    if (effective) *has_effective = 1;
    return 1;
}

/* The log part, tried at the first present and again on the schedule below. */
#define TRIES_MAX 8
static pthread_mutex_t check_lock = PTHREAD_MUTEX_INITIALIZER;
static int state;                /* 0 not started, 1 waiting for DXVK's log, 2 done (atomic) */
static opt opts[OPT_MAX];
static int nopts;
static char logdir[512];
static uint64_t first_ns;
static uint64_t tries_ms[TRIES_MAX] = {5000, 15000, 30000, 60000, 120000};
static int ntries = 5, next_try;
static unsigned presents;
static int at_exit;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void schedule_text(char *out, size_t cap)
{
    size_t l = 0;
    out[0] = 0;
    for (int i = next_try; i < ntries && l + 16 < cap; i++)
        l += (size_t)snprintf(out + l, cap - l, "%s%.3g", l ? (i == ntries - 1 ? " and " : ", ") : "", tries_ms[i] / 1000.0);
}

/* One look at DXVK's logs. final: the last one, which says what it found whatever it is. 1: done. */
static int try_logs(int final)
{
    char paths[LOG_FILES_MAX][512], skipped[700];
    int nl = find_logs(logdir, paths, LOG_FILES_MAX, skipped, sizeof(skipped));
    char sk[760] = "";
    if (skipped[0]) snprintf(sk, sizeof(sk), "; skipped %s", skipped);
    char when[96] = "";
    if (next_try > 0)
        snprintf(when, sizeof(when), " (%.3g s after the first present%s)", (now_ns() - first_ns) / 1e9, at_exit ? ", at the process's exit" : "");
    char sched[96];
    if (!nl) {
        if (final) {
            VKB_WARN("DXVK config: not confirmed - no DXVK log of this run in %s%s%s", logdir, when, sk);
            return 1;
        }
        if (next_try == 0) {
            schedule_text(sched, sizeof(sched));
            VKB_WARN("DXVK config: no DXVK log of this run in %s yet%s; checking again %s s after the first present", logdir, sk, sched);
        }
        return 0;
    }
    for (int i = 0; i < nopts; i++) opts[i].confirmed = 0;
    char said[LOG_FILES_MAX][900];
    int read_any = 0, effective = 0, ok[LOG_FILES_MAX] = {0};
    for (int i = 0; i < nl; i++) ok[i] = check_log(paths[i], opts, nopts, said[i], sizeof(said[i]), &effective), read_any |= ok[i];
    int all = read_any && effective;
    for (int i = 0; i < nopts; i++) all &= opts[i].confirmed;
    if (!all && !final) {
        if (next_try == 0) {
            schedule_text(sched, sizeof(sched));
            VKB_WARN("DXVK config: %s in %s yet%s; checking again %s s after the first present",
                     !read_any ? "no readable DXVK log" : !effective ? "no \"Effective configuration\" list in DXVK's log" : "not every option in DXVK's log",
                     logdir, sk, sched);
        }
        return 0;
    }
    for (int i = 0; i < nl; i++)
        if (ok[i]) VKB_WARN("DXVK config: DXVK's log %s%s", said[i], when);
    if (sk[0]) VKB_WARN("DXVK config: %s", sk + 2);
    if (!read_any || !nopts) return 1;
    char yes[600] = "", no[600] = "";
    size_t ly = 0, ln = 0;
    for (int i = 0; i < nopts; i++) {
        char *b = opts[i].confirmed ? yes : no;
        size_t *l = opts[i].confirmed ? &ly : &ln;
        if (*l + 4 < sizeof(yes)) *l += (size_t)snprintf(b + *l, sizeof(yes) - *l, "%s%s=%s", *l ? ", " : "", opts[i].key, opts[i].val);
    }
    VKB_WARN("DXVK config: applied (in DXVK's effective configuration): %s; not there: %s", ly ? yes : "none", ln ? no : "none");
    return 1;
}

/* At the first present: what the process was given. 1: DXVK's log is worth looking at. */
static int first_check(void)
{
    const char *cfg = getenv("DXVK_CONFIG"), *cfile = getenv("DXVK_CONFIG_FILE"), *vkd3d = getenv("VKD3D_CONFIG");
    int is_dxvk = !strncasecmp(engine, "DXVK", 4), is_vkd3d = !strncasecmp(engine, "vkd3d", 5);
    if (!is_dxvk && !is_vkd3d && !(cfg && *cfg) && !(cfile && *cfile)) return 0;
    opt *o = opts;
    int n = cfg && *cfg ? parse_opts(cfg, ';', o, OPT_MAX) : 0, nenv = n;
    /* the file DXVK reads: DXVK_CONFIG_FILE, else dxvk.conf in the current directory */
    char cwd[400] = "", conf[512];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
    if (cfile && *cfile) snprintf(conf, sizeof(conf), "%s", cfile);
    else snprintf(conf, sizeof(conf), "%s/dxvk.conf", cwd[0] ? cwd : ".");
    char *ftext = read_file(conf, 64 << 10);
    int have_file = ftext != NULL;
    if (ftext) n += parse_opts(ftext, '\n', o + n, OPT_MAX - n);
    free(ftext);
    nopts = n;
    char envs[600], files[600], filepart[800], cfgpart[700];
    opts_text(o, nenv, envs, sizeof(envs));
    opts_text(o + nenv, n - nenv, files, sizeof(files));
    const char *label = cfile && *cfile ? "DXVK_CONFIG_FILE" : "dxvk.conf";
    if (n > nenv) snprintf(filepart, sizeof(filepart), "%s %.300s (%.450s)", label, conf, files);
    else snprintf(filepart, sizeof(filepart), "%s %.300s %s", label, conf, have_file ? "(no options in it)" : "not there");
    if (!cfg) snprintf(cfgpart, sizeof(cfgpart), "DXVK_CONFIG unset");
    else snprintf(cfgpart, sizeof(cfgpart), "DXVK_CONFIG \"%.250s\" (%.400s)", cfg, nenv ? envs : "no options in it");
    char eng[96] = "none named";
    if (engine[0])
        snprintf(eng, sizeof(eng), "%s %u.%u.%u", engine, VK_API_VERSION_MAJOR(engine_version), VK_API_VERSION_MINOR(engine_version),
                 VK_API_VERSION_PATCH(engine_version));
    VKB_WARN("DXVK config: engine %s; %s; %s%s%.200s", eng, cfgpart, filepart, vkd3d ? "; VKD3D_CONFIG=" : "", vkd3d ? vkd3d : "");
    if (!is_dxvk) return 0;
    const char *lvl = getenv("DXVK_LOG_LEVEL");
    if (lvl && (!strcmp(lvl, "none") || !strcmp(lvl, "error") || !strcmp(lvl, "warn"))) {
        VKB_WARN("DXVK config: not confirmed - DXVK's own log is at DXVK_LOG_LEVEL=%s (Proton's default is none); "
                 "DXVK_LOG_LEVEL=info DXVK_LOG_PATH=<dir> in the launch options lets this line check what DXVK applied", lvl);
        return 0;
    }
    const char *lp = getenv("DXVK_LOG_PATH");
    if (lp && !strcmp(lp, "none")) {
        VKB_WARN("DXVK config: not confirmed - DXVK_LOG_PATH=none (DXVK writes no log file)");
        return 0;
    }
    snprintf(logdir, sizeof(logdir), "%s", lp && *lp ? lp : cwd[0] ? cwd : ".");
    /* VKBRIDGE_TEST_DXVKCFG_RETRY_MS=<ms>[,<ms>...] (tests): the later looks, after the first present */
    const char *tr = getenv("VKBRIDGE_TEST_DXVKCFG_RETRY_MS");
    if (tr && *tr) {
        ntries = 0;
        for (const char *p = tr; *p && ntries < TRIES_MAX;) {
            tries_ms[ntries++] = strtoull(p, NULL, 10);
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
    }
    return 1;
}

/* A process that ends before the last look: one last look at its exit. */
static void exit_try(void)
{
    if (pthread_mutex_trylock(&check_lock)) return;
    if (__atomic_load_n(&state, __ATOMIC_RELAXED) == 1) {
        at_exit = 1;
        next_try = ntries;
        try_logs(1);
        __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&check_lock);
}

/* At every present: one atomic load once done; while waiting for DXVK's log, a clock read every
 * 16th present. */
void vkb_dxvkcfg_check(void)
{
    int st = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    if (st == 2) return;
    if (st == 1 && (__atomic_add_fetch(&presents, 1, __ATOMIC_RELAXED) & 15)) return;
    if (pthread_mutex_trylock(&check_lock)) return;
    st = __atomic_load_n(&state, __ATOMIC_RELAXED);
    if (st == 0) {
        first_ns = now_ns();
        if (!first_check()) st = 2;
        else st = try_logs(ntries == 0) ? 2 : 1;
        if (st == 1) atexit(exit_try);
    } else if (st == 1 && now_ns() - first_ns >= tries_ms[next_try] * 1000000ull) {
        next_try++;
        if (try_logs(next_try >= ntries)) st = 2;
    }
    __atomic_store_n(&state, st, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&check_lock);
}
