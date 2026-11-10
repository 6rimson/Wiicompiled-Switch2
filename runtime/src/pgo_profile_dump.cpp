// Recording builds only (MKW_PGO=generate): the game has no exit a headless training run can
// reach, and the profile runtime only writes its file at exit. Write it on SIGTERM or SIGINT too,
// so a run can be stopped from outside.
//
// The write happens on a thread of its own that waits for the signal, not in a signal handler:
// a handler runs on whichever thread is interrupted, often a game fiber with a small stack, and
// writing the profile there crashed one run.
#if defined(MKW_PGO_GENERATE)
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <pthread.h>
#include <unistd.h>

extern "C" int __llvm_profile_write_file(void);

// The profile runtime from LLVM's packages is built against glibc with fortified printf calls,
// which musl doesn't have. Plain versions are enough for its messages and file names.
extern "C" int __fprintf_chk(FILE* stream, int, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = std::vfprintf(stream, format, args);
    va_end(args);
    return result;
}

extern "C" int __snprintf_chk(char* buffer, size_t size, int, size_t, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = std::vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

namespace {

sigset_t g_mkwPgoStopSignals;

void* MkwPgoWaitForStop(void*) {
    int signal = 0;
    sigwait(&g_mkwPgoStopSignals, &signal);
    __llvm_profile_write_file();
    _exit(0);
}

// Runs before main, so every thread the game starts later inherits the blocked signals and only
// the waiting thread receives them.
struct MkwPgoStopWaiter {
    MkwPgoStopWaiter() {
        sigemptyset(&g_mkwPgoStopSignals);
        sigaddset(&g_mkwPgoStopSignals, SIGTERM);
        sigaddset(&g_mkwPgoStopSignals, SIGINT);
        pthread_sigmask(SIG_BLOCK, &g_mkwPgoStopSignals, nullptr);
        pthread_t thread;
        if (pthread_create(&thread, nullptr, MkwPgoWaitForStop, nullptr) == 0) {
            pthread_detach(thread);
        }
    }
} g_mkwPgoStopWaiter;

} // namespace
#endif
