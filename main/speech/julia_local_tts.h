#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#define JULIA_LOCAL_TTS_VOICE_PATH "/sdcard/julia/tts/XIAOLE.DAT"
#define JULIA_LOCAL_TTS_VOICE_BYTES 2938039U
#define JULIA_LOCAL_TTS_VOICE_CRC32 0xa085d260U
#define JULIA_LOCAL_TTS_MAX_TEXT_BYTES 768U

typedef enum {
    JULIA_LOCAL_TTS_UNINITIALIZED = 0,
    JULIA_LOCAL_TTS_LOADING,
    JULIA_LOCAL_TTS_READY,
    JULIA_LOCAL_TTS_UNAVAILABLE,
} julia_local_tts_state_t;

typedef struct {
    julia_local_tts_state_t state;
    esp_err_t last_error;
    size_t voice_bytes;
    size_t free_heap;
    size_t free_psram;
    uint32_t init_attempts;
    uint32_t synthesis_ok;
    uint32_t synthesis_failed;
} julia_local_tts_status_t;

/* 首次使用时从 SD 卡完整校验并加载 voice data，失败后保持安全不可用状态。 */
esp_err_t julia_local_tts_init(void);
bool julia_local_tts_is_ready(void);
void julia_local_tts_get_status(julia_local_tts_status_t *status);

/* 同步合成并经 lipsync 播放。调用方应在独立语音任务中使用。 */
esp_err_t julia_local_tts_speak(const char *text);

/* 维护接口：从串口流原子安装资源，及重命名资源用于故障注入/恢复。 */
esp_err_t julia_local_tts_install_stream(FILE *input, size_t size, uint32_t expected_crc);
esp_err_t julia_local_tts_set_resource_enabled(bool enabled);
