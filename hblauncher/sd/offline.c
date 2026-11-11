/* Starting without the file server (MKW_SD=all with a card prepared by tools/sd-card.sh push).
 * Linked with --wrap=hbxl_vfs_init and --wrap=hbxl_vfs_time_offset_ns; with a server nothing
 * changes except that its connection may take at most PROBE_MS.
 *
 * - Waiting: the runtime connects to the server once, at startup. If nothing answers at that
 *   address (the PC is off, or the app points at an old address), the console's connect gives
 *   up only after about 75 seconds. Here the connect gets PROBE_MS instead.
 * - The time of day: the runtime learns it from the server when it connects. Without one the game
 *   would think it is 1970: saves get wrong dates, and online the certificates would look
 *   invalid. So with no server this reads the console's own clock once, the one in System
 *   Settings. */
#include "hbxl.h"
#include <stdint.h>

void *hbxl_sdk_proc(uint64_t offset);
long __hbx_syscall(long, long, long, long, long, long, long);
int __real_hbxl_vfs_init(void);
int64_t __real_hbxl_vfs_time_offset_ns(void);

enum { PROBE_MS = 3000 };
/* Module offsets from sdk/sdk-symbols.json (SmileBASIC 4.4.9). */
enum {
    TIME_INITIALIZE = 3995872,     /* nn::time::Initialize() */
    TIME_IS_INITIALIZED = 3996624, /* nn::time::IsInitialized() */
    USER_CLOCK_NOW = 4006116,      /* nn::time::StandardUserSystemClock::GetCurrentTime(PosixTime *) */
};
static int64_t console_offset;

/* nn::socket's constants, as in the runtime's net.c. */
enum { F_GET = 3, F_SET = 4, NONBLOCK = 0x800, POLL_OUT = 4, SOCKET_LEVEL = 0xffff, SOCKET_ERROR = 0x1007 };
struct poll_entry { int fd; short events, revents; };
typedef int (*connect_fn)(int, const void *, uint32_t);
static connect_fn sdk_connect;
static int timed_out;

/* Takes the runtime's connect while hbxl_vfs_init runs (one thread, before anything else). */
static int connect_or_give_up(int s, const void *address, uint32_t length) {
    int (*fcntl_)(int, int, int) = HBXL_CALL(IMP_SOCK_FCNTL, int (*)(int, int, int));
    const int flags = fcntl_(s, F_GET, 0);
    if (flags < 0 || fcntl_(s, F_SET, flags | NONBLOCK) < 0)
        return sdk_connect(s, address, length);
    int rc = sdk_connect(s, address, length);
    if (rc < 0) { /* in progress, or refused: poll tells which */
        struct poll_entry entry = {s, POLL_OUT, 0};
        int error = -1;
        uint32_t size = sizeof(error);
        const int ready = HBXL_CALL(IMP_SOCK_POLL, int (*)(void *, uint64_t, int))(&entry, 1, PROBE_MS);
        timed_out = ready == 0;
        if (ready == 1 &&
            HBXL_CALL(IMP_SOCK_GETSOCKOPT, int (*)(int, int, int, void *, uint32_t *))(
                s, SOCKET_LEVEL, SOCKET_ERROR, &error, &size) == 0 && error == 0)
            rc = 0;
    }
    fcntl_(s, F_SET, flags);
    return rc;
}

static void time_from_console(void) {
    _Bool (*is_initialized)(void) = (_Bool (*)(void))hbxl_sdk_proc(TIME_IS_INITIALIZED);
    uint32_t (*initialize)(void) = (uint32_t (*)(void))hbxl_sdk_proc(TIME_INITIALIZE);
    uint32_t (*now)(int64_t *) = (uint32_t (*)(int64_t *))hbxl_sdk_proc(USER_CLOCK_NOW);
    uint32_t result = is_initialized() ? 0 : initialize();
    int64_t seconds = 0;
    if (!result)
        result = now(&seconds);
    if (result || seconds <= 0) {
        hbxl_logf("[sd] no file server, and no time from the console (result 0x%x)", (uint64_t)result);
        return;
    }
    struct { int64_t sec, nsec; } mono;
    __hbx_syscall(113, 1, (long)&mono, 0, 0, 0, 0); /* clock_gettime(CLOCK_MONOTONIC) */
    console_offset = seconds * 1000000000ll - (mono.sec * 1000000000ll + mono.nsec);
    hbxl_logf("[sd] no file server; time of day from the console (%d s since 1970)", seconds);
}

int __wrap_hbxl_vfs_init(void) {
    const uint64_t saved = hbxl_imp[IMP_CONNECT];
    sdk_connect = (connect_fn)(uintptr_t)saved;
    hbxl_imp[IMP_CONNECT] = (uint64_t)(uintptr_t)connect_or_give_up;
    const int rc = __real_hbxl_vfs_init();
    hbxl_imp[IMP_CONNECT] = saved;
    if (rc != 0) {
        if (timed_out)
            hbxl_logf("[sd] the file server didn't answer within %d ms", (int64_t)PROBE_MS);
        time_from_console();
    }
    return rc;
}

int64_t __wrap_hbxl_vfs_time_offset_ns(void) {
    int64_t offset = __real_hbxl_vfs_time_offset_ns();
    return offset ? offset : console_offset;
}
