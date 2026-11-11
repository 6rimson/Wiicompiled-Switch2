#pragma once

// Experimental (branch experimental/sd-card): whole-file reads from the console's microSD card.
//
// The SD backend (outside this tree) answers paths under /wiicompiled/ with one read-only file at
// a time: open(path, O_RDONLY), sequential read of up to 4,096 bytes each, and close. The card
// mirrors the file-server root, so fsroot's /disc/DATA/sys/fst.bin is /wiicompiled/disc/DATA/sys/
// fst.bin on the card. Without the backend these paths go to the file server, which has no
// /wiicompiled folder, and every read here falls back.
#if defined(HBX_SWITCH2)
#include "runtime_log.h"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SdCardFile {

inline constexpr const char* kRoot = "/wiicompiled";
inline constexpr size_t kMaxRead = 4096;

// FNV-1a, logged with each file so a card copy can be checked against its source.
inline uint32_t Fingerprint(const std::vector<uint8_t>& bytes) {
    uint32_t hash = 2166136261u;
    for (const uint8_t value : bytes) {
        hash = (hash ^ value) * 16777619u;
    }
    return hash;
}

// Reads all of kRoot + fsrootPath. The result counts only after a complete read: no more than
// maxBytes, then end of file, then a clean close. A missing file returns nothing quietly; a
// partial one is logged and returns nothing, so the caller uses its usual source.
inline std::optional<std::vector<uint8_t>> ReadAll(const std::string& fsrootPath, size_t maxBytes) {
    const std::string path = std::string(kRoot) + fsrootPath;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> chunk(kMaxRead);  // on the heap: callers may run on small fiber stacks
    bool atEnd = false;
    bool failed = false;
    while (true) {
        // Past maxBytes, ask for one byte: it has to be the end of the file.
        const size_t want = bytes.size() < maxBytes ? std::min(kMaxRead, maxBytes - bytes.size()) : 1;
        const ssize_t got = ::read(fd, chunk.data(), want);
        if (got == 0) {
            atEnd = true;
            break;
        }
        if (got < 0 || bytes.size() + static_cast<size_t>(got) > maxBytes) {
            failed = true;
            break;
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + got);
    }
    const bool closed = ::close(fd) == 0;
    if (failed || !atEnd || !closed) {
        RT_LOGF("sd", "%s incomplete (%zu bytes, end %d, closed %d); using the usual copy\n", path.c_str(),
                bytes.size(), atEnd ? 1 : 0, closed ? 1 : 0);
        return std::nullopt;
    }
    RT_LOGF("sd", "read %s from the SD card: %zu bytes, fingerprint %08x\n", path.c_str(), bytes.size(),
            Fingerprint(bytes));
    return bytes;
}

} // namespace SdCardFile
#endif
