#include "julia_soak_test.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "avatar_anim_engine.h"
#include "avatar_clip_cache.h"
#include "avatar_clip_preload.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_audio.h"
#include "julia_lipsync.h"
#include "julia_local_tts.h"
#include "julia_network.h"
#include "julia_sd.h"
#include "julia_ui.h"
#include "julia_voice.h"
#include "sdkconfig.h"

#define TAG "JULIA_SOAK"
#define REPORT_CSV "/sdcard/julia/soak.csv"
#define REPORT_TXT "/sdcard/julia/soak_report.txt"
#define SNAPSHOT_CAPACITY 256
#define STACK_TASK_CAPACITY 40

typedef struct {
    uint32_t second;
    size_t heap;
    size_t psram;
} memory_sample_t;

static const struct {
    const char *question;
    const char *answer;
} s_dialogs[] = {
    {"现在几点了？", "现在是自动烤机测试时间。"},
    {"一加一等于几？", "一加一等于二。"},
    {"讲一句短诗。", "微风经过窗边，星光落在桌面。"},
    {"提醒我喝水。", "好的，记得慢慢喝一杯水。"},
    {"今天适合散步吗？", "如果天气舒适，可以进行一次轻松散步。"},
    {"你现在在做什么？", "我正在进行稳定性测试，并持续检查自己的状态。"},
    {"说一个稍长的回答。", "长时间运行需要关注内存、任务栈、缓存命中率和异常恢复能力。"},
    {"网络断开怎么办？", "我会尝试本地语音，并在网络恢复后继续服务。"},
    {"你会累吗？", "我会通过健康指标判断系统是否需要恢复。"},
    {"测试思考超时。", "思考等待期间人物仍会保持连续动画。"},
    {"缓存有什么用？", "缓存减少存储读取，让动画切换更加稳定。"},
    {"最后一个测试问题。", "这一轮事件注入已经完整结束。"},
};

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_stop;
static julia_soak_config_t s_config;
static julia_soak_status_t s_status;
static memory_sample_t s_samples[SNAPSHOT_CAPACITY];
static uint32_t s_sample_count;
static size_t s_heap_start, s_psram_start, s_heap_min, s_psram_min;
static uint32_t s_crc_failures;
static uint32_t s_reset_events;
static uint32_t s_stack_low_events;
static uint32_t s_sd_faults, s_network_faults, s_tts_faults, s_epoch_storms;
static int64_t s_network_restore_ms;
static bool s_sd_fault_pending;
static uint64_t s_sd_failed_before;

static uint32_t random_between(uint32_t minimum, uint32_t maximum)
{
    if (maximum <= minimum) return minimum;
    return minimum + esp_random() % (maximum - minimum + 1);
}

static void report_line(const char *format, ...)
{
    if (!julia_sd_is_mounted() || !julia_sd_lock(pdMS_TO_TICKS(1000))) return;
    FILE *file = fopen(REPORT_CSV, "ab");
    if (file) {
        va_list args;
        va_start(args, format);
        vfprintf(file, format, args);
        va_end(args);
        fclose(file);
    }
    julia_sd_unlock();
}

static size_t collect_task_stacks(uint32_t elapsed)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    static TaskStatus_t tasks[STACK_TASK_CAPACITY];
    UBaseType_t count = uxTaskGetSystemState(tasks, STACK_TASK_CAPACITY, NULL);
    size_t minimum = SIZE_MAX;
    for (UBaseType_t i = 0; i < count; ++i) {
        size_t bytes = (size_t)tasks[i].usStackHighWaterMark * sizeof(StackType_t);
        if (bytes < minimum) minimum = bytes;
        report_line("STACK,%lu,%s,%u\n", (unsigned long)elapsed,
                    tasks[i].pcTaskName, (unsigned)bytes);
        if (bytes < 1024) {
            s_stack_low_events++;
            ESP_LOGW(TAG, "low stack task=%s free=%u", tasks[i].pcTaskName,
                     (unsigned)bytes);
        }
    }
    return minimum == SIZE_MAX ? 0 : minimum;
#else
    return 0;
#endif
}

static void take_snapshot(uint32_t elapsed)
{
    avatar_anim_metrics_t anim = {0};
    avatar_clip_cache_metrics_t cache = {0};
    avatar_clip_preload_metrics_t preload = {0};
    avatar_anim_engine_get_metrics(&anim);
    avatar_clip_cache_get_metrics(&cache);
    avatar_clip_preload_get_metrics(&preload);
    size_t heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (!s_heap_min || heap < s_heap_min) s_heap_min = heap;
    if (!s_psram_min || psram < s_psram_min) s_psram_min = psram;
    if (s_sample_count < SNAPSHOT_CAPACITY)
        s_samples[s_sample_count++] = (memory_sample_t){elapsed, heap, psram};
    size_t stack_min = collect_task_stacks(elapsed);
    double hit = cache.hits + cache.misses ?
        100.0 * cache.hits / (cache.hits + cache.misses) : 0.0;
    report_line("SNAP,%lu,%u,%u,%u,%u,%.2f,%u,%llu,%llu,%llu,%u,%u,%u,%u,%u\n",
                (unsigned long)elapsed, (unsigned)heap, (unsigned)psram,
                (unsigned)cache.cached_bytes, anim.decode_underruns, hit,
                cache.evictions, preload.requests, preload.completed, preload.cancelled,
                anim.clip_switches, (unsigned)stack_min, s_status.cloud_tts_count,
                s_status.local_tts_count, s_status.pcm_fallback_count);
    ESP_LOGI(TAG, "snapshot t=%lus heap=%u psram=%u cache=%u hit=%.1f%% underrun=%u stack_min=%u rounds=%u",
             (unsigned long)elapsed, (unsigned)heap, (unsigned)psram,
             (unsigned)cache.cached_bytes, hit, anim.decode_underruns,
             (unsigned)stack_min, s_status.dialog_rounds);
    s_status.snapshots++;
}

static esp_err_t play_synthetic_reply(const char *answer)
{
    int16_t pcm[640];
    size_t frames = 6 + strlen(answer) / 12;
    if (frames > 24) frames = 24;
    julia_lipsync_begin();
    for (size_t frame = 0; frame < frames; ++frame) {
        int amplitude = 500 + (int)(frame % 4) * 900;
        for (size_t i = 0; i < 640; ++i)
            pcm[i] = (int16_t)(amplitude * sinf(2.0f * (float)M_PI * 180.0f * i / 16000.0f));
        esp_err_t err = julia_lipsync_play(pcm, 640);
        if (err != ESP_OK) { julia_lipsync_end(); return err; }
    }
    return julia_lipsync_end();
}

static void inject_epoch_storm(void)
{
    for (unsigned i = 0; i < 12; ++i)
        julia_voice_inject_event((i & 1) ? JULIA_VOICE_INJECT_ASR_DONE :
                                 JULIA_VOICE_INJECT_TTS_READY, "epoch-storm");
    s_epoch_storms++;
    s_status.faults_injected++;
    s_status.faults_recovered++;
}

static void inject_random_state(void)
{
    static const fsm_event_t events[] = {
        EVT_USER_LEAVE, EVT_USER_RETURN, EVT_ROUTINE_BREAK, EVT_USER_REJECT,
        EVT_USER_PERFUNCTORY, EVT_USER_LEFT_DIALOG, EVT_SHARED_ACTIVITY_START,
        EVT_SHARED_ACTIVITY_STOP, EVT_BEDTIME, EVT_SILENCE_TIMEOUT,
    };
    julia_voice_handle_event(events[esp_random() % (sizeof(events) / sizeof(events[0]))]);
}

static void run_dialog_round(void)
{
    unsigned index = esp_random() % (sizeof(s_dialogs) / sizeof(s_dialogs[0]));
    const char *question = s_dialogs[index].question;
    const char *answer = s_dialogs[index].answer;
    julia_voice_inject_event(JULIA_VOICE_INJECT_WAKE, NULL);
    vTaskDelay(pdMS_TO_TICKS(300));
    julia_voice_inject_event(JULIA_VOICE_INJECT_ASR_DONE, question);
    uint32_t thinking_ms = (esp_random() % 10 == 0) ? 8500 : random_between(500, 2500);
    vTaskDelay(pdMS_TO_TICKS(thinking_ms));
    julia_voice_inject_event(JULIA_VOICE_INJECT_LLM_RESPONSE, answer);
    julia_voice_inject_event(JULIA_VOICE_INJECT_TTS_READY, answer);

    bool tts_timeout = (esp_random() % 100) < s_config.fault_percent;
    esp_err_t err;
    if (tts_timeout) {
        s_tts_faults++;
        s_status.faults_injected++;
        err = julia_local_tts_speak(answer);
        if (err == ESP_OK) {
            s_status.local_tts_count++;
            s_status.faults_recovered++;
        } else {
            julia_lipsync_begin();
            err = julia_lipsync_play_file("/sdcard/julia/netmsg.pcm");
            julia_lipsync_end();
            s_status.pcm_fallback_count++;
            if (err == ESP_OK) s_status.faults_recovered++;
        }
    } else {
        err = play_synthetic_reply(answer);
        s_status.cloud_tts_count++;
    }
    julia_voice_inject_event(JULIA_VOICE_INJECT_TTS_DONE, NULL);
    s_status.dialog_rounds++;
    ESP_LOGI(TAG, "round=%u corpus=%u result=%s think=%ums", s_status.dialog_rounds,
             index, esp_err_to_name(err), (unsigned)thinking_ms);
}

static void inject_faults(void)
{
    uint32_t roll = esp_random() % 100;
    if (roll < s_config.fault_percent && !s_sd_fault_pending) {
        avatar_clip_preload_metrics_t preload = {0};
        avatar_clip_preload_get_metrics(&preload);
        s_sd_failed_before = preload.failed;
        avatar_clip_preload_inject_read_failure();
        s_sd_fault_pending = true;
        s_sd_faults++; s_status.faults_injected++;
    } else if (roll < s_config.fault_percent * 2U && !s_network_restore_ms) {
        if (julia_wifi_disconnect() == ESP_OK) {
            s_network_restore_ms = esp_timer_get_time() / 1000 + 30000;
            s_network_faults++; s_status.faults_injected++;
        }
    } else if (roll < s_config.fault_percent * 3U) {
        inject_epoch_storm();
    }
}

static void check_fault_recovery(void)
{
    if (s_sd_fault_pending) {
        avatar_clip_preload_metrics_t preload = {0};
        avatar_clip_preload_get_metrics(&preload);
        if (preload.failed > s_sd_failed_before) {
            s_sd_fault_pending = false;
            s_status.faults_recovered++;
            ESP_LOGI(TAG, "SD fault recovery confirmed");
        }
    }
}

static void finish_report(uint32_t elapsed)
{
    if (s_network_restore_ms) {
        if (julia_wifi_connect(CONFIG_JULIA_WIFI_SSID, CONFIG_JULIA_WIFI_PASSWORD) == ESP_OK)
            s_status.faults_recovered++;
        s_network_restore_ms = 0;
    }
    check_fault_recovery();
    take_snapshot(elapsed);
    double heap_mean = 0, psram_mean = 0;
    unsigned stable_count = 0;
    for (uint32_t i = 0; i < s_sample_count; ++i) {
        if (s_samples[i].second >= 1800) {
            heap_mean += s_samples[i].heap;
            psram_mean += s_samples[i].psram;
            stable_count++;
        }
    }
    if (!stable_count && s_sample_count) {
        heap_mean = s_samples[s_sample_count - 1].heap;
        psram_mean = s_samples[s_sample_count - 1].psram;
        stable_count = 1;
    }
    heap_mean /= stable_count ? stable_count : 1;
    psram_mean /= stable_count ? stable_count : 1;
    size_t heap_end = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t psram_end = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    double heap_drop = heap_mean > 0 ? 100.0 * (heap_mean - heap_end) / heap_mean : 0;
    double psram_drop = psram_mean > 0 ? 100.0 * (psram_mean - psram_end) / psram_mean : 0;
    if (julia_sd_is_mounted() && julia_sd_lock(pdMS_TO_TICKS(1000))) {
        FILE *file = fopen(REPORT_TXT, "wb");
        if (file) {
            fprintf(file, "duration_s=%lu\nrounds=%lu\nheap_start=%u\nheap_end=%u\nheap_min=%u\n",
                    (unsigned long)elapsed, (unsigned long)s_status.dialog_rounds,
                    (unsigned)s_heap_start, (unsigned)heap_end, (unsigned)s_heap_min);
            fprintf(file, "psram_start=%u\npsram_end=%u\npsram_min=%u\nheap_drop_pct=%.3f\npsram_drop_pct=%.3f\n",
                    (unsigned)s_psram_start, (unsigned)psram_end, (unsigned)s_psram_min,
                    heap_drop, psram_drop);
            fprintf(file, "faults=%lu recovered=%lu sd=%lu network=%lu tts=%lu epoch=%lu stack_low=%lu\n",
                    (unsigned long)s_status.faults_injected,
                    (unsigned long)s_status.faults_recovered,
                    (unsigned long)s_sd_faults, (unsigned long)s_network_faults,
                    (unsigned long)s_tts_faults, (unsigned long)s_epoch_storms,
                    (unsigned long)s_stack_low_events);
            fclose(file);
        }
        julia_sd_unlock();
    }
    ESP_LOGI(TAG, "REPORT duration=%lus rounds=%u heap=%u->%u min=%u drop=%.2f%% psram=%u->%u min=%u drop=%.2f%% faults=%u/%u stack_low=%u",
             (unsigned long)elapsed, s_status.dialog_rounds, (unsigned)s_heap_start,
             (unsigned)heap_end, (unsigned)s_heap_min, heap_drop, (unsigned)s_psram_start,
             (unsigned)psram_end, (unsigned)s_psram_min, psram_drop,
             s_status.faults_recovered, s_status.faults_injected, s_stack_low_events);
}

static void soak_task(void *argument)
{
    (void)argument;
    int64_t started = esp_timer_get_time() / 1000;
    uint32_t next_snapshot = 60;
    uint32_t next_dialog = random_between(s_config.min_interval_seconds,
                                          s_config.max_interval_seconds);
    julia_audio_set_muted(s_config.silent);
    take_snapshot(0);
    while (!s_stop) {
        uint32_t elapsed = (uint32_t)((esp_timer_get_time() / 1000 - started) / 1000);
        s_status.elapsed_seconds = elapsed;
        if (elapsed >= s_config.duration_seconds) break;
        if (elapsed >= next_snapshot) { take_snapshot(elapsed); next_snapshot += 60; }
        if (s_network_restore_ms && esp_timer_get_time() / 1000 >= s_network_restore_ms) {
            esp_err_t err = julia_wifi_connect(CONFIG_JULIA_WIFI_SSID, CONFIG_JULIA_WIFI_PASSWORD);
            if (err == ESP_OK) {
                s_status.faults_recovered++;
                s_network_restore_ms = 0;
            } else {
                s_network_restore_ms = esp_timer_get_time() / 1000 + 10000;
                ESP_LOGW(TAG, "network recovery retry: %s", esp_err_to_name(err));
            }
        }
        check_fault_recovery();
        if (elapsed >= next_dialog) {
            run_dialog_round();
            inject_faults();
            if ((esp_random() % 100) < s_config.state_jump_percent) inject_random_state();
            next_dialog = elapsed + random_between(s_config.min_interval_seconds,
                                                   s_config.max_interval_seconds);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    uint32_t elapsed = (uint32_t)((esp_timer_get_time() / 1000 - started) / 1000);
    finish_report(elapsed);
    julia_audio_set_muted(false);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.running = false;
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

esp_err_t julia_soak_test_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t julia_soak_test_start(const julia_soak_config_t *config)
{
    if (!config || !config->duration_seconds || !config->min_interval_seconds ||
        config->max_interval_seconds < config->min_interval_seconds || !s_lock)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.running) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    s_config = *config;
    memset(&s_status, 0, sizeof(s_status));
    s_status.running = true;
    s_stop = false;
    s_sample_count = 0;
    s_heap_start = s_heap_min = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_psram_start = s_psram_min = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_crc_failures = s_reset_events = s_stack_low_events = 0;
    s_sd_faults = s_network_faults = s_tts_faults = s_epoch_storms = 0;
    s_network_restore_ms = 0;
    s_sd_fault_pending = false;
    if (julia_sd_is_mounted() && julia_sd_lock(pdMS_TO_TICKS(1000))) {
        mkdir("/sdcard/julia", 0775);
        remove(REPORT_CSV);
        FILE *file = fopen(REPORT_CSV, "wb");
        if (file) { fputs("type,seconds,values...\n", file); fclose(file); }
        julia_sd_unlock();
    }
    BaseType_t created = xTaskCreateWithCaps(soak_task, "julia_soak", 12288, NULL, 2,
                                             &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) { s_status.running = false; xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void julia_soak_test_stop(void) { s_stop = true; }

void julia_soak_test_get_status(julia_soak_status_t *status)
{
    if (status) *status = s_status;
}

static esp_err_t copy_report_file(const char *source, const char *target, const char *reason)
{
    FILE *in = fopen(source, "rb");
    FILE *out = fopen(target, "wb");
    if (!out) { if (in) fclose(in); return ESP_FAIL; }
    fprintf(out, "termination=%s\nverdict=NOT_EVALUATED\n", reason);
    uint8_t block[1024];
    while (in) {
        size_t got = fread(block, 1, sizeof(block), in);
        if (!got) break;
        if (fwrite(block, 1, got, out) != got) { fclose(in); fclose(out); return ESP_FAIL; }
    }
    if (in) fclose(in);
    /* 部分 FAT 卡固件不实现 fsync；fclose 仍会提交缓存，归档不应因此误报失败。 */
    esp_err_t err = fflush(out) == 0 ? ESP_OK : ESP_FAIL;
    fclose(out);
    return err;
}

esp_err_t julia_soak_test_archive(const char *reason, char *path, size_t path_size)
{
    if (!reason || !path || path_size < 32 || !julia_sd_is_mounted()) return ESP_ERR_INVALID_ARG;
    if (!julia_sd_lock(pdMS_TO_TICKS(3000))) return ESP_ERR_TIMEOUT;
    mkdir("/sdcard/julia/reports", 0775);
    time_t now = time(NULL);
    struct tm local = {0};
    localtime_r(&now, &local);
    /* 此 BSP 的 FAT 配置使用 8.3 文件名。时间戳采用 MMDDhhmm，完整年份和
     * 秒数写入文件内容，避免长文件名创建失败。 */
    char base[96];
    snprintf(base, sizeof(base), "/sdcard/julia/reports/%02d%02d%02d%02d",
             local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
    char txt[112], csv[112];
    snprintf(txt, sizeof(txt), "%s.txt", base);
    snprintf(csv, sizeof(csv), "%s.csv", base);
    esp_err_t txt_err = copy_report_file(REPORT_TXT, txt, reason);
    esp_err_t csv_err = copy_report_file(REPORT_CSV, csv, reason);
    julia_sd_unlock();
    if (txt_err != ESP_OK || csv_err != ESP_OK) return ESP_FAIL;
    snprintf(path, path_size, "%s", base);
    ESP_LOGW(TAG, "archived interrupted soak: %s reason=%s", base, reason);
    return ESP_OK;
}
