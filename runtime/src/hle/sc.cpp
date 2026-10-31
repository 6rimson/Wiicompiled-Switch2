#include "hle_stubs.h"

#include "console_identity.h"
#include "sc_serial_contract.h"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "abi_bridge.h"
#include "memory.h"
#include "runtime_config.h"
#include "aurora_events.h"
#include "runtime_log.h"

namespace {

// Use the SDK's own value tables, including its unknown-region result.
uint32_t LookupProductRegion(uint32_t table, uint32_t stride, uint32_t count,
                             const std::string& value) {
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t entry = table + index * stride;
        if (!Memory::Contains(entry, stride)) {
            break;
        }
        const auto* bytes = static_cast<const uint8_t*>(Memory::GetPointer(entry, stride));
        if (bytes[0] == 0xFF) {
            break;
        }
        if (value.size() < stride - 1 &&
            std::memcmp(bytes + 1, value.c_str(), value.size() + 1) == 0) {
            return bytes[0];
        }
    }
    return 0xFFFFFFFFu;
}

} // namespace

// Defined in runtime/src/hle/storage/nand_async.cpp.
bool NandProcessPendingCallbacks(CpuContext* cpu, int maxToProcess);

// SCCheckStatus is polled in OSInit's busy loop (while(SCCheckStatus()==1) waits on async SYSCONF
// load via NAND IPC). SCInit started that load with SCReloadConfFileAsync: a chain of NAND/IOS
// open -> read -> close -> FinishFromReload. Our async NAND/IOS HLE does the host work at once but
// only queues each guest completion callback, which the alarm pump drains later, after OSInit has
// moved on.
//
// The original SCCheckStatus is also what parses the loaded buffer: the reload sets the SC status
// byte to 1 (busy) and FinishFromReload to 3 (loaded); SCCheckStatus then sees 3 and runs
// ParseConfBuf. Returning 0 without either step left SCGetSimpleAddressData with no items
// ("[SYS] Can't get SimpleAddressData.", cached for the whole boot), so the country and globe
// position used online stayed at their defaults.

namespace {

// SC library state used by the original SCCheckStatus, as PAL addresses (SDA base 0x8038CC00).
constexpr uint32_t kScConfSize = 0x4000;
constexpr uint32_t kScControlBufferOffset = 364;
constexpr uint32_t kScControlSizeOffset = 380;
constexpr uint8_t kScStatusLoaded = 3;

uint32_t ScStatusAddress() { return MKW_GADDR(803869C8); }       // u8: 0 ready, 1 busy, 3 loaded
uint32_t ScDirtyAddress() { return MKW_GADDR(803869DD); }        // u8, cleared once parsed
uint32_t ScHeaderMagicAddress() { return MKW_GADDR(80385C20); }  // "SCv0"
uint32_t ScFooterMagicAddress() { return MKW_GADDR(80385C28); }  // "SCed"
uint32_t ScControlAddress() { return MKW_GADDR(80348260); }
uint32_t ScParsedCopyAddress() { return MKW_GADDR(80348400); }   // the buffer SCFind* reads
uint32_t ParseConfBufAddress() { return MKW_GADDR(801B07D4); }

} // namespace

// 0x801B0220 -> SCCheckStatus()
// Returns: 0 = success, 1 = busy, 2 = error
extern "C" uint32_t SCCheckStatus_HLE()
{
    // 1. Finish the queued SYSCONF open/read/close chain here, as the hardware busy-wait would:
    // each callback queues the next step and the drain keeps taking from the queue until it is
    // empty, so one call runs the whole chain through FinishFromReload. The drain runs each
    // callback on a scratch copy of the current context, so they use the caller's real stack.
    CpuContext* current = TryGetCpuContext();
    NandProcessPendingCallbacks(current, 64);

    // 2. Then do what the original does with a loaded buffer.
    const CpuContext& base = current ? *current : GetPersistentCpuContext();
    try {
        const uint32_t statusAddress = ScStatusAddress();
        const uint8_t status = Memory::Read8(statusAddress);
        if (status != kScStatusLoaded) {
            static bool loggedNotLoaded = false;
            if (!loggedNotLoaded) {
                loggedNotLoaded = true;
                RT_LOGF(RT_TAG_RUNTIME, "SCCheckStatus: SYSCONF not loaded (status %u), using defaults\n",
                        status);
            }
            // Never report busy: OSInit would spin forever with nothing left to finish the load.
            return 0;
        }
        Memory::Write8(statusAddress, 1);
        const uint32_t buffer = Memory::Read32(ScControlAddress() + kScControlBufferOffset);
        const uint32_t size = Memory::Read32(ScControlAddress() + kScControlSizeOffset);

        // ParseConfBuf is guest code; run it on a scratch copy of the context like the callbacks.
        CpuContext parseCpu = base;
        parseCpu.gpr[3] = buffer;
        parseCpu.gpr[4] = size;
        {
            CpuContextScope scope(&parseCpu);
            InvokeIndirectCpu(ParseConfBufAddress(), &parseCpu);
        }
        const int32_t parseResult = static_cast<int32_t>(parseCpu.gpr[3]);

        const uint32_t parsedCopy = ScParsedCopyAddress();
        if (Memory::Contains(buffer, kScConfSize) && Memory::Contains(parsedCopy, kScConfSize)) {
            uint8_t* loaded = Memory::GetPointer(buffer, kScConfSize);
            if (parseResult == 0) {
                // Parsed: publish it to the buffer the SCFind* getters read.
                if (buffer != parsedCopy) {
                    std::memmove(Memory::GetPointer(parsedCopy, kScConfSize), loaded, kScConfSize);
                }
            } else {
                // Unparsable: reset to an empty SYSCONF ("SCv0" ... "SCed", no items).
                std::memset(loaded, 0, kScConfSize);
                std::memcpy(loaded, Memory::GetPointer(ScHeaderMagicAddress(), 4), 4);
                std::memcpy(loaded + kScConfSize - 4, Memory::GetPointer(ScFooterMagicAddress(), 4), 4);
                Memory::Write16(buffer + 6, 8);
            }
        }
        Memory::Write8(ScDirtyAddress(), 0);
        Memory::Write8(statusAddress, 0);
        RT_LOGF(RT_TAG_RUNTIME, "SCCheckStatus: SYSCONF %s (%u bytes)\n",
                parseResult == 0 ? "parsed" : "unparsable, using an empty one", size);
    } catch (const Memory::AccessViolation&) {
        RT_LOGF(RT_TAG_RUNTIME, "SCCheckStatus: SC state not readable, using defaults\n");
    }

    // Still success: a missing or unreadable SYSCONF boots with the defaults, as before.
    return 0;
}

PPC_NATIVE_OVERRIDE(801B0220, SCCheckStatus_HLE, uint32_t, (), ());

// 0x801B1BE4 -> SCGetAspectRatio()
// Returns: 0 = 4:3, 1 = 16:9
extern "C" uint32_t SCGetAspectRatio_HLE()
{
    return RuntimeConfigFile::WidescreenEnabled(true) ? 1u : 0u;
}

PPC_NATIVE_OVERRIDE(801B1BE4, SCGetAspectRatio_HLE, uint32_t, (), ());

// 0x801B1CAC -> SCGetEuRgb60Mode()
// Returns: 0 = PAL50, 1 = PAL60/RGB60
extern "C" uint32_t SCGetEuRgb60Mode_HLE()
{
    // Default to PAL60 so PAL builds do not fall back to the half-rate PAL50
    // sync path on modern displays. Actual texture/cache correctness is handled
    // elsewhere; this only exposes the intended SYSCONF setting.
    return 1;
}

PPC_NATIVE_OVERRIDE(801B1CAC, SCGetEuRgb60Mode_HLE, uint32_t, (), ());

// Expose the selected emulated NAND identity through the SDK SC APIs.

extern "C" uint32_t SCGetProductArea_HLE()
{
    return LookupProductRegion(MKW_GADDR(8029CEB0), 5, 13,
                               RuntimeConsoleIdentity::Current().area);
}

PPC_NATIVE_OVERRIDE(801B23A0, SCGetProductArea_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductCode_HLE()
{
    // Original SC storage for the six-byte CODE value.
    const uint32_t kProductCodeAddress = MKW_GADDR(803869E0);
    const std::string& productCode = RuntimeConsoleIdentity::Current().productCode;
    const size_t size = productCode.size() + 1;
    if (!Memory::Contains(kProductCodeAddress, size)) {
        return 0;
    }
    std::memcpy(Memory::GetPointer(kProductCodeAddress, size),
                productCode.c_str(), size);
    return kProductCodeAddress;
}

PPC_NATIVE_OVERRIDE(801B2424, SCGetProductCode_HLE, uint32_t, (), ());

extern "C" uint32_t SCGetProductSN_HLE(uint32_t serialAddress)
{
    const std::string& serial = RuntimeConsoleIdentity::Current().serial;
    return RuntimeScSerial::Write(serial, serialAddress,
        [](uint32_t address, size_t size) { return Memory::Contains(address, size); },
        [](uint32_t address, uint32_t value) { Memory::Write32(address, value); });
}

PPC_NATIVE_OVERRIDE(801B2460, SCGetProductSN_HLE, uint32_t, (uint32_t serialAddress), (serialAddress));

extern "C" uint32_t SCGetProductGameRegion_HLE()
{
    return LookupProductRegion(MKW_GADDR(8029CEF8), 4, 4,
                               RuntimeConsoleIdentity::Current().gameRegion);
}

PPC_NATIVE_OVERRIDE(801B24C8, SCGetProductGameRegion_HLE, uint32_t, (), ());

// These stubs make the game think all titles are installed; otherwise it checks title ID
// 0x00010004524d4350 ("RMCP", Mario Kart Wii PAL) and reports error code 5.

// 0x801AE4A0 -> OS__IsTitleInstalled(titleIdHi, titleIdLo)
// Returns: 1 = installed, 0 = not installed
extern "C" uint32_t OS__IsTitleInstalled(uint32_t titleIdHi, uint32_t titleIdLo)
{
    RT_LOGF(RT_TAG_HLE, "CINS: OSIsTitleInstalled(0x%08X%08X) -> 1 (stubbed as installed)\n",
            titleIdHi, titleIdLo);
    return 1; // Always report installed
}

PPC_NATIVE_OVERRIDE(801AE4A0, OS__IsTitleInstalled, uint32_t, (uint32_t titleIdHi, uint32_t titleIdLo), (titleIdHi, titleIdLo));

// 0x801AD1D4 -> OS__CheckInstall(requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr): returns 0 with
// outFlagsPtr = 0x3 (bit0 has data, bit1 has update; bit2 would be needs-blocks) i.e. fully installed.
extern "C" uint32_t OS__CheckInstall(uint32_t requiredBlocks, uint32_t titleIdHi, 
                                      uint32_t titleIdLo, uint32_t outFlagsPtr)
{
    RT_LOGF(RT_TAG_HLE, "OS__CheckInstall(blocks=%u, 0x%08X%08X) -> success (stubbed)\n",
            requiredBlocks, titleIdHi, titleIdLo);
    if (outFlagsPtr != 0) {
        Memory::Write32(outFlagsPtr, 0x3); // has data + has update = fully installed
    }
    return 0; // Success
}

PPC_NATIVE_OVERRIDE(801AD1D4, OS__CheckInstall, uint32_t, (uint32_t requiredBlocks, uint32_t titleIdHi, uint32_t titleIdLo, uint32_t outFlagsPtr), (requiredBlocks, titleIdHi, titleIdLo, outFlagsPtr));
