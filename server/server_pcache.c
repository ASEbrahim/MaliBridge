/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Persistent pipeline cache. Mali compiles every pipeline from scratch in each new process unless
 * it is handed a VkPipelineCache; games through DXVK pass none. With a cache directory set
 * (--cache-dir), each device gets a cache loaded from <dir>/<vendor>-<device>-<cacheUUID>-<key>.vkpc,
 * <key> being what the client says (client/conn.c pcache_key: steam<appid> for a Steam game,
 * steam<appid>.run / .dl for Steam's shader pre-caching of it) or else the process name:
 *   - pipelines created without a cache use it;
 *   - the first caches the app creates empty start from it, and the caches the app creates are
 *     merged back into it every few seconds while the app compiles into them and when they or the
 *     device go away - except those made EXTERNALLY_SYNCHRONIZED: Zink makes one per GL program
 *     and keeps each in Mesa's disk cache itself, and never uses this one;
 *   - it is written back every few seconds while new pipelines come in (the app stops the server
 *     with SIGKILL, so there is no reliable last chance) and when the device goes away, the
 *     owner's process killed included;
 *   - several processes on one file (Steam's replayer runs a process per core) each add to what
 *     the others saved instead of replacing it (the replayer's processes start empty and save
 *     every half minute: each would otherwise hold, and reload, all of it);
 *   - a game's cache takes in the pre-caching files when it starts, and they go once it is saved
 *     with them, so "Processing Vulkan shaders" warms the cache the game itself loads.
 * The file's header is checked against the GPU before the driver sees it.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

/* One file per program, with a limit: a single shared file grew to 58 MB over a night of games
 * and every device (gamescope, Xwayland, Steam, each game) loaded all of it, and every cache an app
 * made was seeded with another copy - the server ran out of memory in vkCreatePipelineCache. With
 * a file per game (and pre-caching folded into it) 32 MB was too tight, and 64 MB too: RE2 (vkd3d,
 * ~9 KB a pipeline) passed it within minutes on two tablets, HL2 did too, and from then on nothing
 * new was kept, so the same compile stutter came back at every launch.
 *
 * Now VKBRIDGE_PCACHE_MAX_MB, by default RAM/64 within 64-128 MB, never more than the total limit.
 * The memory free at the device's start no longer lowers it: it used to (a quarter of MemAvailable,
 * at least 32 MB), and one tight start left RE Village's 82 MB file unloaded and the next save
 * replaced it with a few MB (Pixel 8 Pro, 2026-10). Now the memory free decides only whether the
 * file is loaded this time (about twice its size free); when it is not, saves only merge into it,
 * never write this run's smaller cache over it. The cache is one VkPipelineCache in the driver (a pipeline is created with one cache, so
 * all that should hit has to be in it, and the driver holds all of it in memory however the files
 * are cut), and a save that merges with what another process wrote holds about three of them in
 * the driver for a moment (ours, the file's, the merge, while vkGetPipelineCacheData copies it):
 * at 256 MB that was ~750 MB mid-game, enough for the low-memory killer on an 8 GB device. So the
 * default stays at 128 MB until a big save's memory is measured on a tablet, and a merge is skipped
 * when the memory free is under three times what it would hold (the save writes ours alone, said
 * once). What this also keeps down is the server's own copies around it: files are mapped, not
 * read into the heap, saves go from vkGetPipelineCacheData straight into the (preallocated) file's
 * pages, and they come further apart as the cache grows (a save writes all of it). Past the limit
 * the file keeps what it has, and that is said once per device. */
#define PC_MAX_MB_MIN 64u
#define PC_MAX_MB_MAX 128u
#define PC_MAX_MB_UNKNOWN_RAM 64u
#define PC_MAX_MB_SET_MAX (1u << 20) /* VKBRIDGE_PCACHE_MAX_MB above this is taken as this */
/* A <file>.<pid>.<n>.tmp left by a killed save (its size reserved in full) goes at the next trim
 * once its process is gone, or after this long whatever its pid (pids are reused). */
#define PC_TMP_STALE_S 600
/* App-made caches start from the program's cache only while it is this small, and only the first
 * few: Zink makes one per GL program (and keeps each in Mesa's disk cache), so every one of them
 * got - and wrote back to Mesa's cache, and copied over the socket - the whole Steam UI cache. */
/* All the program files together (VKBRIDGE_PCACHE_TOTAL_MB, 0: no limit). On Android the directory
 * is in the app's files now, which the system never trims, and a file per game adds up: past the
 * limit the files used longest ago go, at the server's start and now and then at a new device
 * (a file is touched when it is loaded, so its time is when it was last used or saved). */
#define PC_TOTAL_MB_DEFAULT 1024u
#define PC_TRIM_PERIOD_S 300
#define PC_SEED_MAX (8u << 20)
#define PC_SEED_CACHES 2
#define PC_SAVE_PERIOD_S 10
/* Above this a save waits longer, in proportion (a 256 MB cache: every 60 s at most). */
#define PC_SAVE_BIG (32u << 20)
#define PC_SAVE_PERIOD_MAX_S 60
#define PC_WARM_SAVE_PERIOD_S 30
#define PC_WARM_KINDS 2

static const char *const warm_kinds[PC_WARM_KINDS] = {"run", "dl"}; /* recorded here first */

/* A file as last read or written (another process saving it changes it). */
typedef struct pc_ident {
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime;
    int valid;
} pc_ident;

/* A cache the app made. */
typedef struct pc_app {
    VkPipelineCache cache;
    int extsync; /* VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT: never merged */
    int used;    /* compiled into since it was last merged */
    struct pc_app *next;
} pc_app;

typedef struct vkb_pcache {
    vkb_srv_table *t;
    VkPipelineCache cache;
    char path[512];
    int dirty;
    size_t saved_size;    /* size of `cache` at the last save (or load) */
    size_t mem_size;      /* its size in the driver at the last save, over the limit too (vkb_pcache_stats) */
    pc_ident file;        /* the file as this cache last read or wrote it */
    int covers;           /* the file holds nothing `cache` lacks (it was written from it alone) */
    int full_warned;
    int over_warned;      /* warn_over said for this device */
    int nomerge_warned;   /* a merge skipped for want of memory, said */
    int not_loaded;       /* the file was left unloaded for want of memory: saves only merge into it */
    int keep_warned;      /* a save skipped to keep that file, said */
    uint64_t max;         /* the per-program limit in bytes (file_limit at the device's start) */
    int save_err;         /* errno of the last failed save, said once (0: the last one worked) */
    int warm_role;        /* Steam's shader pre-caching (key steam<id>.run/.dl) */
    time_t last_save;     /* CLOCK_MONOTONIC seconds */
    char warm[PC_WARM_KINDS][512]; /* pre-caching files taken in, removed after the next save */
    pc_ident warm_id[PC_WARM_KINDS];
    int nwarm;
    pthread_rwlock_t use; /* merges into `cache` (write) / pipelines compiled with it (read) */
    pthread_mutex_t alock;
    pc_app *apps;
    unsigned seeded;
    int seed_logged;
    PFN_vkCreateGraphicsPipelines create_gfx;
    PFN_vkCreateComputePipelines create_compute;
    PFN_vkCreatePipelineCache create_cache;
    PFN_vkDestroyPipelineCache destroy_cache;
    struct vkb_pcache *next;
} vkb_pcache;

static const char *pc_dir;
/* pc_lock: the list and trim, held only briefly (a new device takes it). save_lock: every save, so
 * the checks of a file against what another device in this process wrote hold until its rename; a
 * cache is freed only with it held, so the saver can walk the list without pc_lock while saving. */
static pthread_mutex_t pc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned tmp_seq;
static vkb_pcache *pc_list;
static int pc_thread_started;
static uint64_t pc_made; /* pipelines made through the caches (atomic; the server's memory line) */

const char *vkb_pcache_dir(void) { return pc_dir; }

static void trim(const char *keep, int now);

/* Test toggles without restarting the app (the server lives in it and reads its environment once):
 * <cache dir>/env holds KEY=VALUE lines (# comments), read again at every vkCreateDevice, and
 * vkb_srv_env() prefers them to the process environment. Only switches read per device take
 * effect this way (VKBRIDGE_SPIRV_FIX, VKBRIDGE_EMU_OFF, ... - see their users). A removed file
 * drops the overrides at the next device. Old tables stay allocated: a returned value is never freed. */
typedef struct env_kv {
    char *key, *val;
} env_kv;
static env_kv *env_tab;
static unsigned env_n;
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
/* The file as last read (vkb_srv_env_refresh): there or not, and its identity, size and times */
typedef struct env_stamp {
    int there;
    dev_t dev;
    ino_t ino;
    off_t size;
    struct timespec mtime, ctime;
} env_stamp;
static env_stamp env_read;

static env_stamp env_stamp_of(const struct stat *st)
{
    env_stamp s = {0};
    if (!st) return s;
    s.there = 1;
    s.dev = st->st_dev;
    s.ino = st->st_ino;
    s.size = st->st_size;
    s.mtime = st->st_mtim;
    s.ctime = st->st_ctim;
    return s;
}

static int env_stamp_same(const env_stamp *a, const env_stamp *b)
{
    if (a->there != b->there) return 0;
    if (!a->there) return 1;
    return a->dev == b->dev && a->ino == b->ino && a->size == b->size && a->mtime.tv_sec == b->mtime.tv_sec &&
           a->mtime.tv_nsec == b->mtime.tv_nsec && a->ctime.tv_sec == b->ctime.tv_sec && a->ctime.tv_nsec == b->ctime.tv_nsec;
}

void vkb_srv_env_reload(void)
{
    if (!pc_dir) return;
    vkb_cache_dir_ensure(); /* a new device is a good time to notice it went */
    char path[600];
    snprintf(path, sizeof(path), "%s/env", pc_dir);
    FILE *f = fopen(path, "r");
    struct stat st;
    env_stamp stamp = env_stamp_of(f && fstat(fileno(f), &st) == 0 ? &st : NULL);
    env_kv *tab = NULL;
    unsigned n = 0, cap = 0;
    char line[512];
    while (f && fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || !*s) continue;
        size_t len = strcspn(s, "\r\n");
        s[len] = 0;
        char *eq = strchr(s, '=');
        if (!eq || eq == s) continue;
        *eq = 0;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            env_kv *t = realloc(tab, cap * sizeof(*tab));
            if (!t) break;
            tab = t;
        }
        tab[n].key = strdup(s);
        tab[n].val = strdup(eq + 1);
        VKB_INFO("override from %s: %s=%s", path, tab[n].key, tab[n].val);
        n++;
    }
    if (f) fclose(f);
    pthread_mutex_lock(&env_lock);
    if (env_n || n) VKB_INFO("%u override(s) from %s in effect", n, path);
    env_tab = tab; /* the previous table is left allocated (see above) */
    env_n = n;
    env_read = stamp;
    pthread_mutex_unlock(&env_lock);
}

/* Read again only when the file changed since it was last read (written, edited, replaced or
 * removed). At vkbQueryServer: a process asks for its client switches (VKBRIDGE_HEAP_MAX_MB,
 * VKBRIDGE_SPOOF_GPU, VKBRIDGE_PRESENT_TIMEOUT_S, VKBRIDGE_QUERY_MIRROR, ...) before it makes a
 * device, so without this the first process after the file changed got the values from before
 * (only vkCreateDevice read it again). */
void vkb_srv_env_refresh(void)
{
    if (!pc_dir) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/env", pc_dir);
    struct stat st;
    env_stamp now = env_stamp_of(stat(path, &st) == 0 ? &st : NULL);
    pthread_mutex_lock(&env_lock);
    int same = env_stamp_same(&now, &env_read);
    pthread_mutex_unlock(&env_lock);
    if (!same) vkb_srv_env_reload();
}

const char *vkb_srv_env_list(char *out, size_t cap)
{
    size_t len = 0;
    out[0] = 0;
    pthread_mutex_lock(&env_lock);
    for (unsigned i = 0; i < env_n && len < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, "%s%s=%s", len ? " " : "", env_tab[i].key, env_tab[i].val);
    pthread_mutex_unlock(&env_lock);
    return out;
}

const char *vkb_srv_env_override(const char *name)
{
    const char *v = NULL;
    pthread_mutex_lock(&env_lock);
    for (unsigned i = 0; i < env_n && !v; i++)
        if (!strcmp(env_tab[i].key, name)) v = env_tab[i].val;
    pthread_mutex_unlock(&env_lock);
    return v;
}

/* First found wins: on a client's connection thread that game's preset file (the user's own choice
 * for it), then <cache dir>/env (the developer's and tester's override, read again at each
 * device), the server's environment, then that game's built-in preset (server_presets.c). The
 * global places beat the built-in preset so that one line in <cache dir>/env overrides a stale
 * one for every game at once. */
const char *vkb_srv_env_src(const char *name, int *src)
{
    int ps = VKB_ENV_SRC_NONE, s;
    const char *pv = vkb_preset_get(vkb_srv_current_proc(), name, &ps);
    const char *v = NULL;
    if (pv && ps == VKB_ENV_SRC_PRESET_FILE) v = pv, s = ps;
    else if ((v = vkb_srv_env_override(name))) s = VKB_ENV_SRC_CACHE_ENV;
    else if ((v = getenv(name))) s = VKB_ENV_SRC_ENV;
    else v = pv, s = ps;
    if (src) *src = v ? s : VKB_ENV_SRC_NONE;
    return v;
}

const char *vkb_srv_env(const char *name) { return vkb_srv_env_src(name, NULL); }

const char *vkb_srv_env_global(const char *name)
{
    const char *v = vkb_srv_env_override(name);
    return v ? v : getenv(name);
}

const char *vkb_env_src_name(int src)
{
    switch (src) {
    case VKB_ENV_SRC_CACHE_ENV: return "<cache dir>/env";
    case VKB_ENV_SRC_ENV: return "the environment";
    case VKB_ENV_SRC_PRESET_FILE: return "the game's preset file";
    case VKB_ENV_SRC_PRESET_BUILTIN: return "the game's built-in preset";
    default: return "the default";
    }
}

/* mkdir -p: 0, or -1 with errno. */
int vkb_mkdirs(const char *path)
{
    char buf[600];
    size_t n = strlen(path);
    if (!n || n >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, n + 1);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
        *s = '/';
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (stat(buf, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

/* The cache directory can go while the server runs: Android empties an app's cache directory when
 * storage runs out (a tablet at 100% lost it, the server's files and the env file with it, and
 * every save after that failed with ENOENT). Each writer into it calls this when it is missing:
 * it is made again, once, and said once. 1 when the directory is there. */
static pthread_mutex_t dir_lock = PTHREAD_MUTEX_INITIALIZER;
static int dir_fail_logged, nospace;

int vkb_cache_dir_ensure(void)
{
    if (!pc_dir) return 0;
    struct stat st;
    if (stat(pc_dir, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    pthread_mutex_lock(&dir_lock);
    int ok = stat(pc_dir, &st) == 0 && S_ISDIR(st.st_mode); /* another writer made it meanwhile */
    if (!ok) {
        ok = vkb_mkdirs(pc_dir) == 0;
        if (ok) {
            dir_fail_logged = 0;
            VKB_WARN("cache directory %s was removed; recreated (its pipeline caches, probe profile and env file are gone with it)", pc_dir);
        } else if (!dir_fail_logged) {
            dir_fail_logged = 1;
            VKB_WARN("cache directory %s was removed and cannot be made again: %s; nothing is saved until it can", pc_dir, strerror(errno));
        }
    }
    pthread_mutex_unlock(&dir_lock);
    return ok;
}

/* <cache dir>/<sub>, made (and the cache directory with it) if missing; 0 or -1 with errno. */
int vkb_cache_subdir(const char *sub)
{
    if (!pc_dir || !vkb_cache_dir_ensure()) {
        errno = ENOENT;
        return -1;
    }
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", pc_dir, sub);
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* A write into the cache directory failed with errno e. A full storage is said once until a write
 * goes through again (vkb_cache_write_ok), whoever hits it: the server keeps running and the
 * writers keep trying quietly. Returns 1 when the caller should report the error itself. */
int vkb_cache_write_failed(const char *what, int e)
{
    if (e != ENOSPC && e != EDQUOT) return 1;
    if (!__atomic_exchange_n(&nospace, 1, __ATOMIC_RELAXED))
        VKB_WARN("cache directory %s: no space left on the device (%s not written); the bridge keeps running and tries again quietly", pc_dir ? pc_dir : "",
                 what);
    return 0;
}

void vkb_cache_write_ok(void)
{
    if (__atomic_load_n(&nospace, __ATOMIC_RELAXED) && __atomic_exchange_n(&nospace, 0, __ATOMIC_RELAXED))
        VKB_INFO("cache directory %s: writes go through again", pc_dir ? pc_dir : "");
}

void vkb_pcache_set_dir(const char *dir)
{
    if (!dir || !*dir) return;
    vkb_mkdirs(dir);
    /* A copy: the in-app start frees its string after server_init (The412Banner, e40cd361). */
    char *copy = strdup(dir);
    if (!copy) return;
    free((char *)pc_dir);
    pc_dir = copy;
    VKB_INFO("pipeline cache directory %s", dir);
    pthread_mutex_lock(&pc_lock);
    trim(NULL, 1);
    pthread_mutex_unlock(&pc_lock);
}

typedef struct pc_entry {
    char name[256];
    off_t size;
    struct timespec mtime;
} pc_entry;

static int entry_older(const void *a, const void *b)
{
    const pc_entry *x = a, *y = b;
    if (x->mtime.tv_sec != y->mtime.tv_sec) return x->mtime.tv_sec < y->mtime.tv_sec ? -1 : 1;
    return x->mtime.tv_nsec < y->mtime.tv_nsec ? -1 : x->mtime.tv_nsec > y->mtime.tv_nsec;
}

static uint64_t total_limit(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_PCACHE_TOTAL_MB");
    return (uint64_t)(v && *v ? strtoull(v, NULL, 10) : PC_TOTAL_MB_DEFAULT) << 20;
}

/* The limit of one program's file, in bytes (see the top). */
static uint64_t file_limit(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_PCACHE_MAX_MB");
    double set = v && *v ? strtod(v, NULL) : 0; /* fractions too (the host tests' caches are KB) */
    uint64_t bytes;
    if (set > 0) {
        if (set > PC_MAX_MB_SET_MAX) set = PC_MAX_MB_SET_MAX; /* (uint64_t) of a huge double is undefined */
        bytes = set * 1048576.0 < 1024 ? 1024 : (uint64_t)(set * 1048576.0);
    } else {
        uint64_t ram = vkb_memtotal_mb(), mb = ram ? ram / 64 : PC_MAX_MB_UNKNOWN_RAM;
        mb = mb < PC_MAX_MB_MIN ? PC_MAX_MB_MIN : mb > PC_MAX_MB_MAX ? PC_MAX_MB_MAX : mb;
        bytes = mb << 20;
    }
    uint64_t total = total_limit();
    return total && bytes > total ? total : bytes;
}

/* A limit for the log: "256 MB" (or "4 KB" below 1 MB). */
static const char *limit_str(char out[32], uint64_t bytes)
{
    if (bytes >= (1u << 20)) snprintf(out, 32, "%llu MB", (unsigned long long)(bytes >> 20));
    else snprintf(out, 32, "%llu KB", (unsigned long long)(bytes >> 10));
    return out;
}

static time_t mono_s(void);

/* <name> (in d) is a save's <file>.vkpc.<pid>[.<n>].tmp: removed when its process is gone, or when
 * it is older than PC_TMP_STALE_S (a pid may be in use again by now); not one this server is
 * writing (saves take far less than that). */
static void stale_tmp(DIR *d, const char *name, time_t wall)
{
    const char *at = strstr(name, ".vkpc.") + 6;
    long pid = strtol(at, NULL, 10);
    struct stat st;
    if (fstatat(dirfd(d), name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) return;
    int gone = pid > 0 && pid != (long)getpid() && kill((pid_t)pid, 0) != 0 && errno == ESRCH;
    if (!gone && wall - st.st_mtim.tv_sec < PC_TMP_STALE_S) return;
    if (unlinkat(dirfd(d), name, 0) == 0)
        VKB_INFO("pipeline cache %s/%s (%lld KB) removed: left by a save that did not finish", pc_dir, name, (long long)(st.st_size >> 10));
}

/* The .vkpc files over the total limit, oldest first, but never one a device has open (keep: one
 * more, by path); and the temporary files of saves that were killed. With pc_lock held. */
static void trim(const char *keep, int now)
{
    static time_t last;
    time_t t = mono_s();
    if (!pc_dir || (!now && last && t - last < PC_TRIM_PERIOD_S)) return;
    last = t;
    uint64_t limit = total_limit();
    DIR *d = opendir(pc_dir);
    if (!d) return;
    pc_entry *e = NULL;
    size_t n = 0, cap = 0;
    uint64_t total = 0;
    time_t wall = time(NULL);
    for (struct dirent *de; (de = readdir(d));) {
        size_t len = strlen(de->d_name);
        if (len > 4 && !strcmp(de->d_name + len - 4, ".tmp") && strstr(de->d_name, ".vkpc.")) {
            stale_tmp(d, de->d_name, wall);
            continue;
        }
        if (len < 6 || len >= sizeof(e->name) || strcmp(de->d_name + len - 5, ".vkpc")) continue;
        struct stat st;
        if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            pc_entry *g = realloc(e, cap * sizeof(*e));
            if (!g) break;
            e = g;
        }
        memcpy(e[n].name, de->d_name, len + 1);
        e[n].size = st.st_size;
        e[n].mtime = st.st_mtim;
        total += (uint64_t)st.st_size;
        n++;
    }
    closedir(d);
    if (limit && total > limit) {
        qsort(e, n, sizeof(*e), entry_older);
        uint64_t freed = 0;
        unsigned gone = 0;
        for (size_t i = 0; i < n && total - freed > limit; i++) {
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", pc_dir, e[i].name);
            int busy = keep && !strcmp(path, keep);
            for (vkb_pcache *p = pc_list; p && !busy; p = p->next) busy = !strcmp(p->path, path);
            if (busy || unlink(path) != 0) continue;
            freed += (uint64_t)e[i].size;
            gone++;
            VKB_INFO("pipeline cache %s (%lld KB) removed: the least recently used", path, (long long)(e[i].size >> 10));
        }
        VKB_INFO("pipeline caches: %llu MB in %zu files, over the %llu MB limit (VKBRIDGE_PCACHE_TOTAL_MB); %u removed, %llu MB freed",
                 (unsigned long long)(total >> 20), n, (unsigned long long)(limit >> 20), gone, (unsigned long long)(freed >> 20));
    }
    free(e);
}

static vkb_pcache *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->pcache : NULL;
}

/* The header every pipeline cache starts with (VkPipelineCacheHeaderVersionOne). */
static int header_for(const VkPhysicalDeviceProperties *p, const uint8_t *data, size_t size)
{
    VkPipelineCacheHeaderVersionOne h;
    if (size < sizeof(h)) return 0;
    memcpy(&h, data, sizeof(h));
    const VkPhysicalDeviceProperties props = *p;
    return h.headerSize >= sizeof(h) && h.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           h.vendorID == props.vendorID && h.deviceID == props.deviceID &&
           !memcmp(h.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE);
}

static int header_ok(const vkb_srv_table *t, const uint8_t *data, size_t size)
{
    VkPhysicalDeviceProperties props;
    t->real.vkGetPhysicalDeviceProperties(t->physical, &props);
    return header_for(&props, data, size);
}

static void ident_of(const struct stat *st, pc_ident *id)
{
    id->dev = st->st_dev;
    id->ino = st->st_ino;
    id->size = st->st_size;
    id->mtime = st->st_mtim;
    id->valid = 1;
}

static int ident_stat(const char *path, pc_ident *id)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        memset(id, 0, sizeof(*id));
        return 0;
    }
    ident_of(&st, id);
    return 1;
}

static int ident_eq(const pc_ident *a, const pc_ident *b)
{
    return a->valid && b->valid && a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
           a->mtime.tv_sec == b->mtime.tv_sec && a->mtime.tv_nsec == b->mtime.tv_nsec;
}

/* A file's data for the driver: read into the heap, or mapped read-only. A mapping is no heap (a
 * file read into a buffer is that much more memory while the driver makes its copy), but a page
 * that cannot be read (an I/O error, a file cut short under us, storage that goes away: FUSE,
 * sdcardfs, a network or removable file system) is a SIGBUS in whatever touches it, here the
 * driver, which no handler can safely leave. So only big files are mapped, and only on local
 * file systems that do not compress the file; everything else is read, and a read error starts
 * empty. Files are only ever replaced by a rename, never truncated, so a mapping of ours stays
 * valid. VKBRIDGE_PCACHE_MMAP=0 never maps (saves too); =all maps any size where it may. */
#define PC_MAP_MIN ((size_t)16 << 20)

typedef struct pc_map {
    uint8_t *data; /* NULL: missing, empty, over the limit or unreadable */
    size_t size;
    off_t over;    /* > 0: the file's size, over the limit (not loaded) */
    int mapped;    /* data is a mapping (else the heap) */
} pc_map;

/* Local storage where a mapped page is not expected to fail. VKBRIDGE_PCACHE_FAKE_FS=fuse: the
 * host test's stand-in for /sdcard. */
static int fs_local(int fd)
{
    const char *fake = vkb_srv_env_global("VKBRIDGE_PCACHE_FAKE_FS");
    if (fake && *fake) return 0;
    struct statfs sf;
    if (fstatfs(fd, &sf) != 0) return 0;
    switch ((unsigned long)sf.f_type) {
    case 0xEF53UL:     /* ext2/3/4 */
    case 0xF2F52010UL: /* f2fs */
    case 0x01021994UL: /* tmpfs */
    case 0x9123683EUL: /* btrfs */
    case 0x58465342UL: /* xfs */
        break;
    default:
        return 0;
    }
    int flags = 0;
    /* f2fs and btrfs compression: blocks are not reserved, a full storage is a SIGBUS on write */
    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0 && (flags & FS_COMPR_FL)) return 0;
    return 1;
}

/* Whether n bytes of fd are mapped rather than read or written through the heap. */
static int map_ok(int fd, size_t n)
{
    const char *v = vkb_srv_env_global("VKBRIDGE_PCACHE_MMAP");
    if (v && !strcmp(v, "0")) return 0;
    if (n < PC_MAP_MIN && !(v && !strcmp(v, "all"))) return 0;
    return fs_local(fd);
}

static pc_map map_file(const char *path, uint64_t limit, pc_ident *id)
{
    pc_map m = {NULL, 0, 0, 0};
    if (id) memset(id, 0, sizeof(*id));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return m;
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (id) ident_of(&st, id);
        size_t n = (size_t)st.st_size;
        if (st.st_size > 0 && (uint64_t)st.st_size > limit) {
            m.over = st.st_size;
        } else if (st.st_size > 0 && map_ok(fd, n)) {
            void *a = mmap(NULL, n, PROT_READ, MAP_PRIVATE, fd, 0);
            if (a != MAP_FAILED) {
                m.data = a;
                m.size = n;
                m.mapped = 1;
            }
        }
        if (st.st_size > 0 && !m.over && !m.data) {
            uint8_t *b = malloc(n);
            size_t got = 0;
            int e = b ? 0 : ENOMEM;
            while (!e && got < n) {
                ssize_t r = pread(fd, b + got, n - got, (off_t)got);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) e = r < 0 ? errno : EIO; /* cut short under us */
                else got += (size_t)r;
            }
            if (e) {
                VKB_WARN("pipeline cache %s could not be read (%s): starting without it", path, strerror(e));
                free(b);
            } else {
                m.data = b;
                m.size = n;
            }
        }
    }
    close(fd);
    return m;
}

static void unmap_file(pc_map *m)
{
    if (m->data && m->mapped) munmap(m->data, m->size);
    else free(m->data);
    m->data = NULL;
    m->size = 0;
    m->mapped = 0;
}

/* The cache's data written to path: next to it and renamed over it, so a kill mid-write loses
 * nothing; for a big cache on local storage (map_ok) straight from the driver into the file's
 * pages (its blocks reserved first, so a full storage is an error here and not a SIGBUS in the
 * driver), else through the heap, as where the file system cannot reserve blocks.
 * 0; 1: over limit (*size: how big); -1 with errno. */
static int save_to(const vkb_dispatch *r, VkDevice d, VkPipelineCache c, const char *path, uint64_t limit, size_t *size)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.%d.%u.tmp", path, (int)getpid(), __atomic_add_fetch(&tmp_seq, 1, __ATOMIC_RELAXED));
    for (int tries = 0; tries < 3; tries++) {
        size_t n = 0;
        *size = 0;
        if (r->vkGetPipelineCacheData(d, c, &n, NULL) != VK_SUCCESS || !n) {
            errno = EIO;
            return -1;
        }
        *size = n;
        if (n > limit) return 1;
        /* Room for what other threads compile into it meanwhile (the background compiler does,
         * all the time): the file is cut to what was written. */
        n += n / 16 + 4096;
        int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 && errno == ENOENT && vkb_cache_dir_ensure()) fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return -1;
        int e = 0;
        size_t got = n;
        VkResult vr = VK_ERROR_UNKNOWN;
        uint8_t *heap = NULL;
        void *map = MAP_FAILED;
        int use_map = map_ok(fd, n);
        if (use_map && fallocate(fd, 0, 0, (off_t)n) == 0) {
            map = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) e = errno;
            else vr = r->vkGetPipelineCacheData(d, c, &got, map); /* the fsync below writes the pages */
        } else if (use_map && (errno == ENOSPC || errno == EDQUOT || errno == EIO)) {
            e = errno;
        } else if (!(heap = malloc(n))) {
            e = ENOMEM;
        } else if ((vr = r->vkGetPipelineCacheData(d, c, &got, heap)) == VK_SUCCESS) {
            for (size_t done = 0; !e && done < got;) {
                ssize_t w = write(fd, heap + done, got - done);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) e = w < 0 ? errno : EIO;
                else done += (size_t)w;
            }
        }
        if (map != MAP_FAILED) munmap(map, n);
        free(heap);
        if (!e && vr == VK_SUCCESS) {
            if (got < n && ftruncate(fd, (off_t)got) != 0) e = errno;
            if (!e && fsync(fd) != 0) e = errno;
        }
        if (close(fd) != 0 && !e) e = errno;
        if (!e && vr == VK_SUCCESS && got > limit) { /* it grew past the limit into the margin */
            unlink(tmp);
            *size = got;
            return 1;
        }
        if (!e && vr == VK_SUCCESS) {
            if (rename(tmp, path) == 0) {
                *size = got;
                return 0;
            }
            e = errno;
        }
        unlink(tmp);
        if (!e && vr == VK_INCOMPLETE) continue; /* it grew between the two calls */
        errno = e ? e : EIO;
        return -1;
    }
    errno = EAGAIN; /* it kept growing faster than it was written: the next save tries again */
    return -1;
}

/* A driver cache made from a file's data (VK_NULL_HANDLE: not this GPU's, or refused). */
static VkPipelineCache cache_from(vkb_srv_table *t, const uint8_t *data, size_t size)
{
    if (!data || !header_ok(t, data, size)) return VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, size, data};
    VkPipelineCache c = VK_NULL_HANDLE;
    if (t->real.vkCreatePipelineCache(t->device, &ci, NULL, &c) != VK_SUCCESS) c = VK_NULL_HANDLE;
    return c;
}

/* merged: only ours with what another process saved is over (ours alone is not, but saving it
 * would drop theirs). */
static void warn_full(vkb_pcache *p, size_t size, int merged)
{
    if (p->full_warned) return;
    p->full_warned = 1;
    char lim[32];
    VKB_WARN("device %u: pipeline cache %s would be %zu KB%s, over the %s limit for one program (VKBRIDGE_PCACHE_MAX_MB): the file "
             "keeps what it has, pipelines compiled from now on are not saved (they stay in the driver's memory until the device goes)",
             p->t->id, p->path, size >> 10, merged ? " with what another process saved" : "", limit_str(lim, p->max));
}

/* A file over the limit (a lower limit than when it was saved) is not loaded, and the next save
 * replaces it: said, not done silently. */
static void warn_over(vkb_pcache *p, const char *path, off_t size)
{
    if (p->over_warned) return;
    p->over_warned = 1;
    char lim[32];
    VKB_WARN("device %u: %s is %lld KB, over the %s limit for one program (VKBRIDGE_PCACHE_MAX_MB): not loaded, and the next save replaces it", p->t->id, path,
             (long long)(size >> 10), limit_str(lim, p->max));
}

/* Merges the app's caches into ours: all of them (the device is going) or those compiled into
 * since the last time, but never the externally synchronized ones (see the top). */
static int merge_apps(vkb_pcache *p, int all)
{
    vkb_srv_table *t = p->t;
    int n = 0;
    pthread_mutex_lock(&p->alock);
    for (pc_app *a = p->apps; a; a = a->next) {
        if (a->extsync || (!all && !a->used)) continue;
        a->used = 0;
        pthread_rwlock_wrlock(&p->use);
        if (t->real.vkMergePipelineCaches(t->device, p->cache, 1, &a->cache) == VK_SUCCESS) n++;
        pthread_rwlock_unlock(&p->use);
    }
    pthread_mutex_unlock(&p->alock);
    return n;
}

/* The pre-caching files this cache took in are in its file now. */
static void consume_warm(vkb_pcache *p)
{
    for (int i = 0; i < p->nwarm; i++) {
        pc_ident now;
        /* Not if the replayer saved more since (the next start takes that in). */
        if (ident_stat(p->warm[i], &now) && ident_eq(&now, &p->warm_id[i]) && unlink(p->warm[i]) == 0)
            VKB_INFO("device %u: %s is in %s now; removed", p->t->id, p->warm[i], p->path);
    }
    p->nwarm = 0;
}

/* Called with save_lock held (every save in the server is, so the checks of the file hold). */
static void save(vkb_pcache *p)
{
    vkb_srv_table *t = p->t;
    __atomic_store_n(&p->dirty, 0, __ATOMIC_RELAXED); /* pipelines from now on make it dirty again */
    size_t own = 0;
    if (t->real.vkGetPipelineCacheData(t->device, p->cache, &own, NULL) != VK_SUCCESS || !own) return;
    __atomic_store_n(&p->mem_size, own, __ATOMIC_RELAXED);
    if (own == p->saved_size) {
        /* Nothing new was compiled (merges of what it already had leave the size alone). */
        if (p->nwarm && p->file.valid) consume_warm(p);
        return;
    }
    if (own > p->max) {
        warn_full(p, own, 0);
        return;
    }
    pc_ident now;
    int exists = ident_stat(p->path, &now);
    int changed = exists && !ident_eq(&now, &p->file);
    VkPipelineCache tmp = VK_NULL_HANDLE;
    int want_merge = exists && (changed || !p->covers);
    if (want_merge) {
        /* The driver holds ours, the file's and the merge while it is copied out: not when memory
         * is that short (the low-memory killer would take the server, and the game's cache with it). */
        uint64_t avail = vkb_memavail_kb(), need = (((uint64_t)own + (uint64_t)now.size) * 3) >> 10;
        if (avail && avail < need) {
            want_merge = 0;
            if (!p->nomerge_warned) {
                p->nomerge_warned = 1;
                VKB_WARN("device %u: pipeline cache %s saved without what another process saved: %llu KB of memory free, a merge needs about %llu KB",
                         t->id, p->path, (unsigned long long)avail, (unsigned long long)need);
            }
        }
    }
    if (p->not_loaded && exists && !want_merge) {
        /* Ours alone would replace a bigger file this run never loaded. */
        if (!p->keep_warned) {
            p->keep_warned = 1;
            VKB_INFO("device %u: pipeline cache %s not saved: the file was not loaded (memory) and a merge does not fit now; it is kept as it is",
                     t->id, p->path);
        }
        return;
    }
    if (want_merge) {
        /* Another process saved (or this one wrote a merge): ours goes on top of the file. */
        pc_map f = map_file(p->path, p->max, NULL);
        if (f.over) warn_over(p, p->path, f.over);
        tmp = cache_from(t, f.data, f.size);
        unmap_file(&f);
        if (tmp) {
            pthread_rwlock_rdlock(&p->use);
            VkResult r = t->real.vkMergePipelineCaches(t->device, tmp, 1, &p->cache);
            pthread_rwlock_unlock(&p->use);
            if (r != VK_SUCCESS) {
                t->real.vkDestroyPipelineCache(t->device, tmp, NULL);
                tmp = VK_NULL_HANDLE;
            }
        }
    }
    size_t size = 0;
    int merged = tmp != VK_NULL_HANDLE;
    int rc = save_to(&t->real, t->device, merged ? tmp : p->cache, p->path, p->max, &size);
    int e = errno;
    if (tmp) t->real.vkDestroyPipelineCache(t->device, tmp, NULL);
    if (rc == 1) {
        warn_full(p, size, merged);
        return;
    }
    if (rc == 0) {
        VKB_INFO("device %u: pipeline cache saved (%zu KB%s)", t->id, size >> 10, merged ? ", with what another process saved" : "");
        ident_stat(p->path, &p->file);
        p->saved_size = own;
        p->covers = !merged;
        p->save_err = 0;
        vkb_cache_write_ok();
        consume_warm(p);
    } else {
        /* Said once per cause, not at every try (every 10 s while the app compiles). EAGAIN: it
         * grew faster than it was written (compiles on other threads); the next save tries again. */
        if (e != EAGAIN && vkb_cache_write_failed("a pipeline cache", e) && e != p->save_err)
            VKB_WARN("device %u: pipeline cache not saved to %s: %s (tried again quietly)", t->id, p->path, strerror(e));
        p->save_err = e;
        __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
    }
}

/* Is a save due? Each writes the whole cache: a big one waits longer (PC_SAVE_BIG). */
static int save_due(const vkb_pcache *p, time_t now)
{
    if (p->warm_role) return now - p->last_save >= PC_WARM_SAVE_PERIOD_S;
    uint64_t period = PC_SAVE_PERIOD_S;
    if (p->saved_size > PC_SAVE_BIG) period = (uint64_t)PC_SAVE_PERIOD_S * p->saved_size / PC_SAVE_BIG;
    if (period > PC_SAVE_PERIOD_MAX_S) period = PC_SAVE_PERIOD_MAX_S;
    return now - p->last_save >= (time_t)period - 1; /* the saver wakes every PC_SAVE_PERIOD_S */
}

static time_t mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void *saver(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = {PC_SAVE_PERIOD_S, 0};
        nanosleep(&ts, NULL);
        /* A cache is unlinked under pc_lock and freed under save_lock (device_destroy), so
         * with save_lock held every cache reached through the list stays valid; pc_lock is held
         * while stepping the list so unlinks don't race with traversal. */
        pthread_mutex_lock(&save_lock);
        time_t now = mono_s();
        pthread_mutex_lock(&pc_lock);
        for (vkb_pcache *p = pc_list; p; p = p->next) {
            if (__atomic_load_n(&p->dirty, __ATOMIC_RELAXED) && save_due(p, now)) {
                merge_apps(p, 0);
                save(p);
                p->last_save = now;
            }
        }
        pthread_mutex_unlock(&pc_lock);
        pthread_mutex_unlock(&save_lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------ wrappers */

static void app_used(vkb_pcache *p, VkPipelineCache cache)
{
    pthread_mutex_lock(&p->alock);
    for (pc_app *a = p->apps; a; a = a->next) {
        if (a->cache != cache) continue;
        if (!a->extsync) {
            a->used = 1;
            __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
        }
        break;
    }
    pthread_mutex_unlock(&p->alock);
}

static VkResult VKAPI_CALL pc_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                      const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                      VkPipeline *out)
{
    vkb_pcache *p = cur();
    __atomic_add_fetch(&pc_made, n, __ATOMIC_RELAXED);
    if (cache) {
        app_used(p, cache);
        return p->create_gfx(device, cache, n, ci, a, out);
    }
    __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
    pthread_rwlock_rdlock(&p->use);
    VkResult r = p->create_gfx(device, p->cache, n, ci, a, out);
    pthread_rwlock_unlock(&p->use);
    return r;
}

static VkResult VKAPI_CALL pc_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                     const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                     VkPipeline *out)
{
    vkb_pcache *p = cur();
    __atomic_add_fetch(&pc_made, n, __ATOMIC_RELAXED);
    if (cache) {
        app_used(p, cache);
        return p->create_compute(device, cache, n, ci, a, out);
    }
    __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
    pthread_rwlock_rdlock(&p->use);
    VkResult r = p->create_compute(device, p->cache, n, ci, a, out);
    pthread_rwlock_unlock(&p->use);
    return r;
}

static VkResult VKAPI_CALL pc_CreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *ci,
                                                  const VkAllocationCallbacks *a, VkPipelineCache *out)
{
    vkb_pcache *p = cur();
    pc_app *app = calloc(1, sizeof(*app));
    if (!app) return VK_ERROR_OUT_OF_HOST_MEMORY;
    app->extsync = (ci->flags & VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT) != 0;
    VkResult r;
    if (ci->initialDataSize || app->extsync || __atomic_fetch_add(&p->seeded, 1, __ATOMIC_RELAXED) >= PC_SEED_CACHES) {
        if (!ci->initialDataSize && !__atomic_exchange_n(&p->seed_logged, 1, __ATOMIC_RELAXED))
            VKB_INFO("device %u: the app makes several pipeline caches; further empty ones start empty", p->t->id);
        r = p->create_cache(device, ci, a, out);
    } else {
        size_t size = 0;
        uint8_t *buf = NULL;
        if (p->t->real.vkGetPipelineCacheData(device, p->cache, &size, NULL) == VK_SUCCESS && size && size <= PC_SEED_MAX &&
            (buf = malloc(size)) && p->t->real.vkGetPipelineCacheData(device, p->cache, &size, buf) != VK_SUCCESS)
            size = 0;
        VkPipelineCacheCreateInfo seeded = *ci;
        seeded.initialDataSize = buf ? size : 0;
        seeded.pInitialData = buf;
        r = p->create_cache(device, &seeded, a, out);
        free(buf);
    }
    if (r != VK_SUCCESS) {
        free(app);
        return r;
    }
    app->cache = *out;
    pthread_mutex_lock(&p->alock);
    app->next = p->apps;
    p->apps = app;
    pthread_mutex_unlock(&p->alock);
    return r;
}

static void VKAPI_CALL pc_DestroyPipelineCache(VkDevice device, VkPipelineCache cache, const VkAllocationCallbacks *a)
{
    vkb_pcache *p = cur();
    if (!cache) return;
    pthread_mutex_lock(&p->alock);
    pc_app *app = NULL;
    for (pc_app **pp = &p->apps; *pp; pp = &(*pp)->next) {
        if ((*pp)->cache == cache) {
            app = *pp;
            *pp = app->next;
            break;
        }
    }
    if (cache != p->cache && !(app && app->extsync)) {
        pthread_rwlock_wrlock(&p->use);
        if (p->t->real.vkMergePipelineCaches(device, p->cache, 1, &cache) == VK_SUCCESS)
            __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
        pthread_rwlock_unlock(&p->use);
    }
    pthread_mutex_unlock(&p->alock);
    free(app);
    p->destroy_cache(device, cache, a);
}

/* ------------------------------------------------------------------ device hooks */

static void file_name(char *out, size_t cap, const char *s)
{
    snprintf(out, cap, "%s", s);
    for (char *c = out; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '.')) *c = '_';
    if (out[0] == '.') out[0] = '_';
}

void vkb_pcache_device_init(vkb_srv_table *t)
{
    if (!pc_dir || !t->real.vkCreatePipelineCache || !t->real.vkGetPipelineCacheData || !t->real.vkMergePipelineCaches) return;
    const char *off = vkb_srv_env("VKBRIDGE_NO_PCACHE");
    if (off && *off && strcmp(off, "0")) {
        VKB_INFO("device %u: persistent pipeline cache off (VKBRIDGE_NO_PCACHE)", t->id);
        return;
    }
    vkb_pcache *p = calloc(1, sizeof(*p));
    if (!p) return;
    p->t = t;
    p->max = file_limit();
    char lim[32];
    VkPhysicalDeviceProperties props;
    t->real.vkGetPhysicalDeviceProperties(t->physical, &props);
    char uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", props.pipelineCacheUUID[i]);
    char base[400], prog[48] = "any", key[64] = "";
    snprintf(base, sizeof(base), "%s/%04x-%04x-%s-", pc_dir, props.vendorID, props.deviceID, uuid);
    if (t->proc && t->proc->comm[0]) file_name(prog, sizeof(prog), t->proc->comm);
    if (t->proc && t->proc->pkey[0]) file_name(key, sizeof(key), t->proc->pkey);
    snprintf(p->path, sizeof(p->path), "%s%s.vkpc", base, key[0] ? key : prog);
    p->warm_role = strchr(key, '.') != NULL;
    p->last_save = mono_s();
    pthread_mutex_lock(&pc_lock);
    trim(p->path, 0);
    pthread_mutex_unlock(&pc_lock);
    /* Used now: the last the total limit removes (before it is read, so what is read is the file as it is). */
    if (!p->warm_role) utimensat(AT_FDCWD, p->path, NULL, 0);

    pc_map fm = {NULL, 0, 0, 0};
    if (!p->warm_role) fm = map_file(p->path, p->max, &p->file);
    if (fm.over) warn_over(p, p->path, fm.over);
    const char *from = p->path;
    char legacy[512];
    if (!fm.data && !p->file.valid && key[0] && !p->warm_role) {
        /* Before keys, the program's cache was by process name: start from that one. */
        snprintf(legacy, sizeof(legacy), "%s%s.vkpc", base, prog);
        if ((fm = map_file(legacy, p->max, NULL)).data) from = legacy;
    }
    if (fm.data && !header_ok(t, fm.data, fm.size)) {
        VKB_WARN("device %u: %s is not this GPU's pipeline cache; starting empty", t->id, from);
        unmap_file(&fm);
    }
    uint64_t avail_kb = vkb_memavail_kb();
    if (fm.data && from == p->path && avail_kb && avail_kb < ((uint64_t)fm.size * 2 >> 10)) {
        /* The driver copies it into its heap: not when that is half of what is free. Kept for a
         * later start; this run's pipelines go into it by merges when memory allows. */
        VKB_WARN("device %u: pipeline cache %s (%zu KB) not loaded this time: %llu KB of memory free; the file is kept", t->id,
                 p->path, fm.size >> 10, (unsigned long long)avail_kb);
        unmap_file(&fm);
        p->not_loaded = 1;
    }
    const uint8_t *data = fm.data;
    size_t size = fm.size;
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, data ? size : 0, data};
    VkResult r = t->real.vkCreatePipelineCache(t->device, &ci, NULL, &p->cache);
    if (r != VK_SUCCESS && data) {
        VKB_WARN("device %u: driver refused the saved pipeline cache (%d); starting empty", t->id, r);
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        size = 0;
        r = t->real.vkCreatePipelineCache(t->device, &ci, NULL, &p->cache);
    }
    int mapped = fm.mapped && size;
    unmap_file(&fm);
    data = NULL;
    if (r != VK_SUCCESS) {
        VKB_WARN("device %u: no pipeline cache (%d)", t->id, r);
        free(p);
        return;
    }
    p->saved_size = from == p->path ? size : 0;
    p->mem_size = size;
    p->covers = !p->not_loaded;
    if (p->warm_role)
        VKB_INFO("device %u: pipeline cache %s (Steam's shader pre-caching: starts empty, adds to the file)", t->id, p->path);
    else
        VKB_INFO("device %u: pipeline cache %s (%zu KB loaded%s%s), limit %s%s", t->id, p->path, size >> 10, from == p->path ? "" : " from ",
                 from == p->path ? "" : from, limit_str(lim, p->max), mapped ? ", mapped" : "");

    /* A game takes in what Steam's shader pre-caching compiled for it. */
    size_t total = size;
    for (int i = 0; key[0] && !p->warm_role && i < PC_WARM_KINDS; i++) {
        char wpath[512];
        snprintf(wpath, sizeof(wpath), "%s%s.%s.vkpc", base, key, warm_kinds[i]);
        pc_ident wid;
        pc_map wm = map_file(wpath, p->max, &wid);
        size_t wsize = wm.data ? wm.size : (size_t)wm.over;
        if (!wsize) continue;
        VkPipelineCache tmp = VK_NULL_HANDLE;
        if (total + wsize > p->max)
            VKB_WARN("device %u: %s (%zu KB) left out: over the %s limit with the rest (VKBRIDGE_PCACHE_MAX_MB)", t->id, wpath,
                     wsize >> 10, limit_str(lim, p->max));
        else if (!(tmp = cache_from(t, wm.data, wm.size)))
            VKB_WARN("device %u: %s is not this GPU's pipeline cache; left out", t->id, wpath);
        else if (t->real.vkMergePipelineCaches(t->device, p->cache, 1, &tmp) == VK_SUCCESS) {
            VKB_INFO("device %u: %zu KB of Steam's shader pre-caching taken in from %s", t->id, wsize >> 10, wpath);
            total += wsize;
            snprintf(p->warm[p->nwarm], sizeof(p->warm[0]), "%s", wpath);
            p->warm_id[p->nwarm++] = wid;
            p->dirty = 1;
        }
        if (tmp) t->real.vkDestroyPipelineCache(t->device, tmp, NULL);
        unmap_file(&wm);
    }

    pthread_rwlockattr_t ra;
    pthread_rwlockattr_init(&ra);
    /* The saver must get its turn while a game compiles on every thread. */
    pthread_rwlockattr_setkind_np(&ra, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&p->use, &ra);
    pthread_rwlockattr_destroy(&ra);
    pthread_mutex_init(&p->alock, NULL);

    vkb_dispatch *dt = &t->dt;
    p->create_gfx = dt->vkCreateGraphicsPipelines;
    p->create_compute = dt->vkCreateComputePipelines;
    p->create_cache = dt->vkCreatePipelineCache;
    p->destroy_cache = dt->vkDestroyPipelineCache;
    dt->vkCreateGraphicsPipelines = pc_CreateGraphicsPipelines;
    dt->vkCreateComputePipelines = pc_CreateComputePipelines;
    dt->vkCreatePipelineCache = pc_CreatePipelineCache;
    dt->vkDestroyPipelineCache = pc_DestroyPipelineCache;
    t->pcache = p;

    pthread_mutex_lock(&pc_lock);
    p->next = pc_list;
    pc_list = p;
    if (!pc_thread_started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pc_thread_started = pthread_create(&th, &at, saver, NULL) == 0;
        pthread_attr_destroy(&at);
    }
    pthread_mutex_unlock(&pc_lock);
}

VkResult vkb_pcache_create_compute(vkb_srv_table *t, const VkComputePipelineCreateInfo *ci, VkPipeline *out)
{
    vkb_pcache *p = t->pcache;
    if (!p) return t->real.vkCreateComputePipelines(t->device, VK_NULL_HANDLE, 1, ci, NULL, out);
    __atomic_add_fetch(&pc_made, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
    pthread_rwlock_rdlock(&p->use);
    VkResult r = t->real.vkCreateComputePipelines(t->device, p->cache, 1, ci, NULL, out);
    pthread_rwlock_unlock(&p->use);
    return r;
}

void vkb_pcache_stats(uint64_t *pipelines, uint64_t *bytes)
{
    *pipelines = __atomic_load_n(&pc_made, __ATOMIC_RELAXED);
    *bytes = 0;
    pthread_mutex_lock(&pc_lock);
    for (vkb_pcache *p = pc_list; p; p = p->next) *bytes += __atomic_load_n(&p->mem_size, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&pc_lock);
}

/* ------------------------------------------------------------------ the server's own compiles */

/* A cache for compiles the server makes on a device of its own (the startup probe's transcoder
 * builds): <dir>/<vendor>-<device>-<cacheUUID>-<name>.vkpc, loaded here, written back by
 * vkb_pcache_own_close. VK_NULL_HANDLE without a cache directory or with VKBRIDGE_NO_PCACHE. */
VkPipelineCache vkb_pcache_own_open(const vkb_dispatch *r, const VkPhysicalDeviceProperties *gpu, VkDevice d, const char *name, char *path,
                                    size_t cap)
{
    path[0] = 0;
    const char *off = vkb_srv_env("VKBRIDGE_NO_PCACHE");
    if (!pc_dir || (off && *off && strcmp(off, "0")) || !r->vkCreatePipelineCache || !r->vkGetPipelineCacheData) return VK_NULL_HANDLE;
    const VkPhysicalDeviceProperties props = *gpu;
    char uuid[2 * VK_UUID_SIZE + 1];
    for (int i = 0; i < VK_UUID_SIZE; i++) snprintf(uuid + 2 * i, 3, "%02x", props.pipelineCacheUUID[i]);
    snprintf(path, cap, "%s/%04x-%04x-%s-%s.vkpc", pc_dir, props.vendorID, props.deviceID, uuid, name);
    pc_map m = map_file(path, file_limit(), NULL);
    if (m.data && !header_for(&props, m.data, m.size)) unmap_file(&m);
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, m.size, m.data};
    VkPipelineCache c = VK_NULL_HANDLE;
    if (r->vkCreatePipelineCache(d, &ci, NULL, &c) != VK_SUCCESS && m.data) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        if (r->vkCreatePipelineCache(d, &ci, NULL, &c) != VK_SUCCESS) c = VK_NULL_HANDLE;
    }
    unmap_file(&m);
    return c;
}

/* Written back when it grew (the build added to it), then destroyed. */
void vkb_pcache_own_close(const vkb_dispatch *r, VkDevice d, VkPipelineCache c, const char *path)
{
    if (!c) return;
    size_t size = 0, had = 0;
    struct stat st;
    if (path[0] && stat(path, &st) == 0) had = (size_t)st.st_size;
    if (path[0] && r->vkGetPipelineCacheData(d, c, &size, NULL) == VK_SUCCESS && size > had) {
        pthread_mutex_lock(&save_lock);
        int rc = save_to(r, d, c, path, file_limit(), &size), e = errno;
        pthread_mutex_unlock(&save_lock);
        if (rc == 0) VKB_INFO("pipeline cache %s saved (%zu KB)", path, size >> 10);
        else if (rc == 1) VKB_WARN("pipeline cache %s would be %zu KB, over the limit for one program (VKBRIDGE_PCACHE_MAX_MB); not saved", path, size >> 10);
        else if (vkb_cache_write_failed("a pipeline cache", e)) VKB_WARN("pipeline cache %s not saved: %s", path, strerror(e));
    }
    r->vkDestroyPipelineCache(d, c, NULL);
}

/* Before the device is destroyed (by the app, or after its process went): what the app's caches
 * hold goes into ours, a last save, then the caches go. */
void vkb_pcache_device_destroy(vkb_srv_table *t)
{
    vkb_pcache *p = t->pcache;
    if (!p) return;
    pthread_mutex_lock(&save_lock); /* serialize with saver: p stays valid and caches are untouched */
    pthread_mutex_lock(&pc_lock);
    for (vkb_pcache **pp = &pc_list; *pp; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    }
    pthread_mutex_unlock(&pc_lock);
    if (merge_apps(p, 1)) p->dirty = 1;
    if (p->dirty) save(p);
    t->dt.vkCreateGraphicsPipelines = p->create_gfx;
    t->dt.vkCreateComputePipelines = p->create_compute;
    t->dt.vkCreatePipelineCache = p->create_cache;
    t->dt.vkDestroyPipelineCache = p->destroy_cache;
    for (pc_app *a = p->apps, *next; a; a = next) {
        next = a->next;
        p->destroy_cache(t->device, a->cache, NULL); /* left behind by the app */
        free(a);
    }
    t->real.vkDestroyPipelineCache(t->device, p->cache, NULL);
    pthread_rwlock_destroy(&p->use);
    pthread_mutex_destroy(&p->alock);
    t->pcache = NULL;
    free(p);
    pthread_mutex_unlock(&save_lock);
}
