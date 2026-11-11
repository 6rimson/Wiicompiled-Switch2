/* Linux test for the microSD backend (hblauncher/sd/) without a console.
 *
 * A fake card answers the backend's nn::fs messages from a folder, with the console's rules: a
 * file open for writing can't be opened again, deleted or renamed; writes past the end need the
 * append mode; rename never replaces. A second folder stands in for the PC file server. The test
 * calls the backend's system-call entry point directly, as the runtime does.
 *
 *   tests/sd-fake-card.sh   (builds it with the musl toolchain and runs it in the Docker image)
 */
#define _GNU_SOURCE
#include "../sd/storage.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

long __wrap_hbxl_vfs_syscall(long, long, long, long, long, long, long, int *);

static char card_root[256], pc_root[256];

/* ---- the runtime pieces the backend links against ---------------------------------------- */
static struct hbx_ctx context;
const struct hbx_ctx *hbxl_ctx = &context;
uint64_t hbxl_svc_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 19200000u + (uint64_t)ts.tv_nsec * 192u / 10000u;
}
int hbxl_threads_live(void) { return 0; }
void __real_hbxl_vfs_shutdown(void) {}
void hbxl_vfs_write_console(int fd, const void *data, size_t n) { (void)!write(fd, data, n); }
int hbxl_vfs_resolve_path(int fd, const char *name, char *out) {
    (void)fd;
    snprintf(out, 1024, "%s%s", *name == '/' ? "" : "/", name);
    return 0;
}
static void on_pc(const char *path, char *out) { snprintf(out, 1024, "%s%s", pc_root, path); }
/* The file server: the same calls on a folder. */
long __real_hbxl_vfs_syscall(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    char p1[1024], p2[1024];
    *handled = 1;
    long rc;
    switch (n) {
    case SYS_openat: case SYS_newfstatat: case SYS_faccessat: case SYS_mkdirat: case SYS_unlinkat:
    case SYS_fchmodat: case SYS_utimensat:
        if (b && *(const char *)b == '/') {
            on_pc((const char *)b, p1);
            b = (long)p1;
            a = AT_FDCWD;
        }
        break;
    case SYS_renameat: case SYS_renameat2:
        on_pc((const char *)b, p1);
        on_pc((const char *)d, p2);
        b = (long)p1;
        d = (long)p2;
        a = c = AT_FDCWD;
        break;
    case SYS_statx:
        return -ENOSYS; /* the backend asks with newfstatat */
    }
    rc = syscall(n, a, b, c, d, e, f);
    return rc < 0 ? -errno : rc;
}

/* ---- the fake card ------------------------------------------------------------------------ */
enum { ROOT = 1, MAXH = 256 };
struct object {
    int used, dir, mode, fd, position;
    char path[1024];
    struct dirent **entries;
    int count;
};
static struct object objects[MAXH];
static uint32_t command[1024];
static char path_buffer[SD_PATH];
static uint8_t bounce[SD_CHUNK];
static int locked_refusals;

static uint32_t result(int module_description) { return 2 | (module_description << 9); }
#define PATH_NOT_FOUND result(1)
#define ALREADY_EXISTS result(2)
#define TARGET_LOCKED result(7)
#define NOT_EMPTY result(8)
#define NEEDS_APPEND result(6302)

static int to_host(uint64_t addr, char *out) {
    const char *p = (const char *)(uintptr_t)addr;
    if (strncmp(p, "/wiicompiled", 12))
        return -1;
    snprintf(out, 1024, "%s%s", card_root, p + 12);
    return 0;
}
static int open_with_write(const char *host, int any) {
    for (int i = 2; i < MAXH; i++)
        if (objects[i].used && !objects[i].dir && !strcmp(objects[i].path, host) &&
            (any || (objects[i].mode & SD_WRITE)))
            return 1;
    return 0;
}
/* A reply: an optional moved handle, then the data, aligned to 16 bytes (word 4 either way). */
static uint32_t *respond(uint32_t rc, int moved_handle) {
    memset(command, 0, 64);
    const unsigned at = 4;
    if (moved_handle) {
        command[1] = 1u << 31;
        command[2] = 1u << 5;
        command[3] = moved_handle;
    }
    command[at] = 0x4f434653;
    command[at + 2] = rc;
    return command + at + 4;
}
static int new_object(int dir, const char *host, int mode, int fd) {
    for (int i = 2; i < MAXH; i++)
        if (!objects[i].used) {
            memset(&objects[i], 0, sizeof(objects[i]));
            objects[i].used = 1;
            objects[i].dir = dir;
            objects[i].mode = mode;
            objects[i].fd = fd;
            snprintf(objects[i].path, sizeof(objects[i].path), "%s", host);
            return i;
        }
    return -1;
}
static int keep_entry(const struct dirent *e) { return strcmp(e->d_name, ".") && strcmp(e->d_name, ".."); }
static uint32_t send(void *message, uint64_t size, uint32_t handle) {
    (void)size;
    uint32_t *p = message;
    unsigned nx = (p[0] >> 16) & 15, na = (p[0] >> 20) & 15, nb = (p[0] >> 24) & 15;
    uint64_t x[2] = {0}, buffer = 0, buffer_size = 0;
    unsigned at = 2;
    for (unsigned i = 0; i < nx; i++, at += 2)
        x[i] = p[at + 1] | ((uint64_t)((p[at] >> 12) & 15) << 32) | ((uint64_t)((p[at] >> 6) & 7) << 36);
    for (unsigned i = 0; i < na + nb; i++, at += 3) {
        buffer = p[at + 1] | ((uint64_t)(p[at + 2] >> 28) << 32) | ((uint64_t)((p[at + 2] >> 2) & 7) << 36);
        buffer_size = p[at] | ((uint64_t)((p[at + 2] >> 24) & 15) << 32);
    }
    at = (at + 3) & ~3u;
    if (p[at] != 0x49434653)
        return 1;
    unsigned cmd = p[at + 2];
    uint32_t *raw = p + at + 4;
    char host[1024], other[1024];
    struct stat st;
    if (handle == ROOT) {
        if (nx < 1 || to_host(x[0], host) || (nx == 2 && to_host(x[1], other)))
            return 1;
        int exists = stat(host, &st) == 0;
        switch (cmd) {
        case 0: { /* CreateFile */
            int64_t length;
            memcpy(&length, raw + 2, 8);
            if (exists) { respond(ALREADY_EXISTS, 0); return 0; }
            int fd = open(host, O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd < 0) { respond(PATH_NOT_FOUND, 0); return 0; }
            ftruncate(fd, length);
            close(fd);
            respond(0, 0);
            return 0;
        }
        case 1: /* DeleteFile */
            if (!exists || S_ISDIR(st.st_mode)) { respond(PATH_NOT_FOUND, 0); return 0; }
            if (open_with_write(host, 1)) { locked_refusals++; respond(TARGET_LOCKED, 0); return 0; }
            unlink(host);
            respond(0, 0);
            return 0;
        case 2: /* CreateDirectory */
            if (exists) { respond(ALREADY_EXISTS, 0); return 0; }
            respond(mkdir(host, 0755) ? PATH_NOT_FOUND : 0, 0);
            return 0;
        case 3: /* DeleteDirectory */
            if (!exists || !S_ISDIR(st.st_mode)) { respond(PATH_NOT_FOUND, 0); return 0; }
            respond(rmdir(host) ? NOT_EMPTY : 0, 0);
            return 0;
        case 5: case 6: { /* RenameFile, RenameDirectory: never replaces */
            struct stat target;
            if (!exists || !!S_ISDIR(st.st_mode) != (cmd == 6)) { respond(PATH_NOT_FOUND, 0); return 0; }
            if (stat(other, &target) == 0) { respond(ALREADY_EXISTS, 0); return 0; }
            if (open_with_write(host, 1)) { locked_refusals++; respond(TARGET_LOCKED, 0); return 0; }
            respond(rename(host, other) ? PATH_NOT_FOUND : 0, 0);
            return 0;
        }
        case 7: /* GetEntryType */
            if (!exists) { respond(PATH_NOT_FOUND, 0); return 0; }
            *respond(0, 0) = S_ISDIR(st.st_mode) ? 0 : 1;
            return 0;
        case 8: { /* OpenFile */
            int mode = raw[0];
            if (!exists || S_ISDIR(st.st_mode)) { respond(PATH_NOT_FOUND, 0); return 0; }
            if (open_with_write(host, !!(mode & SD_WRITE))) { locked_refusals++; respond(TARGET_LOCKED, 0); return 0; }
            int fd = open(host, mode & SD_WRITE ? O_RDWR : O_RDONLY);
            int h = fd < 0 ? -1 : new_object(0, host, mode, fd);
            if (h < 0) return 1;
            respond(0, h);
            return 0;
        }
        case 9: { /* OpenDirectory */
            if (!exists || !S_ISDIR(st.st_mode)) { respond(PATH_NOT_FOUND, 0); return 0; }
            int h = new_object(1, host, 0, -1);
            objects[h].count = scandir(host, &objects[h].entries, keep_entry, alphasort);
            respond(0, h);
            return 0;
        }
        }
        return 1;
    }
    if (handle >= MAXH || !objects[handle].used)
        return 1;
    struct object *o = &objects[handle];
    int64_t offset, length;
    memcpy(&offset, raw + 2, 8);
    memcpy(&length, raw + 4, 8);
    if (o->dir) {
        if (cmd != 0)
            return 1;
        struct sd_entry *out = (void *)(uintptr_t)buffer;
        int64_t count = 0;
        while (o->position < o->count && (count + 1) * (int64_t)sizeof(*out) <= (int64_t)buffer_size) {
            const struct dirent *e = o->entries[o->position++];
            char child[2048];
            snprintf(child, sizeof(child), "%s/%s", o->path, e->d_name);
            stat(child, &st);
            memset(&out[count], 0, sizeof(out[count]));
            snprintf(out[count].name, sizeof(out[count].name), "%s", e->d_name);
            out[count].type = S_ISDIR(st.st_mode) ? 0 : 1;
            out[count].size = S_ISDIR(st.st_mode) ? 0 : st.st_size;
            count++;
        }
        memcpy(respond(0, 0), &count, 8);
        return 0;
    }
    fstat(o->fd, &st);
    switch (cmd) {
    case 0: { /* Read */
        int64_t got = pread(o->fd, (void *)(uintptr_t)buffer, length, offset);
        memcpy(respond(0, 0), &got, 8);
        return 0;
    }
    case 1: /* Write */
        if (!(o->mode & SD_WRITE)) return 1;
        if (offset + length > st.st_size && !(o->mode & SD_APPEND)) { respond(NEEDS_APPEND, 0); return 0; }
        if (offset > st.st_size) { respond(result(6000), 0); return 0; } /* no gaps on the console */
        pwrite(o->fd, (void *)(uintptr_t)buffer, length, offset);
        respond(0, 0);
        return 0;
    case 2: respond(0, 0); return 0; /* Flush */
    case 3: { /* SetSize */
        int64_t size_arg;
        memcpy(&size_arg, raw, 8);
        ftruncate(o->fd, size_arg);
        respond(0, 0);
        return 0;
    }
    case 4: memcpy(respond(0, 0), &st.st_size, 8); return 0; /* GetSize */
    }
    return 1;
}
static uint32_t close_handle(uint32_t handle) {
    if (handle >= MAXH || !objects[handle].used)
        return 1;
    struct object *o = &objects[handle];
    if (o->fd >= 0)
        close(o->fd);
    for (int i = 0; i < o->count; i++)
        free(o->entries[i]);
    free(o->entries);
    o->used = 0;
    return 0;
}
static struct sd_context sd = {SD_MAGIC, ROOT, send, close_handle, command, path_buffer, bounce};

/* ---- helpers ------------------------------------------------------------------------------ */
static long call(long n, long a, long b, long c, long d, long e) {
    int handled = 0;
    return __wrap_hbxl_vfs_syscall(n, a, b, c, d, e, 0, &handled);
}
static long open_(const char *path, int flags) { return call(SYS_openat, AT_FDCWD, (long)path, flags, 0644, 0); }
static long close_(long fd) { return call(SYS_close, fd, 0, 0, 0, 0); }
static long pwrite_(long fd, const void *b, size_t n, long off) { return call(SYS_pwrite64, fd, (long)b, n, off, 0); }
static long pread_(long fd, void *b, size_t n, long off) { return call(SYS_pread64, fd, (long)b, n, off, 0); }
static long write_(long fd, const void *b, size_t n) { return call(SYS_write, fd, (long)b, n, 0, 0); }
static long rename_(const char *a, const char *b) { return call(SYS_renameat, AT_FDCWD, (long)a, AT_FDCWD, (long)b, 0); }
static long unlink_(const char *p, int flags) { return call(SYS_unlinkat, AT_FDCWD, (long)p, flags, 0, 0); }
static long mkdir_(const char *p) { return call(SYS_mkdirat, AT_FDCWD, (long)p, 0755, 0, 0); }
static long size_(long fd) {
    struct stat st;
    long rc = call(SYS_fstat, fd, (long)&st, 0, 0, 0);
    return rc < 0 ? rc : st.st_size;
}
static void host_write(const char *root, const char *path, const void *data, size_t n) {
    char full[1024], dir[1100];
    snprintf(full, sizeof(full), "%s%s", root, path);
    snprintf(dir, sizeof(dir), "mkdir -p \"$(dirname '%s')\"", full);
    if (system(dir)) exit(2);
    FILE *f = fopen(full, "wb");
    fwrite(data, 1, n, f);
    fclose(f);
}
static long host_read(const char *root, const char *path, void *data, size_t n) {
    char full[1024];
    snprintf(full, sizeof(full), "%s%s", root, path);
    FILE *f = fopen(full, "rb");
    if (!f) return -1;
    long got = fread(data, 1, n, f);
    fclose(f);
    return got;
}
/* The backend checks for a prepared card once per launch; the test starts a "new launch". */
extern int sd_card_prepared;
static void prepared_reset(void) { sd_card_prepared = -1; }
static int failures, checks;
#define CHECK(expr)                                                                                  \
    do {                                                                                             \
        checks++;                                                                                    \
        if (!(expr)) {                                                                               \
            failures++;                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);                                  \
        }                                                                                            \
    } while (0)

static void pattern(uint8_t *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(i * 31 + seed + (i >> 9));
}

int main(void) {
    char base[] = "/tmp/sd-fake-XXXXXX";
    if (!mkdtemp(base)) return 2;
    snprintf(card_root, sizeof(card_root), "%s/card", base);
    snprintf(pc_root, sizeof(pc_root), "%s/pc", base);
    context.tick_hz = 19200000;
    context.args[5] = (uintptr_t)&sd;

    enum { SAVE = 600000, BIG = 700000 };
    static uint8_t save[SAVE], big[BIG], got[BIG];
    pattern(save, SAVE, 7);
    pattern(big, BIG, 3);
    host_write(pc_root, "/UserData/Config.toml", "cfg=pc\n", 7);
    host_write(pc_root, "/RetroRewind/riivolution/save/RetroWFC/RMCP/rksys.dat", save, SAVE);
    host_write(pc_root, "/disc/DATA/sys/fst.bin", "fst-pc", 6);
    host_write(card_root, "/disc/DATA/sys/fst.bin", "fst-card", 8);
    const char *rksys = "/RetroRewind/riivolution/save/RetroWFC/RMCP/rksys.dat";

    /* Default mode: assets from the card, everything else on the PC. */
    unsetenv("MKW_SD");
    long fd = open_("/UserData/Config.toml", O_RDWR);
    CHECK(fd >= 0 && fd < 2048);
    CHECK(close_(fd) == 0);
    fd = open_("/disc/DATA/sys/fst.bin", O_RDONLY);
    CHECK(fd >= 2048 && pread_(fd, got, 16, 0) == 8 && !memcmp(got, "fst-card", 8));
    close_(fd);

    /* MKW_SD=all on an unprepared card stays in the assets mode: writes still go to the PC. */
    setenv("MKW_SD", "all", 1);
    fd = open_("/UserData/Config.toml", O_RDWR);
    CHECK(fd >= 0 && fd < 2048);
    close_(fd);

    /* A prepared card (what tools/sd-card.sh push makes) is the only copy of writable files. */
    host_write(card_root, "/.fsroot", "prepared\n", 9);
    host_write(card_root, "/UserData/Config.toml", "cfg=card\n", 9);
    host_write(card_root, rksys, save, SAVE);
    host_write(pc_root, "/UserData/pc-only.txt", "stale", 5);
    host_write(pc_root, "/UserData/Cache/db-wal", "stale journal", 13);
    prepared_reset();
    fd = open_("/UserData/Config.toml", O_RDONLY);
    CHECK(fd >= 2048 && pread_(fd, got, 16, 0) == 9 && !memcmp(got, "cfg=card", 8));
    close_(fd);
    CHECK(open_("/UserData/pc-only.txt", O_RDONLY) == -ENOENT); /* never from the PC */
    fd = open_(rksys, O_RDWR);
    CHECK(fd >= 2048);
    CHECK(pread_(fd, got, 20, SAVE - 10) == 10 && !memcmp(got, save + SAVE - 10, 10));
    long reader = open_(rksys, O_RDONLY); /* a second open shares the handle */
    CHECK(reader >= 2048);
    CHECK(pwrite_(fd, "HELLO", 5, 100) == 5);
    CHECK(pread_(reader, got, 5, 100) == 5 && !memcmp(got, "HELLO", 5));
    CHECK(unlink_(rksys, 0) == -EBUSY);
    CHECK(close_(reader) == 0 && close_(fd) == 0);
    CHECK(host_read(card_root, rksys, got, BIG) == SAVE && !memcmp(got + 100, "HELLO", 5));
    CHECK(host_read(pc_root, rksys, got, BIG) == SAVE && !memcmp(got, save, SAVE));
    /* A journal the game deletes stays deleted, even though the PC has an old one. */
    fd = open_("/UserData/Cache/db-wal", O_RDWR | O_CREAT);
    CHECK(fd >= 2048 && size_(fd) == 0); /* new and empty, not the PC's stale copy */
    CHECK(write_(fd, "fresh", 5) == 5 && close_(fd) == 0);
    CHECK(unlink_("/UserData/Cache/db-wal", 0) == 0);
    CHECK(open_("/UserData/Cache/db-wal", O_RDONLY) == -ENOENT);
    struct stat gone;
    CHECK(call(SYS_newfstatat, AT_FDCWD, (long)"/UserData/Cache/db-wal", (long)&gone, 0, 0) == -ENOENT);

    /* New files in new folders, more than one bounce buffer at a time, appends and sizes. */
    fd = open_("/UserData/Cache/new/x.bin", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 2048);
    CHECK(write_(fd, big, BIG) == BIG && size_(fd) == BIG);
    CHECK(call(SYS_fsync, fd, 0, 0, 0, 0) == 0);
    CHECK(close_(fd) == 0);
    CHECK(host_read(card_root, "/UserData/Cache/new/x.bin", got, BIG) == BIG && !memcmp(got, big, BIG));
    fd = open_("/UserData/Cache/new/x.bin", O_WRONLY | O_APPEND);
    CHECK(write_(fd, "ab", 2) == 2 && write_(fd, "cd", 2) == 2 && size_(fd) == BIG + 4);
    CHECK(call(SYS_ftruncate, fd, 10, 0, 0, 0) == 0 && size_(fd) == 10);
    CHECK(pwrite_(fd, "Z", 1, 20) == 1 && size_(fd) == 21); /* a gap */
    close_(fd);
    CHECK(host_read(card_root, "/UserData/Cache/new/x.bin", got, 64) == 21 && got[15] == 0 && got[20] == 'Z');
    fd = open_("/UserData/Cache/new/x.bin", O_WRONLY | O_CREAT | O_EXCL);
    CHECK(fd == -EEXIST);
    fd = open_("/UserData/Cache/new/x.bin", O_RDWR | O_TRUNC);
    CHECK(fd >= 2048 && size_(fd) == 0);
    close_(fd);

    /* Saving through a temporary file replaces the old one. */
    fd = open_("/UserData/save.tmp", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(write_(fd, "new", 3) == 3 && close_(fd) == 0);
    fd = open_("/UserData/save.bin", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(write_(fd, "old!", 4) == 4 && close_(fd) == 0);
    CHECK(rename_("/UserData/save.tmp", "/UserData/save.bin") == 0);
    CHECK(host_read(card_root, "/UserData/save.bin", got, 16) == 3 && !memcmp(got, "new", 3));
    CHECK(host_read(card_root, "/UserData/save.tmp", got, 16) < 0);
    CHECK(rename_("/UserData/Config.toml", "/UserData/Config.old") == 0);
    CHECK(host_read(card_root, "/UserData/Config.old", got, 16) == 9);
    CHECK(host_read(pc_root, "/UserData/Config.toml", got, 16) == 7); /* the PC never changes */
    CHECK(rename_("/UserData/pc-only.txt", "/UserData/x.txt") == -ENOENT);

    /* Folders and deletes. */
    CHECK(mkdir_("/UserData") == -EEXIST);
    CHECK(mkdir_("/UserData/Logs") == 0 && mkdir_("/UserData/Logs") == -EEXIST);
    fd = open_("/UserData/Logs/a.log", O_WRONLY | O_CREAT);
    close_(fd);
    CHECK(unlink_("/UserData/Logs", AT_REMOVEDIR) == -ENOTEMPTY);
    CHECK(unlink_("/UserData/Logs/a.log", 0) == 0 && unlink_("/UserData/Logs", AT_REMOVEDIR) == 0);
    CHECK(unlink_("/UserData/pc-only.txt", 0) == -ENOENT); /* only on the PC: not the card's */

    /* The disc stays read-only. */
    CHECK(open_("/disc/DATA/sys/fst.bin", O_RDWR) == -EROFS);
    fd = open_("/disc/DATA/sys/fst.bin", O_RDONLY);
    CHECK(fd >= 2048 && pread_(fd, got, 16, 0) == 8);
    close_(fd);

    /* Metadata, locks and directory listings that merge both sides. */
    struct stat st;
    CHECK(call(SYS_newfstatat, AT_FDCWD, (long)rksys, (long)&st, 0, 0) == 0 && st.st_size == SAVE &&
          (st.st_mode & 0200));
    fd = open_(rksys, O_RDWR);
    struct flock lock = {.l_type = F_WRLCK};
    CHECK(call(SYS_fcntl, fd, F_SETLK, (long)&lock, 0, 0) == 0);
    CHECK(call(SYS_fcntl, fd, F_GETLK, (long)&lock, 0, 0) == 0 && lock.l_type == F_UNLCK);
    close_(fd);
    fd = open_("/UserData", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 2048);
    char listing[4096];
    long bytes = call(SYS_getdents64, fd, (long)listing, sizeof(listing), 0, 0);
    int old = 0, saved = 0, pc_only = 0;
    for (long i = 0; i < bytes;) {
        struct dirent *e = (void *)(listing + i);
        old += !strcmp(e->d_name, "Config.old");
        saved += !strcmp(e->d_name, "save.bin");
        pc_only += !strcmp(e->d_name, "pc-only.txt");
        i += e->d_reclen;
    }
    CHECK(old == 1 && saved == 1 && pc_only == 0); /* only the card's entries */
    close_(fd);

    CHECK(locked_refusals == 0); /* the backend never makes the card refuse an open file */
    printf("%d of %d checks passed\n", checks - failures, checks);
    char cleanup[300];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", base);
    if (system(cleanup)) return 2;
    return failures ? 1 : 0;
}
