/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Connections: one thread per client thread, requests executed in order, replies sent back.
 * A command buffer's recorded commands arrive in one message at vkEndCommandBuffer and are
 * replayed here.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t tables_lock = PTHREAD_MUTEX_INITIALIZER;
static vkb_srv_table *tables[VKB_MAX_TABLES];
static uint32_t next_table_id = 1;

static pthread_mutex_t procs_lock = PTHREAD_MUTEX_INITIALIZER;
static vkb_srv_proc *procs;

static __thread vkb_srv_table *tls_table;
static __thread vkb_srv_proc *tls_proc; /* the process this connection thread serves (its preset) */
__thread const char *vkb_srv_current_cmd;
/* The command buffer being replayed and its last commands, for the crash line (server_main.c). */
__thread VkCommandBuffer vkb_srv_replay_cb;
__thread uint32_t vkb_srv_replay_n;
__thread const char *vkb_srv_replay_ring[VKB_REPLAY_RING];
__thread char vkb_srv_crash_detail[512];

void vkb_srv_note_pipeline_layout(const VkPipelineLayoutCreateInfo *ci)
{
    char *b = vkb_srv_crash_detail;
    size_t cap = sizeof(vkb_srv_crash_detail);
    int n = snprintf(b, cap, "pipeline layout: flags 0x%x, pNext %s, %u set layout(s):", ci->flags,
                     ci->pNext ? "set" : "none", ci->setLayoutCount);
    for (uint32_t i = 0; i < ci->setLayoutCount && n > 0 && (size_t)n < cap; i++)
        n += snprintf(b + n, cap - (size_t)n, " %#llx", (unsigned long long)(uintptr_t)ci->pSetLayouts[i]);
    if (n > 0 && (size_t)n < cap) n += snprintf(b + n, cap - (size_t)n, "; %u push constant range(s):", ci->pushConstantRangeCount);
    for (uint32_t i = 0; i < ci->pushConstantRangeCount && n > 0 && (size_t)n < cap; i++)
        n += snprintf(b + n, cap - (size_t)n, " stages 0x%x offset %u size %u", ci->pPushConstantRanges[i].stageFlags,
                      ci->pPushConstantRanges[i].offset, ci->pPushConstantRanges[i].size);
}

vkb_srv_table *vkb_srv_current_table(void) { return tls_table; }
void vkb_srv_set_current_table(vkb_srv_table *t) { tls_table = t; }
vkb_srv_proc *vkb_srv_current_proc(void) { return tls_proc; }

/* Server side of the generated encoders' helpers: handles are already the server's values. */
uint64_t vkb_remote(const void *h) { return (uint64_t)(uintptr_t)h; }
int vkb_cb_begin_is_secondary(const VkCommandBufferBeginInfo *s) { (void)s; return 1; }

uint32_t vkb_table_register(vkb_srv_table *t)
{
    pthread_mutex_lock(&tables_lock);
    uint32_t id = 0;
    for (uint32_t tries = 0; tries < VKB_MAX_TABLES; tries++) {
        uint32_t cand = next_table_id++;
        if (next_table_id >= VKB_MAX_TABLES) next_table_id = 1;
        if (!tables[cand]) {
            id = cand;
            break;
        }
    }
    if (id) {
        t->id = id;
        tables[id] = t;
    }
    pthread_mutex_unlock(&tables_lock);
    return id;
}

void vkb_table_unregister(vkb_srv_table *t)
{
    pthread_mutex_lock(&tables_lock);
    if (t->id && t->id < VKB_MAX_TABLES && tables[t->id] == t) tables[t->id] = NULL;
    pthread_mutex_unlock(&tables_lock);
}

vkb_srv_table *vkb_table_get(uint32_t id)
{
    if (!id || id >= VKB_MAX_TABLES) return NULL;
    pthread_mutex_lock(&tables_lock);
    vkb_srv_table *t = tables[id];
    pthread_mutex_unlock(&tables_lock);
    return t;
}

void vkb_srv_reply_status(vkb_srv_call *c, uint32_t status)
{
    c->status = status;
}

void vkb_srv_bad_message(vkb_srv_call *c)
{
    VKB_ERR("malformed request");
    c->status = VKB_ST_BAD_MESSAGE;
}

void vkb_srv_missing(vkb_srv_call *c, const char *name)
{
    VKB_ERR("the driver has no %s", name);
    c->status = VKB_ST_MISSING;
}

/* The driver's own list (not what is hidden from apps): the loader's entry point for such a
 * command calls a NULL driver function on a device without the extension (a native run on RADV:
 * vkGetPhysicalDeviceExternalTensorPropertiesARM crashed at address 0, and the server with it). */
int vkb_srv_pd_has_ext(vkb_srv_call *c, VkPhysicalDevice pd, const char *list)
{
    vkb_srv_table *t = c->table;
    if (!t || !t->real.vkEnumerateDeviceExtensionProperties) return 0;
    uint32_t n = 0;
    if (t->real.vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    if (!e) return 0;
    int found = 0;
    if (t->real.vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) >= 0)
        for (uint32_t i = 0; i < n && !found; i++) {
            size_t l = strlen(e[i].extensionName);
            for (const char *p = list; *p && !found;) {
                size_t k = strcspn(p, "|");
                found = k == l && !strncmp(p, e[i].extensionName, l);
                p += k + (p[k] == '|');
            }
        }
    free(e);
    return found;
}

void vkb_srv_unsupported(vkb_srv_call *c, const char *name, const char *list)
{
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8) VKB_INFO("%s: the device has none of %s; not asked of the driver", name, list);
    c->status = VKB_ST_MISSING;
}

void vkb_srv_close_after_send(vkb_srv_call *c, int fd)
{
    if (c->nclose < VKB_MAX_FDS) c->close_after[c->nclose++] = fd;
}

/* ------------------------------------------------------------------ process cleanup */

void vkb_proc_cleanup(vkb_srv_proc *p)
{
    /* Devices first, then their instances. */
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 1; i < VKB_MAX_TABLES; i++) {
            pthread_mutex_lock(&tables_lock);
            vkb_srv_table *t = tables[i];
            int mine = t && t->proc == p && t->is_device == (pass == 0);
            if (mine) tables[i] = NULL;
            pthread_mutex_unlock(&tables_lock);
            if (!mine) continue;
            /* As in vkDestroyDevice, the call's table is the device's: the wrappers its teardown
             * calls through (the errata's on t->real) find their state by it. Without it they
             * took the last device made, gone once that one was destroyed: the dedicated_import
             * record of the null descriptors' buffer locked a NULL mutex (Tab S10+, v0.5.1 rc1). */
            vkb_srv_set_current_table(t);
            if (t->is_device) {
                VKB_INFO("process %d exited without destroying device %u; cleaning up", p->pid, t->id);
                vkb_async_device_destroy(t);
                vkb_emu_device_quiesce(t);
                vkb_cbfail_device_destroy(t);
                if (t->real.vkDeviceWaitIdle) t->real.vkDeviceWaitIdle(t->device);
                vkb_bvmap_device_destroy(t);
                vkb_gprof_device_destroy(t);
                vkb_trace_device_destroy(t);
                vkb_spvfix_device_destroy(t);
                vkb_dsmap_device_destroy(t);
                vkb_nulldesc_device_destroy(t);
                vkb_pools_device_destroy(t);
                vkb_pcache_device_destroy(t);
                vkb_emu_device_destroy(t);
                vkb_errata_device_destroy(t);
                vkb_promote_device_destroy(t); /* its SPIR-V lowering slot, kept by the device handle */
                vkb_hang_device_destroy(t);  /* its watcher thread calls the driver */
                vkb_guard_device_destroy(t); /* as vkDestroyDevice does: no table left pointing at t */
                vkb_report_device_destroy(t);
                vkb_mem_free_all(t);
                t->real.vkDestroyDevice(t->device, NULL);
            } else {
                VKB_INFO("process %d exited without destroying instance %u; cleaning up", p->pid, t->id);
                t->real.vkDestroyInstance(t->instance, NULL);
            }
            vkb_srv_set_current_table(NULL);
            pthread_mutex_destroy(&t->lock);
            free(t);
        }
    }
}

static vkb_srv_proc *proc_join(uint64_t token, int pid)
{
    pthread_mutex_lock(&procs_lock);
    vkb_srv_proc *p = procs;
    int first = 0;
    while (p && p->token != token) p = p->next;
    if (!p) {
        first = !procs;
        p = calloc(1, sizeof(*p));
        p->token = token;
        p->pid = pid;
        pthread_mutex_init(&p->olock, NULL);
        pthread_cond_init(&p->ocond, NULL);
        p->next = procs;
        procs = p;
        VKB_INFO("client process %d connected", pid);
    }
    p->conns++;
    if (first) vkb_probe_clients_gone(0); /* under procs_lock: in order with proc_leave's */
    pthread_mutex_unlock(&procs_lock);
    return p;
}

static void proc_leave(vkb_srv_proc *p)
{
    pthread_mutex_lock(&procs_lock);
    int last = --p->conns == 0;
    if (last) {
        vkb_srv_proc **pp = &procs;
        while (*pp && *pp != p) pp = &(*pp)->next;
        if (*pp) *pp = p->next;
    }
    if (last && !procs) vkb_probe_clients_gone(1); /* before the cleanup: the app's kill may come during it */
    pthread_mutex_unlock(&procs_lock);
    if (last) {
        vkb_proc_cleanup(p);
        VKB_INFO("client process %d gone", p->pid);
        vkb_report_gone(p);
        pthread_mutex_destroy(&p->olock);
        pthread_cond_destroy(&p->ocond);
        if (tls_proc == p) tls_proc = NULL;
        vkb_preset_free(p);
        free(p);
    } else {
        pthread_mutex_lock(&p->olock);
        p->conn_closed = 1;
        pthread_cond_broadcast(&p->ocond);
        pthread_mutex_unlock(&p->olock);
    }
}

/* Waits until every async request of the process up to `barrier` has run. */
static void order_wait(vkb_srv_proc *p, uint64_t barrier)
{
    if (!p || !barrier) return;
    pthread_mutex_lock(&p->olock);
    int waited = 0;
    p->owaiters++;
    while (p->watermark < barrier) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        if (pthread_cond_timedwait(&p->ocond, &p->olock, &ts) != 0 && ++waited >= 3 && p->conn_closed) {
            /* A connection of this process is gone with requests it never sent: go on. */
            VKB_WARN("process %d: async request %llu never arrived; continuing", p->pid, (unsigned long long)(p->watermark + 1));
            p->watermark = barrier;
            memset(p->done, 0, sizeof(p->done));
            break;
        }
    }
    p->owaiters--;
    pthread_mutex_unlock(&p->olock);
}

static void order_done(vkb_srv_proc *p, uint64_t seq)
{
    if (!p || !seq) return;
    pthread_mutex_lock(&p->olock);
    if (seq > p->watermark) {
        p->done[seq % VKB_SEQ_RING] = 1;
        while (p->done[(p->watermark + 1) % VKB_SEQ_RING]) {
            p->done[(p->watermark + 1) % VKB_SEQ_RING] = 0;
            p->watermark++;
        }
        /* Only when someone waits: bionic's broadcast is a futex syscall even with no waiter, one
         * per asynchronous message (~75k/s in Subnautica, 2026-10-12). */
        if (p->owaiters) pthread_cond_broadcast(&p->ocond);
    }
    pthread_mutex_unlock(&p->olock);
}

/* ------------------------------------------------------------------ command buffer replay */

/* Command buffers the driver would not record: their vkBeginCommandBuffer refused (out of memory:
 * Cyberpunk 2077 on the XRing O3, 2026-10-22), their begin not decoded, or the driver's
 * vkEndCommandBuffer failed. Their recorded commands are not replayed - recording into a command
 * buffer that is not recording took the driver, and the server with every game in it, down - and a
 * submit leaves them out, as does a primary's vkCmdExecuteCommands, so the submit's semaphores and
 * fence are still signalled and the app sees a frame dropped rather than a lost session. A later
 * recording of the same handle that begins and ends clears it; so do vkFreeCommandBuffers and the
 * device's end. VKBRIDGE_TEST_BEGIN_FAIL=<n> / VKBRIDGE_TEST_END_FAIL=<n> fail the n-th begin / end
 * as the driver did (host tests). */
typedef struct failed_cb {
    VkCommandBuffer cb;
    uint32_t table; /* the device's table id: its end forgets them */
} failed_cb;
static failed_cb *failed_cbs;
static uint32_t nfailed, failed_cap;
static pthread_mutex_t failed_lock = PTHREAD_MUTEX_INITIALIZER;

static void failed_set(VkCommandBuffer cb, int on)
{
    vkb_srv_table *t = vkb_srv_current_table();
    pthread_mutex_lock(&failed_lock);
    uint32_t i = 0;
    while (i < nfailed && failed_cbs[i].cb != cb) i++;
    if (on && i == nfailed) {
        if (nfailed == failed_cap) {
            uint32_t cap = failed_cap ? failed_cap * 2 : 64;
            failed_cb *n = realloc(failed_cbs, cap * sizeof(*n));
            if (n) failed_cbs = n, failed_cap = cap;
        }
        if (nfailed < failed_cap) failed_cbs[nfailed].cb = cb, failed_cbs[nfailed].table = t ? t->id : 0, nfailed++;
        else VKB_ERR("command buffer %p: no memory to note that the driver did not record it", (void *)cb);
    }
    if (!on && i < nfailed) failed_cbs[i] = failed_cbs[--nfailed];
    pthread_mutex_unlock(&failed_lock);
}

static int failed_has(VkCommandBuffer cb)
{
    if (!__atomic_load_n(&nfailed, __ATOMIC_RELAXED)) return 0;
    pthread_mutex_lock(&failed_lock);
    int hit = 0;
    for (uint32_t i = 0; i < nfailed && !hit; i++) hit = failed_cbs[i].cb == cb;
    pthread_mutex_unlock(&failed_lock);
    return hit;
}

static int test_fail(const char *var, int *n, int *seen)
{
    if (*n < 0) {
        const char *v = getenv(var);
        *n = v && *v ? atoi(v) : 0;
    }
    return *n > 0 && __atomic_add_fetch(seen, 1, __ATOMIC_RELAXED) == *n;
}

static int test_begin_fail(void)
{
    static int n = -1, seen;
    return test_fail("VKBRIDGE_TEST_BEGIN_FAIL", &n, &seen);
}

static int test_end_fail(void)
{
    static int n = -1, seen;
    return test_fail("VKBRIDGE_TEST_END_FAIL", &n, &seen);
}

/* The first few left-out command buffers are said, then every 1000th: a game that keeps
 * submitting one would otherwise fill the log every frame. */
static void left_out(const char *call, uint32_t dropped)
{
    static uint32_t times;
    uint32_t k = __atomic_add_fetch(&times, 1, __ATOMIC_RELAXED);
    if (k <= 5 || k % 1000 == 0)
        VKB_WARN("%s: %u command buffer(s) the driver did not record left out (their work is lost; the session goes on; %u time(s) so far)",
                 call, dropped, k);
}

static PFN_vkQueueSubmit next_submit[VKB_MAX_TABLES];
static PFN_vkQueueSubmit2 next_submit2[VKB_MAX_TABLES];

/* VKBRIDGE_TEST_SUBMIT_FAIL=<n>: the n-th vkQueueSubmit and the two after it return
 * VK_ERROR_DEVICE_LOST without reaching the driver (host tests: the client logs only the first). */
static int test_submit_fail(void)
{
    static int n = -1, seen;
    if (n < 0) {
        const char *v = getenv("VKBRIDGE_TEST_SUBMIT_FAIL");
        n = v && *v ? atoi(v) : 0;
    }
    if (n <= 0) return 0;
    int k = __atomic_add_fetch(&seen, 1, __ATOMIC_RELAXED);
    if (k < n || k > n + 2) return 0;
    VKB_WARN("VKBRIDGE_TEST_SUBMIT_FAIL: vkQueueSubmit fails with a lost device on purpose");
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL cf_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence fence)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueSubmit next = t ? next_submit[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    if (test_submit_fail()) return VK_ERROR_DEVICE_LOST;
    int any = 0;
    for (uint32_t i = 0; i < n && !any; i++)
        for (uint32_t k = 0; k < si[i].commandBufferCount && !any; k++) any = failed_has(si[i].pCommandBuffers[k]);
    if (!any) return next(q, n, si, fence);
    VkSubmitInfo *c = malloc(n * sizeof(*c));
    VkCommandBuffer *cbs = NULL;
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) total += si[i].commandBufferCount;
    cbs = malloc((total ? total : 1) * sizeof(*cbs));
    if (!c || !cbs) {
        free(c), free(cbs);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t o = 0, dropped = 0;
    for (uint32_t i = 0; i < n; i++) {
        c[i] = si[i];
        c[i].pCommandBuffers = cbs + o;
        c[i].commandBufferCount = 0;
        for (uint32_t k = 0; k < si[i].commandBufferCount; k++) {
            if (failed_has(si[i].pCommandBuffers[k])) dropped++;
            else cbs[o++] = si[i].pCommandBuffers[k], c[i].commandBufferCount++;
        }
    }
    left_out("vkQueueSubmit", dropped);
    VkResult r = next(q, n, c, fence);
    free(c), free(cbs);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL cf_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence fence)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueSubmit2 next = t ? next_submit2[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    int any = 0;
    for (uint32_t i = 0; i < n && !any; i++)
        for (uint32_t k = 0; k < si[i].commandBufferInfoCount && !any; k++) any = failed_has(si[i].pCommandBufferInfos[k].commandBuffer);
    if (!any) return next(q, n, si, fence);
    VkSubmitInfo2 *c = malloc(n * sizeof(*c));
    VkCommandBufferSubmitInfo *cbs = NULL;
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) total += si[i].commandBufferInfoCount;
    cbs = malloc((total ? total : 1) * sizeof(*cbs));
    if (!c || !cbs) {
        free(c), free(cbs);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t o = 0, dropped = 0;
    for (uint32_t i = 0; i < n; i++) {
        c[i] = si[i];
        c[i].pCommandBufferInfos = cbs + o;
        c[i].commandBufferInfoCount = 0;
        for (uint32_t k = 0; k < si[i].commandBufferInfoCount; k++) {
            if (failed_has(si[i].pCommandBufferInfos[k].commandBuffer)) dropped++;
            else cbs[o++] = si[i].pCommandBufferInfos[k], c[i].commandBufferInfoCount++;
        }
    }
    left_out("vkQueueSubmit2", dropped);
    VkResult r = next(q, n, c, fence);
    free(c), free(cbs);
    return r;
}

static PFN_vkCmdExecuteCommands next_exec[VKB_MAX_TABLES];
static PFN_vkFreeCommandBuffers next_free[VKB_MAX_TABLES];

/* A secondary the driver did not record never reaches it inside a primary. */
static VKAPI_ATTR void VKAPI_CALL cf_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *secs)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkCmdExecuteCommands next = t ? next_exec[t->id] : NULL;
    if (!next) return;
    int any = 0;
    for (uint32_t i = 0; i < n && !any; i++) any = failed_has(secs[i]);
    if (!any) {
        next(cb, n, secs);
        return;
    }
    VkCommandBuffer *keep = malloc(n * sizeof(*keep));
    if (!keep) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!failed_has(secs[i])) keep[k++] = secs[i];
    left_out("vkCmdExecuteCommands", n - k);
    if (k) next(cb, k, keep);
    free(keep);
}

static VKAPI_ATTR void VKAPI_CALL cf_FreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkFreeCommandBuffers next = t ? next_free[t->id] : NULL;
    if (__atomic_load_n(&nfailed, __ATOMIC_RELAXED))
        for (uint32_t i = 0; i < n; i++)
            if (cbs[i]) failed_set(cbs[i], 0);
    if (next) next(d, pool, n, cbs);
}

/* Over every layer but the report's: what reaches the layers beneath never names such a command buffer */
void vkb_cbfail_device_init(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES) return;
    next_submit[t->id] = t->dt.vkQueueSubmit;
    next_submit2[t->id] = t->dt.vkQueueSubmit2;
    next_exec[t->id] = t->dt.vkCmdExecuteCommands;
    next_free[t->id] = t->dt.vkFreeCommandBuffers;
    if (t->dt.vkQueueSubmit) t->dt.vkQueueSubmit = cf_QueueSubmit;
    if (t->dt.vkQueueSubmit2) t->dt.vkQueueSubmit2 = cf_QueueSubmit2;
    if (t->dt.vkCmdExecuteCommands) t->dt.vkCmdExecuteCommands = cf_CmdExecuteCommands;
    if (t->dt.vkFreeCommandBuffers) t->dt.vkFreeCommandBuffers = cf_FreeCommandBuffers;
}

/* The device's command buffers are gone with it (their pools too: handles the driver may reuse). */
void vkb_cbfail_device_destroy(vkb_srv_table *t)
{
    if (!t || !__atomic_load_n(&nfailed, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&failed_lock);
    for (uint32_t i = 0; i < nfailed;)
        if (failed_cbs[i].table == t->id) failed_cbs[i] = failed_cbs[--nfailed];
        else i++;
    pthread_mutex_unlock(&failed_lock);
}

static int is_stream_cmd(uint32_t id)
{
    const char *n = vkb_cmd_names[id];
    return !strncmp(n, "vkCmd", 5) || id == VKB_CMD_vkBeginCommandBuffer || id == VKB_CMD_vkbPushDescRaw;
}

void vkb_sv_vkEndCommandBuffer(vkb_srv_call *c)
{
    VkCommandBuffer cb = (VkCommandBuffer)(uintptr_t)vkb_dec_u64(&c->d);
    size_t len = 0;
    const uint8_t *s = vkb_dec_blob(&c->d, &len);
    if (c->d.err) {
        vkb_srv_bad_message(c);
        return;
    }
    VkResult begin_r = VK_SUCCESS;
    size_t pos = 0;
    vkb_enc scratch;
    vkb_enc_init(&scratch);
    uint32_t ncmds = 0;
    vkb_srv_replay_cb = cb;
    vkb_srv_replay_n = 0;
    while (s && pos + 8 <= len) {
        uint32_t id, n;
        memcpy(&id, s + pos, 4);
        memcpy(&n, s + pos + 4, 4);
        pos += 8;
        if (id >= VKB_CMD_COUNT || !is_stream_cmd(id) || n > len - pos) {
            VKB_ERR("command stream: bad entry (id %u, %u bytes) after %u commands", id, n, ncmds);
            c->status = VKB_ST_BAD_MESSAGE;
            vkb_srv_replay_cb = VK_NULL_HANDLE;
            vkb_enc_free(&scratch);
            return;
        }
        vkb_srv_call sub;
        memset(&sub, 0, sizeof(sub));
        vkb_dec_init(&sub.d, s + pos, n, c->d.arena, NULL, 0);
        sub.r = scratch;
        vkb_enc_reset(&sub.r);
        sub.dt = c->dt;
        sub.table = c->table;
        sub.conn = c->conn;
        sub.in_stream = 1;
        vkb_srv_current_cmd = vkb_cmd_names[id];
        vkb_srv_replay_ring[ncmds % VKB_REPLAY_RING] = vkb_cmd_names[id];
        vkb_srv_replay_n = ncmds + 1;
        vkb_srv_handlers[id](&sub);
        vkb_srv_current_cmd = vkb_cmd_names[VKB_CMD_vkEndCommandBuffer];
        scratch = sub.r;
        if (sub.status != VKB_ST_OK) {
            VKB_ERR("command stream: %s failed (status %u)", vkb_cmd_names[id], sub.status);
            if (id == VKB_CMD_vkBeginCommandBuffer) {
                /* The begin never reached the driver (its encoding did not decode): recording into
                 * a command buffer that was not begun would hand the driver one in the wrong state. */
                VKB_ERR("command stream: command buffer %p not begun; its %zu bytes of commands are dropped", (void *)cb, len);
                failed_set(cb, 1);
                vkb_srv_replay_cb = VK_NULL_HANDLE;
                vkb_enc_free(&scratch);
                vkb_enc_u32(&c->r, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
                return;
            }
        }
        if (id == VKB_CMD_vkBeginCommandBuffer && scratch.len >= 4) memcpy(&begin_r, scratch.buf, 4);
        if (id == VKB_CMD_vkBeginCommandBuffer && begin_r == VK_SUCCESS && test_begin_fail()) {
            c->dt->vkEndCommandBuffer(cb); /* begun by the driver: the test's refusal leaves it ended */
            begin_r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            VKB_WARN("VKBRIDGE_TEST_BEGIN_FAIL: vkBeginCommandBuffer refused on purpose");
        }
        if (id == VKB_CMD_vkBeginCommandBuffer && begin_r != VK_SUCCESS) {
            /* The driver refused the begin: nothing more goes into this command buffer, no end,
             * and submits leave it out until it is recorded again. */
            VKB_ERR("command buffer %p: the driver would not begin it (%d); its %zu bytes of commands are dropped "
                    "and submits leave it out", (void *)cb, begin_r, len);
            failed_set(cb, 1);
            vkb_srv_replay_cb = VK_NULL_HANDLE;
            vkb_enc_free(&scratch);
            vkb_enc_u32(&c->r, (uint32_t)begin_r);
            return;
        }
        pos += (n + 7) & ~(size_t)7;
        ncmds++;
    }
    vkb_enc_free(&scratch);
    vkb_srv_current_cmd = "vkEndCommandBuffer (the driver's, after the stream)";
    VkResult r = c->dt->vkEndCommandBuffer(cb);
    vkb_srv_current_cmd = vkb_cmd_names[VKB_CMD_vkEndCommandBuffer];
    vkb_srv_replay_cb = VK_NULL_HANDLE;
    if (r == VK_SUCCESS && test_end_fail()) {
        r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VKB_WARN("VKBRIDGE_TEST_END_FAIL: vkEndCommandBuffer failed on purpose");
    }
    if (begin_r != VK_SUCCESS) r = begin_r;
    if (r != VK_SUCCESS)
        VKB_ERR("command buffer %p: the driver's vkEndCommandBuffer failed (%d); submits leave it out", (void *)cb, r);
    failed_set(cb, r != VK_SUCCESS); /* recorded this time, or not */
    if (vkb_verbose) VKB_DBG("command buffer %p: %u commands, %zu bytes -> %d", (void *)cb, ncmds, len, r);
    vkb_enc_u32(&c->r, (uint32_t)r);
}

/* ------------------------------------------------------------------ hello */

void vkb_sv_vkbHello(vkb_srv_call *c)
{
    uint64_t hash = vkb_dec_u64(&c->d);
    uint64_t token = vkb_dec_u64(&c->d);
    uint32_t pid = vkb_dec_u32(&c->d);
    const char *comm = vkb_dec_str(&c->d);
    if (c->d.err) {
        vkb_srv_bad_message(c);
        return;
    }
    vkb_enc_u64(&c->r, VKB_PROTOCOL_HASH);
    if (hash != VKB_PROTOCOL_HASH) {
        VKB_ERR("client %d (%s) speaks protocol %016llx, this server %016llx: refusing", pid,
                comm ? comm : "?", (unsigned long long)hash, (unsigned long long)VKB_PROTOCOL_HASH);
        vkb_enc_u32(&c->r, 0);
        return;
    }
    if (!c->conn->proc) {
        c->conn->proc = proc_join(token, (int)pid);
        if (c->conn->proc->conns == 1) {
            VKB_INFO("process %u is %s", pid, comm ? comm : "?");
            if (comm) {
                /* "name@key": the key picks the persistent pipeline cache (client/conn.c pcache_key) */
                const char *at = strrchr(comm, '@');
                vkb_srv_proc *pr = c->conn->proc;
                snprintf(pr->comm, sizeof(pr->comm), "%.*s", at ? (int)(at - comm) : (int)strlen(comm), comm);
                if (at && at[1]) snprintf(pr->pkey, sizeof(pr->pkey), "%s", at + 1);
            }
            vkb_preset_load(c->conn->proc); /* the app id is known: its preset, before its first request */
            vkb_report_hello(c->conn->proc);
        }
    }
    vkb_enc_u32(&c->r, 1);
}

/* ------------------------------------------------------------------ statistics */

/* VKBRIDGE_STATS=<seconds> on the server: time spent per command, waiting for order, idle. */
static uint64_t st_ns[VKB_CMD_COUNT], st_n[VKB_CMD_COUNT], st_wait_ns, st_last;
static int st_period = -1;

void vkb_srv_stats_reload(void) { st_period = -1; }

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void stats_add(uint32_t cmd, uint64_t ns, uint64_t wait_ns)
{
    if (st_period < 0) {
        /* <cache dir>/env works too: the app starts the server without the session's environment. */
        const char *v = vkb_srv_env("VKBRIDGE_STATS");
        st_period = v ? atoi(v) : 0;
    }
    if (!st_period) return;
    __atomic_add_fetch(&st_ns[cmd], ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&st_n[cmd], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&st_wait_ns, wait_ns, __ATOMIC_RELAXED);
    uint64_t now = mono_ns(), last = __atomic_load_n(&st_last, __ATOMIC_RELAXED);
    if (!last) {
        __atomic_store_n(&st_last, now, __ATOMIC_RELAXED);
        return;
    }
    if (now - last < (uint64_t)st_period * 1000000000ull || !__atomic_compare_exchange_n(&st_last, &last, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    uint64_t total = 0;
    for (uint32_t i = 0; i < VKB_CMD_COUNT; i++) total += st_ns[i];
    VKB_INFO("stats over %ds: %.1f ms in handlers, %.1f ms waiting for order", st_period, total / 1e6, st_wait_ns / 1e6);
    for (int k = 0; k < 8; k++) {
        uint64_t best = 0;
        uint32_t bi = 0;
        for (uint32_t i = 0; i < VKB_CMD_COUNT; i++)
            if (st_ns[i] > best) {
                best = st_ns[i];
                bi = i;
            }
        if (!best) break;
        VKB_INFO("  %-40s %8llu calls %9.2f ms (%.2f us each)", vkb_cmd_names[bi], (unsigned long long)st_n[bi], best / 1e6,
                 best / 1e3 / (double)st_n[bi]);
        st_ns[bi] = 0;
    }
    memset(st_ns, 0, sizeof(st_ns));
    memset(st_n, 0, sizeof(st_n));
    st_wait_ns = 0;
}

/* ------------------------------------------------------------------ connection loop */

/* Reads the next request. Buffered: a burst of small asynchronous requests costs one recvmsg, not
 * two reads each. Descriptors arrive with the first bytes of their message, so they queue up in
 * order and each header takes its nfds from the front. The payload points into the buffer and
 * stays valid until the next call. */
static int srv_recv(vkb_srv_conn *conn, vkb_msg_hdr *h, uint8_t **payload, int *fds, int *nfds)
{
    *nfds = 0;
    if (conn->rpos == conn->rlen && conn->rcap > (16u << 20)) {
        free(conn->rb);
        conn->rb = NULL;
        conn->rcap = conn->rpos = conn->rlen = 0;
    }
    for (;;) {
        size_t avail = conn->rlen - conn->rpos;
        size_t need = sizeof(*h);
        if (avail >= sizeof(*h)) {
            memcpy(h, conn->rb + conn->rpos, sizeof(*h));
            if (h->magic != VKB_MAGIC) {
                VKB_ERR("transport: bad message magic 0x%08x", h->magic);
                return -1;
            }
            if (h->size > (1u << 30) || h->nfds > VKB_MAX_FDS) {
                VKB_ERR("transport: bad message (%u bytes, %u descriptors)", h->size, h->nfds);
                return -1;
            }
            need += h->size;
            if (avail >= need) {
                if ((int)h->nfds > conn->fdn) {
                    VKB_ERR("transport: message arrived without its %u descriptors", h->nfds);
                    return -1;
                }
                *nfds = (int)h->nfds;
                memcpy(fds, conn->fdq, sizeof(int) * h->nfds);
                conn->fdn -= (int)h->nfds;
                memmove(conn->fdq, conn->fdq + h->nfds, sizeof(int) * conn->fdn);
                *payload = conn->rb + conn->rpos + sizeof(*h);
                conn->rpos += need;
                return 0;
            }
        }
        if (conn->rpos) {
            memmove(conn->rb, conn->rb + conn->rpos, avail);
            conn->rpos = 0;
            conn->rlen = avail;
        }
        if (need < (256u << 10)) need = 256u << 10;
        if (need > conn->rcap) {
            size_t nc = conn->rcap ? conn->rcap : (256u << 10);
            while (nc < need) nc *= 2;
            uint8_t *nb = realloc(conn->rb, nc);
            if (!nb) return -1;
            conn->rb = nb;
            conn->rcap = nc;
        }
        union {
            char buf[CMSG_SPACE(sizeof(int) * VKB_MAX_FDS)];
            struct cmsghdr align;
        } cm;
        struct iovec iov = {conn->rb + conn->rlen, conn->rcap - conn->rlen};
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cm.buf;
        msg.msg_controllen = sizeof(cm.buf);
        ssize_t r;
        do {
            r = recvmsg(conn->sock, &msg, MSG_CMSG_CLOEXEC);
        } while (r < 0 && errno == EINTR);
        if (r <= 0) return -1;
        for (struct cmsghdr *cmh = CMSG_FIRSTHDR(&msg); cmh; cmh = CMSG_NXTHDR(&msg, cmh)) {
            if (cmh->cmsg_level != SOL_SOCKET || cmh->cmsg_type != SCM_RIGHTS) continue;
            int n = (int)((cmh->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < n; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(cmh) + i * sizeof(int), sizeof(int));
                if (conn->fdn < (int)(sizeof(conn->fdq) / sizeof(conn->fdq[0]))) conn->fdq[conn->fdn++] = fd;
                else close(fd);
            }
        }
        if (msg.msg_flags & MSG_CTRUNC) VKB_WARN("transport: descriptors truncated");
        conn->rlen += (size_t)r;
    }
}

/* Runs one request whose payload is in c->d: dispatch table lookup, then the handler. */
static void run_request(vkb_srv_conn *conn, vkb_srv_call *c, uint32_t cmd, uint32_t table, uint64_t wait_ns)
{
    vkb_enc_reset(&c->r);
    c->conn = conn;
    c->status = VKB_ST_OK;
    c->nclose = 0;
    c->in_stream = 0;
    c->table = NULL;
    c->dt = &vkb_global_dt;
    if (cmd >= VKB_CMD_COUNT) {
        VKB_ERR("unknown command id %u", cmd);
        c->status = VKB_ST_BAD_MESSAGE;
        return;
    }
    if (!conn->proc && cmd != VKB_CMD_vkbHello) {
        VKB_ERR("%s before hello", vkb_cmd_names[cmd]);
        c->status = VKB_ST_BAD_MESSAGE;
        return;
    }
    if (table) {
        c->table = vkb_table_get(table);
        if (!c->table || c->table->proc != conn->proc) {
            VKB_ERR("%s: unknown dispatch table %u", vkb_cmd_names[cmd], table);
            c->status = VKB_ST_BAD_TABLE;
            return;
        }
        c->dt = &c->table->dt;
    }
    uint64_t t1 = st_period ? mono_ns() : 0;
    vkb_srv_set_current_table(c->table);
    if (vkb_verbose) VKB_DBG("-> %s", vkb_cmd_names[cmd]);
    vkb_srv_current_cmd = vkb_cmd_names[cmd];
    vkb_srv_crash_detail[0] = 0;
    vkb_srv_handlers[cmd](c);
    vkb_srv_current_cmd = NULL;
    vkb_srv_set_current_table(NULL);
    if (st_period) stats_add(cmd, mono_ns() - t1, wait_ns);
}

/* An asynchronous request's outcome: nobody waits for it, so a failure can only be logged. */
static void async_report(const vkb_srv_call *c, uint32_t cmd)
{
    if (c->status != VKB_ST_OK) {
        VKB_ERR("asynchronous %s failed (status %u)", cmd < VKB_CMD_COUNT ? vkb_cmd_names[cmd] : "?", c->status);
    } else if (cmd < VKB_CMD_COUNT && vkb_cmd_result[cmd] && c->r.len >= sizeof(int32_t)) {
        /* The client already told the app VK_SUCCESS. */
        int32_t vr;
        memcpy(&vr, c->r.buf, sizeof(vr));
        /* only VK_SUCCESS or an error is legal for these; a positive code is the driver's bug */
        if (vr != VK_SUCCESS) VKB_ERR("asynchronous %s returned %d", vkb_cmd_names[cmd], vr);
    }
}

/*
 * vkbBatch: the client's queue of asynchronous requests as one message (client/conn.c) - per
 * request a u32 (command | table << 16), a u32 size and the payload padded to 8 bytes; no
 * descriptors. One sequence number for all of them, run here in order. DXVK sends ~100k small
 * requests a second (a set allocation and an update per draw; Subnautica, 2026-10-12), and each
 * one alone costs a header, an ordering wait and wake-up, and a pass through srv_recv.
 */
static void run_batch(vkb_srv_conn *conn, vkb_srv_call *c, const uint8_t *p, size_t len, uint64_t wait_ns)
{
    size_t pos = 0;
    while (pos < len) {
        uint32_t ct, n;
        if (len - pos < 8) goto bad;
        memcpy(&ct, p + pos, 4);
        memcpy(&n, p + pos + 4, 4);
        pos += 8;
        if (n > len - pos) goto bad;
        uint32_t cmd = ct & 0xffff;
        if (cmd == VKB_CMD_vkbHello || cmd == VKB_CMD_vkbBatch) goto bad;
        vkb_dec_init(&c->d, p + pos, n, &conn->arena, NULL, 0);
        run_request(conn, c, cmd, ct >> 16, wait_ns);
        wait_ns = 0;
        vkb_dec_close_untaken(&c->d);
        async_report(c, cmd);
        for (int i = 0; i < c->nclose; i++) close(c->close_after[i]);
        vkb_arena_reset(&conn->arena);
        pos += ((size_t)n + 7) & ~(size_t)7;
    }
    return;
bad:
    VKB_ERR("transport: bad batch entry at %zu of %zu bytes; the rest of the batch dropped", pos, len);
}

/* Only ever handled by conn_main: a batch inside a batch or with a reply is malformed. */
void vkb_sv_vkbBatch(vkb_srv_call *c)
{
    vkb_srv_bad_message(c);
}

static void *conn_main(void *arg)
{
    vkb_srv_conn *conn = arg;
    vkb_srv_call c;
    vkb_enc_init(&c.r);
    int fds[VKB_MAX_FDS];
    for (;;) {
        vkb_msg_hdr h;
        int nfds = 0;
        uint8_t *payload;
        if (srv_recv(conn, &h, &payload, fds, &nfds) < 0) break;
        tls_proc = conn->proc; /* set by its hello */
        uint64_t wait_ns = 0;
        if (conn->proc && h.cmd < VKB_CMD_COUNT && h.cmd != VKB_CMD_vkbHello) {
            uint64_t t0 = st_period ? mono_ns() : 0;
            order_wait(conn->proc, h.barrier);
            if (st_period) wait_ns = mono_ns() - t0;
        }
        if (h.cmd == VKB_CMD_vkbBatch && (h.flags & VKB_F_NOREPLY) && !nfds && conn->proc) {
            run_batch(conn, &c, payload, h.size, wait_ns);
            order_done(conn->proc, h.seq);
            continue;
        }
        vkb_dec_init(&c.d, payload, h.size, &conn->arena, fds, nfds);
        run_request(conn, &c, h.cmd, h.table, wait_ns);
        vkb_dec_close_untaken(&c.d);
        order_done(conn->proc, h.seq);
        if (!(h.flags & VKB_F_NOREPLY)) {
            vkb_msg_hdr rh = {VKB_MAGIC, c.status == VKB_ST_OK ? (uint32_t)c.r.len : 0, c.status, 0,
                              c.status == VKB_ST_OK ? (uint32_t)c.r.nfds : 0, 0, 0, 0};
            if (c.r.oom) {
                rh.cmd = VKB_ST_BAD_MESSAGE;
                rh.size = 0;
                rh.nfds = 0;
            }
            if (vkb_send_msg(conn->sock, &rh, c.r.buf, c.r.fds, (int)rh.nfds) < 0) {
                for (int i = 0; i < c.nclose; i++) close(c.close_after[i]);
                break;
            }
        } else {
            async_report(&c, h.cmd);
        }
        for (int i = 0; i < c.nclose; i++) close(c.close_after[i]);
        vkb_arena_reset(&conn->arena);
    }
    close(conn->sock);
    vkb_enc_free(&c.r);
    free(conn->rb);
    for (int i = 0; i < conn->fdn; i++) close(conn->fdq[i]);
    vkb_arena_free(&conn->arena);
    if (conn->proc) proc_leave(conn->proc);
    free(conn);
    return NULL;
}

void vkb_srv_serve(int lfd)
{
    stats_add(0, 0, 0);
    for (;;) {
        int s = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (s < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            VKB_ERR("accept: %s", strerror(errno));
            continue;
        }
        int buf_sz = 2 * 1024 * 1024;
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));
        vkb_srv_conn *conn = calloc(1, sizeof(*conn));
        conn->sock = s;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        /* Driver calls (pipeline compiles) can use a lot of stack. */
        pthread_attr_setstacksize(&attr, 4u << 20);
        if (pthread_create(&conn->thread, &attr, conn_main, conn) != 0) {
            VKB_ERR("cannot start a connection thread");
            close(s);
            free(conn);
        }
        pthread_attr_destroy(&attr);
    }
}
