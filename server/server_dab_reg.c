/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The register of dma-bufs the bridge handed out over AHardwareBuffers (server_dmabuf_ahb.c).
 * An fd that comes back (a client's import, the compositor's question) is matched to its pages by
 * its open file: every fd of one dma-buf, in any process, refers to the dma-buf's one struct file.
 * On kernels since 5.3 each dma-buf has an inode of its own and (st_dev, st_ino) says it. Before
 * that (the Android 10-11 Kirin phones this path is for: 4.14, 4.19) every dma-buf shares one
 * anonymous inode, so a candidate with the same inode is told apart with flock: a lock taken
 * through the candidate's fd lets the same open file lock again and refuses any other open file of
 * that inode. The register's lock serializes those tests; nothing else flocks dma-bufs.
 * No Vulkan here: the unit test (tests/dabreg_unit.c) builds it alone.
 */
#define _GNU_SOURCE
#include "server_dab.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct dab_entry {
    void *ahb;
    int fd;
    uint64_t bytes;
    dev_t dev;
    ino_t ino;
    int refs;
    vkb_dmabuf_ahb_info image;
    struct dab_entry *next;
};

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static dab_entry *reg_head;
static unsigned reg_n;
static int shared_inode;
static void (*ahb_acquire)(void *);
static void (*ahb_release)(void *);

void dab_reg_set_ahb_ops(void (*acquire)(void *ahb), void (*release)(void *ahb))
{
    ahb_acquire = acquire;
    ahb_release = release;
}

int dab_reg_shared_inode(void)
{
    return __atomic_load_n(&shared_inode, __ATOMIC_RELAXED);
}

void dab_reg_note_shared_inode(int shared)
{
    __atomic_store_n(&shared_inode, shared ? 1 : 0, __ATOMIC_RELAXED);
}

/* Same open file, for two fds of one inode where the inode says nothing (reg_lock held). */
static int same_by_lock(int a, int b)
{
    if (flock(a, LOCK_EX | LOCK_NB) != 0) return 0; /* someone else holds it: not provable, not matched */
    int same = flock(b, LOCK_EX | LOCK_NB) == 0;
    flock(a, LOCK_UN);
    return same;
}

static int same_locked(int a, const struct stat *sa, int b, const struct stat *sb)
{
    if (sa->st_dev != sb->st_dev || sa->st_ino != sb->st_ino) return 0;
    if (a == b) return 1;
    return shared_inode ? same_by_lock(a, b) : 1;
}

int dab_same_file(int a, int b)
{
    struct stat sa, sb;
    if (fstat(a, &sa) != 0 || fstat(b, &sb) != 0) return 0;
    pthread_mutex_lock(&reg_lock);
    int same = same_locked(a, &sa, b, &sb);
    pthread_mutex_unlock(&reg_lock);
    return same;
}

dab_entry *dab_reg_add(void *ahb, int fd, uint64_t bytes, const vkb_dmabuf_ahb_info *image)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) return NULL;
    dab_entry *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->ahb = ahb;
    e->fd = fd;
    e->bytes = bytes;
    e->dev = st.st_dev;
    e->ino = st.st_ino;
    e->refs = 1;
    if (image) e->image = *image;
    pthread_mutex_lock(&reg_lock);
    /* Two live dma-bufs on one inode: this kernel shares it (fresh pages are never the same file). */
    for (dab_entry *o = reg_head; o && !shared_inode; o = o->next)
        if (o->dev == e->dev && o->ino == e->ino) shared_inode = 1;
    e->next = reg_head;
    reg_head = e;
    reg_n++;
    pthread_mutex_unlock(&reg_lock);
    return e;
}

static dab_entry *find_locked(int fd, const struct stat *st)
{
    for (dab_entry *e = reg_head; e; e = e->next) {
        struct stat es = {0};
        es.st_dev = e->dev;
        es.st_ino = e->ino;
        if (same_locked(e->fd, &es, fd, st)) return e;
    }
    return NULL;
}

dab_entry *dab_reg_find(int fd)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) return NULL;
    pthread_mutex_lock(&reg_lock);
    dab_entry *e = find_locked(fd, &st);
    if (e) e->refs++;
    pthread_mutex_unlock(&reg_lock);
    return e;
}

void dab_reg_ref(dab_entry *e)
{
    if (!e) return;
    pthread_mutex_lock(&reg_lock);
    e->refs++;
    pthread_mutex_unlock(&reg_lock);
}

void dab_reg_put(dab_entry *e)
{
    if (!e) return;
    pthread_mutex_lock(&reg_lock);
    int last = --e->refs == 0;
    if (last) {
        for (dab_entry **pp = &reg_head; *pp; pp = &(*pp)->next)
            if (*pp == e) {
                *pp = e->next;
                reg_n--;
                break;
            }
    }
    pthread_mutex_unlock(&reg_lock);
    if (!last) return;
    if (e->ahb && ahb_release) ahb_release(e->ahb);
    if (e->fd >= 0) close(e->fd);
    free(e);
}

int dab_reg_fd(const dab_entry *e)
{
    return e ? e->fd : -1;
}

void *dab_reg_ahb(const dab_entry *e)
{
    return e ? e->ahb : NULL;
}

uint64_t dab_reg_bytes(const dab_entry *e)
{
    return e ? e->bytes : 0;
}

const vkb_dmabuf_ahb_info *dab_reg_image(const dab_entry *e)
{
    return e ? &e->image : NULL;
}

unsigned dab_reg_count(void)
{
    pthread_mutex_lock(&reg_lock);
    unsigned n = reg_n;
    pthread_mutex_unlock(&reg_lock);
    return n;
}

__attribute__((visibility("default"))) int vkb_dmabuf_ahb_lookup(int fd, vkb_dmabuf_ahb_info *out)
{
    if (!out || out->abi != VKB_DMABUF_AHB_ABI || out->struct_size < sizeof(vkb_dmabuf_ahb_info)) return -1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) return 0;
    pthread_mutex_lock(&reg_lock);
    dab_entry *e = reg_n ? find_locked(fd, &st) : NULL;
    if (e) {
        vkb_dmabuf_ahb_info keep = *out;
        *out = e->image;
        out->abi = keep.abi;
        out->struct_size = keep.struct_size;
        out->ahb = e->ahb;
        out->bytes = e->bytes;
        if (e->ahb && ahb_acquire) ahb_acquire(e->ahb);
    }
    pthread_mutex_unlock(&reg_lock);
    return e != NULL;
}
