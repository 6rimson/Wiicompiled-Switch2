// Recording builds only (MKW_PGO=generate): the game has no exit a headless training run can
// reach, and the profile runtime only writes its file at exit. Write it on SIGTERM or SIGINT too,
// so a run can be stopped from outside. Installed before SDL starts, which then keeps it.
#if defined(MKW_PGO_GENERATE)
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
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

void MkwPgoWriteProfileAndExit(int) {
    __llvm_profile_write_file();
    _exit(0);
}

struct MkwPgoSignalInstaller {
    MkwPgoSignalInstaller() {
        std::signal(SIGTERM, MkwPgoWriteProfileAndExit);
        std::signal(SIGINT, MkwPgoWriteProfileAndExit);
    }
} g_mkwPgoSignalInstaller;

} // namespace
#endif
