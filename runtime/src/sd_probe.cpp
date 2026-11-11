// Experimental (branch experimental/sd-card): MKW_SD_PROBE=1 in hbxl-env asks the console, once
// at launch, whether SmileBASIC's process may open the SD card, and logs what it answers. Nothing
// here runs without it.
//
// The calls go to nn::fs in SmileBASIC's own program, as the CPU boost calls nn::oe. Auto-abort is
// turned off around them, so a refusal comes back as an error code instead of ending the process.
#if defined(HBX_SWITCH2)
#include "runtime_log.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void* hbxl_sdk_proc(uint64_t offset) __attribute__((weak));
extern "C" void* hbxl_sdk_memalign(size_t align, size_t size) __attribute__((weak));
extern "C" void hbxl_call_on_stack(void (*fn)(void*), void* arg, void* stackTop) __attribute__((weak));

namespace {

// Offsets in SmileBASIC's program, from HBLauncher's sdk/sdk-symbols.json.
constexpr uint64_t kFsSetEnabledAutoAbort = 0xe2500;  // void (bool)
constexpr uint64_t kFsMountSdCard = 0xdd430;          // Result (const char* name)
constexpr uint64_t kFsOpenDirectory = 0x9e4b0;        // Result (DirectoryHandle*, const char*, int mode)
constexpr uint64_t kFsReadDirectory = 0x9c170;        // Result (int64_t*, DirectoryEntry*, DirectoryHandle, int64_t)
constexpr uint64_t kFsCloseDirectory = 0x9c0e0;       // void (DirectoryHandle)
constexpr uint64_t kFsUnmount = 0x9b520;              // void (const char* name)
constexpr int kOpenDirectoryModeAll = 3;              // directories and files
constexpr size_t kStackBytes = 256 * 1024;
constexpr int kMaxEntries = 32;
constexpr uint32_t kNotRun = 0xFFFFFFFFu;

struct DirectoryEntry {
    char name[769];
    char reserved0[3];
    int8_t type;  // 0 directory, 1 file
    char reserved1[3];
    int64_t size;
};
static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry is 784 bytes");

struct DirectoryHandle {
    void* handle;
};

// Everything the SDK reads or writes is in SDK-heap memory: system services can't use the
// runtime's Vulkan-backed memory.
struct Probe {
    char mountName[16];
    char rootPath[16];
    uint32_t mountResult;
    uint32_t openResult;
    uint32_t readResult;
    int64_t count;
    DirectoryHandle directory;
    DirectoryEntry entries[kMaxEntries];
};

template <typename Function>
Function Sdk(uint64_t offset) {
    return reinterpret_cast<Function>(hbxl_sdk_proc(offset));
}

void RunProbe(void* arg) {
    auto* probe = static_cast<Probe*>(arg);
    Sdk<void (*)(bool)>(kFsSetEnabledAutoAbort)(false);
    probe->mountResult = Sdk<uint32_t (*)(const char*)>(kFsMountSdCard)(probe->mountName);
    if (probe->mountResult == 0) {
        probe->openResult = Sdk<uint32_t (*)(DirectoryHandle*, const char*, int)>(kFsOpenDirectory)(
            &probe->directory, probe->rootPath, kOpenDirectoryModeAll);
        if (probe->openResult == 0) {
            probe->readResult =
                Sdk<uint32_t (*)(int64_t*, DirectoryEntry*, DirectoryHandle, int64_t)>(kFsReadDirectory)(
                    &probe->count, probe->entries, probe->directory, kMaxEntries);
            Sdk<void (*)(DirectoryHandle)>(kFsCloseDirectory)(probe->directory);
        }
        Sdk<void (*)(const char*)>(kFsUnmount)(probe->mountName);
    }
    Sdk<void (*)(bool)>(kFsSetEnabledAutoAbort)(true);
}

// nn::Result: module in the low 9 bits, description in the next 13; shown as 2MMM-DDDD.
void LogResult(const char* what, uint32_t result) {
    if (result == kNotRun) {
        RT_LOGF("sd", "%s: not tried\n", what);
    } else if (result == 0) {
        RT_LOGF("sd", "%s: ok\n", what);
    } else {
        RT_LOGF("sd", "%s: failed 0x%08x (%04u-%04u)\n", what, result, 2000 + (result & 0x1FFu),
                (result >> 9) & 0x1FFFu);
    }
}

} // namespace

void ConsoleSdProbeOnce() {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    const char* enabled = std::getenv("MKW_SD_PROBE");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }
    if (hbxl_sdk_proc == nullptr || hbxl_sdk_memalign == nullptr || hbxl_call_on_stack == nullptr) {
        RT_LOGF("sd", "probe skipped: no SDK calls in this build\n");
        return;
    }
    auto* probe = static_cast<Probe*>(hbxl_sdk_memalign(16, sizeof(Probe)));
    void* stack = hbxl_sdk_memalign(4096, kStackBytes);
    if (probe == nullptr || stack == nullptr) {
        RT_LOGF("sd", "probe skipped: no SDK-heap memory\n");
        return;
    }
    std::memset(probe, 0, sizeof(Probe));
    std::strcpy(probe->mountName, "mkwsd");
    std::strcpy(probe->rootPath, "mkwsd:/");
    probe->mountResult = probe->openResult = probe->readResult = kNotRun;

    RT_LOGF("sd", "asking to mount the SD card\n");
    std::fflush(stderr);
    hbxl_call_on_stack(RunProbe, probe, static_cast<char*>(stack) + kStackBytes);

    LogResult("MountSdCard", probe->mountResult);
    LogResult("OpenDirectory sd:/", probe->openResult);
    LogResult("ReadDirectory", probe->readResult);
    if (probe->readResult == 0) {
        const int64_t shown = probe->count < kMaxEntries ? probe->count : kMaxEntries;
        RT_LOGF("sd", "SD card root: %lld entries\n", static_cast<long long>(probe->count));
        for (int64_t index = 0; index < shown; ++index) {
            const DirectoryEntry& entry = probe->entries[index];
            RT_LOGF("sd", "  %s %s (%lld bytes)\n", entry.type == 0 ? "dir " : "file", entry.name,
                    static_cast<long long>(entry.size));
        }
    }
    std::fflush(stderr);
}
#endif
