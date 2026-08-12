#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AVATAR_CLIP_CACHE_SOFT_LIMIT (5U * 1024U * 1024U)
#define AVATAR_PSRAM_SAFE_WATERMARK  (1200U * 1024U)

typedef enum {
    AVATAR_CLIP_SOURCE_PSRAM = 0,
    AVATAR_CLIP_SOURCE_FLASH,
} avatar_clip_source_t;

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint32_t evictions;
    size_t cached_bytes;
    size_t soft_limit_bytes;
    size_t psram_min_free;
    bool preload_paused;
} avatar_clip_cache_metrics_t;

/* 初始化固定元数据表，不分配片段内容。 */
esp_err_t avatar_clip_cache_init(void);

/* 注册固件内嵌片段。Flash 片段永久驻留，不计入 LRU 和 PSRAM 预算。 */
esp_err_t avatar_clip_cache_register_flash(const char *name, const uint8_t *data, size_t size);

/* 将完整压缩片段一次性发布到 PSRAM。失败时不会留下半个片段。 */
esp_err_t avatar_clip_cache_put(const char *name, const uint8_t *data, size_t size);

/* 为异步预载申请临时完整片段缓冲；发布前对解码侧不可见。 */
esp_err_t avatar_clip_cache_alloc_staging(size_t size, uint8_t **buffer);

/* CRC 校验完成后原子接管 staging 缓冲。成功后缓存拥有 buffer。 */
esp_err_t avatar_clip_cache_publish(const char *name, uint8_t *buffer, size_t size);

/* acquire/release 形成解码期间的所有权保护；引用中的片段不可淘汰。 */
esp_err_t avatar_clip_cache_acquire(const char *name, const uint8_t **data, size_t *size,
                                    avatar_clip_source_t *source);
bool avatar_clip_cache_contains(const char *name);
void avatar_clip_cache_release(const char *name);

/* 当前片段和下一目标片段不可淘汰；传 NULL 可清除对应保护。 */
void avatar_clip_cache_set_current(const char *name);
void avatar_clip_cache_set_next(const char *name);

/* 主动执行安全水位维护，供后续预载任务在每次读卡前调用。 */
void avatar_clip_cache_maintain_watermark(void);
bool avatar_clip_cache_preload_allowed(void);
void avatar_clip_cache_get_metrics(avatar_clip_cache_metrics_t *metrics);
