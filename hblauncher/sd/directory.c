#include "storage.h"
#include "directory.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>

static int append(struct listing *l, const char *name, size_t length, int type, int pc) {
    if (!length || length > 255 || memchr(name, '/', length))
        return -EIO;
    if (l->count == l->capacity) {
        size_t capacity = l->capacity ? 2 * l->capacity : 32;
        char **entries = realloc(l->entries, capacity * sizeof(*entries));
        if (!entries)
            return -ENOMEM;
        l->capacity = capacity;
        l->entries = entries;
    }
    char *s = malloc(length + 3);
    if (!s)
        return -ENOMEM;
    s[0] = pc;
    s[1] = type;
    memcpy(s + 2, name, length);
    s[2 + length] = 0;
    l->entries[l->count++] = s;
    return 0;
}

static int compare(const void *a, const void *b) {
    const char *first = *(char *const *)a, *second = *(char *const *)b;
    int order = strcmp(first + 2, second + 2);
    return order ? order : first[0] - second[0];
}
void sd_free_listing(struct listing *l) {
    for (size_t i = 0; i < l->count; i++)
        free(l->entries[i]);
    free(l->entries);
    memset(l, 0, sizeof(*l));
}

static long load_pc(struct listing *l, const char *path) {
    int handled;
    long fd =
        __real_hbxl_vfs_syscall(SYS_openat, AT_FDCWD, (long)path, O_RDONLY | O_DIRECTORY, 0, 0, 0, &handled);
    if (fd == -ENOENT || fd == -ENOTDIR)
        return 0;
    if (fd < 0)
        return fd;
    long rc;
    for (;;) {
        char *buf = (char *)card()->bounce;
        rc = __real_hbxl_vfs_syscall(SYS_getdents64, fd, (long)buf, SD_CHUNK, 0, 0, 0, &handled);
        if (rc <= 0)
            break;
        long end = rc;
        rc = 0;
        for (long i = 0; i < end;) {
            struct dirent *entry = (void *)(buf + i);
            size_t header = offsetof(struct dirent, d_name);
            if ((size_t)(end - i) <= header || entry->d_reclen <= header || entry->d_reclen > end - i) {
                rc = -EIO;
                break;
            }
            size_t cap = entry->d_reclen - header, len = strnlen(entry->d_name, cap);
            if (len == cap) {
                rc = -EIO;
                break;
            }
            rc = append(l, entry->d_name, len, entry->d_type, 1);
            if (rc < 0)
                break;
            i += entry->d_reclen;
        }
        if (rc < 0)
            break;
    }
    __real_hbxl_vfs_syscall(SYS_close, fd, 0, 0, 0, 0, 0, &handled);
    return rc;
}
static long load(struct listing *l, uint32_t handle, const char *path) {
    long count;
    while ((count = sd_entries(handle)) > 0) {
        const struct sd_entry *entries = (const void *)card()->bounce;
        for (long i = 0; i < count; i++) {
            size_t n = strnlen(entries[i].name, sizeof(entries[i].name));
            if (n == sizeof(entries[i].name))
                return -EIO;
            /* Finder creates AppleDouble files alongside assets copied to FAT. */
            if (!strncmp(entries[i].name, "._", 2) || !strcmp(entries[i].name, ".DS_Store"))
                continue;
            if (!strcmp(path, "/RetroRewind") && strcmp(entries[i].name, "RetroRewind6"))
                continue;
            long rc = append(l, entries[i].name, n, entries[i].type ? DT_REG : DT_DIR, 0);
            if (rc < 0)
                return rc;
        }
    }
    if (count < 0)
        return count;
    long rc = load_pc(l, path);
    if (rc < 0)
        return rc;
    if (l->count)
        qsort(l->entries, l->count, sizeof(*l->entries), compare);
    size_t used = 0;
    for (size_t i = 0; i < l->count; i++) {
        if (used && !strcmp(l->entries[used - 1] + 2, l->entries[i] + 2))
            free(l->entries[i]);
        else
            l->entries[used++] = l->entries[i];
    }
    l->count = used;
    return 0;
}
long sd_list(struct listing *l, uint32_t handle, const char *path, void *buffer, size_t size) {
    if (!l->loaded) {
        long rc = load(l, handle, path);
        l->loaded = rc < 0 ? rc : 1;
    }
    if (l->loaded < 0)
        return l->loaded;
    size_t used = 0;
    while (l->position < l->count) {
        const char *s = l->entries[l->position];
        size_t len = strlen(s + 2) + 1;
        size_t record = (offsetof(struct dirent, d_name) + len + 7) & ~(size_t)7;
        if (record > size - used)
            return used ? (long)used : -EINVAL;
        struct dirent *d = (void *)((char *)buffer + used);
        memset(d, 0, record);
        d->d_ino = l->position + 1;
        d->d_off = ++l->position;
        d->d_reclen = record;
        d->d_type = s[1];
        memcpy(d->d_name, s + 2, len);
        used += record;
    }
    return used;
}
