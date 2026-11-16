/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * cpuprof_unit: the per-thread CPU evidence (server/server_cpuprof.h; the "cpu-threads:" line of server_perf.c) on its own:
 *   1  /proc/<tid>/schedstat: "run_ns wait_ns slices" parsed; short, empty, non-numeric input refused;
 *   2  /proc/<tid>/wchan class: futex, epoll, pipe, socket, sleep, poll; "0" or empty = unreadable (Android hides it);
 *   3  a window of samples (known schedstat counters, states, wchans) gives the percentages run / waiting for a core /
 *      idle (the rest) and the sleeps by wait, the slices a second, as the line prints them; a counter that went
 *      backwards (a reused tid) adds nothing;
 *   4  the window starts again from its last counters (no jump), the line is bounded and parseable.
 * Prints "OK: ..." / "FAILED: ..." last. host/Makefile and host/CMakeLists.txt build it; host/run_tests.sh runs it.
 */
#include "../server/server_cpuprof.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void check(const char *what, int ok)
{
    if (!ok) failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

int main(void)
{
    cpuprof_sched s;
    check("schedstat parsed", cpuprof_parse_schedstat("123456789 2000000 77\n", &s) && s.run_ns == 123456789 && s.wait_ns == 2000000 && s.slices == 77);
    check("schedstat refused: short, empty, text", !cpuprof_parse_schedstat("12 34\n", &s) && !cpuprof_parse_schedstat("", &s) &&
                                                       !cpuprof_parse_schedstat("a b c", &s) && !cpuprof_parse_schedstat(NULL, &s));
    check("wchan: futex", cpuprof_wchan_class("futex_wait_queue") == CPUPROF_WC_FUTEX && cpuprof_wchan_class("do_futex") == CPUPROF_WC_FUTEX);
    check("wchan: epoll, pipe, socket, sleep, poll",
          cpuprof_wchan_class("do_epoll_wait") == CPUPROF_WC_EPOLL && cpuprof_wchan_class("ep_poll") == CPUPROF_WC_EPOLL &&
              cpuprof_wchan_class("pipe_read") == CPUPROF_WC_PIPE && cpuprof_wchan_class("unix_stream_read_generic") == CPUPROF_WC_SOCK &&
              cpuprof_wchan_class("sk_wait_data") == CPUPROF_WC_SOCK && cpuprof_wchan_class("hrtimer_nanosleep") == CPUPROF_WC_SLEEP &&
              cpuprof_wchan_class("do_sys_poll") == CPUPROF_WC_POLL && cpuprof_wchan_class("something_else") == CPUPROF_WC_OTHER);
    check("wchan: 0 / empty / NULL = unreadable", cpuprof_wchan_class("0") == CPUPROF_WC_NONE && cpuprof_wchan_class("") == CPUPROF_WC_NONE &&
                                                      cpuprof_wchan_class(NULL) == CPUPROF_WC_NONE);

    /* a 10 s window: run 7.7 s, waited for a core 0.5 s, 900 slices; 10 samples: 4 running, 6 asleep (3 futex, 2 epoll, 1 unreadable) */
    cpuprof_acc a;
    cpuprof_acc_init(&a);
    cpuprof_sched c = {1000000000ull, 100000000ull, 50};
    cpuprof_acc_sample(&a, &c, 'S', CPUPROF_WC_FUTEX); /* the first sample: the base */
    for (int i = 0; i < 10; i++) {
        c.run_ns += 770000000ull, c.wait_ns += 50000000ull, c.slices += 90;
        cpuprof_acc_sample(&a, &c, i < 4 ? 'R' : 'S', i < 4 ? CPUPROF_WC_NONE : i < 7 ? CPUPROF_WC_FUTEX : i < 9 ? CPUPROF_WC_EPOLL : CPUPROF_WC_NONE);
    }
    char t[256];
    cpuprof_acc_text(&a, "main", 10.0, "p7:100", t, sizeof(t));
    printf("     %s\n", t);
    check("line: run 77 rq 5 idle 18, 90 slices/s", strstr(t, "main{run=77 rq=5 idle=18 sl=90 ") != NULL);
    check("line: states and sleeps", strstr(t, "st=R4S6") != NULL && strstr(t, "wc=futex:3,epoll:2") != NULL);
    check("line: core clusters", strstr(t, "cpu=p7:100}") != NULL);
    check("line parseable: no spaces inside a field, one close brace", strchr(t, '}') == t + strlen(t) - 1 && strchr(t, '}') == strrchr(t, '}'));

    /* a counter going backwards: nothing added, no wrap */
    cpuprof_acc b;
    cpuprof_acc_init(&b);
    cpuprof_sched d = {5000000000ull, 0, 10};
    cpuprof_acc_sample(&b, &d, 'R', CPUPROF_WC_NONE);
    d.run_ns = 1000;
    cpuprof_acc_sample(&b, &d, 'R', CPUPROF_WC_NONE);
    cpuprof_acc_text(&b, "x", 10.0, "", t, sizeof(t));
    check("backwards counter: run=0", strstr(t, "x{run=0 ") != NULL);

    /* the next window starts from the last counters: 1 s of run in 10 s = 10 % */
    cpuprof_acc_reset(&a);
    c.run_ns += 1000000000ull, c.slices += 5;
    cpuprof_acc_sample(&a, &c, 'S', CPUPROF_WC_FUTEX);
    cpuprof_acc_text(&a, "main", 10.0, "p7:100", t, sizeof(t));
    check("next window from the last counters: run 10", strstr(t, "main{run=10 ") != NULL);
    char small[40];
    cpuprof_acc_text(&a, "main", 10.0, "p7:100", small, sizeof(small));
    check("bounded", strlen(small) < sizeof(small));

    printf("%s: cpuprof_unit %d failures\n", failures ? "FAILED" : "OK", failures);
    return failures != 0;
}
