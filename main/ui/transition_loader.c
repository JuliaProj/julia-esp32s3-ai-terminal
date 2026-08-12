#include "transition_loader.h"

#include "transition_cache.h"

esp_err_t transition_loader_init(void) { return transition_player_init(); }

transition_source_t transition_loader_source(julia_main_state_t from, julia_main_state_t to)
{
    /* transition_player_has covers the scanned SD manifest. The player then resolves
     * PSRAM cache first and loads SD only on a miss. */
    return transition_player_has(from, to) ? TRANSITION_SOURCE_SD : TRANSITION_SOURCE_NONE;
}

esp_err_t transition_loader_play(julia_main_state_t from, julia_main_state_t to,
                                 transition_player_done_cb_t callback, void *context)
{
    return transition_player_play(from, to, callback, context);
}

void transition_loader_stop(void) { transition_player_stop(); }

