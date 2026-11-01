#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "guest_abi/guest_region.h"
#include "input_bindings.h"
#include "motion_trick.h"
#include "perf_stats.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

namespace {

std::atomic<bool> g_rumbleEnabled{true};

bool NativeButtonHeld(SDL_Gamepad* gamepad, uint32_t nativeButton) {
    if (gamepad == nullptr || nativeButton == PAD_NATIVE_BUTTON_INVALID ||
        nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

// A digital button bound to L or R has no analog travel of its own. On real
// hardware the click only engages at full depression, so report a full pull.
void FillTriggersHeldByButtons(PADStatus* statuses) {
    if (InputBindings::InputBlocked()) {
        return;
    }
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            continue;
        }
        SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
        if (gamepad == nullptr) {
            continue;
        }
        const auto scan = [&](PADButtonMapping* mappings, u32 count) {
            if (mappings == nullptr) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const PADButtonMapping& mapping = mappings[i];
                if (mapping.padButton != PAD_TRIGGER_L && mapping.padButton != PAD_TRIGGER_R) {
                    continue;
                }
                if (!NativeButtonHeld(gamepad, mapping.nativeButton)) {
                    continue;
                }
                if (mapping.padButton == PAD_TRIGGER_L) {
                    statuses[port].triggerLeft = 255;
                } else {
                    statuses[port].triggerRight = 255;
                }
            }
        };
        u32 count = 0;
        scan(PADGetButtonMappings(port, &count), count);
        count = 0;
        scan(PADGetAltButtonMappings(port, &count), count);
    }
}

void WritePadStatus(uint32_t base, const PADStatus& status) {
    const auto guestStatus = PadStatusContract::Encode({
        status.button,
        status.stickX,
        status.stickY,
        status.substickX,
        status.substickY,
        status.triggerL,
        status.triggerR,
        status.analogA,
        status.analogB,
        status.err,
    });
    uint8_t* dst = Memory::GetPointer(base, guestStatus.size());
    std::memcpy(dst, guestStatus.data(), guestStatus.size());
}

} // namespace

extern "C" bool PAD_HLE_RumbleEnabled()
{
    return g_rumbleEnabled.load(std::memory_order_relaxed);
}

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

extern "C" uint32_t PAD__Init_HLE()
{
    return PADInit() ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF2F0, PAD__Init_HLE, uint32_t, (), ());

#if defined(HBX_SWITCH2)
// Retro Rewind's race HUD asks for pause page 26, but a one-player Battle section only builds the
// base game's page 25, so pausing called through an empty page. Where 26 is missing, point the HUD
// at a sibling class's getter that returns 25.
static void ApplyRacePauseFallback()
{
    constexpr uint32_t kVtableGetPausePage = 100;
    constexpr uint32_t kSectionPages = 8;
    constexpr uint32_t kFirstHudPage = 12;
    constexpr uint32_t kLastHudPage = 21;
    static uint32_t s_patchedSlot = 0;
    static uint32_t s_patchedOriginal = 0;

    const uint32_t manager = Memory::Read32(MKW_GADDR(809C1E38));
    const uint32_t section = Memory::Contains(manager, 4) ? Memory::Read32(manager) : 0;
    if (!Memory::Contains(section, kSectionPages + 256 * 4)) {
        return;
    }
    const bool fallback = Memory::Read32(section + kSectionPages + 26 * 4) == 0 &&
                          Memory::Read32(section + kSectionPages + 25 * 4) != 0;
    if (!fallback) {
        if (s_patchedSlot != 0) {
            Memory::Write32(s_patchedSlot, s_patchedOriginal);
            s_patchedSlot = 0;
        }
        return;
    }
    for (uint32_t id = kFirstHudPage; id <= kLastHudPage; ++id) {
        const uint32_t page = Memory::Read32(section + kSectionPages + id * 4);
        if (!Memory::Contains(page, 4)) {
            continue;
        }
        const uint32_t table = Memory::Read32(page);
        if (!Memory::Contains(table + kVtableGetPausePage, 4)) {
            continue;
        }
        const uint32_t slot = table + kVtableGetPausePage;
        const uint32_t getter = Memory::Read32(slot);
        if (getter == MKW_GADDR(80633948) || getter == MKW_GADDR(80633888)) {
            s_patchedSlot = slot;
            s_patchedOriginal = getter;
            Memory::Write32(slot, MKW_GADDR(80633A98));
            RT_LOGF("hle", "pause: section %u has no page 26, using page 25\n", Memory::Read32(section));
            return;
        }
    }
}
#endif

// PADRead: gathers every GameCube pad source for the frame and writes the statuses to guest memory.
extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    PADStatus statuses[PAD_CHANMAX]{};
    uint32_t rumbleMask = 0;
    {
        PerfStats::WaitScope timed;
        rumbleMask = PADRead(statuses);
    }

#if defined(HBX_SWITCH2)
    // MKW_AUTOPAD=1 taps A on port 1, so a headless test run can reach a race without a person.
    static const bool s_autoPad = std::getenv("MKW_AUTOPAD") != nullptr;
    if (s_autoPad) {
        static uint32_t s_polls = 0;
        statuses[0].err = PAD_ERR_NONE;
        // MKW_AUTOPAD_SCRIPT lists tap times, "12.5:A,14:D" (seconds from the first poll; A, B or
        // U/D/L/R on the d-pad). With a script set, only those taps happen.
        static const char* s_script = std::getenv("MKW_AUTOPAD_SCRIPT");
        if (s_script == nullptr) {
            if ((s_polls++ % 40) < 3) {
                statuses[0].button |= PAD_BUTTON_A;
            }
        } else {
            static const auto s_start = std::chrono::steady_clock::now();
            const double now =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
            for (const char* entry = s_script; *entry != '\0';) {
                char* end = nullptr;
                const double when = std::strtod(entry, &end);
                if (end == entry || *end != ':' || end[1] == '\0') {
                    break;
                }
                if (now >= when && now < when + 0.12) {
                    switch (end[1]) {
                    case 'A': statuses[0].button |= PAD_BUTTON_A; break;
                    case 'B': statuses[0].button |= PAD_BUTTON_B; break;
                    case 'S': statuses[0].button |= PAD_BUTTON_START; break;
                    case 'U': statuses[0].button |= PAD_BUTTON_UP; break;
                    case 'D': statuses[0].button |= PAD_BUTTON_DOWN; break;
                    case 'L': statuses[0].button |= PAD_BUTTON_LEFT; break;
                    case 'R': statuses[0].button |= PAD_BUTTON_RIGHT; break;
                    default: break;
                    }
                }
                entry = end[2] == ',' ? end + 3 : end + 2;
            }
        }
    }
#endif

    FillTriggersHeldByButtons(statuses);
    InputBindings::Apply(statuses);
    MotionTrick::Apply(statuses);

    try {
#if defined(HBX_SWITCH2)
        ApplyRacePauseFallback();
#endif
        for (uint32_t i = 0; i < PAD_CHANMAX; ++i) {
            WritePadStatus(statusPtr + static_cast<uint32_t>(i * PadStatusContract::kGuestStatusSize),
                           statuses[i]);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }

    return rumbleMask;
}
PPC_NATIVE_OVERRIDE(801AF44C, PAD__Read_HLE, uint32_t, (uint32_t statusPtr), (statusPtr));

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF0DC, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(801AF1E4, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" void PAD__ControlMotor_HLE(int32_t chan, uint32_t command)
{
    if (command == PAD_MOTOR_RUMBLE && !g_rumbleEnabled.load(std::memory_order_relaxed)) {
        command = PAD_MOTOR_STOP;
    }
    PADControlMotor(chan, command);
}
PPC_NATIVE_OVERRIDE_VOID(801AF908, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
