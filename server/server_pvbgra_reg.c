/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_bgra_store (PV-07): the register of dma-bufs whose texels are in R8G8B8A8 order although their
 * client made the image B8G8R8A8 (server_errata_pvbgra.c made it R8G8B8A8 for the driver). DroidDeck's
 * compositor runs in this process and asks vkb_dmabuf_content_format() for each client buffer it
 * imports: gamescope's output images reach it as XR24 wl_buffers, and it reads such a buffer as RGBA.
 * A dma-buf is told by its open file's (st_dev, st_ino): every fd of one dma-buf, in any process,
 * refers to the dma-buf's one struct file, and since Linux 5.3 each dma-buf has an inode of its own.
 * Before that every dma-buf shares one anonymous inode, so nothing is registered there (the screen
 * keeps today's colours: no false match). No Vulkan here: tests/pvbgrareg_unit.c builds it alone.
 */
#define _GNU_SOURCE
#include "server_pvbgra_reg.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>

typedef struct reg_entry {
    dev_t dev;
    ino_t ino;
    uint64_t mem;
    uint32_t table;
    struct reg_entry *next;
} reg_entry;

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static reg_entry *reg_head;
static unsigned reg_n;

int vkb_pvbgra_reg_usable(void)
{
    static int v = -1;
    int r = __atomic_load_n(&v, __ATOMIC_RELAXED);
    if (r >= 0) return r;
    struct utsname u;
    unsigned major = 0, minor = 0;
    r = uname(&u) == 0 && sscanf(u.release, "%u.%u", &major, &minor) == 2 && (major > 5 || (major == 5 && minor >= 3));
    __atomic_store_n(&v, r, __ATOMIC_RELAXED);
    return r;
}

int vkb_pvbgra_reg_add(int fd, uint64_t mem, uint32_t table, char *key, size_t cap)
{
    struct stat st;
    if (key && cap) key[0] = 0;
    if (fd < 0 || fstat(fd, &st) != 0 || !vkb_pvbgra_reg_usable()) return 0;
    if (key && cap) snprintf(key, cap, "%llu:%llu", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino);
    pthread_mutex_lock(&reg_lock);
    int have = 0;
    for (reg_entry *e = reg_head; e && !have; e = e->next) have = e->dev == st.st_dev && e->ino == st.st_ino && e->mem == mem;
    reg_entry *e = have ? NULL : calloc(1, sizeof(*e));
    if (e) {
        e->dev = st.st_dev, e->ino = st.st_ino, e->mem = mem, e->table = table;
        e->next = reg_head;
        reg_head = e;
        reg_n++;
    }
    pthread_mutex_unlock(&reg_lock);
    return e != NULL || have;
}

static void drop_if(int (*match)(const reg_entry *, uint64_t, uint32_t), uint64_t v, uint32_t table)
{
    pthread_mutex_lock(&reg_lock);
    for (reg_entry **pp = &reg_head; *pp;) {
        if (match(*pp, v, table)) {
            reg_entry *e = *pp;
            *pp = e->next;
            free(e);
            reg_n--;
        } else
            pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&reg_lock);
}

static int by_mem(const reg_entry *e, uint64_t v, uint32_t table) { return e->mem == v && e->table == table; }
static int by_table(const reg_entry *e, uint64_t v, uint32_t table) { (void)v; return e->table == table; }

void vkb_pvbgra_reg_drop_mem(uint64_t mem, uint32_t table) { drop_if(by_mem, mem, table); }
void vkb_pvbgra_reg_drop_table(uint32_t table) { drop_if(by_table, 0, table); }

int vkb_pvbgra_reg_find(int fd)
{
    struct stat st;
    if (fd < 0 || !__atomic_load_n(&reg_n, __ATOMIC_RELAXED) || fstat(fd, &st) != 0) return 0;
    int found = 0;
    pthread_mutex_lock(&reg_lock);
    for (reg_entry *e = reg_head; e && !found; e = e->next) found = e->dev == st.st_dev && e->ino == st.st_ino;
    pthread_mutex_unlock(&reg_lock);
    return found;
}

unsigned vkb_pvbgra_reg_count(void)
{
    pthread_mutex_lock(&reg_lock);
    unsigned n = reg_n;
    pthread_mutex_unlock(&reg_lock);
    return n;
}

/* The dma-buf named VKB_DMABUF_RGBA8_NAME, for a compositor in another process (DroidDeck runs the server as its own
 * process): 0, or the errno (a memfd or another file: ENOTTY) */
int vkb_pvbgra_name(int fd)
{
    if (fd < 0) return EBADF;
    return ioctl(fd, DMA_BUF_SET_NAME, VKB_DMABUF_RGBA8_NAME) == 0 ? 0 : errno;
}

/* 1 when fd's file is a dma-buf so named (what the compositor checks: "/dmabuf:<name>" behind /proc/self/fd/<fd>) */
int vkb_pvbgra_named(int fd)
{
    char p[40], l[128];
    snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    ssize_t n = fd >= 0 ? readlink(p, l, sizeof(l) - 1) : -1;
    if (n <= 0) return 0;
    l[n] = 0;
    return strncmp(l, "/dmabuf:", 8) == 0 && strcmp(l + 8, VKB_DMABUF_RGBA8_NAME) == 0;
}

__attribute__((visibility("default"))) uint32_t vkb_dmabuf_content_format(int fd)
{
    return vkb_pvbgra_reg_find(fd) ? VKB_DMABUF_CONTENT_RGBA8 : 0;
}
