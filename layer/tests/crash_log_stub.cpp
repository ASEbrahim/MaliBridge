// The layer tests link every server/*.c except server_main.c (CMakeLists.txt), but server_lost.c's fault
// report calls vkb_crash_log_raw, which lives in server_main.c next to the server's log file. In this
// test binary a crash line goes to stderr only.
#include <cstddef>
#include <unistd.h>

extern "C" void vkb_crash_log_raw(const char *line, size_t n)
{
    if (write(2, line, n) < 0) {
        // nothing else to report to from inside a crash handler
    }
}
