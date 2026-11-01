#pragma once

#include <chrono>

// MKW_PERF_STATS=1: logs the game thread's busy time per presented frame, i.e. wall time minus the
// time it spends blocked on VI pacing, the GX worker or the audio mixer.
#include <cstddef>
#include <cstdint>

namespace PerfStats {

bool Enabled() noexcept;
#if defined(HBX_SWITCH2)
// Times integer work, a small buffer and guest RAM on this thread and on five new ones, which land
// on cores 1-5. Compares each core's speed and shows what guest memory costs.
void LogCoreCalibration(const uint8_t* guestRam, size_t size);
// Rough CPU clock from a two-cycle integer loop; takes about 40 ms.
float MeasureCpuMhz();
#endif
enum class WaitKind { Pacing, Audio, Gx, Other, Count };
void AddWait(std::chrono::steady_clock::duration wait, WaitKind kind) noexcept;
// Time the guest scheduler has nothing to run, reported per frame so a slow frame can be told from
// one that only waited.
void AddIdle(std::chrono::steady_clock::duration idle) noexcept;
void OnFramePresented() noexcept;

// Time the game thread spends in one presentation step, reported separately from guest code.
enum class Phase { EndFrame, Events, BeginFrame, Count };
void AddPhase(Phase phase, std::chrono::steady_clock::duration time) noexcept;

class PhaseScope {
public:
    explicit PhaseScope(Phase phase) noexcept
        : m_phase(phase), m_start(Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~PhaseScope() {
        if (m_start != std::chrono::steady_clock::time_point{}) {
            AddPhase(m_phase, std::chrono::steady_clock::now() - m_start);
        }
    }
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;

private:
    Phase m_phase;
    std::chrono::steady_clock::time_point m_start;
};

class IdleScope {
public:
    IdleScope() noexcept : m_start(Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~IdleScope() {
        if (m_start != std::chrono::steady_clock::time_point{}) {
            AddIdle(std::chrono::steady_clock::now() - m_start);
        }
    }
    IdleScope(const IdleScope&) = delete;
    IdleScope& operator=(const IdleScope&) = delete;

private:
    std::chrono::steady_clock::time_point m_start;
};

class WaitScope {
public:
    explicit WaitScope(WaitKind kind = WaitKind::Other) noexcept
        : m_kind(kind), m_start(Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~WaitScope() {
        if (m_start != std::chrono::steady_clock::time_point{}) {
            AddWait(std::chrono::steady_clock::now() - m_start, m_kind);
        }
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    WaitKind m_kind;
    std::chrono::steady_clock::time_point m_start;
};

} // namespace PerfStats
