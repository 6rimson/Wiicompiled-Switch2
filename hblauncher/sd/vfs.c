/* Read-only asset overlay for the HBXL Linux personality. All IPC uses a single
 * SDK-heap scratch area. An open description owns its offset and is shared by dup. */
#define _GNU_SOURCE
#include "storage.h"
#include "directory.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

enum { FIRST = 2048, LIMIT = 4096, PATH_BYTES = 1024 };
/* MKW_SD=all: every open of one writable file shares a node, because the console refuses a
 * second open of a file that is open for writing. */
struct node {
    struct node *next;
    uint32_t handle;
    int refs, dirty;
    int64_t size;
    char path[];
};
static struct node *nodes;
struct description {
    uint32_t handle;
    int directory, refs, flags;
    int64_t position, size;
    struct node *node;
    struct listing listing;
    char path[];
};
static int64_t fsize(const struct description *f) { return f->node ? f->node->size : f->size; }
static struct description *table[LIMIT - FIRST];
static unsigned char cloexec[LIMIT - FIRST];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
extern int hbxl_vfs_resolve_path(int, const char *, char *);
static int in_range(long fd) { return fd >= FIRST && fd < LIMIT; }

struct metrics {
    uint64_t opens, bytes, ticks, written;
};
static struct metrics sd_stats, pc_stats;
static uint64_t fallbacks;
#define ADD(counter, n) __atomic_fetch_add(&(counter), (uint64_t)(n), __ATOMIC_RELAXED)
#define GET(counter) __atomic_load_n(&(counter), __ATOMIC_RELAXED)
void hbxl_storage_perf(void) {
    double hz = hbxl_ctx->tick_hz ? hbxl_ctx->tick_hz : 19200000.0;
    struct metrics *sides[] = {&sd_stats, &pc_stats};
    static uint64_t previous[7];
    uint64_t totals[7];
    for (int i = 0; i < 2; i++) {
        totals[3 * i] = GET(sides[i]->opens);
        totals[3 * i + 1] = GET(sides[i]->bytes);
        totals[3 * i + 2] = GET(sides[i]->ticks);
    }
    totals[6] = GET(fallbacks);
    const uint64_t written = GET(sd_stats.written);
    static uint64_t previous_written;
    if (!memcmp(previous, totals, sizeof(totals)) && written == previous_written)
        return;
    memcpy(previous, totals, sizeof(totals));
    previous_written = written;
    double sd_rate = totals[2] ? totals[1] / 1048576.0 * hz / totals[2] : 0.0;
    double pc_rate = totals[5] ? totals[4] / 1048576.0 * hz / totals[5] : 0.0;
    fprintf(stderr,
            "[perf] storage cumulative: SD %llu opens %llu B %.1f ms %.2f MiB/s, "
            "PC %llu opens %llu B %.1f ms %.2f MiB/s, %llu "
            "files not on SD, %llu B written to SD\n",
            (unsigned long long)totals[0], (unsigned long long)totals[1], totals[2] * 1000.0 / hz, sd_rate,
            (unsigned long long)totals[3], (unsigned long long)totals[4], totals[5] * 1000.0 / hz, pc_rate,
            (unsigned long long)totals[6], (unsigned long long)written);
}

static int root(const char *p, const char *prefix) {
    size_t n = strlen(prefix);
    return !strncmp(p, prefix, n) && (p[n] == '/' || !p[n]);
}
static int asset_path(const char *p) {
    return !strcmp(p, "/RetroRewind") || root(p, "/disc") || root(p, "/RetroRewind/RetroRewind6") ||
           !strcmp(p, "/dsp_coef.bin");
}
/* MKW_SD: unset = game assets from the card; 0 = all from the PC; all = everything from the
 * card, with writes only to the card (experimental). */
enum { SD_OFF, SD_ASSETS, SD_ALL };
/* MKW_SD=all needs a card prepared by tools/sd-card.sh push, which copies the whole file-server
 * root and writes /wiicompiled/.fsroot. Without it the game would start with no NAND or save,
 * so an unprepared card stays in the assets mode. Checked once per launch. */
/* The first file calls come before libc has set up thread data, so this must not use anything
 * that needs it (pthread_once does): it is a plain flag, set under the mutex on first use. */
int sd_card_prepared = -1; /* not static: tests/sd-fake-card.c resets it */
extern void hbxl_vfs_write_console(int, const void *, size_t);
static int prepared_card(void) { /* called with the mutex held */
    if (sd_card_prepared < 0) {
        sd_card_prepared = sd_type("/.fsroot") == 1;
        if (!sd_card_prepared) {
            static const char note[] = "[sd] MKW_SD=all needs a card prepared with tools/sd-card.sh "
                                       "push; using game data from the card only\n";
            hbxl_vfs_write_console(2, note, sizeof(note) - 1);
        }
    }
    return sd_card_prepared;
}
/* MKW_SD as set; outside the mutex, "all" is only a request. */
static int sd_request(void) {
    const struct sd_context *ctx = card();
    if (!ctx || ctx->magic != SD_MAGIC)
        return SD_OFF;
    const char *v = getenv("MKW_SD");
    if (!v)
        return SD_ASSETS;
    if (!strcmp(v, "0"))
        return SD_OFF;
    return strcmp(v, "all") ? SD_ASSETS : SD_ALL;
}
/* The mode in effect: "all" needs a prepared card. Only call it with the mutex held. */
static int sd_mode(void) {
    int mode = sd_request();
    return mode == SD_ALL && !prepared_card() ? SD_ASSETS : mode;
}
static int enabled(void) { return sd_request() != SD_OFF; }
/* Paths the card answers in this mode. /hbxl-env is read before the card is attached. */
static int routed(const char *p) {
    if (sd_mode() != SD_ALL)
        return asset_path(p);
    return *p == '/' && strcmp(p, "/hbxl-env") && !root(p, "/dev") && !root(p, "/proc") && !root(p, "/sys");
}
/* Paths the game may change on the card. The disc and Retro Rewind's own folder stay read-only. */
static int writable(const char *p) { return sd_mode() == SD_ALL && routed(p) && !asset_path(p); }
static void normalize(char *path) {
    char *dst = path, *src = path;
    while (*src) {
        while (*src == '/')
            src++;
        char *part = src;
        while (*src && *src != '/')
            src++;
        size_t n = src - part;
        if (!n || (n == 1 && part[0] == '.'))
            continue;
        if (n == 2 && !memcmp(part, "..", 2)) {
            while (dst > path && dst[-1] != '/')
                dst--;
            if (dst > path)
                dst--;
        } else {
            *dst++ = '/';
            memmove(dst, part, n);
            dst += n;
        }
    }
    if (dst == path)
        *dst++ = '/';
    *dst = 0;
}
static long resolve(long fd, const char *name, char *out) {
    if (!name)
        return -EFAULT;
    if (!*name)
        return -ENOENT;
    if (*name != '/' && in_range(fd)) {
        struct description *dir = table[fd - FIRST];
        if (!dir)
            return -EBADF;
        if (!dir->directory)
            return -ENOTDIR;
        size_t a = strlen(dir->path), b = strlen(name);
        if (a + 1 + b >= PATH_BYTES)
            return -ENAMETOOLONG;
        memcpy(out, dir->path, a);
        out[a] = '/';
        memcpy(out + a + 1, name, b + 1);
    } else {
        long rc = hbxl_vfs_resolve_path(fd, name, out);
        if (rc < 0)
            return rc;
    }
    normalize(out);
    return 0;
}

static int vacant(long low) {
    if (low < FIRST)
        low = FIRST;
    for (; low < LIMIT; low++)
        if (!table[low - FIRST])
            return low;
    return -EMFILE;
}
static long open_asset(const char *path, int flags) {
    int fd = vacant(FIRST);
    if (fd < 0)
        return fd;
    /* A read-only open without O_DIRECTORY can open a directory, too. */
    long type = sd_type(path);
    if (type < 0)
        return type;
    int dir = type == 0;
    if ((flags & O_DIRECTORY) && !dir)
        return -ENOTDIR;
    const int scan = dir && (root(path, "/disc") || root(path, "/RetroRewind/RetroRewind6"));
    if (scan)
        sd_trace("[sd] opening folder %s", path);
    long handle = sd_open(path, dir);
    if (handle < 0)
        return handle;
    long size = dir ? 0 : sd_size(handle);
    if (size < 0) {
        sd_close(handle);
        return size;
    }
    size_t n = strlen(path) + 1;
    struct description *f = calloc(1, sizeof(*f) + n);
    if (!f) {
        sd_close(handle);
        return -ENOMEM;
    }
    f->refs = 1;
    f->handle = handle;
    f->size = size;
    f->directory = dir;
    /* With MKW_SD=all the card is a full copy of the root (sd-card.sh push), so folders list the
     * card only: the PC's copy may be stale, or not running at all. */
    f->listing.card_only = dir && sd_mode() == SD_ALL;
    /* The game can't change these, so stat may answer from what the listing said (sd_known). */
    f->listing.remember = scan;
    if (dir) {
        /* The whole listing now, and the folder closed on the card at once. The console's file
         * service stops for good (the whole console with it) when one more folder is opened while
         * a few are open: rebuilding the game's file index, which walks folders with all their
         * parents open, froze the console opening its 5th. Reading ahead keeps one open at most. */
        long listed = sd_load(&f->listing, handle, path);
        long closed = sd_close(handle);
        f->handle = 0;
        if (listed < 0 || closed < 0) {
            sd_free_listing(&f->listing);
            free(f);
            return listed < 0 ? listed : closed;
        }
    }
    f->flags = flags;
    memcpy(f->path, path, n);
    table[fd - FIRST] = f;
    cloexec[fd - FIRST] = !!(flags & O_CLOEXEC);
    ADD(sd_stats.opens, 1);
    return fd;
}
static struct node *find_node(const char *path) {
    for (struct node *n = nodes; n; n = n->next)
        if (!strcmp(n->path, path))
            return n;
    return NULL;
}
static long release_node(struct node *n) {
    if (--n->refs)
        return 0;
    long rc = n->dirty ? sd_flush(n->handle) : 0;
    long closed = sd_close(n->handle);
    for (struct node **link = &nodes; *link; link = &(*link)->next)
        if (*link == n) {
            *link = n->next;
            break;
        }
    free(n);
    return rc < 0 ? rc : closed;
}
static long close_fd(long fd) {
    struct description *f = table[fd - FIRST];
    table[fd - FIRST] = NULL;
    if (--f->refs)
        return 0;
    long rc = f->node ? release_node(f->node) : f->directory ? 0 : sd_close(f->handle); /* see open_asset */
    sd_free_listing(&f->listing);
    free(f);
    return rc;
}

/* ---- MKW_SD=all: writing to the card ------------------------------------------------------ */

/* Creates every missing directory above path on the card. */
static long make_parents(const char *path) {
    char partial[PATH_BYTES];
    size_t n = strlen(path);
    if (n >= sizeof(partial))
        return -ENAMETOOLONG;
    memcpy(partial, path, n + 1);
    for (char *slash = strchr(partial + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        long type = sd_type(partial);
        if (type == -ENOENT)
            type = sd_mkdir(partial);
        else if (type == 1)
            type = -ENOTDIR;
        *slash = '/';
        if (type < 0 && type != -EEXIST)
            return type;
    }
    return 0;
}

/* Opens a writable-area file on the card through its shared node. In MKW_SD=all the card is
 * the only copy of these files: nothing falls back to the PC or is copied from it, so a file the
 * game deleted (a SQLite journal, say) can't come back from the PC's older copy. */
static long open_card(const char *path, int flags) {
    const int access = flags & O_ACCMODE;
    const int writes = access != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND));
    int fd = vacant(FIRST);
    if (fd < 0)
        return fd;
    long type = sd_type(path);
    if (type == -ENOENT && (flags & O_CREAT)) {
        long rc = make_parents(path);
        if (!rc)
            rc = sd_create(path, 0);
        if (rc < 0)
            return rc;
        type = 1;
    } else if (type < 0)
        return type;
    else if ((flags & O_CREAT) && (flags & O_EXCL))
        return -EEXIST;
    if (type == 0) {
        if (writes)
            return -EISDIR;
        return -EAGAIN; /* a directory: the caller opens it like an asset directory */
    }
    if (flags & O_DIRECTORY)
        return -ENOTDIR;
    struct node *node = find_node(path);
    size_t n = strlen(path) + 1;
    if (!node) {
        long handle = sd_open_mode(path, SD_READ | SD_WRITE | SD_APPEND);
        if (handle < 0)
            return handle;
        long size = sd_size(handle);
        node = size < 0 ? NULL : calloc(1, sizeof(*node) + n);
        if (!node) {
            sd_close(handle);
            return size < 0 ? size : -ENOMEM;
        }
        node->handle = handle;
        node->size = size;
        memcpy(node->path, path, n);
        node->next = nodes;
        nodes = node;
    }
    struct description *f = calloc(1, sizeof(*f) + n);
    if (!f) {
        if (!node->refs)
            release_node((node->refs = 1, node));
        return -ENOMEM;
    }
    node->refs++;
    if ((flags & O_TRUNC) && access != O_RDONLY && node->size) {
        long rc = sd_set_size(node->handle, 0);
        if (rc < 0) {
            release_node(node);
            free(f);
            return rc;
        }
        node->size = 0;
        node->dirty = 1;
    }
    f->refs = 1;
    f->handle = node->handle;
    f->node = node;
    f->flags = flags;
    memcpy(f->path, path, n);
    table[fd - FIRST] = f;
    cloexec[fd - FIRST] = !!(flags & O_CLOEXEC);
    ADD(sd_stats.opens, 1);
    return fd;
}

static long write_at(struct description *f, const void *buffer, size_t size, int64_t off) {
    struct node *node = f->node;
    if (!node || (f->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (off < 0 || size > LONG_MAX)
        return -EINVAL;
    if (off > node->size) { /* a gap: grow first, as a POSIX file would */
        long rc = sd_set_size(node->handle, off);
        if (rc < 0)
            return rc;
        node->size = off;
    }
    size_t done = 0;
    while (done < size) {
        size_t want = size - done < SD_CHUNK ? size - done : SD_CHUNK;
        memcpy(card()->bounce, (const char *)buffer + done, want);
        long rc = sd_write(node->handle, off + done, want);
        if (rc < 0)
            return done ? (long)done : rc;
        done += want;
    }
    if (off + (int64_t)done > node->size)
        node->size = off + done;
    node->dirty = 1;
    ADD(sd_stats.written, done);
    return done;
}
static long vector_write(struct description *f, const struct iovec *vec, long count, int64_t off) {
    if (count < 0 || count > IOV_MAX || off < 0)
        return -EINVAL;
    long done = 0;
    for (long i = 0; i < count; i++) {
        long put = write_at(f, vec[i].iov_base, vec[i].iov_len, off + done);
        if (put < 0)
            return done ? done : put;
        done += put;
    }
    return done;
}
static long truncate_node(struct description *f, int64_t length) {
    if (!f->node || (f->flags & O_ACCMODE) == O_RDONLY)
        return -EINVAL;
    long rc = sd_set_size(f->node->handle, length);
    if (rc < 0)
        return rc;
    f->node->size = length;
    f->node->dirty = 1;
    return 0;
}
static long read_at(struct description *f, void *buffer, size_t size, int64_t off) {
    if (f->directory)
        return -EISDIR;
    if (off < 0 || size > LONG_MAX)
        return -EINVAL;
    const int64_t end = fsize(f);
    if (off >= end)
        return 0;
    if (size > (size_t)(end - off))
        size = end - off;
    size_t done = 0;
    while (done < size) {
        size_t want = size - done < SD_CHUNK ? size - done : SD_CHUNK;
        long got = sd_read(f->handle, off + done, want);
        if (got < 0)
            return done ? (long)done : got;
        memcpy((char *)buffer + done, card()->bounce, got);
        done += got;
        if ((size_t)got < want)
            break;
    }
    return done;
}
static long vector_read(struct description *f, const struct iovec *vec, long count, int64_t off) {
    if (count < 0 || count > IOV_MAX || off < 0)
        return -EINVAL;
    size_t length = 0;
    for (long i = 0; i < count; i++) {
        if (vec[i].iov_len > (size_t)LONG_MAX - length)
            return -EINVAL;
        length += vec[i].iov_len;
    }
    long done = 0;
    for (long i = 0; i < count; i++) {
        long got = read_at(f, vec[i].iov_base, vec[i].iov_len, off + done);
        if (got < 0)
            return done ? done : got;
        done += got;
        if ((size_t)got < vec[i].iov_len)
            break;
    }
    return done;
}
static long seek(struct description *f, long offset, long whence) {
    int64_t base = f->directory ? (int64_t)f->listing.position : f->position;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_END && !f->directory)
        base = fsize(f);
    else if (whence != SEEK_CUR)
        return -EINVAL;
    int64_t next;
    if (__builtin_add_overflow(base, offset, &next))
        return -EOVERFLOW;
    if (next < 0)
        return -EINVAL;
    if (f->directory) {
        if ((uint64_t)next > f->listing.count)
            return -EINVAL;
        f->listing.position = next;
    } else
        f->position = next;
    return next;
}

static void metadata(const char *path, int dir, int64_t size, struct stat *st) {
    const int rw = writable(path);
    uint64_t ino = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        ino = (ino ^ *p) * 1099511628211ull;
    memset(st, 0, sizeof(*st));
    st->st_dev = 2;
    st->st_ino = ino;
    st->st_nlink = 1;
    st->st_mode = dir ? S_IFDIR | (rw ? 0755 : 0555) : S_IFREG | (rw ? 0644 : 0444);
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_blocks = (size + 511ull) / 512;
    /* The game deletes logs older than four days by their date, so files it can change get the
     * card's date (the console's local time, which is close enough for that), or now where the
     * card has none, as for folders. Game data stays at 0: one card call less. */
    if (rw) {
        int64_t seconds;
        st->st_mtim.tv_sec = dir || sd_modified(path, &seconds) ? time(NULL) : seconds;
        st->st_atim = st->st_ctim = st->st_mtim;
    }
}
static void extended(const struct stat *st, struct statx *sx) {
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_SIZE | STATX_BLOCKS |
                   STATX_MTIME | STATX_ATIME | STATX_CTIME;
    sx->stx_mtime.tv_sec = sx->stx_atime.tv_sec = sx->stx_ctime.tv_sec = st->st_mtim.tv_sec;
    sx->stx_mode = st->st_mode;
    sx->stx_size = st->st_size;
    sx->stx_ino = st->st_ino;
    sx->stx_blocks = st->st_blocks;
    sx->stx_blksize = st->st_blksize;
    sx->stx_nlink = st->st_nlink;
    sx->stx_dev_minor = st->st_dev;
}
static long path_stat(const char *path, long n, long c, long e) {
    int directory;
    int64_t size = 0;
    const int known = sd_known(path, &directory, &size); /* no card calls */
    long type = known ? !directory : sd_type(path);
    if (type < 0)
        return type;
    if (n == SYS_faccessat || n == 439)
        return ((c & X_OK) && type != 0) || ((c & W_OK) && !writable(path)) ? -EACCES : 0;
    const struct node *open_node = type != 0 && !known ? find_node(path) : NULL;
    if (known)
        size = type ? size : 0;
    else if (open_node)
        size = open_node->size;
    else if (type != 0) {
        long file = sd_open(path, 0);
        if (file < 0)
            return file;
        size = sd_size(file);
        long rc = sd_close(file);
        if (size < 0 || rc < 0)
            return size < 0 ? size : rc;
    }
    struct stat st;
    metadata(path, type == 0, size, &st);
    if (n == SYS_statx)
        extended(&st, (void *)e);
    else
        memcpy((void *)c, &st, sizeof(st));
    return 0;
}

static long fd_call(struct description *f, long n, long a, long b, long c, long d, long e) {
    if (n == SYS_close)
        return close_fd(a);
    if (n == SYS_lseek)
        return seek(f, b, c);
    if (n == SYS_getdents64)
        return f->directory ? sd_list(&f->listing, f->handle, f->path, (void *)b, c) : -ENOTDIR;
    if (n == SYS_fstat || n == SYS_newfstatat || n == SYS_statx) {
        struct stat st;
        metadata(f->path, f->directory, fsize(f), &st);
        if (n == SYS_statx)
            extended(&st, (void *)e);
        else
            memcpy((void *)(n == SYS_fstat ? b : c), &st, sizeof(st));
        return 0;
    }
    if (n == SYS_fcntl || n == SYS_dup) {
        if (n == SYS_dup || b == F_DUPFD || b == F_DUPFD_CLOEXEC) {
            if (n == SYS_fcntl && c < 0)
                return -EINVAL;
            int fd = vacant(n == SYS_dup ? FIRST : c);
            if (fd >= 0) {
                table[fd - FIRST] = f;
                f->refs++;
                cloexec[fd - FIRST] = n == SYS_fcntl && b == F_DUPFD_CLOEXEC;
            }
            return fd;
        }
        if (b == F_GETFL)
            return f->flags;
        if (b == F_GETFD)
            return cloexec[a - FIRST] ? FD_CLOEXEC : 0;
        if (b == F_SETFD) {
            cloexec[a - FIRST] = !!(c & FD_CLOEXEC);
            return 0;
        }
        if (b == F_SETFL) {
            f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (c & (O_APPEND | O_NONBLOCK));
            return 0;
        }
        /* One process owns the card's files, as with the file server: locks are always granted. */
        if (b == F_GETLK || b == F_OFD_GETLK) {
            ((struct flock *)c)->l_type = F_UNLCK;
            return 0;
        }
        if (b == F_SETLK || b == F_SETLKW || b == F_OFD_SETLK || b == F_OFD_SETLKW)
            return 0;
        return -EINVAL;
    }
    if (n == SYS_read || n == SYS_pread64 || n == SYS_readv || n == SYS_preadv) {
        uint64_t start = hbxl_svc_tick();
        long got;
        if (n == SYS_read || n == SYS_pread64)
            got = read_at(f, (void *)b, c, n == SYS_read ? f->position : d);
        else {
            int64_t off = n == SYS_readv ? f->position : (int64_t)((uint64_t)d | ((uint64_t)e << 32));
            got = vector_read(f, (void *)b, c, off);
        }
        if (got > 0) {
            if (n == SYS_read || n == SYS_readv)
                f->position += got;
            ADD(sd_stats.bytes, got);
        }
        ADD(sd_stats.ticks, hbxl_svc_tick() - start);
        return got;
    }
    if (n == SYS_write || n == SYS_pwrite64 || n == SYS_writev || n == SYS_pwritev) {
        const int sequential = n == SYS_write || n == SYS_writev;
        int64_t off = n == SYS_pwrite64 ? d
                      : n == SYS_pwritev ? (int64_t)((uint64_t)d | ((uint64_t)e << 32))
                      : (f->flags & O_APPEND) && f->node ? f->node->size
                                                         : f->position;
        long put = n == SYS_write || n == SYS_pwrite64 ? write_at(f, (const void *)b, c, off)
                                                       : vector_write(f, (const void *)b, c, off);
        if (put > 0 && sequential)
            f->position = off + put;
        return put;
    }
    if (n == SYS_ftruncate)
        return b < 0 ? -EINVAL : truncate_node(f, b);
    if (n == SYS_fsync || n == SYS_fdatasync) {
        if (!f->node || !f->node->dirty)
            return 0;
        long rc = sd_flush(f->node->handle);
        if (!rc)
            f->node->dirty = 0;
        return rc;
    }
    if (n == SYS_fchmod || n == SYS_fchown || n == SYS_flock || n == SYS_utimensat)
        return 0;
    return -EBADF;
}
static int read_op(long n) { return n == SYS_read || n == SYS_readv || n == SYS_pread64 || n == SYS_preadv; }
static long pc_call(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    if (n == SYS_statx) {
        struct stat st;
        long rc = __real_hbxl_vfs_syscall(SYS_newfstatat, a, b, (long)&st, c, 0, 0, handled);
        if (!rc) {
            struct statx *sx = (void *)e;
            extended(&st, sx);
            sx->stx_mtime.tv_sec = st.st_mtim.tv_sec;
            sx->stx_mtime.tv_nsec = st.st_mtim.tv_nsec;
            sx->stx_atime.tv_sec = st.st_atim.tv_sec;
            sx->stx_atime.tv_nsec = st.st_atim.tv_nsec;
            sx->stx_ctime.tv_sec = st.st_ctim.tv_sec;
            sx->stx_ctime.tv_nsec = st.st_ctim.tv_nsec;
            sx->stx_mask |= STATX_MTIME | STATX_ATIME | STATX_CTIME;
        }
        return rc;
    }
    if (n == SYS_preadv) {
        const struct iovec *v = (void *)b;
        if (c < 0 || c > IOV_MAX) {
            *handled = 1;
            return -EINVAL;
        }
        int64_t off = (int64_t)((uint64_t)d | ((uint64_t)e << 32));
        long total = 0;
        *handled = 1;
        if (off < 0)
            return -EINVAL;
        for (long i = 0; i < c; i++) {
            if (v[i].iov_len > (size_t)(LONG_MAX - total) || off > LONG_MAX - total)
                return -EINVAL;
            long got = pc_call(SYS_pread64, a, (long)v[i].iov_base, v[i].iov_len, off + total, 0, 0, handled);
            if (got < 0)
                return total ? total : got;
            total += got;
            if ((size_t)got < v[i].iov_len)
                break;
        }
        return total;
    }
    int timed = a >= 3 && a < FIRST && read_op(n);
    uint64_t start = timed ? hbxl_svc_tick() : 0;
    long rc = __real_hbxl_vfs_syscall(n, a, b, c, d, e, f, handled);
    if (n == SYS_openat && rc >= 0 && *handled)
        ADD(pc_stats.opens, 1);
    if (timed && *handled) {
        if (rc > 0)
            ADD(pc_stats.bytes, rc);
        ADD(pc_stats.ticks, hbxl_svc_tick() - start);
    }
    return rc;
}
static int fd_op(long n) {
    return read_op(n) || n == SYS_close || n == SYS_fstat || n == SYS_getdents64 || n == SYS_lseek ||
           n == SYS_fcntl || n == SYS_dup || n == SYS_dup3 || n == SYS_write || n == SYS_writev ||
           n == SYS_pwrite64 || n == SYS_pwritev || n == SYS_ftruncate || n == SYS_fsync ||
           n == SYS_fdatasync || n == SYS_fchmod || n == SYS_fchown || n == SYS_flock;
}

/* ---- MKW_SD=all: the game log on the card ------------------------------------------------- */

/* Everything the game and the runtime print goes to /UserData/Logs/console.log on the card too
 * (the previous launch's is kept as console.prev.log), so a session needs no file server to keep
 * its log. Text collects in a buffer, and the buffer goes to the card at most every quarter
 * second, then is flushed so a crash keeps what came before it. Log text can come from inside a
 * card operation that holds the mutex, so writing out only ever tries the mutex. */
enum { LOG_BYTES = 64 * 1024 };
static char log_buffer[LOG_BYTES];
static size_t log_used;
static uint64_t log_dropped, log_last_flush;
static int log_state; /* 0 not decided yet, 1 writing to the card, -1 off */
static uint32_t log_file;
static int64_t log_offset;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Before libc starts, environ is still unset and so is MKW_SD: the runtime's first lines are kept
 * in case the mode turns out to be "all". Only one thread runs then, so no lock is needed (or
 * safe: libc hasn't set up thread data). */
extern char **environ;
static void log_append(const void *data, size_t length) {
    const int early = !environ, mode = sd_request();
    if (log_state < 0 || !length || mode == SD_OFF || (!early && mode != SD_ALL))
        return;
    if (!early)
        pthread_mutex_lock(&log_mutex);
    if (log_used + length <= LOG_BYTES) {
        memcpy(log_buffer + log_used, data, length);
        log_used += length;
    } else
        log_dropped += length;
    if (!early)
        pthread_mutex_unlock(&log_mutex);
}

/* Called with the mutex held. */
static int log_open(void) {
    static const char path[] = "/UserData/Logs/console.log", previous[] = "/UserData/Logs/console.prev.log";
    if (sd_mode() != SD_ALL || make_parents(path) < 0)
        return -1;
    if (sd_type(path) == 1) {
        sd_delete(previous);
        long rc = sd_rename(path, previous, 0);
        if (rc < 0) {
            char line[96];
            int n = snprintf(line, sizeof(line), "[sd] couldn't keep the previous log: %ld (result %04u-%04u)\n", rc,
                             2000 + (sd_last_result & 0x1ff), sd_last_result >> 9);
            log_append(line, n > 0 && n < (int)sizeof(line) ? (size_t)n : 0);
            sd_delete(path);
        }
    }
    if (sd_create(path, 0) < 0)
        return -1;
    long handle = sd_open_mode(path, SD_WRITE | SD_APPEND);
    if (handle < 0)
        return -1;
    log_file = handle;
    log_offset = 0;
    return 1;
}

/* Called with the mutex held. The text is copied out first and log_mutex released before any
 * card call, since a card error prints a line that comes back through log_append. */
static char log_out[LOG_BYTES + 64];
static void log_write_out(void) {
    if (!log_state)
        log_state = log_open();
    pthread_mutex_lock(&log_mutex);
    size_t used = log_state < 0 ? 0 : log_used;
    memcpy(log_out, log_buffer, used);
    const uint64_t dropped = log_dropped;
    log_used = 0;
    log_dropped = 0;
    pthread_mutex_unlock(&log_mutex);
    if (log_state < 0)
        return;
    if (dropped) {
        int n = snprintf(log_out + used, 64, "\n[sd] %llu bytes of log dropped here\n",
                         (unsigned long long)dropped);
        if (n > 0 && n < 64)
            used += n;
    }
    long rc = 0;
    for (size_t done = 0; done < used && rc >= 0;) {
        size_t want = used - done < SD_CHUNK ? used - done : SD_CHUNK;
        memcpy(card()->bounce, log_out + done, want);
        rc = sd_write(log_file, log_offset, want);
        if (rc >= 0) {
            log_offset += want;
            done += want;
        }
    }
    if (rc < 0 || sd_flush(log_file) < 0) {
        sd_close(log_file);
        log_state = -1; /* the card refused: stop trying */
    }
}

/* Writes the buffer out if a quarter second has passed or it's getting full. */
static void log_maybe_write_out(int force) {
    if (!__atomic_load_n(&log_used, __ATOMIC_RELAXED) || log_state < 0)
        return;
    const uint64_t now = hbxl_svc_tick();
    const uint64_t hz = hbxl_ctx && hbxl_ctx->tick_hz ? hbxl_ctx->tick_hz : 19200000;
    if (!force && now - log_last_flush < hz / 4 && log_used < LOG_BYTES / 2)
        return;
    if (pthread_mutex_trylock(&mutex))
        return; /* inside a card operation (maybe this thread's): next time */
    log_last_flush = now;
    log_write_out();
    pthread_mutex_unlock(&mutex);
}

/* A line for the log that reaches the card at once, for steps that might stop the console (a
 * folder scan): the last line then shows where. Call with the mutex held and the bounce buffer
 * free, as the write uses both. */
void sd_trace(const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    if (sd_mode() != SD_ALL) {
        hbxl_vfs_write_console(2, line, n);
        return;
    }
    log_append(line, n);
    log_write_out();
}

/* hbxl_log and hbxl_logf from the runtime come through here (linked with --wrap). */
extern void __real_hbxl_vfs_write_console(int, const void *, size_t);
void __wrap_hbxl_vfs_write_console(int fd, const void *data, size_t length) {
    log_append(data, length);
    __real_hbxl_vfs_write_console(fd, data, length);
}

/* Starting without the file server. The runtime reads /hbxl-env (the settings, MKW_SD=all among
 * them) from the server before libc starts. With no server it gets the card's copy, written by
 * tools/sd-card.sh push. That read happens before any other thread exists and before libc can
 * allocate, so the copy lives in a static buffer behind a reserved descriptor. */
enum { ENV_FD = LIMIT - 1 };
static char env_copy[4096];
static long env_length, env_at;
static int env_open;
static long card_env(void) {
    static const char missing[] = "[sd] no file server, and no settings on the card: prepare it with tools/sd-card.sh push\n",
                      found[] = "[sd] no file server; settings from the card's hbxl-env\n";
    if (sd_type("/.fsroot") != 1 || sd_type("/hbxl-env") != 1) {
        hbxl_vfs_write_console(2, missing, sizeof(missing) - 1);
        return -EIO;
    }
    long handle = sd_open("/hbxl-env", 0);
    if (handle < 0)
        return handle;
    long got = sd_size(handle);
    if (got > (long)sizeof(env_copy) - 1)
        got = sizeof(env_copy) - 1;
    if (got > 0)
        got = sd_read(handle, 0, got);
    if (got > 0)
        memcpy(env_copy, card()->bounce, got);
    sd_close(handle);
    if (got < 0)
        return got;
    env_length = got;
    env_at = 0;
    env_open = 1;
    sd_card_prepared = 1;
    hbxl_vfs_write_console(2, found, sizeof(found) - 1);
    return ENV_FD;
}
static long env_call(long n, long buffer, long size) {
    if (n == SYS_close) {
        env_open = 0;
        return 0;
    }
    if (n != SYS_read)
        return -EINVAL;
    long k = size < env_length - env_at ? size : env_length - env_at;
    if (k < 0)
        return -EINVAL;
    memcpy((void *)buffer, env_copy + env_at, k);
    env_at += k;
    return k;
}

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif
static int change_op(long n) {
    return n == SYS_mkdirat || n == SYS_unlinkat || n == SYS_renameat || n == SYS_renameat2 ||
           n == SYS_fchmodat || n == SYS_fchownat || n == SYS_utimensat;
}
/* MKW_SD=all: creating, deleting and renaming happen on the card only. Called with the mutex. */
static long card_change(long n, const char *path, const char *target, long c, long e) {
    if (!writable(path) || (target && !writable(target)))
        return -EROFS;
    if (n == SYS_mkdirat) {
        long type = sd_type(path);
        if (type >= 0)
            return -EEXIST;
        if (type != -ENOENT)
            return type;
        long rc = make_parents(path);
        return rc ? rc : sd_mkdir(path);
    }
    if (n == SYS_unlinkat) {
        if (find_node(path))
            return -EBUSY; /* the console can't delete an open file */
        if (c & AT_REMOVEDIR)
            return sd_rmdir(path);
        long rc = sd_delete(path);
        return rc == -ENOENT && sd_type(path) == 0 ? -EISDIR : rc; /* remove() then tries rmdir */
    }
    if (n == SYS_renameat || n == SYS_renameat2) {
        const long flags = n == SYS_renameat2 ? e : 0;
        if (flags & ~(long)RENAME_NOREPLACE)
            return -EINVAL;
        if (find_node(path) || find_node(target))
            return -EBUSY;
        long type = sd_type(path);
        if (type < 0)
            return type;
        long existing = sd_type(target);
        if (existing >= 0) {
            if (flags & RENAME_NOREPLACE)
                return -EEXIST;
            if (existing != type)
                return existing == 0 ? -EISDIR : -ENOTDIR;
            /* nn::fs won't replace a file on rename: remove the old one first. */
            long rc = existing == 0 ? sd_rmdir(target) : sd_delete(target);
            if (rc < 0)
                return rc;
        } else if (existing != -ENOENT)
            return existing;
        long rc = make_parents(target);
        return rc ? rc : sd_rename(path, target, type == 0);
    }
    /* chmod, chown and timestamps: FAT keeps none of them. */
    long type = sd_type(path);
    return type >= 0 ? 0 : type;
}

long __wrap_hbxl_vfs_syscall(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    const int all = sd_request() == SD_ALL; /* confirmed under the mutex below */
    if (all) {
        if ((a == 1 || a == 2) && (n == SYS_write || n == SYS_writev)) {
            if (n == SYS_write)
                log_append((const void *)b, c);
            else
                for (long i = 0; i < c && i < IOV_MAX; i++)
                    log_append(((const struct iovec *)b)[i].iov_base, ((const struct iovec *)b)[i].iov_len);
        }
        log_maybe_write_out(0);
    }
    if (env_open && a == ENV_FD) {
        *handled = 1;
        return env_call(n, b, c);
    }
    if (n == SYS_openat && b && !strcmp((const char *)b, "/hbxl-env") && enabled()) {
        long rc = pc_call(n, a, b, c, d, e, f, handled);
        if (rc != -EIO)
            return rc;
        *handled = 1;
        return card_env(); /* -EIO: no file server */
    }
    int path_op = n == SYS_openat || n == SYS_newfstatat || n == SYS_faccessat || n == 439 || n == SYS_statx ||
                  (all && change_op(n) && !(n == SYS_utimensat && !b));
    int empty = (n == SYS_newfstatat || n == SYS_statx) && ((n == SYS_statx ? c : d) & AT_EMPTY_PATH) &&
                (!b || !*(const char *)b);
    if (in_range(a) && (fd_op(n) || empty || (n == SYS_utimensat && !b))) {
        *handled = 1;
        pthread_mutex_lock(&mutex);
        struct description *desc = table[a - FIRST];
        long rc = desc ? fd_call(desc, n, a, b, c, d, e) : -EBADF;
        pthread_mutex_unlock(&mutex);
        return rc;
    }
    if (!path_op || !enabled() || empty)
        return pc_call(n, a, b, c, d, e, f, handled);
    /* All pre-libc paths (including /hbxl-env) bypass the overlay lock. */
    if (!all && b && *(const char *)b == '/' && !asset_path((const char *)b) && !strstr((const char *)b, "/.") &&
        !strstr((const char *)b, "//"))
        return pc_call(n, a, b, c, d, e, f, handled);
    char path[PATH_BYTES], target[PATH_BYTES];
    pthread_mutex_lock(&mutex);
    long rc = resolve(a, (const char *)b, path);
    const int rename = n == SYS_renameat || n == SYS_renameat2;
    if (rc == 0 && rename)
        rc = resolve(c, (const char *)d, target);
    if (all && change_op(n)) {
        const int mine = rc == 0 && sd_mode() == SD_ALL && routed(path) && (!rename || routed(target));
        if (mine)
            rc = card_change(n, path, rename ? target : NULL, c, e);
        pthread_mutex_unlock(&mutex);
        if (mine || rc < 0) {
            *handled = 1;
            return rc;
        }
        return pc_call(n, a, b, c, d, e, f, handled);
    }
    int route = rc == 0 && routed(path);
    const int writes = (n == SYS_openat && (c & (O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND))) ||
                       ((n == SYS_faccessat || n == 439) && (c & W_OK));
    if (route && writes && !writable(path)) {
        if (sd_mode() == SD_ALL)
            rc = n == SYS_openat ? -EROFS : -EACCES; /* the disc and Retro Rewind stay read-only */
        route = 0;
    }
    if (route) {
        if (n == SYS_openat) {
            rc = writable(path) ? open_card(path, c) : open_asset(path, c);
            if (rc == -EAGAIN)
                rc = open_asset(path, c); /* a directory in the writable area */
        } else
            rc = path_stat(path, n, c, e);
        if (rc == -ENOENT && n == SYS_openat)
            ADD(fallbacks, 1);
    }
    const int card_only = route && writable(path);
    pthread_mutex_unlock(&mutex);
    *handled = 1;
    /* In MKW_SD=all the card is the only copy of writable files: no fallback for them. */
    if (route && rc == -ENOENT && (writes || card_only))
        return rc;
    if ((route && rc != -ENOENT) || (!route && rc < 0))
        return rc;
    long pc = pc_call(n, AT_FDCWD, (long)path, c, d, e, f, handled);
    /* -EIO: no file server. A file the card doesn't have is then simply missing. */
    return route && pc == -EIO ? -ENOENT : pc;
}

extern void __real_hbxl_vfs_shutdown(void);
void __wrap_hbxl_vfs_shutdown(void) {
    log_maybe_write_out(1);
    /* The runtime can stop while worker threads are still unwinding. Let
     * normal shutdown join them before freeing any remaining SD descriptions. */
    if (!hbxl_threads_live()) {
        if (log_state > 0) {
            sd_close(log_file);
            log_state = -1;
        }
        for (int fd = FIRST; fd < LIMIT; fd++)
            if (table[fd - FIRST])
                close_fd(fd);
    }
    __real_hbxl_vfs_shutdown();
}
