#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hbx_sdk.h"
#include "hbxl.h"

/* ABI shared with tools/sd-session.cjs. All IPC buffers are in SDK heap memory. */
enum { SD_MAGIC = 0x53444632, SD_CHUNK = 0x40000, SD_PATH = 0x301 };
struct sd_context {
    uint32_t magic, sd;
    uint32_t (*send)(void *, uint64_t, uint32_t);
    uint32_t (*close_handle)(uint32_t);
    uint32_t *command;
    char *path;
    uint8_t *bounce;
};
static inline struct sd_context *card(void) { return hbxl_ctx ? (void *)(uintptr_t)hbxl_ctx->args[5] : NULL; }
struct sd_entry {
    char name[0x301];
    uint8_t pad[3], type, reserved[3];
    int64_t size;
};
_Static_assert(sizeof(struct sd_entry) == 0x310, "directory entry ABI");
/* nn::fs open modes */
enum { SD_READ = 1, SD_WRITE = 2, SD_APPEND = 4 };
long sd_type(const char *path);
long sd_open(const char *path, int directory);
long sd_open_mode(const char *path, uint32_t mode);
long sd_size(uint32_t file);
long sd_read(uint32_t file, int64_t offset, size_t length);
long sd_entries(uint32_t dir);
long sd_close(uint32_t handle);
/* Writing (MKW_SD=all). sd_write sends the first `length` bytes of the bounce buffer. */
long sd_create(const char *path, int64_t size);
long sd_delete(const char *path);
long sd_mkdir(const char *path);
long sd_rmdir(const char *path);
long sd_rename(const char *from, const char *to, int directory);
long sd_write(uint32_t file, int64_t offset, size_t length);
long sd_flush(uint32_t file);
long sd_set_size(uint32_t file, int64_t size);
long sd_modified(const char *path, int64_t *seconds);
extern uint32_t sd_last_result;
long __real_hbxl_vfs_syscall(long, long, long, long, long, long, long, int *);
