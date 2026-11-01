/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Driver crashes inside a shader or pipeline compile, in the layer: that call fails instead of the
 * game ending (server_guard.c arms the compile; the bridge's server_main.c on_crash jumps back).
 *
 * In the layer the process is the game's, and its runtime (Wine, box64, FEX, a JIT) takes faults
 * of its own all the time and handles them in its own handler. So the handler here only acts
 * when the faulting thread is inside a guarded driver call; every other fault goes to the handler
 * that was there before, called in place with the same siginfo and context, as if the layer had
 * never been loaded. With no handler before it, the default action is put back and the fault
 * repeats (the process ends as it would have). No logging here: nothing in a signal handler that
 * is not async-signal-safe.
 *
 * A runtime that installs its handler after the layer replaces this one: containment is then off
 * for that process, nothing else changes.
 *
 * Installed at the first vkCreateDevice. Off with VKBRIDGE_NO_CRASH_GUARD=1 (the guard's own
 * switch) or VKBRIDGE_LAYER_CRASH_HANDLER=0 (the guard's watchdog still interrupts compiles that
 * never end).
 */
#include "layer_crash.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>

extern "C" {
#include "vkb_server.h"
}

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
struct sigaction g_prev[32];

void on_fault(int sig, siginfo_t *si, void *uc) {
    int saved = errno;
    /* returns only when this thread is not inside a guarded call */
    vkb_guard_fault(sig, si ? si->si_addr : nullptr);
    errno = saved;
    if (sig <= 0 || sig >= 32)
        return;
    const struct sigaction *p = &g_prev[sig];
    /* the mask the handler before us asked for (installed with SA_NODEFER, so it is ours to set;
     * the kernel puts the thread's own mask back when the handler returns) */
    sigset_t mask = p->sa_mask;
    if (!(p->sa_flags & SA_NODEFER))
        sigaddset(&mask, sig);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (p->sa_flags & SA_SIGINFO) {
        if (p->sa_sigaction) {
            p->sa_sigaction(sig, si, uc);
            return;
        }
    } else if (p->sa_handler == SIG_IGN) {
        return;
    } else if (p->sa_handler != SIG_DFL) {
        p->sa_handler(sig);
        return;
    }
    /* nobody before us: the default action, and the fault again (or the signal, if it was sent) */
    signal(sig, SIG_DFL);
    if (!si || si->si_code <= 0)
        raise(sig);
}

bool wanted() {
    const char *off = getenv("VKBRIDGE_NO_CRASH_GUARD");
    if (off && *off && *off != '0')
        return false;
    const char *h = getenv("VKBRIDGE_LAYER_CRASH_HANDLER");
    return !(h && !std::strcmp(h, "0"));
}

} // namespace

void layer_crash_install() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!wanted()) {
            VKB_INFO("layer: no crash handler (VKBRIDGE_NO_CRASH_GUARD / VKBRIDGE_LAYER_CRASH_HANDLER=0): "
                     "a driver crash in a compile ends the process");
            return;
        }
        int n = 0;
        for (int sig : kSignals) {
            struct sigaction sa;
            std::memset(&sa, 0, sizeof(sa));
            sa.sa_sigaction = on_fault;
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
            sigemptyset(&sa.sa_mask);
            if (sigaction(sig, &sa, &g_prev[sig]) == 0)
                n++;
        }
        VKB_INFO("layer: driver crashes in pipeline and shader compiles fail that call (%d signals; other faults go to the "
                 "process's own handler)", n);
    });
}
