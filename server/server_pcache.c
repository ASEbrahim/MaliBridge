/* SPDX-License-Identifier: GPL-3.0-or-later
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* One file per program, at most 64 MB: a single shared file grew to 58 MB over a night of games
 * and every device (gamescope, Xwayland, Steam, each game) loaded all of it, and every cache an app
 * made was seeded with another copy - the server ran out of memory in vkCreatePipelineCache. With
 * a file per game (and pre-caching folded into it) 32 MB was too tight. */
#define PC_MAX_FILE (64u << 20)
/* App-made caches start from the program's cache only while it is this small, and only the first
 * few: Zink makes one per GL program (and keeps each in Mesa's disk cache), so every one of them
 * got - and wrote back to Mesa's cache, and copied over the socket - the whole Steam UI cache. */
#define PC_SEED_MAX (8u << 20)
#define PC_SEED_CACHES 2
#define PC_SAVE_PERIOD_S 10
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
    pc_ident file;        /* the file as this cache last read or wrote it */
    int covers;           /* the file holds nothing `cache` lacks (it was written from it alone) */
    int full_warned;
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
static pthread_mutex_t pc_lock = PTHREAD_MUTEX_INITIALIZER; /* the list, and every save */
static vkb_pcache *pc_list;
static int pc_thread_started;

const char *vkb_pcache_dir(void) { return pc_dir; }

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

void vkb_srv_env_reload(void)
{
    if (!pc_dir) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/env", pc_dir);
    FILE *f = fopen(path, "r");
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
    pthread_mutex_unlock(&env_lock);
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

const char *vkb_srv_env(const char *name)
{
    const char *v = NULL;
    pthread_mutex_lock(&env_lock);
    for (unsigned i = 0; i < env_n && !v; i++)
        if (!strcmp(env_tab[i].key, name)) v = env_tab[i].val;
    pthread_mutex_unlock(&env_lock);
    return v ? v : getenv(name);
}

void vkb_pcache_set_dir(const char *dir)
{
    if (!dir || !*dir) return;
    mkdir(dir, 0700);
    /* A copy: the in-app start frees its string after server_init (The412Banner, e40cd361). */
    char *copy = strdup(dir);
    if (!copy) return;
    free((char *)pc_dir);
    pc_dir = copy;
    VKB_INFO("pipeline cache directory %s", dir);
}

static vkb_pcache *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->pcache : NULL;
}

/* The header every pipeline cache starts with (VkPipelineCacheHeaderVersionOne). */
static int header_ok(const vkb_srv_table *t, const uint8_t *data, size_t size)
{
    VkPipelineCacheHeaderVersionOne h;
    if (size < sizeof(h)) return 0;
    memcpy(&h, data, sizeof(h));
    VkPhysicalDeviceProperties props;
    t->real.vkGetPhysicalDeviceProperties(t->physical, &props);
    return h.headerSize >= sizeof(h) && h.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           h.vendorID == props.vendorID && h.deviceID == props.deviceID &&
           !memcmp(h.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE);
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

/* The file's contents (NULL: missing, empty, over the limit or unreadable); *id: what was read. */
static uint8_t *read_file(const char *path, size_t *size, pc_ident *id)
{
    *size = 0;
    if (id) memset(id, 0, sizeof(*id));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (fstat(fd, &st) == 0) {
        if (id) ident_of(&st, id);
        if (st.st_size > 0 && (uint64_t)st.st_size <= PC_MAX_FILE && (buf = malloc((size_t)st.st_size))) {
            size_t got = 0;
            while (got < (size_t)st.st_size) {
                ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
                if (r <= 0) break;
                got += (size_t)r;
            }
            if (got == (size_t)st.st_size) {
                *size = got;
            } else {
                free(buf);
                buf = NULL;
            }
        }
    }
    close(fd);
    return buf;
}

/* Written next to the file and renamed over it, so a kill mid-write loses nothing. */
static int write_file(const char *path, const uint8_t *buf, size_t size)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int ok = fd >= 0;
    for (size_t done = 0; ok && done < size;) {
        ssize_t w = write(fd, buf + done, size - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = 0;
        else done += (size_t)w;
    }
    if (fd >= 0 && fsync(fd) != 0) ok = 0;
    if (fd >= 0) close(fd);
    if (ok && rename(tmp, path) == 0) return 0;
    int e = errno;
    unlink(tmp);
    errno = e;
    return -1;
}

/* The cache's data, malloc'd (NULL: none, or over the limit when *size says so). */
static uint8_t *cache_data(vkb_srv_table *t, VkPipelineCache c, size_t *size)
{
    *size = 0;
    if (t->real.vkGetPipelineCacheData(t->device, c, size, NULL) != VK_SUCCESS || !*size || *size > PC_MAX_FILE) return NULL;
    uint8_t *buf = malloc(*size);
    if (buf && t->real.vkGetPipelineCacheData(t->device, c, size, buf) != VK_SUCCESS) {
        free(buf);
        buf = NULL;
        *size = 0;
    }
    return buf;
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

static void warn_full(vkb_pcache *p, size_t size)
{
    if (p->full_warned) return;
    p->full_warned = 1;
    VKB_WARN("device %u: pipeline cache %s would be %zu KB, over the %u MB limit; it keeps what it has", p->t->id, p->path,
             size >> 10, PC_MAX_FILE >> 20);
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

/* Called with pc_lock held (every save in the server is, so the checks of the file hold). */
static void save(vkb_pcache *p)
{
    vkb_srv_table *t = p->t;
    __atomic_store_n(&p->dirty, 0, __ATOMIC_RELAXED); /* pipelines from now on make it dirty again */
    size_t own = 0;
    if (t->real.vkGetPipelineCacheData(t->device, p->cache, &own, NULL) != VK_SUCCESS || !own) return;
    if (own == p->saved_size) {
        /* Nothing new was compiled (merges of what it already had leave the size alone). */
        if (p->nwarm && p->file.valid) consume_warm(p);
        return;
    }
    if (own > PC_MAX_FILE) {
        warn_full(p, own);
        return;
    }
    pc_ident now;
    int exists = ident_stat(p->path, &now);
    int changed = exists && !ident_eq(&now, &p->file);
    uint8_t *out = NULL;
    size_t size = 0;
    int merged = 0;
    if (exists && (changed || !p->covers)) {
        /* Another process saved (or this one wrote a merge): ours goes on top of the file. */
        size_t fsize;
        uint8_t *fdata = read_file(p->path, &fsize, NULL);
        VkPipelineCache tmp = cache_from(t, fdata, fsize);
        free(fdata);
        if (tmp) {
            pthread_rwlock_rdlock(&p->use);
            VkResult r = t->real.vkMergePipelineCaches(t->device, tmp, 1, &p->cache);
            pthread_rwlock_unlock(&p->use);
            if (r == VK_SUCCESS) {
                out = cache_data(t, tmp, &size);
                if (!out && size > PC_MAX_FILE) {
                    t->real.vkDestroyPipelineCache(t->device, tmp, NULL);
                    warn_full(p, size);
                    return;
                }
                merged = out != NULL;
            }
            t->real.vkDestroyPipelineCache(t->device, tmp, NULL);
        }
    }
    if (!out) out = cache_data(t, p->cache, &size);
    if (!out) {
        __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
        return;
    }
    if (write_file(p->path, out, size) == 0) {
        VKB_INFO("device %u: pipeline cache saved (%zu KB%s)", t->id, size >> 10, merged ? ", with what another process saved" : "");
        ident_stat(p->path, &p->file);
        p->saved_size = own;
        p->covers = !merged;
        consume_warm(p);
    } else {
        VKB_WARN("device %u: pipeline cache not saved to %s: %s", t->id, p->path, strerror(errno));
        __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
    }
    free(out);
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
        pthread_mutex_lock(&pc_lock);
        time_t now = mono_s();
        for (vkb_pcache *p = pc_list; p; p = p->next) {
            if (!__atomic_load_n(&p->dirty, __ATOMIC_RELAXED)) continue;
            if (p->warm_role && now - p->last_save < PC_WARM_SAVE_PERIOD_S) continue;
            merge_apps(p, 0);
            save(p);
            p->last_save = now;
        }
        pthread_mutex_unlock(&pc_lock);
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

    size_t size = 0;
    uint8_t *data = p->warm_role ? NULL : read_file(p->path, &size, &p->file);
    const char *from = p->path;
    char legacy[512];
    if (!data && !p->file.valid && key[0] && !p->warm_role) {
        /* Before keys, the program's cache was by process name: start from that one. */
        snprintf(legacy, sizeof(legacy), "%s%s.vkpc", base, prog);
        if ((data = read_file(legacy, &size, NULL))) from = legacy;
    }
    if (data && !header_ok(t, data, size)) {
        VKB_WARN("device %u: %s is not this GPU's pipeline cache; starting empty", t->id, from);
        free(data);
        data = NULL;
        size = 0;
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, data ? size : 0, data};
    VkResult r = t->real.vkCreatePipelineCache(t->device, &ci, NULL, &p->cache);
    if (r != VK_SUCCESS && data) {
        VKB_WARN("device %u: driver refused the saved pipeline cache (%d); starting empty", t->id, r);
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        size = 0;
        r = t->real.vkCreatePipelineCache(t->device, &ci, NULL, &p->cache);
    }
    free(data);
    if (r != VK_SUCCESS) {
        VKB_WARN("device %u: no pipeline cache (%d)", t->id, r);
        free(p);
        return;
    }
    p->saved_size = from == p->path ? size : 0;
    p->covers = 1;
    if (p->warm_role)
        VKB_INFO("device %u: pipeline cache %s (Steam's shader pre-caching: starts empty, adds to the file)", t->id, p->path);
    else
        VKB_INFO("device %u: pipeline cache %s (%zu KB loaded%s%s)", t->id, p->path, size >> 10, from == p->path ? "" : " from ",
                 from == p->path ? "" : from);

    /* A game takes in what Steam's shader pre-caching compiled for it. */
    size_t total = size;
    for (int i = 0; key[0] && !p->warm_role && i < PC_WARM_KINDS; i++) {
        char wpath[512];
        snprintf(wpath, sizeof(wpath), "%s%s.%s.vkpc", base, key, warm_kinds[i]);
        size_t wsize;
        pc_ident wid;
        uint8_t *w = read_file(wpath, &wsize, &wid);
        if (!w) continue;
        VkPipelineCache tmp = VK_NULL_HANDLE;
        if (total + wsize > PC_MAX_FILE)
            VKB_WARN("device %u: %s (%zu KB) left out: over the %u MB limit with the rest", t->id, wpath, wsize >> 10, PC_MAX_FILE >> 20);
        else if (!(tmp = cache_from(t, w, wsize)))
            VKB_WARN("device %u: %s is not this GPU's pipeline cache; left out", t->id, wpath);
        else if (t->real.vkMergePipelineCaches(t->device, p->cache, 1, &tmp) == VK_SUCCESS) {
            VKB_INFO("device %u: %zu KB of Steam's shader pre-caching taken in from %s", t->id, wsize >> 10, wpath);
            total += wsize;
            snprintf(p->warm[p->nwarm], sizeof(p->warm[0]), "%s", wpath);
            p->warm_id[p->nwarm++] = wid;
            p->dirty = 1;
        }
        if (tmp) t->real.vkDestroyPipelineCache(t->device, tmp, NULL);
        free(w);
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
    __atomic_store_n(&p->dirty, 1, __ATOMIC_RELAXED);
    pthread_rwlock_rdlock(&p->use);
    VkResult r = t->real.vkCreateComputePipelines(t->device, p->cache, 1, ci, NULL, out);
    pthread_rwlock_unlock(&p->use);
    return r;
}

/* Before the device is destroyed (by the app, or after its process went): what the app's caches
 * hold goes into ours, a last save, then the caches go. */
void vkb_pcache_device_destroy(vkb_srv_table *t)
{
    vkb_pcache *p = t->pcache;
    if (!p) return;
    pthread_mutex_lock(&pc_lock);
    for (vkb_pcache **pp = &pc_list; *pp; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    }
    if (merge_apps(p, 1)) p->dirty = 1;
    if (p->dirty) save(p);
    pthread_mutex_unlock(&pc_lock);
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
}
