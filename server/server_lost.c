/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * A lost device stays one program's problem. The one server serves every program; one program's
 * device loss must not reach into the others.
 *
 * Why: Pixel 9, Mali-G715, driver r54p3, WoW Classic through vkd3d-proton: the game's
 * device was lost (vkWaitForFences -4), the game submitted four more times (each -4), and 19 ms
 * after the fourth the driver's own thread (mali-event-hand: basep_event_thread ->
 * basep_cpu_queue_process -> signal_fence_cb -> device_faults_handler_internal::record_internal)
 * read address 0x101; the server died with Steam, the compositor and everything. G715
 * Immortalis, r38p1, which has no device-fault recording took 25 submits after its loss and lived.
 *
 * Two parts, both off the path of a device that works:
 *   1. The latch. A device is taken as lost when a submission (vkQueueSubmit, vkQueueSubmit2,
 *      vkQueueBindSparse) returns VK_ERROR_DEVICE_LOST (the driver refused the work itself), when
 *      the bridge itself says so (server_guard.c, a contained fault in a bind), or when the driver
 *      confirms a lost answer of a fence or semaphore wait: a wait whose -4 the poll right after
 *      repeats is only a suspicion, and the device is latched when a second answer says lost again
 *      - a submission's -4, or a wait's or a fence status's -4 at least CONFIRM_MS and at most
 *      SUSPECT_MS later - with no VK_SUCCESS of a wait, poll or fence status in between; a lost
 *      answer later than SUSPECT_MS is a new suspicion (the clearing VK_SUCCESS may never reach the
 *      server: the client answers finished fences itself). Arm's job-manager drivers end a
 *      timed-out semaphore wait with a -4 that a poll takes back (server_wait.c, the A1 black
 *      screen); a slow one can still say -4 on the first poll and work again after it, and a
 *      wait or fence status that answers VK_SUCCESS after a suspicion clears it (the device ran
 *      work: "said lost, then fine", logged). The app gets the driver's answers as they come,
 *      as without the latch, until the latch is set. From then on, submissions,
 *      waits and fence/semaphore polls on it return VK_ERROR_DEVICE_LOST without reaching the
 *      driver: a lost device runs nothing more, and every new submission is one more fence the
 *      driver's thread signals with an error (the path E6ZA's driver died on). Vulkan allows
 *      exactly this: once a device is lost, these calls may return VK_ERROR_DEVICE_LOST. Installed
 *      beneath every layer, so the layers' own submissions stop too. A device that works pays one
 *      load of its flag per call. VKBRIDGE_LOST_LATCH=0 turns it off (the A/B).
 *   2. A driver thread that faults after a loss. A SIGSEGV or SIGBUS on one of the driver's own
 *      threads (named "mali-..."; never one of the server's) with the pc in Arm's driver, while a
 *      device is latched lost: the thread is stopped for good inside the crash handler instead of
 *      taking the process down (on_crash, server_main.c). Arm's driver runs an event thread per
 *      GPU context (each VkDevice its own), so what stops is the lost device's event handling;
 *      whatever lock that thread held stays held, so the lost device (every device latched lost at
 *      that moment) is "stopped" as a whole: the server refuses every further request on it
 *      without reaching the driver (the client takes the refusal as a lost device), and it is never
 *      destroyed or freed - its process's exit leaves it, and its instance, as they are, as a
 *      sync-fd export still blocked in the driver already does. The memory it holds stays until
 *      the session ends. If the driver's event thread was not the device's own after all, other
 *      programs' waits stop finishing: a watchdog sees calls in the driver and none finishing for
 *      STALL_S and lets the stopped thread go on to die exactly as before (debuggerd's tombstone
 *      of its own fault). At most PARK_MAX threads; past that, as before. VKBRIDGE_NO_CRASH_GUARD=1
 *      or VKBRIDGE_LOST_PARK=0 turns it off.
 *
 * Imitation (host tests), VKBRIDGE_TEST_LOST=<n>[:after|:loss]: beneath the latch, the n-th
 * submission of a device of the program VKBRIDGE_TEST_LOST_COMM names (default lost_check) and every call the latch covers after it return VK_ERROR_DEVICE_LOST
 * (an imitated loss). ":after": every submission that reaches the imitated lost device after the
 * loss starts a thread named "mali-event-hand" that faults inside the real driver (a driver
 * query writing through address 0x101), as E6ZA's driver did on the submissions after its loss.
 * ":loss": that thread starts at the loss itself, where the latch cannot help.
 * ":blip": the n-th FENCE WAIT answers -4 and so does the poll right after it, then the driver
 * answers as it is (the device is fine): a slow job-manager driver's transient; ":blip:again": the
 * next fence wait does it once more. ":stop" (with the memory-fault watchdog, server_guard.c): a
 * device that watchdog stops has its "mali-event-hand" thread fault right after, and while that
 * thread is parked every fence wait hangs in the imitated driver (the driver needed the thread);
 * VKBRIDGE_TEST_PARK_STALL_S shortens the release's STALL_S. ":wait": the device
 * is lost from the n-th fence wait on (waits, polls and submissions answer -4). ":wait:evt": as
 * ":wait", and every submission that reaches the lost device starts the faulting "mali-event-hand"
 * thread, as ":after" does: E6ZA's own order (the loss first answered by vkWaitForFences, then the
 * game's submissions answered -4, then the driver's event thread faulted).
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define PARK_MAX 4
#define STALL_S 30
#define CONFIRM_MS 100 /* a wait's lost answer is confirmed by another this much later (not the same burst of polls) */
#define SUSPECT_MS 2000 /* ... and no later than this: an older suspicion nothing confirmed is dropped, the next lost answer is a new one */

typedef struct lost_dev {
    vkb_srv_table *t;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
    PFN_vkQueueBindSparse sparse;
    PFN_vkWaitForFences wait_fences;
    PFN_vkWaitSemaphores wait_sems;
    PFN_vkGetFenceStatus fence_status;
    PFN_vkGetSemaphoreCounterValue sem_value;
    PFN_vkDeviceWaitIdle device_idle;
    PFN_vkQueueWaitIdle queue_idle;
    /* the imitation's (host tests): what it wraps */
    PFN_vkQueueSubmit i_submit;
    PFN_vkQueueSubmit2 i_submit2;
    PFN_vkWaitForFences i_wait_fences;
    PFN_vkWaitSemaphores i_wait_sems;
    PFN_vkGetFenceStatus i_fence_status;
    volatile int lost;    /* latched */
    uint64_t suspect_ns;  /* a wait's lost answer not confirmed yet: when (0: none) */
    unsigned long blips;  /* suspicions a VK_SUCCESS took back */
    unsigned long stale;  /* suspicions dropped for age (SUSPECT_MS) */
    char why_buf[160];    /* a confirmed suspicion's reason (written once, before the latch) */
    int confirming;
    volatile int stopped; /* a driver thread stopped while it was lost: never touched again */
    uint32_t imi_submits, imi_waits;
    volatile int imi_lost;
    volatile int imi_blip; /* VKBRIDGE_TEST_LOST=<n>:blip: lost answers left before the driver's own again */
    const char *why;
} lost_dev;

static lost_dev *devs[VKB_MAX_TABLES];
static uint32_t devs_top; /* one past the highest id recorded: what a scan by handle covers */
static int latch_off = -1, park_off = -1;
static int imi_at = -1, imi_mode, imi_evt; /* imi_evt: ":wait:evt" */ /* VKBRIDGE_TEST_LOST: the n-th submission; 1 :after, 2 :loss; the n-th fence wait: 3 :blip, 4 :wait; 5 :stop */
static int imi_again; /* ":blip:again": the next fence wait blips again */
static int stall_s = STALL_S; /* VKBRIDGE_TEST_PARK_STALL_S (host tests) */
static volatile int lost_count, stopped_count, parked_threads;
static volatile int park_release; /* the watchdog lets the stopped threads die as before */
static volatile long progress, in_flight;

static void env_once(void)
{
    if (latch_off >= 0) return;
    const char *s = vkb_srv_env_global("VKBRIDGE_LOST_LATCH");
    latch_off = s && *s == '0';
    s = vkb_srv_env_global("VKBRIDGE_LOST_PARK");
    const char *g = vkb_srv_env_global("VKBRIDGE_NO_CRASH_GUARD");
    park_off = (s && *s == '0') || (g && *g && *g != '0');
    s = getenv("VKBRIDGE_TEST_LOST");
    if (s && *s) {
        imi_at = atoi(s);
        imi_mode = strstr(s, ":after") ? 1 : strstr(s, ":loss") ? 2 : strstr(s, ":blip") ? 3 : strstr(s, ":wait") ? 4 : strstr(s, ":stop") ? 5 : 0;
        imi_evt = imi_mode == 4 && strstr(s, ":evt") != NULL;
        imi_again = imi_mode == 3 && strstr(s, ":again") != NULL;
        if (imi_mode == 5)
            VKB_WARN("VKBRIDGE_TEST_LOST=%s: a device the memory-fault watchdog stops has its driver event thread fault after the "
                     "stop, and while it is stopped every fence wait hangs in the driver (imitated)", s);
        else if (imi_mode >= 3)
            VKB_WARN("VKBRIDGE_TEST_LOST=%s: a client device's fence wait %d answers VK_ERROR_DEVICE_LOST %s (imitated)", s, imi_at,
                     imi_mode == 3 ? "with its poll, then the device works again" : "and loses it");
        else
            VKB_WARN("VKBRIDGE_TEST_LOST=%s: a client device's submission %d loses it (imitated)", s, imi_at);
    }
    s = getenv("VKBRIDGE_TEST_PARK_STALL_S");
    if (s && atoi(s) > 0) stall_s = atoi(s);
    if (latch_off) VKB_INFO("lost devices: the latch is off (VKBRIDGE_LOST_LATCH=0)");
}

static lost_dev *dev_of_table(vkb_srv_table *t)
{
    return t && t->id < VKB_MAX_TABLES ? devs[t->id] : NULL;
}

/* The call's device: the current table's, else by handle (a server thread on another device's
 * behalf). */
static lost_dev *dev_of(VkDevice d)
{
    lost_dev *l = dev_of_table(vkb_srv_current_table());
    if (l && (!d || l->t->device == d)) return l;
    uint32_t top = __atomic_load_n(&devs_top, __ATOMIC_ACQUIRE);
    for (uint32_t i = 1; d && i < top; i++) {
        lost_dev *x = __atomic_load_n(&devs[i], __ATOMIC_ACQUIRE);
        if (x && x->t->device == d) return x;
    }
    return NULL;
}

static void *watch_main(void *arg);
static void imi_fault(lost_dev *l);

/* The watchdog of a stopped driver thread (vkb_lost_fault): started with the first lost device, by the
 * latch or by a stop (vkb_lost_stop), so that a thread parked after either can be released; never from
 * the crash handler. */
static void watch_start(void)
{
    static int watching;
    pthread_t th;
    if (!park_off && !__atomic_exchange_n(&watching, 1, __ATOMIC_ACQ_REL) && pthread_create(&th, NULL, watch_main, NULL) == 0)
        pthread_detach(th);
}

static void latch(lost_dev *l, const char *why)
{
    if (!l || latch_off || __atomic_exchange_n(&l->lost, 1, __ATOMIC_ACQ_REL)) return;
    l->why = why;
    __atomic_add_fetch(&lost_count, 1, __ATOMIC_RELAXED);
    watch_start();
    VKB_ERR("device %u (pid %d) is lost (%s): its submissions and waits now answer VK_ERROR_DEVICE_LOST without reaching the "
            "driver; the other programs go on", l->t->id, l->t->proc ? l->t->proc->pid : 0, why);
}

void vkb_lost_mark(vkb_srv_table *t, const char *why)
{
    env_once();
    latch(dev_of_table(t), why);
}

/* server_conn.c vkb_srv_end_proc (the memory-fault stall watchdog, server_guard.c): the device is
 * taken as lost and stopped - its requests are refused and it is never destroyed - because a
 * thread of its program is stuck in the driver behind a lock a contained fault left held. */
void vkb_lost_stop(vkb_srv_table *t, const char *why)
{
    env_once();
    lost_dev *l = dev_of_table(t);
    if (!l) return;
    if (!__atomic_exchange_n(&l->lost, 1, __ATOMIC_ACQ_REL)) {
        l->why = why;
        __atomic_add_fetch(&lost_count, 1, __ATOMIC_RELAXED);
    }
    /* a lost device now counts (vkb_lost_fault parks a driver thread's fault): its release must be able to come
     * (test11 audit P2: an A2 stop alone never started it, and a parked thread the driver needed froze every program) */
    watch_start();
    if (!__atomic_exchange_n(&l->stopped, 1, __ATOMIC_ACQ_REL)) __atomic_add_fetch(&stopped_count, 1, __ATOMIC_RELAXED);
    if (imi_mode == 5) imi_fault(l); /* host tests: the stopped device's driver event thread faults after the stop */
}

int vkb_lost_is(const vkb_srv_table *t)
{
    lost_dev *l = t && t->id < VKB_MAX_TABLES ? devs[t->id] : NULL;
    return l && __atomic_load_n(&l->lost, __ATOMIC_RELAXED);
}

int vkb_lost_stopped(const vkb_srv_table *t)
{
    if (!t || !__atomic_load_n(&stopped_count, __ATOMIC_RELAXED)) return 0;
    if (!t->is_device) {
        /* an instance with a stopped device: left as it is too */
        for (uint32_t i = 1; i < __atomic_load_n(&devs_top, __ATOMIC_ACQUIRE); i++) {
            lost_dev *x = __atomic_load_n(&devs[i], __ATOMIC_ACQUIRE);
            if (x && x->stopped && x->t->parent == t) return 1;
        }
        return 0;
    }
    lost_dev *l = t->id < VKB_MAX_TABLES ? devs[t->id] : NULL;
    return l && l->stopped;
}

/* After a stop, the watchdog's view of the driver: calls in it and calls coming back. */
static inline int counting(void)
{
    return __atomic_load_n(&parked_threads, __ATOMIC_RELAXED) != 0;
}
#define ENTER() int cnt_ = counting(); if (cnt_) __atomic_add_fetch(&in_flight, 1, __ATOMIC_RELAXED)
#define LEAVE() if (cnt_) { __atomic_sub_fetch(&in_flight, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&progress, 1, __ATOMIC_RELAXED); }

static int is_lost(const lost_dev *l)
{
    return __atomic_load_n(&l->lost, __ATOMIC_RELAXED);
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A wait's (or a fence status's) lost answer, repeated by the driver: the first is a suspicion,
 * one CONFIRM_MS or more after it with no VK_SUCCESS between latches the device. */
static void suspect(lost_dev *l, const char *what)
{
    if (latch_off) return;
    uint64_t now = mono_ns(), was = __atomic_load_n(&l->suspect_ns, __ATOMIC_ACQUIRE);
    if (!was) {
        if (__atomic_compare_exchange_n(&l->suspect_ns, &was, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            VKB_WARN("device %u (pid %d): %s answered VK_ERROR_DEVICE_LOST, and so did a poll right after: not taken as lost "
                     "until the driver says so again (a job-manager driver's timed-out wait can work again after it)",
                     l->t->id, l->t->proc ? l->t->proc->pid : 0, what);
        return;
    }
    if (now - was < (uint64_t)CONFIRM_MS * 1000000ull) return; /* the same burst of polls */
    if (now - was > (uint64_t)SUSPECT_MS * 1000000ull) {
        /* too old to confirm: nothing said lost for SUSPECT_MS, and a VK_SUCCESS that would have taken it back may
         * never reach the server (the client answers finished fences itself). A job-manager driver's transients
         * (wait -4, poll -4, then it works) must not add up into a latch (test11 audit P3, ledger 166). */
        if (__atomic_compare_exchange_n(&l->suspect_ns, &was, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            unsigned long n = __atomic_add_fetch(&l->stale, 1, __ATOMIC_RELAXED);
            if (n <= 3 || n % 100 == 0)
                VKB_WARN("device %u (pid %d): %s answered VK_ERROR_DEVICE_LOST again %.1f s after a lost answer nothing confirmed "
                         "within %d ms: a new suspicion, not taken as lost (%lu time(s))", l->t->id, l->t->proc ? l->t->proc->pid : 0,
                         what, (now - was) / 1e9, SUSPECT_MS, n);
        }
        return;
    }
    if (__atomic_exchange_n(&l->confirming, 1, __ATOMIC_ACQ_REL)) return; /* another thread confirms it now */
    if (!is_lost(l)) {
        snprintf(l->why_buf, sizeof(l->why_buf), "%s answered VK_ERROR_DEVICE_LOST again %.0f ms after the first lost answer", what,
                 (now - was) / 1e6);
        latch(l, l->why_buf);
    }
    __atomic_store_n(&l->confirming, 0, __ATOMIC_RELEASE);
}

/* A wait, poll or fence status that answered VK_SUCCESS: the device ran work; a suspicion is taken back. */
static inline void fine(lost_dev *l)
{
    if (__builtin_expect(!__atomic_load_n(&l->suspect_ns, __ATOMIC_RELAXED), 1)) return;
    if (!__atomic_exchange_n(&l->suspect_ns, 0, __ATOMIC_ACQ_REL)) return;
    unsigned long n = __atomic_add_fetch(&l->blips, 1, __ATOMIC_RELAXED);
    if (n <= 3 || n % 100 == 0)
        VKB_INFO("device %u (pid %d): said lost, then finished work: not lost (%lu time(s))", l->t->id,
                 l->t->proc ? l->t->proc->pid : 0, n);
}

/* ------------------------------------------------------------------ the latch */

static VKAPI_ATTR VkResult VKAPI_CALL lo_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    lost_dev *l = dev_of_table(vkb_srv_current_table());
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    ENTER();
    VkResult r = l->submit(q, n, si, f);
    LEAVE();
    if (r == VK_ERROR_DEVICE_LOST) latch(l, "vkQueueSubmit returned VK_ERROR_DEVICE_LOST");
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lo_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f)
{
    lost_dev *l = dev_of_table(vkb_srv_current_table());
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    ENTER();
    VkResult r = l->submit2(q, n, si, f);
    LEAVE();
    if (r == VK_ERROR_DEVICE_LOST) latch(l, "vkQueueSubmit2 returned VK_ERROR_DEVICE_LOST");
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lo_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *bi, VkFence f)
{
    lost_dev *l = dev_of_table(vkb_srv_current_table());
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    ENTER();
    VkResult r = l->sparse(q, n, bi, f);
    LEAVE();
    if (r == VK_ERROR_DEVICE_LOST) latch(l, "vkQueueBindSparse returned VK_ERROR_DEVICE_LOST");
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lo_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{
    lost_dev *l = dev_of(d);
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    ENTER();
    VkResult r = l->wait_fences(d, n, f, all, timeout);
    LEAVE();
    /* lost for real when the fence's status says so too */
    if (r == VK_SUCCESS) fine(l);
    else if (r == VK_ERROR_DEVICE_LOST && n && f && l->fence_status && l->fence_status(d, f[0]) == VK_ERROR_DEVICE_LOST)
        suspect(l, "vkWaitForFences");
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lo_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    lost_dev *l = dev_of(d);
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    ENTER();
    VkResult r = l->wait_sems(d, wi, timeout);
    LEAVE();
    /* Arm's job-manager drivers end a timed-out wait with -4 that a poll takes back (server_wait.c) */
    if (r == VK_SUCCESS) fine(l);
    else if (r == VK_ERROR_DEVICE_LOST && wi && l->wait_sems(d, wi, 0) == VK_ERROR_DEVICE_LOST)
        suspect(l, "vkWaitSemaphores");
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lo_GetFenceStatus(VkDevice d, VkFence f)
{
    lost_dev *l = dev_of(d);
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    VkResult r = l->fence_status(d, f);
    if (r == VK_SUCCESS) fine(l);
    /* a fence status's -4 only confirms a wait's: a poll loop alone never latches on its first answer */
    else if (r == VK_ERROR_DEVICE_LOST && __atomic_load_n(&l->suspect_ns, __ATOMIC_RELAXED)) suspect(l, "vkGetFenceStatus");
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lo_GetSemaphoreCounterValue(VkDevice d, VkSemaphore s, uint64_t *v)
{
    lost_dev *l = dev_of(d);
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    return l->sem_value(d, s, v);
}

static VKAPI_ATTR VkResult VKAPI_CALL lo_DeviceWaitIdle(VkDevice d)
{
    lost_dev *l = dev_of(d);
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    ENTER();
    VkResult r = l->device_idle(d);
    LEAVE();
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lo_QueueWaitIdle(VkQueue q)
{
    lost_dev *l = dev_of_table(vkb_srv_current_table());
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (is_lost(l)) return VK_ERROR_DEVICE_LOST;
    ENTER();
    VkResult r = l->queue_idle(q);
    LEAVE();
    return r;
}

/* ------------------------------------------------------------------ the imitation (host tests) */

typedef struct imi_fault_arg {
    PFN_vkGetPhysicalDeviceQueueFamilyProperties query;
    VkPhysicalDevice pd;
} imi_fault_arg;

static void *imi_fault_main(void *arg)
{
    imi_fault_arg a = *(imi_fault_arg *)arg;
    free(arg);
    prctl(PR_SET_NAME, "mali-event-hand", 0, 0, 0);
    struct timespec ts = {0, 50000000}; /* E6ZA's came 111 ms after the loss, 19 ms after the last submission */
    nanosleep(&ts, NULL);
    VKB_WARN("VKBRIDGE_TEST_LOST: the imitated driver's event thread faults inside the driver");
    /* a driver query writing its count through address 0x101: the fault is inside the driver */
    a.query(a.pd, (uint32_t *)(uintptr_t)0x101, NULL);
    return NULL;
}

static void imi_fault(lost_dev *l)
{
    imi_fault_arg *a = malloc(sizeof(*a));
    if (!a) return;
    a->query = l->t->parent ? l->t->parent->real.vkGetPhysicalDeviceQueueFamilyProperties : NULL;
    if (!a->query) {
        free(a);
        return;
    }
    a->pd = l->t->physical;
    pthread_t th;
    if (pthread_create(&th, NULL, imi_fault_main, a) == 0) pthread_detach(th);
    else free(a);
}

/* only the program the test names (VKBRIDGE_TEST_LOST_COMM, default lost_check): the others work */
static int imi_named(const lost_dev *l)
{
    const char *comm = getenv("VKBRIDGE_TEST_LOST_COMM");
    return l->t->proc && !strcmp(l->t->proc->comm, comm && *comm ? comm : "lost_check");
}

/* A fence wait reaching the imitated driver (:blip, :wait): the n-th answers -4. :blip: the poll
 * right after does too (a slow job-manager driver), then the driver answers as it is (fine);
 * :wait: the device is lost from it on (waits, polls and submissions answer -4). */
static int imi_wait(lost_dev *l)
{
    if (imi_mode < 3 || imi_mode > 4 || !imi_named(l)) return 0;
    int nth = (int)__atomic_add_fetch(&l->imi_waits, 1, __ATOMIC_RELAXED);
    if (nth != imi_at && !(imi_again && nth == imi_at + 1)) return 0;
    if (imi_mode == 3) {
        __atomic_store_n(&l->imi_blip, 1, __ATOMIC_RELEASE);
        VKB_WARN("VKBRIDGE_TEST_LOST: device %u: fence wait %d answers VK_ERROR_DEVICE_LOST, its poll too, then the device works (imitated)",
                 l->t->id, nth);
    } else {
        __atomic_store_n(&l->imi_lost, 1, __ATOMIC_RELEASE);
        VKB_WARN("VKBRIDGE_TEST_LOST: device %u lost (imitated) at its fence wait %d", l->t->id, imi_at);
    }
    return 1;
}

static int imi_blip_take(lost_dev *l)
{
    int b = __atomic_load_n(&l->imi_blip, __ATOMIC_ACQUIRE);
    return b > 0 && __atomic_compare_exchange_n(&l->imi_blip, &b, b - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* A submission reaching the imitated driver: the n-th loses the device; after the loss each one
 * (":after") or the loss itself (":loss") makes the imitated event thread fault. */
static int imi_submit(lost_dev *l)
{
    if (__atomic_load_n(&l->imi_lost, __ATOMIC_ACQUIRE)) {
        if (imi_mode == 1 || imi_evt) imi_fault(l);
        return 1;
    }
    if (imi_mode >= 3 || !imi_named(l)) return 0; /* :blip and :wait count fence waits */
    if ((int)__atomic_add_fetch(&l->imi_submits, 1, __ATOMIC_RELAXED) != imi_at) return 0;
    __atomic_store_n(&l->imi_lost, 1, __ATOMIC_RELEASE);
    VKB_WARN("VKBRIDGE_TEST_LOST: device %u lost (imitated) at its submission %d", l->t->id, imi_at);
    if (imi_mode == 2) imi_fault(l);
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    lost_dev *l = dev_of_table(vkb_srv_current_table());
    if (l && imi_submit(l)) return VK_ERROR_DEVICE_LOST;
    return l ? l->i_submit(q, n, si, f) : VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f)
{
    lost_dev *l = dev_of_table(vkb_srv_current_table());
    if (l && imi_submit(l)) return VK_ERROR_DEVICE_LOST;
    return l ? l->i_submit2(q, n, si, f) : VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{
    lost_dev *l = dev_of(d);
    if (l && __atomic_load_n(&l->imi_lost, __ATOMIC_ACQUIRE)) return VK_ERROR_DEVICE_LOST;
    if (!l) return VK_ERROR_DEVICE_LOST;
    if (imi_mode == 5 && __atomic_load_n(&parked_threads, __ATOMIC_ACQUIRE)) {
        /* :stop: the driver needed the stopped thread after all - every program's fence wait hangs in it */
        VKB_WARN("VKBRIDGE_TEST_LOST: device %u: a fence wait hangs in the imitated driver (a stopped driver thread)", l->t->id);
        while (!__atomic_load_n(&park_release, __ATOMIC_ACQUIRE)) {
            struct timespec ts = {0, 100000000};
            nanosleep(&ts, NULL);
        }
        return VK_ERROR_DEVICE_LOST;
    }
    VkResult r = l->i_wait_fences(d, n, f, all, timeout);
    return imi_wait(l) ? VK_ERROR_DEVICE_LOST : r;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    lost_dev *l = dev_of(d);
    if (l && (__atomic_load_n(&l->imi_lost, __ATOMIC_ACQUIRE) || imi_blip_take(l))) return VK_ERROR_DEVICE_LOST;
    return l ? l->i_wait_sems(d, wi, timeout) : VK_ERROR_DEVICE_LOST;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_GetFenceStatus(VkDevice d, VkFence f)
{
    lost_dev *l = dev_of(d);
    if (l && (__atomic_load_n(&l->imi_lost, __ATOMIC_ACQUIRE) || imi_blip_take(l))) return VK_ERROR_DEVICE_LOST;
    return l ? l->i_fence_status(d, f) : VK_ERROR_DEVICE_LOST;
}

/* ------------------------------------------------------------------ install */

#define LO_HOOK(field, fn, save)                                                                     \
    do {                                                                                             \
        if (t->real.field && t->dt.field == t->real.field) {                                        \
            l->save = t->real.field;                                                                 \
            t->dt.field = t->real.field = (fn);                                                      \
        }                                                                                            \
    } while (0)

void vkb_lost_device_init(vkb_srv_table *t)
{
    env_once();
    if (!t || t->id >= VKB_MAX_TABLES || !t->is_device) return;
    lost_dev *l = calloc(1, sizeof(*l));
    if (!l) return;
    l->t = t;
    if (imi_at > 0) {
        LO_HOOK(vkQueueSubmit, imi_QueueSubmit, i_submit);
        LO_HOOK(vkQueueSubmit2, imi_QueueSubmit2, i_submit2);
        LO_HOOK(vkWaitForFences, imi_WaitForFences, i_wait_fences);
        LO_HOOK(vkWaitSemaphores, imi_WaitSemaphores, i_wait_sems);
        LO_HOOK(vkGetFenceStatus, imi_GetFenceStatus, i_fence_status);
    }
    if (!latch_off) {
        LO_HOOK(vkQueueSubmit, lo_QueueSubmit, submit);
        LO_HOOK(vkQueueSubmit2, lo_QueueSubmit2, submit2);
        LO_HOOK(vkQueueBindSparse, lo_QueueBindSparse, sparse);
        LO_HOOK(vkWaitForFences, lo_WaitForFences, wait_fences);
        LO_HOOK(vkWaitSemaphores, lo_WaitSemaphores, wait_sems);
        LO_HOOK(vkGetFenceStatus, lo_GetFenceStatus, fence_status);
        LO_HOOK(vkGetSemaphoreCounterValue, lo_GetSemaphoreCounterValue, sem_value);
        LO_HOOK(vkDeviceWaitIdle, lo_DeviceWaitIdle, device_idle);
        LO_HOOK(vkQueueWaitIdle, lo_QueueWaitIdle, queue_idle);
    }
    __atomic_store_n(&devs[t->id], l, __ATOMIC_RELEASE);
    for (uint32_t top = __atomic_load_n(&devs_top, __ATOMIC_RELAXED); top <= t->id;)
        if (__atomic_compare_exchange_n(&devs_top, &top, t->id + 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) break;
}

void vkb_lost_device_destroy(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES) return;
    lost_dev *l = devs[t->id];
    if (!l || l->t != t || l->stopped) return; /* a stopped device keeps its record: it is never destroyed */
    __atomic_store_n(&devs[t->id], NULL, __ATOMIC_RELEASE);
    if (l->lost) __atomic_sub_fetch(&lost_count, 1, __ATOMIC_RELAXED);
    /* its wrappers stay in t->real until the table goes, which is right after */
    free(l);
}

/* ------------------------------------------------------------------ a driver thread's fault after a loss */

static void *watch_main(void *arg)
{
    (void)arg;
    long last = __atomic_load_n(&progress, __ATOMIC_RELAXED);
    int still = 0;
    for (;;) {
        if (!__atomic_load_n(&parked_threads, __ATOMIC_RELAXED)) {
            struct timespec idle = {1, 0};
            nanosleep(&idle, NULL);
            last = __atomic_load_n(&progress, __ATOMIC_RELAXED);
            continue;
        }
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
        long p = __atomic_load_n(&progress, __ATOMIC_RELAXED), f = __atomic_load_n(&in_flight, __ATOMIC_RELAXED);
        if (p != last || f <= 0) {
            last = p;
            still = 0;
            continue;
        }
        if (++still < stall_s) continue;
        VKB_ERR("lost devices: %ld call(s) in the driver and none came back for %d s after a driver thread was stopped: the "
                "driver needs that thread after all; it now dies of its fault as it would have", f, stall_s);
        __atomic_store_n(&park_release, 1, __ATOMIC_RELEASE);
        return NULL;
    }
}

/* From the crash handler (server_main.c on_crash), before anything else. Returns at once unless
 * this is a driver thread's fault after a loss; then only when the watchdog lets it die. */
void vkb_lost_fault(int sig, const void *addr, uintptr_t pc, const char *lib, const char *lib_name, uintptr_t lib_off)
{
    if (sig != SIGSEGV && sig != SIGBUS) return;
    /* with the latch off (VKBRIDGE_LOST_LATCH=0) no wrapper counts the driver calls the watchdog judges by, so a
     * parked thread could never be released: it dies as before */
    if (park_off != 0 || latch_off || !__atomic_load_n(&lost_count, __ATOMIC_RELAXED)) return;
    char name[17] = "";
    prctl(PR_GET_NAME, name, 0, 0, 0);
    name[16] = 0;
    if (strncmp(name, "mali-", 5) != 0) return; /* the driver's threads; never one of the server's */
    int driver = lib && strstr(lib, "mali") != NULL;
    if (!driver && imi_at > 0) driver = lib && !strstr(lib, "vkbridge"); /* host tests: the desktop driver */
    if (!driver) return;
    if (__atomic_add_fetch(&parked_threads, 1, __ATOMIC_RELAXED) > PARK_MAX) return;
    /* Every device lost at this moment: the thread was one of theirs. */
    char ids[160];
    int n = 0;
    ids[0] = 0;
    for (uint32_t i = 1; i < __atomic_load_n(&devs_top, __ATOMIC_ACQUIRE); i++) {
        lost_dev *x = __atomic_load_n(&devs[i], __ATOMIC_ACQUIRE);
        if (!x || !x->lost || x->stopped) continue;
        x->stopped = 1;
        __atomic_add_fetch(&stopped_count, 1, __ATOMIC_RELAXED);
        if (n < (int)sizeof(ids) - 24) n += snprintf(ids + n, sizeof(ids) - (size_t)n, "%s%u (pid %d)", n ? ", " : "", x->t->id,
                                                     x->t->proc ? x->t->proc->pid : 0);
    }
    char line[640];
    int k = snprintf(line, sizeof(line),
                     "vkbridge-server E driver thread %ld \"%s\" faulted (signal %d, address %p, pc %p %s+0x%lx) after device %s was lost: "
                     "the thread is stopped and the server goes on; that device is left as it is (never destroyed) and its "
                     "program's further calls are refused\n",
                     (long)syscall(SYS_gettid), name, sig, addr, (void *)pc, lib_name ? lib_name : "?", (unsigned long)lib_off,
                     n ? ids : "(none left)");
    vkb_crash_log_raw(line, k > 0 && k < (int)sizeof(line) ? (size_t)k : sizeof(line) - 1);
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    while (!__atomic_load_n(&park_release, __ATOMIC_ACQUIRE)) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
    }
    sigdelset(&all, sig);
    pthread_sigmask(SIG_SETMASK, &all, NULL); /* dies of its own signal next */
}

/* run_request (server_conn.c): a request on a stopped device is refused without reaching the driver. */
int vkb_lost_refuse(const vkb_srv_table *t)
{
    return __atomic_load_n(&stopped_count, __ATOMIC_RELAXED) && t && t->is_device && vkb_lost_stopped(t);
}
