#include "julia_psram.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "transition_cache.h"
void julia_psram_get_snapshot(julia_psram_snapshot_t *s)
{
    if (!s) return;
    s->free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s->minimum_free_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    s->largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    s->transition_cache_bytes = transition_cache_bytes();
}
void julia_psram_print(void)
{
    julia_psram_snapshot_t s; julia_psram_get_snapshot(&s);
    printf("PSRAM free=%u min=%u largest=%u transition_cache=%u fragmentation=%u\n",
           (unsigned)s.free_bytes, (unsigned)s.minimum_free_bytes, (unsigned)s.largest_block,
           (unsigned)s.transition_cache_bytes, (unsigned)(s.free_bytes - s.largest_block));
}
