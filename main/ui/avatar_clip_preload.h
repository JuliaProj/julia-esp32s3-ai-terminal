#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    AVATAR_PRELOAD_BACKGROUND = 0,
    AVATAR_PRELOAD_PREDICTED = 1,
    AVATAR_PRELOAD_EXPLICIT = 2,
} avatar_preload_priority_t;

typedef struct {
    uint64_t requests;
    uint64_t completed;
    uint64_t cancelled;
    uint64_t failed;
    uint64_t cache_hits;
    uint64_t sd_bytes;
    uint64_t total_us;
    uint32_t pause_events;
    uint32_t resume_events;
    uint32_t pause_max_ms;
} avatar_clip_preload_metrics_t;

esp_err_t avatar_clip_preload_init(void);

/* path 必须是显式文件路径；本阶段不扫描目录。expected_crc32 为压缩文件 CRC。 */
esp_err_t avatar_clip_preload_request(const char *name, const char *path,
                                      uint32_t expected_crc32,
                                      avatar_preload_priority_t priority);

/* Task 4 加载清单后用此接口注册状态/阶段的 next_hint。 */
esp_err_t avatar_clip_preload_register_hint(uint8_t state, uint8_t phase,
                                            const char *name, const char *path,
                                            uint32_t expected_crc32);
void avatar_clip_preload_on_state(uint8_t state);
void avatar_clip_preload_on_dialog_phase(uint8_t phase);

void avatar_clip_preload_get_metrics(avatar_clip_preload_metrics_t *metrics);
bool avatar_clip_preload_is_pending(const char *name);
void avatar_clip_preload_cancel_except(const char *keep_name);

/* 维护命令专用：异步执行 8MiB 缓存颠簸与 CRC 错误注入测试。 */
esp_err_t avatar_clip_preload_run_churn_test(void);
void avatar_clip_preload_inject_read_failure(void);
