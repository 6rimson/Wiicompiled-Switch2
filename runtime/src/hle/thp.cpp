// Native THP video/audio decode, replacing the guest's own decoder.
//
// THP is essentially MJPEG, and the guest decoder is the single most expensive thing in the game
// outside a race: profiles put the Select Cup screen at 9 FPS with >55% of the frame inside
// __THPInverseDCTNoYPos / __THPInverseDCTY8 / __THPHuffDecodeDCTComp*, and the attract-mode demo
// at ~72% THP decode. Running it as translated PPC costs roughly six ARM64 instructions per guest
// instruction, so a native decoder is worth far more here than anywhere else in the game.
//
// The decoder itself is aurora's (upstream commit 3251f4e2, "Add aurora::thp implementation"),
// which upstream verified bit-exact against Dolphin for both THPVideoDecode and THPAudioDecode.
// This file is only the guest-ABI shim: guest addresses in, host pointers out.

#include "hle_stubs.h"
#include "memory.h"
#include "os/os_internal.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <dolphin/thp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

void OS_HLE_WakeupThreadNoReschedule(CpuContext* ctx, uint32_t waitQueue);

namespace {

// The guest hands us MEM1/MEM2 addresses. Sizes are not known up front (the frame header carries
// them), so map a single byte and rely on the decoder staying inside the buffer the game sized -
// the same contract the translated code had.
void* GuestPtr(uint32_t addr) {
    if (addr == 0) {
        return nullptr;
    }
    try {
        return Memory::GetPointer(addr, 1);
    } catch (const Memory::AccessViolation& e) {
        LogMemoryError(RT_TAG_RUNTIME, "THP guest pointer", e);
        return nullptr;
    }
}

// Bytes one decoded plane occupies: aurora writes 8x4 I8 tiles, a whole tile per partial edge.
uint32_t TiledPlaneBytes(uint32_t width, uint32_t height) {
    return ((width + 7u) / 8u) * ((height + 3u) / 4u) * 32u;
}

#if defined(HBX_SWITCH2)
extern "C" void hbxl_pin_next_thread(int core) __attribute__((weak));

// The console's cores are slow enough that decoding a 672x736 movie frame on the game thread costs
// the menu its 60 FPS, and guest threads all share that one host thread. So the decode runs on its
// own core: the guest's decode thread sleeps until it finishes and the game keeps running. The
// scheduler's alarm pump wakes it (Thp_HLE_ProcessCompletions).
struct DecodeJob {
    const void* file = nullptr;
    void* tileY = nullptr;
    void* tileU = nullptr;
    void* tileV = nullptr;
    void* work = nullptr;
};

std::mutex g_jobMutex;
std::condition_variable g_jobReady;
DecodeJob g_job;
bool g_jobPending = false;
std::atomic<bool> g_jobBusy{false};
std::atomic<bool> g_jobDone{false};
std::atomic<uint32_t> g_jobResult{0};
std::atomic<uint32_t> g_jobQueue{0};

void DecodeWorker() {
    for (;;) {
        DecodeJob job;
        {
            std::unique_lock lock{g_jobMutex};
            g_jobReady.wait(lock, [] { return g_jobPending; });
            g_jobPending = false;
            job = g_job;
        }
        g_jobResult.store(static_cast<uint32_t>(THPVideoDecode(job.file, job.tileY, job.tileU, job.tileV, job.work)),
                          std::memory_order_relaxed);
        g_jobDone.store(true, std::memory_order_release);
    }
}

bool DecodeOnWorker(const void* file, void* tileY, void* tileU, void* tileV, void* work, uint32_t queue,
                    uint32_t& result) {
    CpuContext* cpu = TryGetCpuContext();
    if (cpu == nullptr || queue == 0 || !Memory::Contains(queue, 8) || g_jobBusy.exchange(true)) {
        return false;
    }
    static std::once_flag started;
    std::call_once(started, [] {
        if (hbxl_pin_next_thread != nullptr) {
            hbxl_pin_next_thread(5);
        }
        std::thread(DecodeWorker).detach();
    });
    Memory::Write32(queue, 0);
    Memory::Write32(queue + 4, 0);
    g_jobDone.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock{g_jobMutex};
        g_job = {file, tileY, tileU, tileV, work};
        g_jobPending = true;
    }
    const int32_t irqState = OS__DisableInterrupts_801a65ac();
    g_jobQueue.store(queue, std::memory_order_release);
    g_jobReady.notify_one();
    while (!g_jobDone.load(std::memory_order_acquire) || g_jobQueue.load(std::memory_order_acquire) != 0) {
        cpu->gpr[3] = queue;
        OSSleepThread_HLE_801aa9b8(cpu);
    }
    OS__RestoreInterrupts_801a65d4(irqState);
    result = g_jobResult.load(std::memory_order_relaxed);
    return true;
}
#endif

} // namespace

bool Thp_HLE_ProcessCompletions(CpuContext* cpu) {
#if defined(HBX_SWITCH2)
    if (cpu == nullptr || !g_jobDone.load(std::memory_order_acquire)) {
        return false;
    }
    const uint32_t queue = g_jobQueue.exchange(0, std::memory_order_acq_rel);
    if (queue == 0) {
        return false;
    }
    g_jobBusy.store(false, std::memory_order_release);
    OS_HLE_WakeupThreadNoReschedule(cpu, queue);
    return true;
#else
    (void)cpu;
    return false;
#endif
}

// Defined in gx_objects.cpp: the entry point for host writes into guest RAM that no DC flush covers.
extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);

extern "C" uint32_t THP__VideoDecode_801b3bac(uint32_t fileAddr, uint32_t tileYAddr, uint32_t tileUAddr,
                                              uint32_t tileVAddr, uint32_t workAddr) {
    // Pass nulls straight through rather than short-circuiting: aurora returns kNoInput for a null
    // file and kNoOutput for a null tile, whereas returning 0 here would tell the game the frame
    // decoded fine and leave it presenting stale tile memory.
    // `work` is unused by aurora's decoder, but pass it through so the signature keeps matching.
    const void* file = GuestPtr(fileAddr);
    uint32_t result = 0;
#if defined(HBX_SWITCH2)
    if (!DecodeOnWorker(file, GuestPtr(tileYAddr), GuestPtr(tileUAddr), GuestPtr(tileVAddr), GuestPtr(workAddr),
                        workAddr, result))
#endif
    result = static_cast<uint32_t>(
        THPVideoDecode(file, GuestPtr(tileYAddr), GuestPtr(tileUAddr), GuestPtr(tileVAddr), GuestPtr(workAddr)));

    // The guest decoder wrote its output through the locked cache, whose DMA the runtime reports
    // to the GX texture caches. A native decoder writes behind their back, so they kept serving the
    // first frame's planes wherever no other write happened to share a 64 KiB granule - movies
    // came out with live luma over the opening title card's neutral chroma: greyscale. Report
    // exactly what was written.
    u16 width = 0;
    u16 height = 0;
    if (THPVideoFrameSize(file, &width, &height)) {
        const uint32_t chromaWidth = (width + 1u) / 2u;
        const uint32_t chromaHeight = (height + 1u) / 2u;
        GxNotifyGuestRamDmaWrite(tileYAddr, TiledPlaneBytes(width, height));
        GxNotifyGuestRamDmaWrite(tileUAddr, TiledPlaneBytes(chromaWidth, chromaHeight));
        GxNotifyGuestRamDmaWrite(tileVAddr, TiledPlaneBytes(chromaWidth, chromaHeight));
    }
    return result;
}
PPC_NATIVE_OVERRIDE(801B3BAC, THP__VideoDecode_801b3bac, uint32_t,
                    (uint32_t fileAddr, uint32_t tileYAddr, uint32_t tileUAddr, uint32_t tileVAddr,
                     uint32_t workAddr),
                    (fileAddr, tileYAddr, tileUAddr, tileVAddr, workAddr));

// NOT overriding THPInit (0x801b6ee0). aurora's THPInit() is just `return TRUE`, because its
// decoder needs no setup - but the guest's is 39 real instructions, and the game's *own* THP code
// (THP::AudioDecoder and the player at 0x805508e8 onwards) still runs as translated PPC and may
// depend on whatever state that sets up. Replacing it with a stub left that code running against
// uninitialised state. Only the pure leaf decode function is safe to take over.
