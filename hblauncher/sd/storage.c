#include "storage.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum { SFCI = 0x49434653, SFCO = 0x4f434653 };
uint32_t sd_last_result; /* the card's last result code, for log lines */

static long reply(uint32_t handle, int moved, uint32_t **data) {
    struct sd_context *ctx = card();
    uint32_t *p = ctx->command;
    if (ctx->send(p, 4096, handle))
        return -EIO;
    unsigned at = 2;
    uint32_t child = 0;
    if (p[1] >> 31) {
        if (p[2] != (1u << 5))
            return -EPROTO;
        child = p[3];
        at = 4;
    }
    at += 2 * ((p[0] >> 16) & 15) + 3 * (((p[0] >> 20) & 15) + ((p[0] >> 24) & 15) + (p[0] >> 28));
    at = (at + 3) & ~3u;
    if (at + 6 > 1024 || p[at] != SFCO)
        return -EPROTO;
    uint32_t rc = p[at + 2] & 0x3fffff;
    sd_last_result = rc;
    if (rc) {
        switch (rc) {
        case 0x202: return -ENOENT;     /* FS 2002-0001 PathNotFound */
        case 0x402: return -EEXIST;     /* 2002-0002 PathAlreadyExists */
        case 0xe02: return -EBUSY;      /* 2002-0007 TargetLocked (the file is open) */
        case 0x1002: return -ENOTEMPTY; /* 2002-0008 DirectoryNotEmpty */
        case 0x3c02: return -ENOSPC;    /* 2002-0030 UsableSpaceNotEnough */
        }
        static int reported;
        if (reported++ < 16)
            fprintf(stderr, "[sd] file system result %04u-%04u\n", 2000 + (rc & 0x1ff), rc >> 9);
        return -EIO;
    }
    if (!!child != moved)
        return -EPROTO;
    if (data)
        *data = p + at + 4;
    return child;
}

/* Writes "/wiicompiled" + path into an SDK-heap buffer and its X (pointer) descriptor at d. */
static long put_path(uint32_t *d, unsigned index, char *buffer, const char *path) {
    static const char prefix[] = "/wiicompiled";
    size_t n = strlen(path);
    if (sizeof(prefix) + n > SD_PATH)
        return -ENAMETOOLONG;
    memcpy(buffer, prefix, sizeof(prefix) - 1);
    memcpy(buffer + sizeof(prefix) - 1, path, n + 1);
    uintptr_t addr = (uintptr_t)buffer;
    d[0] = index | (SD_PATH << 16) | (((addr >> 36) & 7) << 6) | (((addr >> 32) & 15) << 12);
    d[1] = addr;
    return 0;
}

/* IFileSystem commands taking one or two paths and up to four words of arguments. The second
 * path goes in the bounce buffer, which no path command uses otherwise. */
static long paths_call(unsigned command, const char *first, const char *second, const uint32_t *args,
                       unsigned words, int moved, uint32_t **data) {
    struct sd_context *ctx = card();
    uint32_t *p = ctx->command;
    memset(p, 0, 256);
    unsigned count = second ? 2 : 1;
    long rc = put_path(p + 2, 0, ctx->path, first);
    if (!rc && second)
        rc = put_path(p + 4, 1, (char *)ctx->bounce, second);
    if (rc)
        return rc;
    unsigned at = (2 + 2 * count + 3) & ~3u;
    p[0] = 4 | (count << 16);
    p[1] = 8 + words;
    p[at] = SFCI;
    p[at + 2] = command;
    if (words)
        memcpy(p + at + 4, args, 4 * words);
    return reply(ctx->sd, moved, data);
}
static long path_call(const char *path, unsigned command, uint32_t mode, uint32_t **data) {
    return paths_call(command, path, NULL, &mode, command == 7 ? 0 : 1, command != 7, data);
}

long sd_type(const char *path) {
    uint32_t *data;
    long rc = path_call(path, 7, 0, &data);
    if (rc < 0)
        return rc;
    return *data <= 1 ? (long)*data : -EPROTO;
}
long sd_open(const char *path, int directory) {
    return path_call(path, directory ? 9 : 8, directory ? 3 : SD_READ, NULL);
}
long sd_open_mode(const char *path, uint32_t mode) { return path_call(path, 8, mode, NULL); }
long sd_create(const char *path, int64_t size) {
    uint32_t args[4] = {0, 0, (uint32_t)size, (uint32_t)((uint64_t)size >> 32)}; /* option, pad, size */
    return paths_call(0, path, NULL, args, 4, 0, NULL);
}
long sd_delete(const char *path) { return paths_call(1, path, NULL, NULL, 0, 0, NULL); }
long sd_mkdir(const char *path) { return paths_call(2, path, NULL, NULL, 0, 0, NULL); }
long sd_rmdir(const char *path) { return paths_call(3, path, NULL, NULL, 0, 0, NULL); }
long sd_rename(const char *from, const char *to, int directory) {
    return paths_call(directory ? 6 : 5, from, to, NULL, 0, 0, NULL);
}
/* IFileSystem::GetFileTimeStampRaw (14): created, modified and accessed in seconds, then whether
 * the card has them. */
long sd_modified(const char *path, int64_t *seconds) {
    uint32_t *data;
    long rc = paths_call(14, path, NULL, NULL, 0, 0, &data);
    if (rc < 0)
        return rc;
    if (!(data[6] & 0xff))
        return -ENODATA;
    memcpy(seconds, data + 2, sizeof(*seconds));
    return 0;
}

static long count_reply(uint32_t handle) {
    uint32_t *data;
    long rc = reply(handle, 0, &data);
    if (rc < 0)
        return rc;
    uint64_t count;
    memcpy(&count, data, sizeof(count));
    return count > LONG_MAX ? -EOVERFLOW : (long)count;
}
long sd_size(uint32_t file) {
    uint32_t *p = card()->command;
    memset(p, 0, 256);
    p[0] = 4;
    p[1] = 8;
    p[4] = SFCI;
    p[6] = 4;
    return count_reply(file);
}

static uint32_t *receive_command(unsigned bytes, unsigned mode) {
    struct sd_context *ctx = card();
    uintptr_t addr = (uintptr_t)ctx->bounce;
    uint32_t *p = ctx->command;
    memset(p, 0, 256);
    p[0] = 4 | (1u << 24);
    p[2] = bytes;
    p[3] = addr;
    p[4] = mode | (((addr >> 36) & 7) << 2) | (((addr >> 32) & 15) << 28);
    p[8] = SFCI;
    return p;
}
long sd_read(uint32_t file, int64_t offset, size_t length) {
    if (length > SD_CHUNK || offset < 0)
        return -EINVAL;
    uint32_t *p = receive_command(length, 1);
    p[1] = 14;
    memcpy(p + 14, &offset, 8);
    p[16] = length;
    long rc = count_reply(file);
    return rc > (long)length ? -EPROTO : rc;
}
long sd_entries(uint32_t dir) {
    unsigned capacity = SD_CHUNK / sizeof(struct sd_entry);
    uint32_t *p = receive_command(capacity * sizeof(struct sd_entry), 0);
    p[1] = 8;
    long rc = count_reply(dir);
    return rc > capacity && rc >= 0 ? -EPROTO : rc;
}
long sd_close(uint32_t handle) { return card()->close_handle(handle) ? -EIO : 0; }

/* IFile commands without buffers: Flush (2), SetSize (3). */
static long file_call(uint32_t file, unsigned command, const uint32_t *args, unsigned words) {
    uint32_t *p = card()->command;
    memset(p, 0, 256);
    p[0] = 4;
    p[1] = 8 + words;
    p[4] = SFCI;
    p[6] = command;
    if (words)
        memcpy(p + 8, args, 4 * words);
    return reply(file, 0, NULL);
}
long sd_flush(uint32_t file) { return file_call(file, 2, NULL, 0); }
long sd_set_size(uint32_t file, int64_t size) {
    if (size < 0)
        return -EINVAL;
    uint32_t args[2] = {(uint32_t)size, (uint32_t)((uint64_t)size >> 32)};
    return file_call(file, 3, args, 2);
}
/* IFile::Write (1): option, pad, offset, size, with the data in an A (send) descriptor. */
long sd_write(uint32_t file, int64_t offset, size_t length) {
    if (length > SD_CHUNK || offset < 0)
        return -EINVAL;
    struct sd_context *ctx = card();
    uintptr_t addr = (uintptr_t)ctx->bounce;
    uint32_t *p = ctx->command;
    memset(p, 0, 256);
    p[0] = 4 | (1u << 20);
    p[1] = 14;
    p[2] = length;
    p[3] = addr;
    p[4] = 1 | (((addr >> 36) & 7) << 2) | (((addr >> 32) & 15) << 28);
    p[8] = SFCI;
    p[10] = 1;
    memcpy(p + 14, &offset, 8);
    uint64_t size = length;
    memcpy(p + 16, &size, 8);
    long rc = reply(file, 0, NULL);
    return rc < 0 ? rc : (long)length;
}
