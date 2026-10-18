/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Per-game presets, by Steam app id. A client says its app id in its hello (client/conn.c
 * pcache_key: "steam<appid>", with ".dl"/".run" for Steam's shader pre-caching of that game); at
 * that hello the process gets its preset, for its own devices only:
 *   - <cache dir>/presets/<appid>.env, written by a tester or a user: KEY=VALUE lines, the same
 *     keys as <cache dir>/env, '#' comments and blank lines skipped;
 *   - the built-in preset, presets_builtin.inc, compiled in (data only: one line per game), taken
 *     only on a GPU it was tested on: each entry lists deviceName substrings ("Mali-G720"); when
 *     none of them is in the name of a GPU the server drives (its self-test's), or the list is
 *     empty, the entry is not applied and its lines are said once, to copy into the file.
 *
 * Where a switch comes from, first found wins (vkb_srv_env_src, server_pcache.c):
 *   1. <cache dir>/presets/<appid>.env   the user's own choice for that one game
 *   2. <cache dir>/env                   the developer's and tester's global override, read at each device
 *   3. the server's environment
 *   4. the built-in preset
 *   5. the mode's default                (server_mode.c; VKBRIDGE_MODE itself: Setup's switch, then safe)
 * Someone's own file for a game is the most specific choice there is, so it beats the global
 * places; those still beat the built-in presets, so one line in <cache dir>/env holds for every
 * game's built-in preset, a stale one included ("everything with the fix off"). A preset beats
 * the mode's defaults and, for VKBRIDGE_MODE, Setup's switch.
 *
 * Only the keys below are taken from a preset: those the server reads on the client's connection
 * thread and keeps per device (or per call). A switch the server reads once for all processes
 * (VKBRIDGE_LAYER_GS_FOLD, VKBRIDGE_DISCARD_SHADERS, the probe's and the test imitations) would
 * leak from one game into the next; a preset line naming one is ignored with a warning, as is a
 * line that is not KEY=VALUE. The client's switches (descriptor batching, the query mirror) go to
 * the client in vkbQueryServer's reply (vkb_server_pd_info.client_knobs, a field that was
 * reserved: no protocol change), where the guest's own environment still beats them.
 *
 * VKBRIDGE_MODE in a preset puts that game's devices in that mode (their emulation masks, errata
 * and compile watchdog are chosen at device creation). The server's mode is one value, so with two
 * games of different modes at once the session report's first line shows the last device's.
 *
 * Logged once per app id and preset: "preset for steam<appid>: KEY=VALUE ... (from file), ...
 * (from built-in)", and for a built-in entry not tested on this GPU "preset for steam<appid>: a
 * built-in preset exists but was not tested on <gpu>; not applied. To try it, copy it to
 * <cache dir>/presets/<appid>.env: KEY=VALUE ...". The session report lists each process's preset
 * and that line.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRESET_MAX 32

typedef struct preset_kv {
    char key[48];
    char val[128];
    int src; /* VKB_ENV_SRC_PRESET_FILE / _BUILTIN */
} preset_kv;

struct vkb_preset {
    char appid[24];
    unsigned n;
    preset_kv kv[PRESET_MAX];
    char untested[512]; /* built-in lines not applied: not tested on this GPU */
    char note[1100];    /* the line saying so, or "" */
};

static const struct {
    const char *appid, *gpus, *kv, *why;
} builtin[] = {
#include "presets_builtin.inc"
    {NULL, NULL, NULL, NULL},
};

/* The keys a preset may set (see above) */
static const char *const keys[] = {
    /* the mode, and what it decides per device */
    "VKBRIDGE_MODE", "VKBRIDGE_ERRATA", "VKBRIDGE_COMPILE_TIMEOUT_S", "VKBRIDGE_COMPILE_FALLBACKS", "VKBRIDGE_EMU_OFF",
    /* textures, descriptors, shaders */
    "VKBRIDGE_BC_TRANSCODE", "VKBRIDGE_BC_TRANSCODE_QUALITY", "VKBRIDGE_DESC_CUT", "VKBRIDGE_VIEW_QUARANTINE", "VKBRIDGE_SPIRV_FIX",
    "VKBRIDGE_NO_XFB_FOLD", "VKBRIDGE_NO_NULLDESC", "VKBRIDGE_NULLDESC_2D", "VKBRIDGE_NO_ELASTIC_POOLS", "VKBRIDGE_FLIP_RETAIN_MB",
    "VKBRIDGE_BITWISE_ALL", "VKBRIDGE_NO_COMPRESSION", "VKBRIDGE_NO_FORCED_ROBUSTNESS", "VKBRIDGE_NO_PCACHE",
    /* extensions */
    "VKBRIDGE_HIDE_CALIBRATED_TS",
    /* hangs */
    "VKBRIDGE_HANG_DETECT", "VKBRIDGE_HANG_AFTER_MS", "VKBRIDGE_SKIP_SHADERS",
    /* the client's (vkb_preset_client_knobs) */
    "VKBRIDGE_DESC_BATCH", "VKBRIDGE_NO_DESC_BATCH", "VKBRIDGE_QUERY_MIRROR",
};

static int allowed(const char *k)
{
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!strcmp(keys[i], k)) return 1;
    return 0;
}

/* Each text logged once (app ids and their presets, bad lines): Steam's pre-caching starts a
 * process per archive, and each would say it again. */
static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t once_seen[128];
static unsigned once_n;

static int first_time(const char *text)
{
    uint64_t h = 1469598103934665603ull;
    for (const char *s = text; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    pthread_mutex_lock(&once_lock);
    int seen = 0;
    for (unsigned i = 0; i < once_n && i < 128 && !seen; i++) seen = once_seen[i] == h;
    if (!seen) once_seen[once_n++ % 128] = h;
    pthread_mutex_unlock(&once_lock);
    return !seen;
}

/* "steam<digits>", then the end or '.': the digits, else "" */
static void app_id(const char *pkey, char *out, size_t cap)
{
    out[0] = 0;
    if (strncmp(pkey, "steam", 5)) return;
    const char *d = pkey + 5;
    size_t n = strspn(d, "0123456789");
    if (!n || n >= cap || (d[n] && d[n] != '.')) return;
    memcpy(out, d, n);
    out[n] = 0;
}

static void put(struct vkb_preset *p, const char *k, const char *v, int src)
{
    preset_kv *kv = NULL;
    for (unsigned i = 0; i < p->n && !kv; i++)
        if (!strcmp(p->kv[i].key, k)) kv = &p->kv[i];
    if (!kv && p->n < PRESET_MAX) kv = &p->kv[p->n++];
    if (!kv) return;
    snprintf(kv->key, sizeof(kv->key), "%s", k);
    snprintf(kv->val, sizeof(kv->val), "%s", v);
    kv->src = src;
}

static void warn_once(const char *fmt, const char *where, int line, const char *text)
{
    char msg[400];
    snprintf(msg, sizeof(msg), fmt, where, line, text);
    if (first_time(msg)) VKB_WARN("%s", msg);
}

/* One KEY=VALUE (leading and trailing blanks gone); 1 when taken */
static int take(struct vkb_preset *p, char *s, int src, const char *where, int line)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) s[--len] = 0;
    if (!*s || *s == '#') return 0;
    char *eq = strchr(s, '=');
    size_t kl = eq ? (size_t)(eq - s) : 0;
    int ok = kl > 0 && kl < sizeof(p->kv[0].key) && strlen(eq + 1) < sizeof(p->kv[0].val);
    for (size_t i = 0; ok && i < kl; i++) ok = (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_';
    if (!ok) {
        warn_once("preset: %s line %d: \"%.120s\" is not KEY=VALUE; ignored", where, line, s);
        return 0;
    }
    *eq = 0;
    if (!allowed(s)) {
        warn_once("preset: %s line %d: %s is not a per-game switch (the server reads it once for every process, or it is not one of "
                  "the bridge's); ignored - <cache dir>/env takes it for all games",
                  where, line, s);
        return 0;
    }
    put(p, s, eq + 1, src);
    return 1;
}

/* A built-in entry's "KEY=VALUE KEY=VALUE" */
static void take_words(struct vkb_preset *p, const char *text, int src, const char *where)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", text);
    int n = 0;
    for (char *save = NULL, *w = strtok_r(buf, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) take(p, w, src, where, ++n);
}

/* The GPUs the server drives (its self-test's), "a, b"; "an unknown GPU" before the self-test */
static void gpu_names(char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < vkb_npds && o < cap; i++)
        if (vkb_pds[i].name[0]) o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? ", " : "", vkb_pds[i].name);
    if (!out[0]) snprintf(out, cap, "an unknown GPU");
}

/* 1 when a GPU the server drives has one of tested's comma-separated names in its deviceName */
static int tested_here(const char *tested)
{
    for (const char *t = tested; t && *t;) {
        size_t n = strcspn(t, ",");
        char one[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
        snprintf(one, sizeof(one), "%.*s", (int)n, t);
        for (int i = 0; one[0] && i < vkb_npds; i++)
            if (strstr(vkb_pds[i].name, one)) return 1;
        t += n;
        if (*t) t++;
    }
    return 0;
}

/* One built-in entry: taken on a GPU it was tested on, else kept aside to be said */
static void take_entry(struct vkb_preset *p, const char *gpus, const char *kv, const char *where)
{
    if (tested_here(gpus)) {
        take_words(p, kv, VKB_ENV_SRC_PRESET_BUILTIN, where);
        return;
    }
    size_t o = strlen(p->untested);
    if (o < sizeof(p->untested) - 1) snprintf(p->untested + o, sizeof(p->untested) - o, "%s%s", o ? " " : "", kv);
}

static void load_builtin(struct vkb_preset *p)
{
    char where[64];
    snprintf(where, sizeof(where), "built-in preset steam%s", p->appid);
    for (size_t i = 0; builtin[i].appid; i++)
        if (!strcmp(builtin[i].appid, p->appid)) take_entry(p, builtin[i].gpus, builtin[i].kv, where);
#ifndef __ANDROID__
    /* Host tests: VKBRIDGE_PRESET_FAKE_BUILTIN=<appid>[@<gpu>,<gpu>]:KEY=VALUE KEY=VALUE;<appid>...
     * stands in for entries of presets_builtin.inc (no "@...": an empty tested list) */
    const char *fake = vkb_srv_env_global("VKBRIDGE_PRESET_FAKE_BUILTIN");
    size_t al = strlen(p->appid);
    for (const char *e = fake; e && *e;) {
        size_t len = strcspn(e, ";");
        const char *colon = memchr(e, ':', len);
        if (colon && (size_t)(colon - e) >= al && !strncmp(e, p->appid, al) && (e[al] == ':' || e[al] == '@')) {
            char gpus[256] = "", one[1024];
            if (e[al] == '@') snprintf(gpus, sizeof(gpus), "%.*s", (int)(colon - e - al - 1), e + al + 1);
            snprintf(one, sizeof(one), "%.*s", (int)(len - (size_t)(colon - e) - 1), colon + 1);
            take_entry(p, gpus, one, where);
        }
        e += len;
        if (*e) e++;
    }
#endif
}

static void load_file(struct vkb_preset *p)
{
    const char *dir = vkb_pcache_dir();
    if (!dir) return;
    char path[700], line[512];
    snprintf(path, sizeof(path), "%s/presets/%s.env", dir, p->appid);
    FILE *f = fopen(path, "re");
    if (!f) return;
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        n++;
        if (!strchr(line, '\n') && !feof(f)) { /* longer than a line can be: the rest of it skipped */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {}
            warn_once("preset: %s line %d: %.40s... is too long; ignored", path, n, line);
            continue;
        }
        take(p, line, VKB_ENV_SRC_PRESET_FILE, path, n);
    }
    fclose(f);
}

/* "K=V K=V (from file), K=V (from built-in)" */
static size_t text_of(const struct vkb_preset *p, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    static const int order[2] = {VKB_ENV_SRC_PRESET_FILE, VKB_ENV_SRC_PRESET_BUILTIN};
    for (int k = 0; k < 2; k++) {
        int any = 0;
        for (unsigned i = 0; i < p->n; i++) {
            if (p->kv[i].src != order[k]) continue;
            if (o < cap) o += (size_t)snprintf(out + o, cap - o, "%s%s=%s", any ? " " : o ? ", " : "", p->kv[i].key, p->kv[i].val);
            any = 1;
        }
        if (any && o < cap) o += (size_t)snprintf(out + o, cap - o, " (from %s)", k ? "built-in" : "file");
    }
    return o < cap ? o : cap - 1;
}

void vkb_preset_load(vkb_srv_proc *pr)
{
    char id[24];
    app_id(pr->pkey, id, sizeof(id));
    if (!id[0]) return;
    struct vkb_preset *p = calloc(1, sizeof(*p));
    if (!p) return;
    snprintf(p->appid, sizeof(p->appid), "%s", id);
    load_builtin(p);
    load_file(p);
    if (p->untested[0]) {
        char gpu[300];
        gpu_names(gpu, sizeof(gpu));
        const char *dir = vkb_pcache_dir();
        snprintf(p->note, sizeof(p->note),
                 "preset for steam%s: a built-in preset exists but was not tested on %s; not applied. To try it, copy it to "
                 "%s/presets/%s.env: %s",
                 p->appid, gpu, dir ? dir : "<cache dir>", p->appid, p->untested);
        if (first_time(p->note)) VKB_INFO("%s", p->note);
    }
    if (!p->n && !p->note[0]) {
        free(p);
        return;
    }
    if (p->n) {
        char text[1500], line[2300];
        text_of(p, text, sizeof(text));
        /* the global places beat the built-in preset (not the file): said here, so a preset that
         * seems to do nothing is explained */
        char shadow[600] = "";
        size_t so = 0;
        for (unsigned i = 0; i < p->n; i++) {
            if (p->kv[i].src != VKB_ENV_SRC_PRESET_BUILTIN) continue;
            const char *ov = vkb_srv_env_override(p->kv[i].key);
            const char *ev = ov ? NULL : getenv(p->kv[i].key);
            if ((ov || ev) && so < sizeof(shadow))
                so += (size_t)snprintf(shadow + so, sizeof(shadow) - so, "%s%s=%s from %s", so ? ", " : "", p->kv[i].key, ov ? ov : ev,
                                       ov ? "<cache dir>/env" : "the environment");
        }
        snprintf(line, sizeof(line), "preset for steam%s: %s%s%s", p->appid, text, shadow[0] ? "; overridden: " : "", shadow);
        if (first_time(line)) VKB_INFO("%s", line);
    }
    __atomic_store_n(&pr->preset, p, __ATOMIC_RELEASE);
}

void vkb_preset_free(vkb_srv_proc *pr)
{
    struct vkb_preset *p = __atomic_exchange_n(&pr->preset, NULL, __ATOMIC_ACQ_REL);
    free(p);
}

const char *vkb_preset_get(const vkb_srv_proc *pr, const char *name, int *src)
{
    const struct vkb_preset *p = pr ? __atomic_load_n(&pr->preset, __ATOMIC_ACQUIRE) : NULL;
    for (unsigned i = 0; p && i < p->n; i++)
        if (!strcmp(p->kv[i].key, name)) {
            if (src) *src = p->kv[i].src;
            return p->kv[i].val;
        }
    return NULL;
}

const char *vkb_preset_text(const vkb_srv_proc *pr, char *out, size_t cap)
{
    const struct vkb_preset *p = pr ? __atomic_load_n(&pr->preset, __ATOMIC_ACQUIRE) : NULL;
    if (!cap) return out;
    out[0] = 0;
    if (p) text_of(p, out, cap);
    return out;
}

const char *vkb_preset_note(const vkb_srv_proc *pr, char *out, size_t cap)
{
    const struct vkb_preset *p = pr ? __atomic_load_n(&pr->preset, __ATOMIC_ACQUIRE) : NULL;
    if (!cap) return out;
    snprintf(out, cap, "%s", p ? p->note : "");
    return out;
}

static uint32_t knob(const char *v) { return !strcmp(v, "0") ? VKB_KNOB_OFF : VKB_KNOB_ON; }

uint32_t vkb_preset_client_knobs(void)
{
    uint32_t w = 0;
    const char *off = vkb_srv_env("VKBRIDGE_NO_DESC_BATCH"), *on = vkb_srv_env("VKBRIDGE_DESC_BATCH");
    if (off && *off && strcmp(off, "0")) w |= VKB_KNOB_OFF << VKB_KNOB_DESC_BATCH_SHIFT;
    else if (on && *on) w |= knob(on) << VKB_KNOB_DESC_BATCH_SHIFT;
    const char *qm = vkb_srv_env("VKBRIDGE_QUERY_MIRROR");
    if (qm && *qm) w |= knob(qm) << VKB_KNOB_QUERY_MIRROR_SHIFT;
    return w;
}
