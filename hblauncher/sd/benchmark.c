/* Optional same-file comparison. Run once after musl initializes. Both data
 * paths read the same FST bytes, with independent descriptors and offsets. */
#define _GNU_SOURCE
#include "storage.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static long pc(long n, long a, long b, long c) {
    int handled;
    return __real_hbxl_vfs_syscall(n, a, b, c, 0, 0, 0, &handled);
}

__attribute__((constructor(150))) static void storage_benchmark(void) {
    const char *requested = getenv("MKW_SD_BENCH");
    const char *disabled = getenv("MKW_SD");
    if (!requested || strcmp(requested, "1") || (disabled && !strcmp(disabled, "0")))
        return;
    const char *path = "/disc/DATA/sys/fst.bin";
    const size_t chunk = SD_CHUNK;
    unsigned char *buffer = malloc(chunk);
    if (!buffer)
        return;
    int sd = open(path, O_RDONLY);
    int server = pc(SYS_openat, AT_FDCWD, (long)path, O_RDONLY);
    if (sd >= 2048 && sd < 4096 && server >= 0) {
        /* Eight passes amortize timing on the small, always-present FST. */
        uint64_t ticks[2] = {0}, bytes[2] = {0};
        uint32_t hash[2] = {2166136261u, 2166136261u};
        int ok = 1;
        for (int pass = 0; pass < 8 && ok; pass++)
            for (int i = 0; i < 2; i++) {
                long off = 0, got;
                do {
                    uint64_t start = hbxl_svc_tick();
                    if (i) {
                        int handled;
                        got = __real_hbxl_vfs_syscall(SYS_pread64, server, (long)buffer, chunk, off, 0, 0,
                                                      &handled);
                    } else
                        got = pread(sd, buffer, chunk, off);
                    ticks[i] += hbxl_svc_tick() - start;
                    if (got < 0) {
                        ok = 0;
                        break;
                    }
                    for (long j = 0; j < got; j++)
                        hash[i] = (hash[i] ^ buffer[j]) * 16777619u;
                    bytes[i] += got;
                    off += got;
                } while (got > 0);
            }
        if (ok && bytes[0] && bytes[0] == bytes[1] && hash[0] == hash[1] && ticks[0] && ticks[1]) {
            double hz = hbxl_ctx->tick_hz ? hbxl_ctx->tick_hz : 19200000.0;
            fprintf(stderr,
                    "[sd] FST benchmark: %llu matching bytes per side in 8 passes, SD %.2f MiB/s, PC %.2f "
                    "MiB/s\n",
                    (unsigned long long)bytes[0], bytes[0] / 1048576.0 * hz / ticks[0],
                    bytes[1] / 1048576.0 * hz / ticks[1]);
        } else
            fprintf(stderr, "[sd] FST benchmark: read failed or contents differ\n");
    } else
        fprintf(stderr, "[sd] FST benchmark: both copies are required\n");
    if (sd >= 0)
        close(sd);
    if (server >= 0)
        pc(SYS_close, server, 0, 0);
    free(buffer);
}
