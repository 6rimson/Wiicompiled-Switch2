/* Read-only asset overlay for the HBXL Linux personality. All IPC uses a single
 * SDK-heap scratch area. An open description owns its offset and is shared by dup. */
#define _GNU_SOURCE
#include "storage.h"
#include "directory.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum { FIRST = 2048, LIMIT = 4096, PATH_BYTES = 1024 };
struct description {
    uint32_t handle;
    int directory, refs, flags;
    int64_t position, size;
    struct listing listing;
    char path[];
};
static struct description *table[LIMIT - FIRST];
static unsigned char cloexec[LIMIT - FIRST];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
extern int hbxl_vfs_resolve_path(int, const char *, char *);
static int in_range(long fd) { return fd >= FIRST && fd < LIMIT; }

struct metrics {
    uint64_t opens, bytes, ticks;
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
    if (!memcmp(previous, totals, sizeof(totals)))
        return;
    memcpy(previous, totals, sizeof(totals));
    double sd_rate = totals[2] ? totals[1] / 1048576.0 * hz / totals[2] : 0.0;
    double pc_rate = totals[5] ? totals[4] / 1048576.0 * hz / totals[5] : 0.0;
    fprintf(stderr,
            "[perf] storage cumulative: SD %llu opens %llu B %.1f ms %.2f MiB/s, "
            "PC %llu opens %llu B %.1f ms %.2f MiB/s, %llu "
            "missing-file fallbacks\n",
            (unsigned long long)totals[0], (unsigned long long)totals[1], totals[2] * 1000.0 / hz, sd_rate,
            (unsigned long long)totals[3], (unsigned long long)totals[4], totals[5] * 1000.0 / hz, pc_rate,
            (unsigned long long)totals[6]);
}

static int root(const char *p, const char *prefix) {
    size_t n = strlen(prefix);
    return !strncmp(p, prefix, n) && (p[n] == '/' || !p[n]);
}
static int asset_path(const char *p) {
    return !strcmp(p, "/RetroRewind") || root(p, "/disc") || root(p, "/RetroRewind/RetroRewind6") ||
           !strcmp(p, "/dsp_coef.bin");
}
static int enabled(void) {
    const struct sd_context *ctx = card();
    if (!ctx || ctx->magic != SD_MAGIC)
        return 0;
    const char *v = getenv("MKW_SD");
    return !v || strcmp(v, "0");
}
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
    f->flags = flags;
    memcpy(f->path, path, n);
    table[fd - FIRST] = f;
    cloexec[fd - FIRST] = !!(flags & O_CLOEXEC);
    ADD(sd_stats.opens, 1);
    return fd;
}
static long close_fd(long fd) {
    struct description *f = table[fd - FIRST];
    table[fd - FIRST] = NULL;
    if (--f->refs)
        return 0;
    long rc = sd_close(f->handle);
    sd_free_listing(&f->listing);
    free(f);
    return rc;
}
static long read_at(struct description *f, void *buffer, size_t size, int64_t off) {
    if (f->directory)
        return -EISDIR;
    if (off < 0 || size > LONG_MAX)
        return -EINVAL;
    if (off >= f->size)
        return 0;
    if (size > (size_t)(f->size - off))
        size = f->size - off;
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
        base = f->size;
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
    uint64_t ino = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        ino = (ino ^ *p) * 1099511628211ull;
    memset(st, 0, sizeof(*st));
    st->st_dev = 2;
    st->st_ino = ino;
    st->st_nlink = 1;
    st->st_mode = dir ? S_IFDIR | 0555 : S_IFREG | 0444;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_blocks = (size + 511ull) / 512;
}
static void extended(const struct stat *st, struct statx *sx) {
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_SIZE | STATX_BLOCKS;
    sx->stx_mode = st->st_mode;
    sx->stx_size = st->st_size;
    sx->stx_ino = st->st_ino;
    sx->stx_blocks = st->st_blocks;
    sx->stx_blksize = st->st_blksize;
    sx->stx_nlink = st->st_nlink;
    sx->stx_dev_minor = st->st_dev;
}
static long path_stat(const char *path, long n, long c, long e) {
    long type = sd_type(path);
    if (type < 0)
        return type;
    if (n == SYS_faccessat || n == 439)
        return (c & X_OK) && type != 0 ? -EACCES : 0;
    long size = 0;
    if (type != 0) {
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
        metadata(f->path, f->directory, f->size, &st);
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
           n == SYS_pwrite64 || n == SYS_ftruncate;
}

long __wrap_hbxl_vfs_syscall(long n, long a, long b, long c, long d, long e, long f, int *handled) {
    int path_op = n == SYS_openat || n == SYS_newfstatat || n == SYS_faccessat || n == 439 || n == SYS_statx;
    int empty = (n == SYS_newfstatat || n == SYS_statx) && ((n == SYS_statx ? c : d) & AT_EMPTY_PATH) &&
                (!b || !*(const char *)b);
    if (in_range(a) && (fd_op(n) || empty)) {
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
    if (b && *(const char *)b == '/' && !asset_path((const char *)b) && !strstr((const char *)b, "/.") &&
        !strstr((const char *)b, "//"))
        return pc_call(n, a, b, c, d, e, f, handled);
    char path[PATH_BYTES];
    pthread_mutex_lock(&mutex);
    long rc = resolve(a, (const char *)b, path);
    int route = rc == 0 && asset_path(path);
    if (n == SYS_openat && (c & (O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)))
        route = 0;
    if ((n == SYS_faccessat || n == 439) && (c & W_OK))
        route = 0;
    if (route) {
        rc = n == SYS_openat ? open_asset(path, c) : path_stat(path, n, c, e);
        if (rc == -ENOENT && n == SYS_openat)
            ADD(fallbacks, 1);
    }
    pthread_mutex_unlock(&mutex);
    *handled = 1;
    if ((route && rc != -ENOENT) || (!route && rc < 0))
        return rc;
    return pc_call(n, AT_FDCWD, (long)path, c, d, e, f, handled);
}

extern void __real_hbxl_vfs_shutdown(void);
void __wrap_hbxl_vfs_shutdown(void) {
    /* The runtime can stop while worker threads are still unwinding. Let
     * normal shutdown join them before freeing any remaining SD descriptions. */
    if (!hbxl_threads_live()) {
        for (int fd = FIRST; fd < LIMIT; fd++)
            if (table[fd - FIRST])
                close_fd(fd);
    }
    __real_hbxl_vfs_shutdown();
}
