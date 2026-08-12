#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    AVATAR_CLIP_LOOP = 0,
    AVATAR_CLIP_PING_PONG,
    AVATAR_CLIP_ONCE_HOLD,
} avatar_clip_play_mode_t;

typedef enum {
    AVATAR_TRANSITION_CROSS_FADE = 0,
} avatar_clip_transition_t;

typedef struct avatar_clip_descriptor {
    const char *name;
    const char *path;
    uint32_t compressed_crc32;
    uint16_t frame_count;
    uint16_t fps;
    uint16_t transition_ms;
    bool contains_blink;
    avatar_clip_play_mode_t mode;
    avatar_clip_transition_t transition;
    uint32_t frame_offsets[24];
    uint32_t frame_sizes[24];
    uint32_t decoded_crc32[24];
    const char *next_hints[3];
} avatar_clip_descriptor_t;

/* 安装首版占位 clip 到 SD，并验证文件大小与 CRC。 */
esp_err_t avatar_clip_map_init(void);
const avatar_clip_descriptor_t *avatar_clip_map_find(const char *name);
const avatar_clip_descriptor_t *avatar_clip_map_resolve(uint8_t state, uint8_t dialog_phase);

/* UI 状态入口：取消过期预测、显式请求目标，并保存新上下文。 */
void avatar_clip_map_set_state(uint8_t state);
void avatar_clip_map_set_dialog_phase(uint8_t phase);

/* 目标首帧上屏后注册并请求该映射项的 next_hint。 */
void avatar_clip_map_schedule_hints(const avatar_clip_descriptor_t *clip,
                                    uint8_t state, uint8_t phase);
void avatar_clip_map_get_context(uint8_t *state, uint8_t *phase);
void avatar_clip_map_set_loop_mode(bool enabled);
bool avatar_clip_map_loop_mode(void);
