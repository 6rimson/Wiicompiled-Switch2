#pragma once
#include <stddef.h>
#include <stdint.h>

struct listing {
    char **entries; /* each string carries d_type in the byte before the name */
    size_t count, capacity, position;
    int loaded;
    int card_only; /* MKW_SD=all writable folders: no entries from the PC */
    int remember;  /* read-only game data: keep the card's entries for sd_known */
};
/* Reads a folder's whole listing from the card (open_asset does it at open, then closes it). */
long sd_load(struct listing *, uint32_t, const char *);
long sd_list(struct listing *, uint32_t, const char *, void *, size_t);
void sd_free_listing(struct listing *);
/* What a remembered listing said about path: 1 with *directory and *size set, 0 if unknown. */
int sd_known(const char *path, int *directory, int64_t *size);
/* A log line written to the card at once (vfs.c). Mutex held, bounce buffer free. */
void sd_trace(const char *format, ...);
