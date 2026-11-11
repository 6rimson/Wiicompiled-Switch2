/* Optional hardware regression, linked only with -DMKW_TEST_SD_IO=ON.
 * Uses the same FST on the card and PC. The write-mode test never writes. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static const char fst[] = "/disc/DATA/sys/fst.bin";
struct worker {
    int fd;
    off_t offset;
    unsigned char reference[64];
    int ok;
};

static void *check_worker(void *opaque) {
    struct worker *w = opaque;
    unsigned char bytes[64];
    for (int i = 0; i < 8; ++i) {
        if (pread(w->fd, bytes, sizeof(bytes), w->offset) != sizeof(bytes) ||
            memcmp(bytes, w->reference, sizeof(bytes)))
            return NULL;
    }
    w->ok = 1;
    return NULL;
}

#define CHECK(expr)                                                                                          \
    do {                                                                                                     \
        if (!(expr)) {                                                                                       \
            fprintf(stderr, "[sd-check] FAIL line %d: %s (errno %d)\n", __LINE__, #expr, errno);             \
            goto cleanup;                                                                                    \
        }                                                                                                    \
    } while (0)

__attribute__((constructor(155))) static void check_storage(void) {
    int a = -1, b = -1, pc = -1, alias = -1, writable = -1;
    DIR *dir = NULL;
    struct stat st;
    struct statx sx;
    struct worker w[2] = {0};
    pthread_t threads[2];
    int started = 0;
    unsigned char ref[128], split[128];
    struct iovec vec[] = {{split, 17}, {split + 17, 111}};
    const char *was = getenv("MKW_SD");
    char *saved = was ? strdup(was) : NULL;
    CHECK(!was || saved);
    CHECK(setenv("MKW_SD", "0", 1) == 0);
    pc = open(fst, O_RDONLY);
    CHECK(pc >= 3 && pc < 2048 && pread(pc, ref, sizeof(ref), 5) == sizeof(ref));
    CHECK(setenv("MKW_SD", "1", 1) == 0);
    a = open(fst, O_RDONLY);
    CHECK(a >= 2048 && a < 4096);
    CHECK(stat(fst, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 133);
    CHECK(access(fst, R_OK) == 0);
    CHECK(syscall(SYS_statx, AT_FDCWD, fst, 0, STATX_BASIC_STATS, &sx) == 0 &&
          sx.stx_size == (uint64_t)st.st_size);
    CHECK(lseek(a, -5, SEEK_END) == st.st_size - 5 && read(a, split, 128) == 5 && read(a, split, 1) == 0);
    CHECK(lseek(a, 5, SEEK_SET) == 5 && readv(a, vec, 2) == 128 && !memcmp(split, ref, 128));
    CHECK(preadv(a, vec, 2, 5) == 128 && !memcmp(split, ref, 128) && lseek(a, 0, SEEK_CUR) == 133);
    alias = dup(a);
    CHECK(alias >= 2048 && lseek(alias, 9, SEEK_SET) == 9 && lseek(a, 0, SEEK_CUR) == 9);
    dir = opendir("/disc/DATA/sys");
    CHECK(dir && dirfd(dir) >= 2048);
    unsigned seen = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        CHECK(strncmp(entry->d_name, "._", 2));
        if (!strcmp(entry->d_name, "fst.bin"))
            ++seen;
    }
    CHECK(!errno && seen == 1);
    CHECK(fstatat(dirfd(dir), "fst.bin", &st, 0) == 0);
    b = openat(dirfd(dir), "fst.bin", O_RDONLY);
    CHECK(b >= 2048 && b != a && fstat(b, &st) == 0 && st.st_size == (off_t)sx.stx_size);
    writable = openat(dirfd(dir), "fst.bin", O_WRONLY);
    CHECK(writable >= 3 && writable < 2048);
    for (int i = 0; i < 2; i++) {
        w[i].fd = i ? b : a;
        w[i].offset = 5 + 64 * i;
        memcpy(w[i].reference, ref + 64 * i, 64);
        CHECK(!pthread_create(&threads[i], NULL, check_worker, &w[i]));
        started++;
    }
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    started = 0;
    CHECK(w[0].ok && w[1].ok);
    fprintf(stderr, "[sd-check] PASS: disabled route, write route, metadata, merged directory, relative "
                    "open, shared offset, vectored I/O, concurrent reads\n");
cleanup:
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    if (writable >= 0)
        close(writable);
    if (b >= 0)
        close(b);
    if (dir)
        closedir(dir);
    if (alias >= 0)
        close(alias);
    if (a >= 0)
        close(a);
    if (pc >= 0)
        close(pc);
    if (saved) {
        setenv("MKW_SD", saved, 1);
        free(saved);
    } else
        unsetenv("MKW_SD");
}
