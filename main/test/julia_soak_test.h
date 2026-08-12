#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t duration_seconds;
    uint32_t min_interval_seconds;
    uint32_t max_interval_seconds;
    uint8_t state_jump_percent;
    uint8_t fault_percent;
    bool silent;
} julia_soak_config_t;

typedef struct {
    bool running;
    uint32_t elapsed_seconds;
    uint32_t dialog_rounds;
    uint32_t snapshots;
    uint32_t faults_injected;
    uint32_t faults_recovered;
    uint32_t cloud_tts_count;
    uint32_t local_tts_count;
    uint32_t pcm_fallback_count;
} julia_soak_status_t;

esp_err_t julia_soak_test_init(void);
esp_err_t julia_soak_test_start(const julia_soak_config_t *config);
void julia_soak_test_stop(void);
void julia_soak_test_get_status(julia_soak_status_t *status);
/* 将最近一次报告归档。reason 会写入报告，归档不作 PASS/FAIL 判定。 */
esp_err_t julia_soak_test_archive(const char *reason, char *path, size_t path_size);
