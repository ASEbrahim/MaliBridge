/*
 * libvkb_mali_profile.so (host tests only, LD_PRELOAD): the Mali profile (host/mali_profile.sh) for
 * every bridge server and client the suite starts, also those started with a cleaned environment
 * (env -u VKBRIDGE_FAKE_MISSING ...). getenv() of three bridge switches answers with the profile
 * added to what the test itself set:
 *   VKBRIDGE_FAKE_MISSING      the test's list + VKB_PROFILE_FAKE_MISSING (feature tokens)
 *   VKBRIDGE_SERVER_HIDE_EXTS  the test's list + VKB_PROFILE_HIDE_EXTS (extensions no profile Mali has)
 *   VKBRIDGE_FAKE_API          the test's value, else VKB_PROFILE_FAKE_API (the profile's Vulkan version)
 * Every other name, and these three without a VKB_PROFILE_* value, read as they are. Nothing in the
 * bridge changes: the same switches a test could set by hand, composed with the test's own.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static const char *raw(const char *name)
{
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

enum { APPEND, DEFAULT };
static const struct {
    const char *name, *prof;
    int mode;
} ents[] = {
    {"VKBRIDGE_FAKE_MISSING", "VKB_PROFILE_FAKE_MISSING", APPEND},
    {"VKBRIDGE_SERVER_HIDE_EXTS", "VKB_PROFILE_HIDE_EXTS", APPEND},
    {"VKBRIDGE_FAKE_API", "VKB_PROFILE_FAKE_API", DEFAULT},
};
#define N_ENTS (sizeof ents / sizeof ents[0])

/* the last answer per name, kept while the inputs stay the same (callers may hold the pointer;
 * a changed input gets a new string and the old one is left allocated) */
static struct {
    char *base, *prof, *out;
} cache[N_ENTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int same(const char *a, const char *b) { return (!a && !b) || (a && b && !strcmp(a, b)); }

char *getenv(const char *name)
{
    if (strncmp(name, "VKBRIDGE_", 9)) return (char *)raw(name);
    for (size_t i = 0; i < N_ENTS; i++) {
        if (strcmp(name, ents[i].name)) continue;
        const char *base = raw(name), *prof = raw(ents[i].prof);
        if (!prof || !*prof) return (char *)base;
        pthread_mutex_lock(&lock);
        if (!cache[i].out || !same(cache[i].base, base) || !same(cache[i].prof, prof)) {
            char *out;
            if (ents[i].mode == APPEND) {
                if (base && *base) {
                    out = malloc(strlen(base) + strlen(prof) + 2);
                    if (out) sprintf(out, "%s,%s", base, prof);
                } else
                    out = strdup(prof);
            } else
                out = strdup(base && *base ? base : prof);
            cache[i].base = base ? strdup(base) : NULL;
            cache[i].prof = strdup(prof);
            cache[i].out = out;
        }
        char *r = cache[i].out;
        pthread_mutex_unlock(&lock);
        return r;
    }
    return (char *)raw(name);
}
