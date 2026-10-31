#include "perf_stats.h"

#include "guest_abi/guest_region.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#if defined(HBX_SWITCH2)
#include <aurora/gfx.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

extern std::atomic<uint64_t> aurora_hbx_skipped_draws;
extern std::atomic<uint64_t> aurora_hbx_tex_misses;
extern std::atomic<uint64_t> aurora_hbx_tex_zero;
extern std::atomic<uint64_t> g_hbxMemorySlowCalls;
extern std::atomic<uint64_t> g_hbxTimeBaseReads;
extern std::atomic<uint64_t> aurora_hbx_gpu_wait_us;
extern std::atomic<uint64_t> aurora_hbx_render_cb_us;
extern std::atomic<uint64_t> aurora_hbx_gpu_done_us;
extern std::atomic<uint64_t> aurora_hbx_gpu_done_count;
extern std::atomic<uint64_t> aurora_hbx_drain_us;
extern std::atomic<uint64_t> aurora_hbx_drain_count;
extern "C" uint64_t hbxl_syscall_counts[512] __attribute__((weak));
extern "C" volatile long hbxl_net_inflight_call __attribute__((weak));
extern "C" volatile long hbxl_net_inflight_fd __attribute__((weak));
extern "C" volatile int hbxl_net_resolving __attribute__((weak));
extern "C" void hbxl_logf(const char* format, ...) __attribute__((weak));
#endif

namespace PerfStats {
namespace {

using Clock = std::chrono::steady_clock;

#if defined(HBX_SWITCH2)
constexpr size_t kWindow = 60; // a report every few seconds even at low frame rates
#else
constexpr size_t kWindow = 240;
#endif

thread_local Clock::duration t_wait{};

struct FrameWindow {
    Clock::time_point last{};
    std::array<double, kWindow> busyMs{};
    size_t count = 0;
    Clock::time_point start{};
    Clock::duration waited{};
};

FrameWindow g_window;

#if defined(HBX_SWITCH2)
std::atomic<int64_t> g_lastFrameMs{0};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// Says what the console is doing when frames stop. It writes to the launcher's log as well as the
// file server's, so a stuck connection to the file server can't hide the report.
void FreezeWatchdog() {
    constexpr uint32_t kRunningThreadAddr = 0x800000E4u;
    const uint32_t kSchedulerDisableAddr = MKW_GADDR(80386918);
    constexpr int kMaxReports = 20;
    std::array<uint64_t, 512> previous{};
    int reports = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        int topCall[3] = {};
        uint64_t topDelta[3] = {};
        if (hbxl_syscall_counts != nullptr) {
            for (int i = 0; i < 512; ++i) {
                const uint64_t count = __atomic_load_n(&hbxl_syscall_counts[i], __ATOMIC_RELAXED);
                uint64_t delta = count - previous[i];
                int call = i;
                previous[i] = count;
                for (int slot = 0; slot < 3; ++slot) {
                    if (delta > topDelta[slot]) {
                        std::swap(delta, topDelta[slot]);
                        std::swap(call, topCall[slot]);
                    }
                }
            }
        }
        const int64_t age = NowMs() - g_lastFrameMs.load(std::memory_order_relaxed);
        if (age < 3000) {
            reports = 0;
            continue;
        }
        if (reports >= kMaxReports) {
            continue;
        }
        ++reports;
        const uint32_t thread = Memory::Read32(kRunningThreadAddr);
        const bool validThread = Memory::Contains(thread + 0x2C8u, 0x20u);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "watchdog: no frame for %lld ms; net call %ld fd %ld, lookups %d; guest thread 0x%x "
                      "state %u queue 0x%x, scheduler off %u; syscalls/s %d:%llu %d:%llu %d:%llu",
                      static_cast<long long>(age), &hbxl_net_inflight_call != nullptr ? hbxl_net_inflight_call : 0L,
                      &hbxl_net_inflight_fd != nullptr ? hbxl_net_inflight_fd : 0L,
                      &hbxl_net_resolving != nullptr ? hbxl_net_resolving : 0, thread,
                      validThread ? Memory::Read16(thread + 0x2C8u) : 0u,
                      validThread ? Memory::Read32(thread + 0x2DCu) : 0u, Memory::Read32(kSchedulerDisableAddr),
                      topCall[0], static_cast<unsigned long long>(topDelta[0]), topCall[1],
                      static_cast<unsigned long long>(topDelta[1]), topCall[2],
                      static_cast<unsigned long long>(topDelta[2]));
        if (hbxl_logf != nullptr) {
            hbxl_logf("hbxl: %s", line);
        }
        RT_LOGF("perf", "%s\n", line);
    }
}
#endif

} // namespace

bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("MKW_PERF_STATS");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void AddWait(Clock::duration wait) noexcept { t_wait += wait; }

namespace {
std::array<Clock::duration, static_cast<size_t>(Phase::Count)> g_phases{};
} // namespace

void AddPhase(Phase phase, Clock::duration time) noexcept { g_phases[static_cast<size_t>(phase)] += time; }

void OnFramePresented() noexcept {
    if (!Enabled()) {
        return;
    }
#if defined(HBX_SWITCH2)
    if (g_lastFrameMs.exchange(NowMs(), std::memory_order_relaxed) == 0) {
        std::thread(FreezeWatchdog).detach();
    }
#endif
    const auto now = Clock::now();
    const Clock::duration wait = t_wait;
    t_wait = {};
    if (g_window.last == Clock::time_point{}) {
        g_window.last = now;
        return;
    }
    if (g_window.count == 0) {
        g_window.start = g_window.last;
        g_window.waited = {};
    }
    g_window.waited += wait;
    const auto busy = (now - g_window.last) - wait;
    g_window.last = now;
    g_window.busyMs[g_window.count++] = std::chrono::duration<double, std::milli>(busy).count();
    if (g_window.count < kWindow) {
        return;
    }
    std::array<double, kWindow> sorted = g_window.busyMs;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (const double ms : sorted) {
        sum += ms;
    }
    RT_LOGF("perf", "busy ms/frame over %zu frames: avg %.3f median %.3f p95 %.3f max %.3f\n", kWindow,
            sum / kWindow, sorted[kWindow / 2], sorted[kWindow * 95 / 100], sorted[kWindow - 1]);
#if defined(HBX_SWITCH2)
    const double seconds = std::chrono::duration<double>(now - g_window.start).count();
    const double waitedMs = std::chrono::duration<double, std::milli>(g_window.waited).count();
    const AuroraStats* stats = aurora_get_stats();
    RT_LOGF("perf",
            "%.1f fps, waiting on GX/pacing/audio %.1f ms/frame; last frame %u draws; pipelines queued %u built %u, "
            "skipped draws %llu; textures uploaded %llu, all-zero %llu\n",
            kWindow / seconds, waitedMs / kWindow, stats->drawCallCount, stats->queuedPipelines,
            stats->createdPipelines, static_cast<unsigned long long>(aurora_hbx_skipped_draws.load()),
            static_cast<unsigned long long>(aurora_hbx_tex_misses.load()),
            static_cast<unsigned long long>(aurora_hbx_tex_zero.load()));
    static uint64_t lastSlow = 0;
    const uint64_t slow = g_hbxMemorySlowCalls.load(std::memory_order_relaxed);
    RT_LOGF("perf", "memory slow-path calls/frame %llu\n",
            static_cast<unsigned long long>((slow - lastSlow) / kWindow));
    lastSlow = slow;
    {
        const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count() / kWindow; };
        static uint64_t lastGpuWait = 0, lastRender = 0, lastDone = 0, lastDoneCount = 0;
        const uint64_t gpuWait = aurora_hbx_gpu_wait_us.load(), render = aurora_hbx_render_cb_us.load();
        const uint64_t done = aurora_hbx_gpu_done_us.load(), doneCount = aurora_hbx_gpu_done_count.load();
        const uint64_t frames = doneCount - lastDoneCount;
        RT_LOGF("perf",
                "stages ms/frame: end %.1f events %.1f begin %.1f | gpu-progress wait %.1f, render/present %.1f, "
                "submit-to-GPU-done %.1f (%llu frames)\n",
                ms(g_phases[0]), ms(g_phases[1]), ms(g_phases[2]), (gpuWait - lastGpuWait) / 1000.0 / kWindow,
                (render - lastRender) / 1000.0 / kWindow, frames ? (done - lastDone) / 1000.0 / frames : 0.0,
                static_cast<unsigned long long>(frames));
        static uint64_t lastDrain = 0, lastDrainCount = 0;
        const uint64_t drain = aurora_hbx_drain_us.load(), drainCount = aurora_hbx_drain_count.load();
        RT_LOGF("perf", "game thread waiting on GX FIFO %.1f ms/frame over %.1f waits/frame\n",
                (drain - lastDrain) / 1000.0 / kWindow, static_cast<double>(drainCount - lastDrainCount) / kWindow);
        lastDrain = drain;
        lastDrainCount = drainCount;
        lastGpuWait = gpuWait;
        lastRender = render;
        lastDone = done;
        lastDoneCount = doneCount;
        g_phases = {};
    }
    static uint64_t lastReads = 0;
    const uint64_t reads = g_hbxTimeBaseReads.load(std::memory_order_relaxed);
    const auto clockStart = Clock::now();
    for (int i = 0; i < 1000; ++i) {
        (void)Clock::now();
    }
    const double clockNs = std::chrono::duration<double, std::nano>(Clock::now() - clockStart).count() / 1000.0;
    RT_LOGF("perf", "game time-base reads/frame %llu; one clock read costs %.0f ns\n",
            static_cast<unsigned long long>((reads - lastReads) / kWindow), clockNs);
    lastReads = reads;
    if (hbxl_syscall_counts != nullptr) {
        static uint64_t lastCounts[512];
        uint64_t top[5] = {};
        int topId[5] = {-1, -1, -1, -1, -1};
        uint64_t total = 0;
        for (int i = 0; i < 512; ++i) {
            const uint64_t now = __atomic_load_n(&hbxl_syscall_counts[i], __ATOMIC_RELAXED);
            uint64_t delta = now - lastCounts[i];
            lastCounts[i] = now;
            total += delta;
            int id = i;
            for (int k = 0; k < 5; ++k) {
                if (delta > top[k]) {
                    std::swap(delta, top[k]);
                    std::swap(id, topId[k]);
                }
            }
        }
        RT_LOGF("perf", "syscalls/frame %llu; top %d:%llu %d:%llu %d:%llu %d:%llu %d:%llu\n",
                static_cast<unsigned long long>(total / kWindow), topId[0],
                static_cast<unsigned long long>(top[0] / kWindow), topId[1],
                static_cast<unsigned long long>(top[1] / kWindow), topId[2],
                static_cast<unsigned long long>(top[2] / kWindow), topId[3],
                static_cast<unsigned long long>(top[3] / kWindow), topId[4],
                static_cast<unsigned long long>(top[4] / kWindow));
    }
#endif
    g_window.count = 0;
}

} // namespace PerfStats

#if defined(HBX_SWITCH2)
namespace PerfStats {
namespace {
double NsPer(uint64_t count, std::chrono::steady_clock::time_point start) {
    const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
    return elapsed.count() / static_cast<double>(count);
}

uint64_t ReadStride(const uint8_t* base, size_t mask, uint64_t count) {
    uint64_t sum = 0;
    size_t at = 0;
    for (uint64_t i = 0; i < count; ++i) {
        sum += base[at];
        at = (at + 4099u * 64u) & mask;
    }
    return sum;
}

void CalibrateThisThread(const char* who, const uint8_t* guestRam, size_t size) {
    constexpr uint64_t kAlu = 100000000;
    auto start = std::chrono::steady_clock::now();
    uint64_t value = 1;
    for (uint64_t i = 0; i < kAlu; ++i) {
        value = value * 3 + 1;
        __asm__ volatile("" : "+r"(value));
    }
    const double alu = NsPer(kAlu, start);

    std::vector<uint8_t> local(32u << 20, 1);
    constexpr uint64_t kLoads = 4000000;
    start = std::chrono::steady_clock::now();
    uint64_t sum = ReadStride(local.data(), local.size() - 1, kLoads);
    const double hostMemory = NsPer(kLoads, start);

    size_t guestSize = size_t{1} << 20;
    while (guestSize * 2 <= size && guestSize < (32u << 20)) {
        guestSize *= 2;
    }
    start = std::chrono::steady_clock::now();
    sum += ReadStride(guestRam, guestSize - 1, kLoads);
    const double guestMemory = NsPer(kLoads, start);
    RT_LOGF("perf", "calibration %s: integer op %.2f ns, host memory load %.1f ns, guest memory load %.1f ns (%llu)\n",
            who, alu, hostMemory, guestMemory, static_cast<unsigned long long>(sum & 1));
}
} // namespace

float MeasureCpuMhz() {
    constexpr uint64_t kOps = 20000000;
    const auto start = std::chrono::steady_clock::now();
    uint64_t value = 1;
    for (uint64_t i = 0; i < kOps; ++i) {
        value = value * 3 + 1;
        __asm__ volatile("" : "+r"(value));
    }
    return static_cast<float>(2000.0 / NsPer(kOps, start));
}

void LogCoreCalibration(const uint8_t* guestRam, size_t size) {
    CalibrateThisThread("main thread", guestRam, size);
    for (int i = 0; i < 5; ++i) {
        std::thread([=] { CalibrateThisThread("new thread", guestRam, size); }).join();
    }
}
} // namespace PerfStats
#endif
