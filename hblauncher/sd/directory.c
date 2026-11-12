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
/* Folders of read-only game data listed this launch. Rebuilding the game's file index lists
 * every folder and then asks for the type and size of each of its ~6,000 files, 4 card calls
 * apiece when the card is asked, and that many calls froze the console. The listing already
 * says both, and the game can't change these folders, so stat answers from it instead. */
struct known_entry {
    char *name;
    int64_t size;
    int directory;
};
struct known_folder {
    struct known_folder *next;
    struct known_entry *entries; /* sorted by name */
    size_t count;
    char path[];
};
enum { BUCKETS = 1024, KNOWN_LIMIT = 50000 };
static struct known_folder *known[BUCKETS];
static size_t known_total;

static unsigned bucket(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h & (BUCKETS - 1);
}
static struct known_folder *find_folder(const char *path, size_t n) {
    for (struct known_folder *f = known[bucket(path, n)]; f; f = f->next)
        if (!strncmp(f->path, path, n) && !f->path[n])
            return f;
    return NULL;
}
static int by_name(const void *a, const void *b) {
    return strcmp(((const struct known_entry *)a)->name, ((const struct known_entry *)b)->name);
}
static void forget(struct known_entry *entries, size_t count) {
    for (size_t i = 0; i < count; i++)
        free(entries[i].name);
    free(entries);
}
/* Takes ownership of entries. Memory runs short? Then that folder just isn't remembered. */
static void remember(const char *path, struct known_entry *entries, size_t count) {
    const size_t n = strlen(path);
    struct known_folder *f = NULL;
    if (!find_folder(path, n) && known_total + count <= KNOWN_LIMIT)
        f = malloc(sizeof(*f) + n + 1);
    if (!f) {
        forget(entries, count);
        return;
    }
    qsort(entries, count, sizeof(*entries), by_name);
    f->entries = entries;
    f->count = count;
    memcpy(f->path, path, n + 1);
    const unsigned b = bucket(path, n);
    f->next = known[b];
    known[b] = f;
    known_total += count;
}
int sd_known(const char *path, int *directory, int64_t *size) {
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1])
        return 0;
    const struct known_folder *f = find_folder(path, slash == path ? 1 : (size_t)(slash - path));
    if (!f)
        return 0;
    struct known_entry key = {(char *)slash + 1, 0, 0};
    const struct known_entry *e = bsearch(&key, f->entries, f->count, sizeof(key), by_name);
    if (!e)
        return 0;
    *directory = e->directory;
    *size = e->size;
    return 1;
}

static long load(struct listing *l, uint32_t handle, const char *path) {
    const char *mode = getenv("MKW_SD");
    const int all = mode && !strcmp(mode, "all");
    struct known_entry *seen = NULL;
    size_t seen_count = 0, seen_capacity = 0;
    long count, reads = 0;
    for (;;) {
        /* Before each card read, where the bounce buffer is free (it holds the entries after). */
        if (l->remember)
            sd_trace("[sd] %s: reading entries (%zu so far, read %ld)", path, l->count, reads + 1);
        if ((count = sd_entries(handle)) <= 0)
            break;
        reads++;
        const struct sd_entry *entries = (const void *)card()->bounce;
        for (long i = 0; i < count; i++) {
            size_t n = strnlen(entries[i].name, sizeof(entries[i].name));
            long rc = n == sizeof(entries[i].name) ? -EIO : 0;
            /* Finder creates AppleDouble files alongside assets copied to FAT. */
            if (!rc && (!strncmp(entries[i].name, "._", 2) || !strcmp(entries[i].name, ".DS_Store")))
                continue;
            /* Outside MKW_SD=all the card only answers for RetroRewind6 in this folder. */
            if (!rc && !strcmp(path, "/RetroRewind") && strcmp(entries[i].name, "RetroRewind6") && !all)
                continue;
            if (!rc)
                rc = append(l, entries[i].name, n, entries[i].type ? DT_REG : DT_DIR, 0);
            if (!rc && l->remember && seen_count == seen_capacity) {
                size_t capacity = seen_capacity ? 2 * seen_capacity : 64;
                struct known_entry *grown = realloc(seen, capacity * sizeof(*seen));
                if (grown) {
                    seen = grown;
                    seen_capacity = capacity;
                } else
                    l->remember = 0; /* stat asks the card, as before */
            }
            if (!rc && l->remember) {
                char *name = strndup(entries[i].name, n);
                if (name)
                    seen[seen_count++] = (struct known_entry){name, entries[i].size, !entries[i].type};
                else
                    l->remember = 0;
            }
            if (rc < 0) {
                forget(seen, seen_count);
                return rc;
            }
        }
    }
    if (l->remember)
        sd_trace("[sd] %s: %zu entries in %ld read(s), result %ld", path, l->count, reads, count);
    if (count < 0 || !l->remember)
        forget(seen, seen_count);
    if (count < 0)
        return count;
    if (l->remember)
        remember(path, seen, seen_count);
    long rc = l->card_only ? 0 : load_pc(l, path);
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
long sd_load(struct listing *l, uint32_t handle, const char *path) {
    long rc = load(l, handle, path);
    l->loaded = rc < 0 ? rc : 1;
    return rc < 0 ? rc : 0;
}
long sd_list(struct listing *l, uint32_t handle, const char *path, void *buffer, size_t size) {
    if (!l->loaded)
        sd_load(l, handle, path);
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
