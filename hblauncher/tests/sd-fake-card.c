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
/* The runtime's log formatter: the format alone is enough here. */
static char last_log[256];
void hbxl_logf(const char *format, ...) { snprintf(last_log, sizeof(last_log), "%s", format); }
/* What still reaches the file server's log. */
static char server_log[4096];
static size_t server_used;
void __real_hbxl_vfs_write_console(int fd, const void *data, size_t n) {
    (void)fd;
    if (server_used + n < sizeof(server_log)) {
        memcpy(server_log + server_used, data, n);
        server_used += n;
    }
}
void __wrap_hbxl_vfs_write_console(int, const void *, size_t);
void __wrap_hbxl_vfs_shutdown(void);
int hbxl_vfs_resolve_path(int fd, const char *name, char *out) {
    (void)fd;
    snprintf(out, 1024, "%s%s", *name == '/' ? "" : "/", name);
    return 0;
}
static void on_pc(const char *path, char *out) { snprintf(out, 1024, "%s%s", pc_root, path); }
/* The file server: the same calls on a folder, or -EIO for everything when it isn't running. */
static int offline;
long __real_hbxl_vfs_syscall(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    char p1[1024], p2[1024];
    *handled = 1;
    if (offline)
        return -EIO;
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
/* The console's file service freezes the whole console when a 5th folder is opened while 4 are
 * open (found on hardware): count how many are open at once. */
static int open_folders, most_open_folders;
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
            if (dir && ++open_folders > most_open_folders)
                most_open_folders = open_folders;
            objects[i].mode = mode;
            objects[i].fd = fd;
            snprintf(objects[i].path, sizeof(objects[i].path), "%s", host);
            return i;
        }
    return -1;
}
static int keep_entry(const struct dirent *e) { return strcmp(e->d_name, ".") && strcmp(e->d_name, ".."); }
static long card_calls; /* every request to the card */
/* The CPU's clock session (clock.c): reads and sets one rate, within the stock range. */
enum { CPU_HANDLE = 0x9000, LAUNCH_HZ = 1100800000, MAX_HZ = 1702400000 };
static uint32_t cpu_hz = LAUNCH_HZ;
static long clock_requests;
static uint32_t clock_send(uint32_t *p) {
    clock_requests++;
    if (p[4] != 0x49434653 || p[0] != 4)
        return 1;
    const uint32_t cmd = p[6], hz = p[8];
    uint32_t rc = 0, value = 0;
    if (cmd == 8)
        value = cpu_hz;
    else if (cmd == 7 && hz >= 115200000 && hz <= MAX_HZ)
        cpu_hz = hz;
    else
        rc = 2 | (1 << 9);
    memset(p, 0, 64);
    p[4] = 0x4f434653;
    p[6] = rc;
    p[8] = value;
    return 0;
}
static uint32_t send(void *message, uint64_t size, uint32_t handle) {
    (void)size;
    if (handle == CPU_HANDLE)
        return clock_send(message);
    card_calls++;
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
        case 14: { /* GetFileTimeStampRaw */
            if (!exists) { respond(PATH_NOT_FOUND, 0); return 0; }
            uint32_t *out = respond(0, 0);
            int64_t when = st.st_mtime;
            for (int i = 0; i < 3; i++)
                memcpy(out + 2 * i, &when, 8);
            out[6] = 1;
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
    card_calls++;
    if (handle >= MAXH || !objects[handle].used)
        return 1;
    struct object *o = &objects[handle];
    if (o->fd >= 0)
        close(o->fd);
    for (int i = 0; i < o->count; i++)
        free(o->entries[i]);
    free(o->entries);
    open_folders -= o->dir;
    o->used = 0;
    return 0;
}
static uint32_t cpu_command[1024] __attribute__((aligned(4096)));
static struct sd_context sd = {SD_MAGIC,     ROOT,       send,   close_handle, command,    path_buffer,
                               bounce,       CPU_MAGIC,  CPU_HANDLE, LAUNCH_HZ, MAX_HZ, cpu_command};
int hbxl_cpu_clock_rates(uint32_t *, uint32_t *);
long hbxl_cpu_clock_boost(int);
void hbxl_cpu_clock_restore(void);

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

    /* The game log goes to the card: the runtime's lines and the game's own writes to stdout and
     * stderr. The previous launch's log is kept. */
    host_write(card_root, "/UserData/Logs/console.log", "last launch\n", 12);
    char **environment = environ; /* before libc starts, environ is unset and so is MKW_SD */
    environ = NULL;
    __wrap_hbxl_vfs_write_console(2, "hbxl: before libc\n", 18);
    environ = environment;
    __wrap_hbxl_vfs_write_console(2, "hbxl: runtime line\n", 19);
    CHECK(call(SYS_write, 2, (long)"game stderr\n", 12, 0, 0) == 12);
    struct iovec parts[2] = {{"game ", 5}, {"stdout\n", 7}};
    CHECK(call(SYS_writev, 1, (long)parts, 2, 0, 0) == 12);
    /* Rebuilding the game's file index: a folder bigger than one card read (334 entries), then
     * the type and size of each file, which the listing already gave: no card calls. */
    enum { RACE_FILES = 800 };
    host_write(card_root, "/disc/files/Race/Course/sub/x.szs", "x", 1);
    host_write(card_root, "/disc/files/Race/Unlisted/y.szs", "y", 1);
    for (int i = 0; i < RACE_FILES; i++) {
        char name[1100];
        snprintf(name, sizeof(name), "%s/disc/files/Race/Course/track%03d.szs", card_root, i);
        FILE *file = fopen(name, "wb");
        for (int k = 0; k < i; k++)
            fputc(k, file);
        fclose(file);
    }
    /* The scan keeps every parent folder open while it walks: the card has one open at most. */
    most_open_folders = open_folders;
    const char *chain[] = {"/disc", "/disc/files", "/disc/files/Race", "/disc/files/Race/Course",
                           "/disc/files/Race/Course/sub"};
    long parents[5];
    for (int i = 0; i < 5; i++)
        parents[i] = open_(chain[i], O_RDONLY | O_DIRECTORY);
    long first = call(SYS_getdents64, parents[4], (long)listing, sizeof(listing), 0, 0);
    CHECK(parents[0] >= 2048 && parents[4] >= 2048 && first > 0 && most_open_folders <= 1);
    CHECK(call(SYS_lseek, parents[4], 0, SEEK_SET, 0, 0) == 0 &&
          call(SYS_getdents64, parents[4], (long)listing, sizeof(listing), 0, 0) == first); /* rewinds */
    for (int i = 0; i < 5; i++)
        close_(parents[i]);
    CHECK(open_folders == 0);
    fd = open_("/disc/files/Race/Course", O_RDONLY | O_DIRECTORY);
    long names = 0, got_bytes;
    while ((got_bytes = call(SYS_getdents64, fd, (long)listing, sizeof(listing), 0, 0)) > 0)
        for (long i = 0; i < got_bytes; i += ((struct dirent *)(listing + i))->d_reclen)
            names++;
    CHECK(fd >= 2048 && got_bytes == 0 && names == RACE_FILES + 1);
    close_(fd);
    /* Each step of a scan is in the card's log at once: the last line shows where a scan stopped. */
    static char trace[1 << 20];
    CHECK(host_read(card_root, "/UserData/Logs/console.log", trace, sizeof(trace) - 1) > 0 &&
          strstr(trace, "[sd] opening folder /disc/files/Race/Course\n") &&
          strstr(trace, "[sd] /disc/files/Race/Course: reading entries (668 so far, read 3)\n") &&
          strstr(trace, "[sd] /disc/files/Race/Course: 801 entries in 3 read(s), result 0\n"));
    const long before = card_calls;
    int right = 0;
    for (int i = 0; i < RACE_FILES; i++) {
        char name[128];
        struct stat st;
        snprintf(name, sizeof(name), "/disc/files/Race/Course/track%03d.szs", i);
        right += call(SYS_newfstatat, AT_FDCWD, (long)name, (long)&st, 0, 0) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size == i;
    }
    struct stat folder;
    CHECK(right == RACE_FILES);
    CHECK(call(SYS_newfstatat, AT_FDCWD, (long)"/disc/files/Race/Course/sub", (long)&folder, 0, 0) == 0 &&
          S_ISDIR(folder.st_mode));
    CHECK(card_calls == before);
    /* Not listed: asked from the card, as before. */
    CHECK(call(SYS_newfstatat, AT_FDCWD, (long)"/disc/files/Race/Unlisted/y.szs", (long)&folder, 0, 0) == 0 &&
          folder.st_size == 1 && card_calls > before);
    CHECK(call(SYS_newfstatat, AT_FDCWD, (long)"/disc/files/Race/Course/none.szs", (long)&folder, 0, 0) == -ENOENT);

    __wrap_hbxl_vfs_shutdown(); /* writes out what's left, as at exit */
    char log[256] = {0};
    CHECK(host_read(card_root, "/UserData/Logs/console.log", log, sizeof(log) - 1) > 0);
    CHECK(strstr(log, "hbxl: runtime line") && strstr(log, "game stderr") && strstr(log, "game stdout"));
    CHECK(strstr(log, "hbxl: before libc") && strstr(log, "before libc") < strstr(log, "runtime line"));
    CHECK(host_read(card_root, "/UserData/Logs/console.prev.log", log, sizeof(log) - 1) == 12);
    CHECK(server_used && memmem(server_log, server_used, "runtime line", 12)); /* the server still gets it */

    /* Dates: the game prunes its Logs folder by them. Removing a folder with unlink fails as on
     * Linux, so remove() goes on to rmdir. */
    struct stat when;
    char old_log[300];
    snprintf(old_log, sizeof(old_log), "%s/UserData/Logs/console.prev.log", card_root);
    struct timespec days_ago[2] = {{time(NULL) - 5 * 86400, 0}, {time(NULL) - 5 * 86400, 0}};
    utimensat(AT_FDCWD, old_log, days_ago, 0);
    CHECK(call(SYS_newfstatat, AT_FDCWD, (long)"/UserData/Logs/console.prev.log", (long)&when, 0, 0) == 0 &&
          when.st_mtime == days_ago[1].tv_sec);
    CHECK(call(SYS_newfstatat, AT_FDCWD, (long)"/UserData/Logs/console.log", (long)&when, 0, 0) == 0 &&
          when.st_mtime > time(NULL) - 60);
    CHECK(mkdir_("/UserData/Logs/run") == 0 && unlink_("/UserData/Logs/run", 0) == -EISDIR &&
          unlink_("/UserData/Logs/run", AT_REMOVEDIR) == 0);

    /* Starting without the file server: the settings come from the card's copy of /hbxl-env. */
    static const char card_env[] = "MKW_SD=all\nHBXL_ARGS=--x\n";
    host_write(pc_root, "/hbxl-env", "MKW_SD=0\n", 9);
    host_write(card_root, "/hbxl-env", card_env, sizeof(card_env) - 1);
    char env[64] = {0};
    fd = open_("/hbxl-env", O_RDONLY); /* the server is up: its copy */
    CHECK(fd >= 0 && fd < 2048 && call(SYS_read, fd, (long)env, sizeof(env) - 1, 0, 0) == 9);
    close_(fd);
    offline = 1;
    memset(env, 0, sizeof(env));
    fd = open_("/hbxl-env", O_RDONLY);
    CHECK(fd == 4095 && call(SYS_read, fd, (long)env, 10, 0, 0) == 10 &&
          call(SYS_read, fd, (long)env + 10, sizeof(env) - 11, 0, 0) == (long)sizeof(card_env) - 11);
    CHECK(!strcmp(env, card_env) && call(SYS_read, fd, (long)env, 8, 0, 0) == 0);
    CHECK(close_(fd) == 0 && call(SYS_read, 4095, (long)env, 8, 0, 0) == -EBADF);
    char card_copy[300];
    snprintf(card_copy, sizeof(card_copy), "%s/hbxl-env", card_root);
    unlink(card_copy);
    fd = open_("/hbxl-env", O_RDONLY); /* a full card without settings: the defaults */
    CHECK(fd == 4095 && call(SYS_read, fd, (long)env, 8, 0, 0) == 0 && close_(fd) == 0);
    /* Game folders list from the card alone, and a file the card lacks is missing, not an error. */
    fd = open_("/disc/DATA/sys", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 2048 && call(SYS_getdents64, fd, (long)listing, sizeof(listing), 0, 0) > 0);
    close_(fd);
    CHECK(open_("/disc/DATA/sys/missing.bin", O_RDONLY) == -ENOENT);
    offline = 0;

    /* A copy of the whole root made by hand (no /.fsroot, no MKW_SD): everything from the card. */
    char marker[300];
    snprintf(marker, sizeof(marker), "%s/.fsroot", card_root);
    unlink(marker);
    unsetenv("MKW_SD");
    prepared_reset();
    offline = 1;
    CHECK(open_("/hbxl-env", O_RDONLY) == -EIO); /* no NAND yet: not a full card */
    offline = 0;
    host_write(card_root, "/UserData/hand.txt", "card", 4);
    host_write(pc_root, "/UserData/hand.txt", "pc!!", 4);
    prepared_reset();
    fd = open_("/UserData/hand.txt", O_RDONLY);
    CHECK(fd >= 0 && fd < 2048 && call(SYS_read, fd, (long)got, 16, 0, 0) == 4 && !memcmp(got, "pc!!", 4));
    close_(fd);
    host_write(card_root, "/UserData/NAND/shared2/sys/SYSCONF", "sysconf", 7);
    prepared_reset();
    fd = open_("/UserData/hand.txt", O_RDONLY);
    CHECK(fd >= 2048 && pread_(fd, got, 16, 0) == 4 && !memcmp(got, "card", 4));
    close_(fd);

    /* CPU boost: the maximum, then the launch rate, each read back; at exit the launch rate again,
     * unless the system chose another rate since. No card requests. */
    const long card_before = card_calls;
    uint32_t launch = 0, top = 0;
    CHECK(hbxl_cpu_clock_rates(&launch, &top) && launch == LAUNCH_HZ && top == MAX_HZ);
    CHECK(hbxl_cpu_clock_boost(1) == 0 && cpu_hz == MAX_HZ);
    CHECK(hbxl_cpu_clock_boost(0) == 0 && cpu_hz == LAUNCH_HZ);
    CHECK(hbxl_cpu_clock_boost(1) == 0);
    hbxl_cpu_clock_restore();
    CHECK(cpu_hz == LAUNCH_HZ);
    long requests = clock_requests;
    hbxl_cpu_clock_restore(); /* twice: nothing to do */
    CHECK(clock_requests == requests);
    CHECK(hbxl_cpu_clock_boost(1) == 0);
    cpu_hz = 1523200000; /* the system's own change */
    hbxl_cpu_clock_restore();
    CHECK(cpu_hz == 1523200000 && card_calls == card_before && strstr(last_log, "left as it is"));
    sd.cpu_magic = 0; /* an older launcher */
    CHECK(!hbxl_cpu_clock_rates(&launch, &top) && hbxl_cpu_clock_boost(1) == -ENODEV);
    sd.cpu_magic = CPU_MAGIC;

    CHECK(locked_refusals == 0); /* the backend never makes the card refuse an open file */
    printf("%d of %d checks passed\n", checks - failures, checks);
    char cleanup[300];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", base);
    if (system(cleanup)) return 2;
    return failures ? 1 : 0;
}
