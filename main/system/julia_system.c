#include "julia_system.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_flash_encrypt.h"
#include "esp_heap_caps.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_vfs_fat.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_network.h"
#include "julia_fsm.h"
#include "julia_local_tts.h"
#include "julia_lipsync.h"
#include "julia_ui.h"
#include "julia_ui_showcase.h"
#include "julia_display_theme.h"
#include "julia_memory.h"
#include "avatar_clip_preload.h"
#include "avatar_clip_map.h"
#include "idle_player.h"
#include "transition_player.h"
#include "avatar_face.h"
#include "julia_backlight.h"
#include "esp_crc.h"
#include "julia_sd.h"
#include "wake_reply.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#if CONFIG_JULIA_SOAK_TEST
#include "julia_soak_test.h"
#endif

#define TAG "JULIA_SYSTEM"
#define CONFIG_NAMESPACE "julia_cfg"
#define DIAGNOSTIC_PATH "/sdcard/julia/diag.log"
#define DIAGNOSTIC_OLD_PATH "/sdcard/julia/diag.old"
#define MAX_DIAGNOSTIC_BYTES (256 * 1024)

static nvs_handle_t s_config_nvs;
static int64_t s_started_ms;
static volatile bool s_ota_busy;

#define CLIP_UPLOAD_BLOCK 4096U
#define CLIP_UPLOAD_BEGIN_BYTES 20U
#define CLIP_IO_RETRIES 3U

static bool ensure_parent_directories(const char *path)
{
    char directory[128];
    size_t length = strlen(path);
    if (!length || length >= sizeof(directory)) return false;
    memcpy(directory, path, length + 1);
    for (char *p = directory + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(directory, 0775) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return true;
}

static FILE *open_clip_temp_with_retry(const char *path)
{
    for (unsigned attempt = 1; attempt <= CLIP_IO_RETRIES; ++attempt) {
        if (ensure_parent_directories(path)) {
            FILE *file = fopen(path, "wb");
            if (file) return file;
        }
        ESP_LOGW(TAG, "clip open retry=%u/%u path=%s errno=%d", attempt,
                 CLIP_IO_RETRIES, path, errno);
        vTaskDelay(pdMS_TO_TICKS(100U * attempt));
    }
    return NULL;
}

static bool write_clip_block_with_retry(FILE *file, const uint8_t *data, size_t length,
                                        long offset)
{
    for (unsigned attempt = 1; attempt <= CLIP_IO_RETRIES; ++attempt) {
        clearerr(file);
        if (fseek(file, offset, SEEK_SET) == 0 && fwrite(data, 1, length, file) == length)
            return true;
        ESP_LOGW(TAG, "clip write retry=%u/%u offset=%ld bytes=%u errno=%d", attempt,
                 CLIP_IO_RETRIES, offset, (unsigned)length, errno);
        vTaskDelay(pdMS_TO_TICKS(100U * attempt));
    }
    return false;
}

static uint16_t read_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool usb_read_exact(uint8_t *output, size_t bytes, uint32_t timeout_ms, bool debug)
{
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (received < bytes && esp_timer_get_time() < deadline) {
        int part = usb_serial_jtag_read_bytes(output + received, bytes - received,
                                              pdMS_TO_TICKS(100));
        if (part > 0) {
            if (debug) {
                char hex[65]; size_t shown = (size_t)part > 32 ? 32 : (size_t)part;
                for (size_t i = 0; i < shown; ++i) snprintf(hex + i * 2, 3, "%02x", output[received + i]);
                hex[shown * 2] = 0;
                ESP_LOGI(TAG, "clip rx bytes=%d first=%s", part, hex);
            }
            received += (size_t)part;
            deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        }
    }
    return received == bytes;
}

static esp_err_t receive_clip(const avatar_clip_descriptor_t *clip, size_t *actual_bytes,
                              bool debug, size_t required_size)
{
    uint8_t begin[CLIP_UPLOAD_BEGIN_BYTES];
    uint8_t header[4];
    uint8_t crc_bytes[4];
    uint8_t *block = heap_caps_malloc(CLIP_UPLOAD_BLOCK,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!clip || !actual_bytes || !block) { heap_caps_free(block); return ESP_ERR_NO_MEM; }
    *actual_bytes = 0;
    if (!usb_read_exact(begin, sizeof(begin), 10000, debug) ||
        memcmp(begin, "CLIP_BEGIN", 10) != 0) {
        printf("CLIP_BEGIN ERR protocol\n"); fflush(stdout);
        heap_caps_free(block); return ESP_ERR_TIMEOUT;
    }
    size_t total_size = read_le32(begin + 10);
    uint16_t total_blocks = read_le16(begin + 14);
    uint32_t expected_crc = read_le32(begin + 16);
    uint16_t expected_blocks = (uint16_t)((total_size + CLIP_UPLOAD_BLOCK - 1) /
                                          CLIP_UPLOAD_BLOCK);
    uint64_t total_bytes = 0, free_bytes = 0;
    bool space_ok = esp_vfs_fat_info(JULIA_SD_MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK &&
                    free_bytes >= total_size + CLIP_UPLOAD_BLOCK;
    if (!total_size || total_size > 6U * 1024U * 1024U ||
        (required_size && total_size != required_size) ||
        total_blocks != expected_blocks || expected_crc != clip->compressed_crc32 || !space_ok) {
        printf("CLIP_BEGIN ERR size_or_crc_or_space\n"); fflush(stdout);
        heap_caps_free(block); return ESP_ERR_INVALID_ARG;
    }
    if (!julia_sd_lock(pdMS_TO_TICKS(3000))) {
        printf("CLIP_BEGIN ERR sd_busy\n"); fflush(stdout);
        heap_caps_free(block); return ESP_ERR_TIMEOUT;
    }
    char temporary[128]; snprintf(temporary, sizeof(temporary), "%s.tmp", clip->path);
    FILE *file = open_clip_temp_with_retry(temporary);
    if (!file) {
        int open_errno = errno;
        julia_sd_unlock(); heap_caps_free(block);
        printf("CLIP_BEGIN ERR open path=%s errno=%d\n", temporary, open_errno);
        fflush(stdout); return ESP_FAIL;
    }
    printf("CLIP_BEGIN OK blocks=%u\n", total_blocks); fflush(stdout);
    uint32_t total_crc = 0;
    esp_err_t result = ESP_OK;
    for (uint16_t sequence = 0; sequence < total_blocks && result == ESP_OK; ++sequence) {
        unsigned failures = 0;
        while (true) {
            if (!usb_read_exact(header, sizeof(header), 10000, debug)) { result = ESP_ERR_TIMEOUT; break; }
            uint16_t incoming_sequence = read_le16(header);
            uint16_t length = read_le16(header + 2);
            size_t remaining = total_size - *actual_bytes;
            size_t expected_length = remaining > CLIP_UPLOAD_BLOCK ? CLIP_UPLOAD_BLOCK : remaining;
            if (!length || length > CLIP_UPLOAD_BLOCK ||
                !usb_read_exact(block, length, 10000, debug) ||
                !usb_read_exact(crc_bytes, sizeof(crc_bytes), 10000, debug)) {
                result = ESP_ERR_TIMEOUT; break;
            }
            uint32_t actual_crc = esp_crc32_le(0, block, length);
            uint32_t incoming_crc = read_le32(crc_bytes);
            if (incoming_sequence != sequence || length != expected_length ||
                incoming_crc != actual_crc) {
                printf("NAK %u\n", sequence); fflush(stdout);
                if (++failures >= 3) { result = ESP_ERR_INVALID_CRC; break; }
                continue;
            }
            if (!write_clip_block_with_retry(file, block, length, (long)*actual_bytes)) {
                result = ESP_FAIL; break;
            }
            total_crc = esp_crc32_le(total_crc, block, length);
            *actual_bytes += length;
            printf("ACK %u\n", sequence); fflush(stdout);
            break;
        }
    }
    if (fflush(file) != 0) result = ESP_FAIL;
    fclose(file);
    if (result == ESP_OK && (*actual_bytes != total_size || total_crc != expected_crc))
        result = ESP_ERR_INVALID_CRC;
    if (result == ESP_OK) {
        remove(clip->path);
        if (rename(temporary, clip->path) != 0) result = ESP_FAIL;
    }
    if (result != ESP_OK) remove(temporary);
    julia_sd_unlock();
    heap_caps_free(block);
    return result;
}

static bool valid_key(const char *key)
{
    if (!key || !key[0] || strlen(key) > 15) return false;
    for (const char *p = key; *p; ++p)
        if (!(('a' <= *p && *p <= 'z') || ('0' <= *p && *p <= '9') || *p == '_')) return false;
    return true;
}

esp_err_t julia_system_get_status(julia_system_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(status->firmware_version,
            CONFIG_JULIA_FIRMWARE_VERSION[0] ? CONFIG_JULIA_FIRMWARE_VERSION : app->version,
            sizeof(status->firmware_version));
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) strlcpy(status->partition_label, running->label, sizeof(status->partition_label));
    status->free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    status->minimum_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    status->wifi_rssi = julia_wifi_get_rssi();
    status->reset_reason = esp_reset_reason();
    status->uptime_seconds = (unsigned)((esp_timer_get_time() / 1000 - s_started_ms) / 1000);
    return ESP_OK;
}

static void rotate_diagnostic_log(FILE **file)
{
    if (fseek(*file, 0, SEEK_END) != 0 || ftell(*file) < MAX_DIAGNOSTIC_BYTES) return;
    fclose(*file); *file = NULL;
    remove(DIAGNOSTIC_OLD_PATH);
    rename(DIAGNOSTIC_PATH, DIAGNOSTIC_OLD_PATH);
    *file = fopen(DIAGNOSTIC_PATH, "ab");
}

static void diagnostic_task(void *arg)
{
    (void)arg;
    while (true) {
        julia_system_status_t status;
        julia_system_get_status(&status);
        ESP_LOGI(TAG, "diag: version=%s part=%s uptime=%us heap=%u min=%u rssi=%d reset=%d",
                 status.firmware_version, status.partition_label, status.uptime_seconds,
                 (unsigned)status.free_heap, (unsigned)status.minimum_free_heap,
                 status.wifi_rssi, status.reset_reason);
        if (julia_sd_lock(pdMS_TO_TICKS(1000))) {
            FILE *file = fopen(DIAGNOSTIC_PATH, "ab+");
            if (file) {
                rotate_diagnostic_log(&file);
                if (file) {
                    fprintf(file, "%lld,%s,%s,%u,%u,%u,%d,%d\n", (long long)time(NULL),
                            status.firmware_version, status.partition_label, status.uptime_seconds,
                            (unsigned)status.free_heap, (unsigned)status.minimum_free_heap,
                            status.wifi_rssi, status.reset_reason);
                    fclose(file);
                }
            }
            julia_sd_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_DIAGNOSTIC_PERIOD_SECONDS * 1000));
    }
}

esp_err_t julia_system_config_set(const char *key, const char *value, bool secret)
{
    if (!valid_key(key) || !value || !s_config_nvs) return ESP_ERR_INVALID_ARG;
    if (secret && !esp_flash_encryption_enabled()) {
        ESP_LOGE(TAG, "Refusing to persist secret '%s': flash encryption is disabled", key);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_ERROR(nvs_set_str(s_config_nvs, key, value), TAG, "set config");
    return nvs_commit(s_config_nvs);
}

esp_err_t julia_system_config_get(const char *key, char *value, size_t value_size)
{
    if (!valid_key(key) || !value || !value_size || !s_config_nvs) return ESP_ERR_INVALID_ARG;
    size_t required = value_size;
    return nvs_get_str(s_config_nvs, key, value, &required);
}

esp_err_t julia_system_ota_update(const char *https_url)
{
    if (!https_url || strncmp(https_url, "https://", 8) != 0) return ESP_ERR_INVALID_ARG;
    if (s_ota_busy) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(julia_wifi_wait_cloud_ready(10000), TAG, "network not ready");
    s_ota_busy = true;
    ESP_LOGI(TAG, "OTA started");
    esp_http_client_config_t http = {
        .url = https_url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota = {.http_config = &http};
    esp_err_t err = esp_https_ota(&ota);
    s_ota_busy = false;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA verified; rebooting into the new slot");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    return err;
}

static void print_status(void)
{
    julia_system_status_t s;
    if (julia_system_get_status(&s) == ESP_OK)
        printf("STATUS version=%s partition=%s uptime=%us heap=%u min_heap=%u psram=%u rssi=%d reset=%d\n",
               s.firmware_version, s.partition_label, s.uptime_seconds,
               (unsigned)s.free_heap, (unsigned)s.minimum_free_heap,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               s.wifi_rssi, s.reset_reason);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[384];
    printf("Julia maintenance commands: status | state <0-19> | showcase <start|status> | speak <rms> | mouth <0-3> | button <down|up> | demo <on|off|status> | doze <enter|exit|status> | backlight breathe <min> <max> <period> <segments> | backlight gamma <on|off> | backlight test <min> <max> <period> | swap test | voice-test | reply-test | preload-test | memory-test | clip-mode <loop|program> | clip-upload <name> | transition upload <route> <file> <bytes> <crc32> | idle upload <file> <bytes> <crc32> | theme <next|name|status> | screen-timeout <seconds> | screen-wake | soak <start|stop|status|archive> | ota [https-url]\n");
    while (true) {
        errno = 0;
        if (!fgets(line, sizeof(line), stdin)) {
            // The ESP-IDF console may report EAGAIN before the first UART byte arrives.
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, "status") == 0) {
            print_status();
        } else if (strcmp(line, "voice-test") == 0) {
            julia_lipsync_begin();
            esp_err_t voice_test_err = julia_lipsync_play_file("/sdcard/julia/netmsg.pcm");
            esp_err_t voice_stop_err = julia_lipsync_end();
            if (voice_test_err == ESP_OK) voice_test_err = voice_stop_err;
            printf("Voice test: %s\n", esp_err_to_name(voice_test_err));
        } else if (strcmp(line, "reply-test") == 0) {
            printf("REPLY_TEST result=%s\n", esp_err_to_name(wake_reply_play()));
        } else if (strncmp(line, "reply-upload ", 13) == 0) {
            char filename[48] = {0};
            unsigned long bytes = 0, crc = 0;
            if (sscanf(line + 13, "%47s %lu %lx", filename, &bytes, &crc) != 3 ||
                !wake_reply_is_known_asset(filename, bytes)) {
                printf("Usage: reply-upload <file> <bytes> <crc32>\n");
            } else {
                printf("REPLY_UPLOAD_READY file=%s bytes=%lu\n", filename, bytes);
                fflush(stdout);
                char path[96];
                mkdir("/sdcard/julia/replies", 0775);
                snprintf(path, sizeof(path), "/sdcard/julia/replies/%s", filename);
                avatar_clip_descriptor_t reply = {
                    .name = filename, .path = path, .compressed_crc32 = (uint32_t)crc,
                };
                size_t actual = 0;
                esp_err_t upload_err = receive_clip(&reply, &actual, false, bytes);
                printf("REPLY_UPLOAD file=%s bytes=%u result=%s\n", filename,
                       (unsigned)actual, esp_err_to_name(upload_err));
            }
        } else if (strcmp(line, "tts-status") == 0) {
            julia_local_tts_status_t tts;
            julia_local_tts_get_status(&tts);
            printf("TTS_STATUS state=%d error=%s voice=%u heap=%u psram=%u attempts=%lu ok=%lu failed=%lu\n",
                   tts.state, esp_err_to_name(tts.last_error), (unsigned)tts.voice_bytes,
                   (unsigned)tts.free_heap, (unsigned)tts.free_psram,
                   (unsigned long)tts.init_attempts, (unsigned long)tts.synthesis_ok,
                   (unsigned long)tts.synthesis_failed);
        } else if (strcmp(line, "tts-test") == 0) {
            static const char *sentences[] = {
                "你好，我是 Julia。",
                "本地语音合成已经准备好了。",
                "即使网络暂时不可用，我也可以继续回应你。",
            };
            for (size_t i = 0; i < sizeof(sentences) / sizeof(sentences[0]); ++i) {
                esp_err_t test_err = julia_local_tts_speak(sentences[i]);
                printf("TTS_TEST index=%u result=%s\n", (unsigned)i + 1,
                       esp_err_to_name(test_err));
                if (test_err != ESP_OK) break;
            }
        } else if (strncmp(line, "tts-upload ", 11) == 0) {
            char *end = NULL;
            unsigned long bytes = strtoul(line + 11, &end, 10);
            while (*end == ' ') ++end;
            char *crc_end = NULL;
            unsigned long crc = strtoul(end, &crc_end, 16);
            if (*crc_end || bytes != JULIA_LOCAL_TTS_VOICE_BYTES ||
                crc != JULIA_LOCAL_TTS_VOICE_CRC32) {
                printf("Usage: tts-upload %u %08x\n", JULIA_LOCAL_TTS_VOICE_BYTES,
                       JULIA_LOCAL_TTS_VOICE_CRC32);
            } else {
                printf("TTS_UPLOAD_READY bytes=%lu\n", bytes);
                fflush(stdout);
                esp_err_t upload_err = julia_local_tts_install_stream(stdin, bytes, crc);
                printf("TTS_UPLOAD result=%s; reboot required\n", esp_err_to_name(upload_err));
            }
        } else if (strcmp(line, "tts-resource disable") == 0) {
            printf("TTS_RESOURCE disable=%s; reboot required\n",
                   esp_err_to_name(julia_local_tts_set_resource_enabled(false)));
        } else if (strcmp(line, "tts-resource restore") == 0) {
            printf("TTS_RESOURCE restore=%s; reboot required\n",
                   esp_err_to_name(julia_local_tts_set_resource_enabled(true)));
        } else if (strcmp(line, "voice-test-legacy-disabled") == 0) {
            printf("Voice test: %s\n", esp_err_to_name(
                       julia_local_tts_speak("你好，我是 Julia，很高兴见到你")));
        } else if (strcmp(line, "doze enter") == 0) {
            display_breathing_start();
            printf("DOZE active=%d command=enter\n", display_breathing_active());
        } else if (strcmp(line, "doze exit") == 0) {
            display_breathing_stop();
            printf("DOZE active=%d command=exit\n", display_breathing_active());
        } else if (strcmp(line, "doze status") == 0) {
            printf("DOZE active=%d\n", display_breathing_active());
        } else if (strncmp(line, "backlight breathe ", 20) == 0) {
            unsigned min = 0, max = 0, period = 0, segments = 0;
            char tail = 0;
            if (sscanf(line + 20, "%u %u %u %u %c", &min, &max, &period,
                       &segments, &tail) != 4 || min >= max || max > 100U ||
                !segments || segments > 120U || period / segments < 5U) {
                printf("Usage: backlight breathe <min 0-99> <max 1-100> "
                       "<period_ms> <segments 1-120>; segment must be >=5ms\n");
            } else {
                esp_err_t err = julia_backlight_breathe_start_ex(
                    (uint8_t)min, (uint8_t)max, period, (uint16_t)segments);
                printf("BACKLIGHT_BREATHE min=%u max=%u period_ms=%u segments=%u "
                       "gamma=%u result=%s\n", min, max, period, segments,
                       julia_backlight_gamma_enabled() ? 1U : 0U,
                       esp_err_to_name(err));
            }
        } else if (strcmp(line, "backlight gamma on") == 0 ||
                   strcmp(line, "backlight gamma off") == 0) {
            bool enabled = line[16] == 'o' && line[17] == 'n';
            esp_err_t err = julia_backlight_set_gamma(enabled);
            printf("BACKLIGHT_GAMMA enabled=%u result=%s\n", enabled ? 1U : 0U,
                   esp_err_to_name(err));
        } else if (strncmp(line, "backlight test ", 15) == 0) {
            unsigned min = 0, max = 0, period = 0;
            char tail = 0;
            if (sscanf(line + 15, "%u %u %u %c", &min, &max, &period, &tail) != 3 ||
                min >= max || max > 100U) {
                printf("Usage: backlight test <min 0-99> <max 1-100> <period_ms>\n");
            } else {
                esp_err_t err = julia_backlight_breathe_start((uint8_t)min, (uint8_t)max,
                                                               period);
                printf("BACKLIGHT_TEST min=%u max=%u period_ms=%u result=%s\n",
                       min, max, period, esp_err_to_name(err));
            }
        } else if (strcmp(line, "swap test") == 0) {
            printf("SWAP_TEST CONFIG_LV_COLOR_16_SWAP=%d LV_COLOR_16_SWAP=%d asset_order=%s\n",
                   CONFIG_LV_COLOR_16_SWAP, LV_COLOR_16_SWAP,
                   LV_COLOR_16_SWAP ? "high-byte-first" : "low-byte-first");
        } else if (strncmp(line, "state ", 6) == 0) {
            char *end = NULL;
            long state = strtol(line + 6, &end, 10);
            if (*end || state < 0 || state >= JULIA_SUB_STATE_COUNT) {
                printf("Usage: state <0-%d>\n", JULIA_SUB_STATE_COUNT - 1);
            } else {
                avatar_face_note_activity();
                julia_ui_set_state((julia_sub_state_t)state);
                printf("State: %s\n", julia_fsm_sub_state_name((julia_sub_state_t)state));
            }
        } else if (strcmp(line, "showcase start") == 0) {
            printf("SHOWCASE start=%d\n", julia_ui_showcase_start() ? 1 : 0);
        } else if (strcmp(line, "showcase status") == 0) {
            printf("SHOWCASE running=%d\n", julia_ui_showcase_is_running() ? 1 : 0);
        } else if (strncmp(line, "speak ", 6) == 0) {
            char *end = NULL;
            long rms = strtol(line + 6, &end, 10);
            if (*end || rms < 0 || rms > 65535) {
                printf("Usage: speak <rms 0-65535>\n");
            } else {
                avatar_face_note_activity();
                avatar_face_set_rms((uint16_t)rms);
                printf("SPEAK rms=%ld\n", rms);
            }
        } else if (strcmp(line, "button down") == 0 || strcmp(line, "button up") == 0) {
            bool pressed = line[7] == 'd';
            avatar_face_button_set_pressed(pressed);
            printf("BUTTON pressed=%d\n", pressed);
        } else if (strcmp(line, "demo on") == 0 || strcmp(line, "demo off") == 0) {
            bool enabled = line[5] == 'o' && line[6] == 'n';
            avatar_face_demo_set_enabled(enabled);
            printf("DEMO active=%d\n", avatar_face_demo_enabled());
        } else if (strcmp(line, "demo status") == 0) {
            printf("DEMO active=%d\n", avatar_face_demo_enabled());
        } else if (strncmp(line, "mouth ", 6) == 0) {
            char *end = NULL;
            long level = strtol(line + 6, &end, 10);
            if (*end || level < 0 || level > 3) {
                printf("Usage: mouth <0-3>\n");
            } else {
                avatar_face_note_activity();
                julia_ui_talking_start();
                julia_ui_set_mouth_level((uint8_t)level);
                vTaskDelay(pdMS_TO_TICKS(1500));
                julia_ui_talking_stop();
                printf("Mouth level %ld tested\n", level);
            }
        } else if (strncmp(line, "phase ", 6) == 0) {
            char *end = NULL;
            long phase = strtol(line + 6, &end, 10);
            if (*end || phase < 0 || phase > 3) {
                printf("Usage: phase <0-3>\n");
            } else {
                julia_ui_set_dialog_phase((julia_dialog_phase_t)phase);
                printf("Dialog phase: %ld\n", phase);
            }
        } else if (strcmp(line, "preload-test") == 0) {
            printf("Preload churn test: %s\n",
                   esp_err_to_name(avatar_clip_preload_run_churn_test()));
        } else if (strcmp(line, "memory-test") == 0) {
            unsigned errors = 0;
            for (unsigned i = 0; i < 500; ++i) {
                char summary[64]; snprintf(summary, sizeof(summary), "memory-test-%03u keyword-julia", i);
                if (julia_memory_append(i % 4, JULIA_MEMORY_EMOTION_CALM, summary) != ESP_OK) errors++;
            }
            /* 等待异步写入任务清空队列后核对最新在前。 */
            vTaskDelay(pdMS_TO_TICKS(3000));
            julia_event_t *events = heap_caps_malloc(50 * sizeof(*events),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int count = events ? julia_memory_get_recent(50, events, 50) : 0;
            bool ordered = count == 50 && strstr(events[0].summary, "499") != NULL &&
                           strstr(events[49].summary, "450") != NULL;
            julia_event_t hits[4]; int recalled = julia_memory_recall_keyword("keyword-julia", hits, 4);
            char prompt[201]; int prompt_bytes = julia_memory_format_for_prompt(prompt, sizeof(prompt));
            printf("MEMORY_TEST writes=500 errors=%u recent=%d ordered=%d recall=%d prompt_bytes=%d prompt=%s\n",
                   errors, count, ordered, recalled, prompt_bytes, prompt);
            heap_caps_free(events);
        } else if (strcmp(line, "clip-mode loop") == 0 || strcmp(line, "clip-mode program") == 0) {
            bool enabled = line[10] == 'l'; avatar_clip_map_set_loop_mode(enabled);
            printf("CLIP_MODE loop=%d\n", enabled);
        } else if (strncmp(line, "transition upload ", 18) == 0) {
            char route[16] = {0}, filename[64] = {0};
            unsigned long bytes = 0, crc = 0;
            unsigned from = 0, to = 0;
            bool valid = sscanf(line + 18, "%15s %63s %lu %lx", route, filename,
                                &bytes, &crc) == 4 &&
                         sscanf(route, "S%u_S%u", &from, &to) == 2 &&
                         from < JULIA_MAIN_STATE_COUNT && to < JULIA_MAIN_STATE_COUNT &&
                         strstr(filename, "..") == NULL && strchr(filename, '/') == NULL &&
                         strchr(filename, '\\') == NULL && bytes > 0 &&
                         bytes <= 6U * 1024U * 1024U;
            if (!valid) {
                printf("Usage: transition upload <Sx_Sy> <file.trn> <bytes> <crc32>\n");
            } else {
                idle_player_exit();
                char directory[96], path[160];
                snprintf(directory, sizeof(directory), "/sdcard/julia/transitions/%s", route);
                if (julia_sd_lock(pdMS_TO_TICKS(3000))) {
                    mkdir("/sdcard/julia", 0775);
                    mkdir("/sdcard/julia/transitions", 0775);
                    mkdir(directory, 0775);
                    julia_sd_unlock();
                }
                snprintf(path, sizeof(path), "%s/%s", directory, filename);
                avatar_clip_descriptor_t asset = {
                    .name = filename, .path = path, .compressed_crc32 = (uint32_t)crc,
                };
                printf("TRANSITION_UPLOAD_READY route=%s file=%s bytes=%lu\n",
                       route, filename, bytes);
                fflush(stdout);
                size_t actual = 0;
                esp_err_t upload = receive_clip(&asset, &actual, false, bytes);
                if (upload == ESP_OK) transition_player_rescan();
                printf("TRANSITION_UPLOAD result=%s route=%s file=%s bytes=%u error=%s\n",
                       upload == ESP_OK ? "OK" : "FAIL", route, filename,
                       (unsigned)actual, esp_err_to_name(upload));
            }
        } else if (strncmp(line, "idle upload ", 12) == 0) {
            char filename[64] = {0};
            unsigned long bytes = 0, crc = 0;
            bool valid = sscanf(line + 12, "%63s %lu %lx", filename, &bytes, &crc) == 3 &&
                         strstr(filename, "..") == NULL && strchr(filename, '/') == NULL &&
                         strchr(filename, '\\') == NULL && bytes > 0 &&
                         bytes <= 6U * 1024U * 1024U;
            if (!valid) {
                printf("Usage: idle upload <file.trn> <bytes> <crc32>\n");
            } else {
                idle_player_exit();
                if (julia_sd_lock(pdMS_TO_TICKS(3000))) {
                    mkdir("/sdcard/julia", 0775);
                    mkdir("/sdcard/julia/idle", 0775);
                    julia_sd_unlock();
                }
                char path[128];
                snprintf(path, sizeof(path), "/sdcard/julia/idle/%s", filename);
                avatar_clip_descriptor_t idle = {
                    .name = filename, .path = path, .compressed_crc32 = (uint32_t)crc,
                };
                printf("IDLE_UPLOAD_READY file=%s bytes=%lu\n", filename, bytes);
                fflush(stdout);
                size_t actual = 0;
                esp_err_t upload = receive_clip(&idle, &actual, false, bytes);
                printf("IDLE_UPLOAD result=%s file=%s bytes=%u error=%s\n",
                       upload == ESP_OK ? "OK" : "FAIL", filename, (unsigned)actual,
                       esp_err_to_name(upload));
            }
        } else if (strncmp(line, "clip-upload ", 12) == 0 ||
                   strncmp(line, "clip-upload-debug ", 18) == 0) {
            bool upload_debug = strncmp(line, "clip-upload-debug ", 18) == 0;
            const char *name = line + (upload_debug ? 18 : 12);
            const avatar_clip_descriptor_t *clip = avatar_clip_map_find(name);
            if (!clip) printf("CLIP_UPLOAD result=FAIL %s 0 not_found\n", name);
            else {
                printf("CLIP_UPLOAD_READY name=%s block=%u\n", name, CLIP_UPLOAD_BLOCK);
                fflush(stdout);
                size_t actual = 0;
                esp_err_t upload = receive_clip(clip, &actual, upload_debug, 0);
                printf("CLIP_UPLOAD result=%s %s %u error=%s\n",
                       upload == ESP_OK ? "OK" : "FAIL", name, (unsigned)actual,
                       esp_err_to_name(upload));
            }
        } else if (strncmp(line, "clip-remove ", 12) == 0) {
            const avatar_clip_descriptor_t *clip = avatar_clip_map_find(line + 12);
            esp_err_t remove_err = ESP_ERR_NOT_FOUND;
            if (clip && julia_sd_lock(pdMS_TO_TICKS(1000))) {
                remove_err = remove(clip->path) == 0 ? ESP_OK : ESP_FAIL;
                julia_sd_unlock();
            }
            printf("CLIP_REMOVE name=%s result=%s\n", line + 12, esp_err_to_name(remove_err));
        } else if (strcmp(line, "theme next") == 0) {
            printf("THEME result=%s current=%s count=%u\n", esp_err_to_name(julia_theme_next()),
                   julia_theme_current(), (unsigned)julia_theme_count());
        } else if (strcmp(line, "theme status") == 0) {
            printf("THEME current=%s count=%u rendering=%d\n", julia_theme_current(),
                   (unsigned)julia_theme_count(), julia_display_theme_rendering());
        } else if (strncmp(line, "theme ", 6) == 0) {
            esp_err_t theme_err = julia_theme_select(line + 6);
            printf("THEME result=%s current=%s\n", esp_err_to_name(theme_err), julia_theme_current());
        } else if (strncmp(line, "screen-timeout ", 15) == 0) {
            unsigned seconds = (unsigned)strtoul(line + 15, NULL, 10);
            if (!seconds) printf("Usage: screen-timeout <seconds>\n");
            else { julia_display_theme_set_idle_timeout(seconds * 1000U); printf("SCREEN_TIMEOUT seconds=%u\n", seconds); }
        } else if (strcmp(line, "screen-wake") == 0) {
            reset_idle_timer();
            printf("SCREEN_WAKE requested\n");
#if CONFIG_JULIA_SOAK_TEST
        } else if (strncmp(line, "soak start ", 11) == 0) {
            unsigned duration = 0, minimum = 30, maximum = 120, fault = 3, state = 15;
            int parsed = sscanf(line + 11, "%u %u %u %u %u", &duration, &minimum,
                                &maximum, &fault, &state);
            if (parsed < 1 || !duration || !minimum || maximum < minimum ||
                fault > 20 || state > 100) {
                printf("Usage: soak start <seconds> [min_interval max_interval fault_percent state_percent]\n");
            } else {
                julia_soak_config_t config = {
                    .duration_seconds = duration,
                    .min_interval_seconds = minimum,
                    .max_interval_seconds = maximum,
                    .fault_percent = fault,
                    .state_jump_percent = state,
                    .silent = true,
                };
                printf("SOAK_START result=%s duration=%u interval=%u-%u fault=%u state=%u silent=1\n",
                       esp_err_to_name(julia_soak_test_start(&config)), duration,
                       minimum, maximum, fault, state);
            }
        } else if (strcmp(line, "soak stop") == 0) {
            julia_soak_test_stop();
            printf("SOAK_STOP requested\n");
        } else if (strcmp(line, "soak status") == 0) {
            julia_soak_status_t soak;
            julia_soak_test_get_status(&soak);
            printf("SOAK_STATUS running=%d elapsed=%u rounds=%u snapshots=%u faults=%u recovered=%u tts=%u/%u/%u\n",
                   soak.running, (unsigned)soak.elapsed_seconds,
                   (unsigned)soak.dialog_rounds, (unsigned)soak.snapshots,
                   (unsigned)soak.faults_injected, (unsigned)soak.faults_recovered,
                   (unsigned)soak.cloud_tts_count, (unsigned)soak.local_tts_count,
                   (unsigned)soak.pcm_fallback_count);
        } else if (strcmp(line, "soak archive") == 0) {
            char archive[128] = {0};
            esp_err_t archive_err = julia_soak_test_archive("中途终止·演示优先", archive, sizeof(archive));
            printf("SOAK_ARCHIVE result=%s path=%s verdict=NOT_EVALUATED\n",
                   esp_err_to_name(archive_err), archive);
#endif
        } else if (strncmp(line, "ota", 3) == 0) {
            const char *url = line + 3;
            while (*url == ' ') ++url;
            if (!url[0]) url = CONFIG_JULIA_OTA_URL;
            printf("OTA result: %s\n", esp_err_to_name(julia_system_ota_update(url)));
        } else if (strncmp(line, "config-set ", 11) == 0) {
            char *key = line + 11;
            char *value = strchr(key, ' ');
            if (!value) { printf("Usage: config-set key value\n"); continue; }
            *value++ = 0;
            bool secret = strstr(key, "key") || strstr(key, "pass") || strstr(key, "token");
            printf("Config result: %s\n", esp_err_to_name(julia_system_config_set(key, value, secret)));
        } else if (strncmp(line, "config-get ", 11) == 0) {
            const char *key = line + 11;
            char value[256] = {0};
            esp_err_t result = julia_system_config_get(key, value, sizeof(value));
            bool secret = strstr(key, "key") || strstr(key, "pass") || strstr(key, "token");
            printf("Config result: %s value=%s\n", esp_err_to_name(result),
                   result == ESP_OK ? (secret ? "<redacted>" : value) : "");
        } else if (strcmp(line, "factory-reset CONFIRM") == 0) {
            printf("Erasing NVS and restarting\n");
            nvs_flash_erase();
            esp_restart();
        } else if (line[0]) {
            printf("Unknown command\n");
        }
    }
}

esp_err_t julia_system_init(void)
{
    s_started_ms = esp_timer_get_time() / 1000;
    usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 32768,
    };
    esp_err_t usb_err = usb_serial_jtag_driver_install(&usb_config);
    if (usb_err == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        ESP_LOGI(TAG, "USB Serial/JTAG buffered driver ready");
    } else {
        ESP_LOGE(TAG, "USB Serial/JTAG driver init failed: %s", esp_err_to_name(usb_err));
        return usb_err;
    }
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &s_config_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "runtime config unavailable: %s", esp_err_to_name(err));

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_RETURN_ON_ERROR(esp_ota_mark_app_valid_cancel_rollback(), TAG, "confirm OTA image");
        ESP_LOGI(TAG, "OTA image marked valid");
    }
    if (xTaskCreate(diagnostic_task, "julia_diag", 4096, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(console_task, "julia_console", 7168, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK && wdt_err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "main task watchdog unavailable: %s", esp_err_to_name(wdt_err));
    print_status();
    return ESP_OK;
}

void julia_system_watchdog_feed(void)
{
    esp_task_wdt_reset();
}
