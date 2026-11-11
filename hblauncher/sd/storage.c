#include "storage.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

enum { SFCI = 0x49434653, SFCO = 0x4f434653 };

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
    if (rc == 0x202)
        return -ENOENT; /* FS PathNotFound */
    if (rc)
        return -EIO;
    if (!!child != moved)
        return -EPROTO;
    if (data)
        *data = p + at + 4;
    return child;
}

static long path_call(const char *path, unsigned command, uint32_t mode, uint32_t **data) {
    struct sd_context *ctx = card();
    static const char prefix[] = "/wiicompiled";
    size_t n = strlen(path);
    if (sizeof(prefix) + n > SD_PATH)
        return -ENAMETOOLONG;
    memcpy(ctx->path, prefix, sizeof(prefix) - 1);
    memcpy(ctx->path + sizeof(prefix) - 1, path, n + 1);
    uintptr_t addr = (uintptr_t)ctx->path;
    uint32_t *p = ctx->command;
    memset(p, 0, 256);
    p[0] = 4 | (1u << 16);
    p[1] = command == 7 ? 8 : 9;
    p[2] = (SD_PATH << 16) | (((addr >> 36) & 7) << 6) | (((addr >> 32) & 15) << 12);
    p[3] = addr;
    p[4] = SFCI;
    p[6] = command;
    p[8] = mode;
    return reply(ctx->sd, command != 7, data);
}

long sd_type(const char *path) {
    uint32_t *data;
    long rc = path_call(path, 7, 0, &data);
    if (rc < 0)
        return rc;
    return *data <= 1 ? (long)*data : -EPROTO;
}
long sd_open(const char *path, int directory) {
    return path_call(path, directory ? 9 : 8, directory ? 3 : 1, NULL);
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
