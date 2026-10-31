#pragma once

// Stall attribution: times host work done on behalf of the guest and logs anything slow, stamped
// with t=<steady-clock ms> so it lines up with [OSReport] lines and the per-frame [frame] report.
// It matters most where file and network I/O is slow (SD cards, a TCP file server).
//
// Timers tagged with an Io category also add their time to per-frame totals, which the VI present
// path reports for any frame that took long (see VI_HLE_PresentFrame): a screen-change freeze made
// of many small reads/writes shows up there even though no single one crosses kThresholdMs.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace RuntimeStall {

inline long long NowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline long long NowUs() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline long long ThresholdUs() noexcept {
    // MKW_STALL_ALL=1 logs every timed operation; test runs use it to see which files load.
    static const long long thresholdUs = std::getenv("MKW_STALL_ALL") != nullptr ? 0 : 50 * 1000;
    return thresholdUs;
}

enum class Io { None = -1, Dvd = 0, Nand = 1, Count = 2 };

struct IoTotals {
    std::atomic<long long> us[static_cast<int>(Io::Count)]{};
    std::atomic<long long> ops[static_cast<int>(Io::Count)]{};
    std::atomic<long long> bytes[static_cast<int>(Io::Count)]{};
};

inline IoTotals& FrameIo() noexcept {
    static IoTotals totals;
    return totals;
}

class Timer {
public:
    Timer(const char* what, std::string detail = {}, Io io = Io::None, long long bytes = 0) noexcept
        : m_what(what), m_detail(std::move(detail)), m_io(io), m_bytes(bytes), m_startUs(NowUs()) {}
    ~Timer() {
        const long long elapsedUs = NowUs() - m_startUs;
        if (m_io != Io::None) {
            const int index = static_cast<int>(m_io);
            IoTotals& totals = FrameIo();
            totals.us[index].fetch_add(elapsedUs, std::memory_order_relaxed);
            totals.ops[index].fetch_add(1, std::memory_order_relaxed);
            totals.bytes[index].fetch_add(m_bytes, std::memory_order_relaxed);
        }
        if (elapsedUs >= ThresholdUs()) {
            std::fprintf(stderr, "[stall] t=%lld %s took %lld ms%s%s\n", m_startUs / 1000, m_what,
                         elapsedUs / 1000, m_detail.empty() ? "" : " ", m_detail.c_str());
        }
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    const char* m_what;
    std::string m_detail;
    Io m_io;
    long long m_bytes;
    long long m_startUs;
};

}  // namespace RuntimeStall
