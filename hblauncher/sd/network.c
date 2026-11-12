/* Network access after the console sleeps.
 *
 * A Switch app may use the network only while the system grants its network request (nifm). Sleep
 * takes that away, and apps are expected to ask again when they wake. The game runs inside
 * SmileBASIC, which never asks again, so after sleep every connection fails with "network
 * unreachable" although the console shows Wi-Fi connected. Here, before the game opens a
 * connection, and when one fails that way, the request is submitted again (at most every few
 * seconds) and a failed call is retried once. Normally the request is granted and this costs one
 * check per connection. Linked with --wrap=hbxl_net_syscall. */
#include "hbx_sdk.h"
#include "hbxl.h"
#include <pthread.h>
#include <stdint.h>

void *hbxl_sdk_proc(uint64_t offset);
void *hbxl_sdk_memalign(size_t align, size_t size);
void hbxl_call_on_stack(void (*fn)(void *), void *arg, void *stack_top);
long __real_hbxl_net_syscall(long, long, long, long, long, long, long, int *);

/* Module offsets from sdk/sdk-symbols.json (SmileBASIC 4.4.9). */
enum {
    NIFM_INITIALIZE = 3736736,         /* nn::nifm::Initialize() */
    NIFM_IS_AVAILABLE = 3741104,       /* nn::nifm::IsNetworkAvailable() */
    NIFM_SUBMIT_AND_WAIT = 3740880,    /* nn::nifm::SubmitNetworkRequestAndWait() */
};
enum { CONNECT = 203, SENDTO = 206, SENDMSG = 211, ENETDOWN_ = 100, ENETUNREACH_ = 101 };
enum { STACK_BYTES = 64 << 10 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *stack;
static uint64_t last_check;
static int initialized;

/* System calls keep their buffers on the caller's stack, which must be SDK memory: run them on
 * one of our own. */
static void check_network(void *out) {
    _Bool (*available)(void) = (_Bool (*)(void))hbxl_sdk_proc(NIFM_IS_AVAILABLE);
    if (!initialized) {
        ((uint32_t (*)(void))hbxl_sdk_proc(NIFM_INITIALIZE))(); /* already done by SmileBASIC: no-op */
        initialized = 1;
    }
    int ok = available();
    if (!ok) {
        hbxl_log("[net] the game has no network access (the console slept?): asking for it again");
        ((void (*)(void))hbxl_sdk_proc(NIFM_SUBMIT_AND_WAIT))();
        ok = available();
        hbxl_log(ok ? "[net] network access is back" : "[net] the console didn't grant network access");
    }
    *(int *)out = ok;
}

/* Returns whether the network is available after asking again if needed; 1 when it was checked
 * a moment ago (the answer then can't have changed much). */
static int ensure_network(int force) {
    const uint64_t hz = hbxl_ctx && hbxl_ctx->tick_hz ? hbxl_ctx->tick_hz : 19200000;
    int ok = 1;
    pthread_mutex_lock(&lock);
    const uint64_t now = hbxl_svc_tick();
    if (force || !last_check || now - last_check > 5 * hz) {
        if (!stack)
            stack = hbxl_sdk_memalign(4096, STACK_BYTES);
        if (stack) {
            hbxl_call_on_stack(check_network, &ok, (char *)stack + STACK_BYTES);
            last_check = hbxl_svc_tick();
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

long __wrap_hbxl_net_syscall(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    if (n == CONNECT)
        ensure_network(0);
    long r = __real_hbxl_net_syscall(n, a, b, c, d, e, f, handled);
    if (*handled && (n == CONNECT || n == SENDTO || n == SENDMSG) && (r == -ENETUNREACH_ || r == -ENETDOWN_) &&
        ensure_network(1))
        r = __real_hbxl_net_syscall(n, a, b, c, d, e, f, handled);
    return r;
}
