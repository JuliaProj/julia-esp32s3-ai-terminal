#include "avatar_loop_player.h"

#include <string.h>

static uint32_t period_us(const avatar_clip_descriptor_t *clip)
{
    uint16_t fps = clip && clip->fps ? clip->fps : 10;
    if (fps < 4) fps = 4;
    if (fps > 15) fps = 15;
    return 1000000U / fps;
}

void avatar_loop_player_start(avatar_loop_player_t *player,
                              const avatar_clip_descriptor_t *clip, int64_t now_us)
{
    if (!player) return;
    memset(player, 0, sizeof(*player));
    player->direction = 1;
    player->next_due_us = now_us + period_us(clip);
}

uint16_t avatar_loop_player_next(avatar_loop_player_t *player,
                                 const avatar_clip_descriptor_t *clip)
{
    if (!player || !clip || clip->frame_count < 2) return 0;
    if (clip->mode == AVATAR_CLIP_ONCE_HOLD)
        return player->frame + 1 < clip->frame_count ? ++player->frame : player->frame;
    if (clip->mode == AVATAR_CLIP_PING_PONG) {
        int next = (int)player->frame + player->direction;
        if (next >= clip->frame_count) { player->direction = -1; next = clip->frame_count - 2; }
        if (next < 0) { player->direction = 1; next = 1; }
        player->frame = (uint16_t)next;
    } else {
        player->frame = (uint16_t)((player->frame + 1U) % clip->frame_count);
    }
    return player->frame;
}

bool avatar_loop_player_due(avatar_loop_player_t *player,
                            const avatar_clip_descriptor_t *clip, int64_t now_us)
{
    if (!player || now_us < player->next_due_us) return false;
    uint32_t period = period_us(clip);
    int64_t late = now_us - player->next_due_us;
    if (late >= period) player->dropped_frames += (uint32_t)(late / period);
    player->next_due_us = now_us + period; /* 跳帧而不积压。 */
    return true;
}
