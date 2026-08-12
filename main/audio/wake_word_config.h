#pragma once

#include <stddef.h>

/* Default production WakeNet model packed into the ESP-SR "model" partition. */
#define WAKE_WORD_MODEL_NAME "wn9_nihaoxiaozhi_tts"
#define WAKE_WORD_DISPLAY_TEXT "你好小智"

/* Raw PCM format shared by SD and embedded replies: 16 kHz, signed LE16, mono. */
#define WAKE_REPLY_SAMPLE_RATE 16000U
#define WAKE_REPLY_SD_DIRECTORY "/sdcard/julia/replies"
#define WAKE_REPLY_REPEAT_WINDOW_S 60U
#define WAKE_REPLY_RECENT_HISTORY 5U

/* Keep all spoken copy here. The generator parses this catalog and emits PCM
 * plus size/duration metadata; changing reply copy never touches voice logic. */
#define WAKE_REPLY_CATALOG(X) \
    X(COMMON_01, COMMON, "我在",                  "reply_common_01.pcm", 1) \
    X(COMMON_02, COMMON, "嗯？",                  "reply_common_02.pcm", 1) \
    X(COMMON_03, COMMON, "说吧",                  "reply_common_03.pcm", 1) \
    X(COMMON_04, COMMON, "怎么啦",                "reply_common_04.pcm", 1) \
    X(COMMON_05, COMMON, "听着呢",                "reply_common_05.pcm", 0) \
    X(COMMON_06, COMMON, "来啦",                  "reply_common_06.pcm", 0) \
    X(COMMON_07, COMMON, "我在呢",                "reply_common_07.pcm", 0) \
    X(COMMON_08, COMMON, "你说",                  "reply_common_08.pcm", 0) \
    X(COMMON_09, COMMON, "怎么啦我在",            "reply_common_09.pcm", 0) \
    X(COMMON_10, COMMON, "嗯我在听",              "reply_common_10.pcm", 0) \
    X(COMMON_11, COMMON, "在呢",                  "reply_common_11.pcm", 0) \
    X(COMMON_12, COMMON, "我听着",                "reply_common_12.pcm", 0) \
    X(COMMON_13, COMMON, "有什么事呀",            "reply_common_13.pcm", 0) \
    X(MORNING_01, MORNING, "早呀",                "reply_morning_01.pcm", 0) \
    X(MORNING_02, MORNING, "醒啦",                "reply_morning_02.pcm", 0) \
    X(MORNING_03, MORNING, "早上好呀",            "reply_morning_03.pcm", 0) \
    X(MORNING_04, MORNING, "早，我在呢",          "reply_morning_04.pcm", 0) \
    X(LATE_NIGHT_01, LATE_NIGHT, "还没睡呀",      "reply_late_night_01.pcm", 0) \
    X(LATE_NIGHT_02, LATE_NIGHT, "这么晚了，我在", "reply_late_night_02.pcm", 0) \
    X(LATE_NIGHT_03, LATE_NIGHT, "夜深啦，我在呢", "reply_late_night_03.pcm", 0) \
    X(REPEAT_01, REPEAT, "又来啦",                "reply_repeat_01.pcm", 0) \
    X(REPEAT_02, REPEAT, "我在我在",              "reply_repeat_02.pcm", 0) \
    X(REPEAT_03, REPEAT, "听到了，我在",          "reply_repeat_03.pcm", 0)

/* Optional network refresh remains available, but demonstration builds use
 * the deterministic offline library by default. */
#ifndef WAKE_REPLY_ENABLE_ONLINE_CACHE
#define WAKE_REPLY_ENABLE_ONLINE_CACHE 0
#endif

/* Future custom "你好 Julia" integration:
 * 1. Train/export the WakeNet model with Espressif's TTS Pipeline.
 * 2. Add the generated model blob and metadata to the ESP-SR model pack, or
 *    expose linker symbols for an embedded array in a custom loader adapter.
 * 3. Set CUSTOM_WAKENET_MODEL=1 and CUSTOM_WAKENET_MODEL_NAME to the exact
 *    registered ESP_WN_PREFIX name. The current ESP-SR API loads registered
 *    models from CUSTOM_WAKENET_MODEL_PARTITION; raw arrays require that
 *    adapter to register the array before esp_srmodel_filter() is called.
 * 4. Rebuild srmodels.bin and verify heap/PSRAM plus false-trigger rate.
 */
#ifndef CUSTOM_WAKENET_MODEL
#define CUSTOM_WAKENET_MODEL 0
#endif
#ifndef CUSTOM_WAKENET_MODEL_NAME
#define CUSTOM_WAKENET_MODEL_NAME "wn9_nihaojulia"
#endif
#ifndef CUSTOM_WAKENET_MODEL_PARTITION
#define CUSTOM_WAKENET_MODEL_PARTITION "model"
#endif

#if CUSTOM_WAKENET_MODEL
#define ACTIVE_WAKENET_MODEL_NAME CUSTOM_WAKENET_MODEL_NAME
#define ACTIVE_WAKENET_MODEL_PARTITION CUSTOM_WAKENET_MODEL_PARTITION
#else
#define ACTIVE_WAKENET_MODEL_NAME WAKE_WORD_MODEL_NAME
#define ACTIVE_WAKENET_MODEL_PARTITION "model"
#endif
