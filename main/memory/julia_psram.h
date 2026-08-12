#pragma once
#include <stddef.h>
typedef struct { size_t free_bytes, minimum_free_bytes, largest_block, transition_cache_bytes; }
    julia_psram_snapshot_t;
void julia_psram_get_snapshot(julia_psram_snapshot_t *snapshot);
void julia_psram_print(void);
