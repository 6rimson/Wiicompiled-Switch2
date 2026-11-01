#include "hle/rr_frame_gate.h"

#include "abi_bridge.h"
#include "ppc_runtime.h"

#include <atomic>
#include <cstdlib>

#if defined(HBX_SWITCH2)
// Retro Rewind's main loop calls this before each frame. It compares the real clock with the frames
// produced and returns 0 in r25 to run logic without drawing, which is how a slow console keeps full
// game speed. The loop's own result is what skips the draw, so answering "draw" turns skipping off.

extern "C" void rr_kamek_81806904(CpuContext* ctx) __attribute__((weak));  // only in Retro Rewind builds
bool VI_HLE_InRaceSection();

namespace {
std::atomic<bool> g_frameSkip{std::getenv("MKW_NO_FRAME_SKIP") == nullptr};
constexpr uint32_t kCr0Mask = 0xF0000000u;
constexpr uint32_t kCr0Greater = 0x40000000u;
} // namespace

bool RetroRewindFrameSkip() noexcept { return g_frameSkip.load(std::memory_order_relaxed); }
void SetRetroRewindFrameSkip(bool on) noexcept { g_frameSkip.store(on, std::memory_order_relaxed); }

extern "C" void RetroRewindFrameGate_81806904(CpuContext* ctx) {
    // Only races: outside them the helper also drives the game's loading and screen changes.
    if ((g_frameSkip.load(std::memory_order_relaxed) || !VI_HLE_InRaceSection()) &&
        rr_kamek_81806904 != nullptr) {
        rr_kamek_81806904(ctx);
        return;
    }
    ctx->gpr[25] = 1;
    ctx->cr = (ctx->cr & ~kCr0Mask) | kCr0Greater;
}
REGISTER_NATIVE_WRAPPER_AS(0x81806904, RetroRewindFrameGate_81806904, "RetroRewindFrameGate");
#endif
