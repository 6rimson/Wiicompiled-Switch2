#pragma once
#include <stddef.h>
#include <stdint.h>

struct listing {
    char **entries; /* each string carries d_type in the byte before the name */
    size_t count, capacity, position;
    int loaded;
    int card_only; /* MKW_SD=all writable folders: no entries from the PC */
};
long sd_list(struct listing *, uint32_t, const char *, void *, size_t);
void sd_free_listing(struct listing *);
