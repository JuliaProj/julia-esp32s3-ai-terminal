#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "avatar_clip_map.h"

typedef struct {
    uint16_t frame;
    int8_t direction;
    int64_t next_due_us;
    uint32_t dropped_frames;
} avatar_loop_player_t;

/* 纯 O(1) 的循环游标。解码和显示仍由 avatar_anim_engine 的双槽流水线负责。 */
void avatar_loop_player_start(avatar_loop_player_t *player,
                              const avatar_clip_descriptor_t *clip, int64_t now_us);
uint16_t avatar_loop_player_next(avatar_loop_player_t *player,
                                 const avatar_clip_descriptor_t *clip);
bool avatar_loop_player_due(avatar_loop_player_t *player,
                            const avatar_clip_descriptor_t *clip, int64_t now_us);
