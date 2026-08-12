#include "julia_speech_cloud.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_audio.h"
#include "julia_network.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#define TAG "JULIA_SPEECH"
#define MULTIMODAL_URL "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
#define MULTIMODAL_PATH "/api/v1/services/aigc/multimodal-generation/generation"
#define DASHSCOPE_HOST "dashscope.aliyuncs.com"
#define MAX_RESPONSE_BYTES (1024 * 1024)
#define CLOUD_FAILURE_COOLDOWN_MS (8 * 1000LL)

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} response_t;

static int64_t s_cloud_retry_after_ms;

static esp_err_t wait_for_valid_clock(void)
{
    for (int i = 0; i < 30; ++i) {
        if (time(NULL) >= 1704067200) return ESP_OK;
        if (i == 0) ESP_LOGI(TAG, "Waiting for SNTP before TLS request");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "System clock is not synchronized");
    return ESP_ERR_INVALID_STATE;
}

static void put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void write_wav_header(uint8_t *p, size_t pcm_bytes)
{
    memcpy(p, "RIFF", 4); put_le32(p + 4, (uint32_t)pcm_bytes + 36);
    memcpy(p + 8, "WAVEfmt ", 8); put_le32(p + 16, 16); put_le16(p + 20, 1);
    put_le16(p + 22, 1); put_le32(p + 24, 16000); put_le32(p + 28, 32000);
    put_le16(p + 32, 2); put_le16(p + 34, 16);
    memcpy(p + 36, "data", 4); put_le32(p + 40, (uint32_t)pcm_bytes);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    size_t needed = response->size + event->data_len + 1;
    if (needed > MAX_RESPONSE_BYTES) return ESP_ERR_NO_MEM;
    if (needed > response->capacity) {
        size_t capacity = response->capacity ? response->capacity * 2 : 4096;
        while (capacity < needed) capacity *= 2;
        uint8_t *data = heap_caps_realloc(response->data, capacity,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!data) return ESP_ERR_NO_MEM;
        response->data = data;
        response->capacity = capacity;
    }
    memcpy(response->data + response->size, event->data, event->data_len);
    response->size += event->data_len;
    response->data[response->size] = 0;
    return ESP_OK;
}

static esp_err_t perform(const char *url, const char *content_type,
                         const void *body, size_t body_size, response_t *response)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < s_cloud_retry_after_ms) {
        ESP_LOGW(TAG, "Cloud retry cooldown: %lldms remaining", s_cloud_retry_after_ms - now_ms);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t clock_err = wait_for_valid_clock();
    if (clock_err != ESP_OK) return clock_err;
    esp_err_t network_err = julia_wifi_wait_cloud_ready(5000);
    if (network_err != ESP_OK) {
        ESP_LOGW(TAG, "Cloud network is not stable: RSSI=%d dBm", julia_wifi_get_rssi());
        return network_err;
    }
    esp_err_t dns_err = julia_wifi_check_dns(DASHSCOPE_HOST);
    bool fallback_available = CONFIG_JULIA_DASHSCOPE_FALLBACK_IP[0] != '\0';
    if (dns_err != ESP_OK && !fallback_available) {
        ESP_LOGE(TAG, "DashScope DNS unavailable and IPv4 fallback is disabled");
        s_cloud_retry_after_ms = esp_timer_get_time() / 1000 + CLOUD_FAILURE_COOLDOWN_MS;
        return dns_err;
    }
    char fallback_url[160] = {0};
    if (fallback_available) {
        snprintf(fallback_url, sizeof(fallback_url), "https://%s%s",
                 CONFIG_JULIA_DASHSCOPE_FALLBACK_IP, MULTIMODAL_PATH);
    }
    esp_err_t err = ESP_FAIL;
    int status = 0;
    /* Some phone/lab hotspots return a reachable-looking DashScope address that
     * the ESP32 cannot establish TLS with. Try the normal hostname first, then
     * one configured IPv4 address. Keeping each attempt short also prevents the
     * voice task from appearing frozen for almost a minute. */
    int attempt_count = fallback_available ? 2 : 1;
    if (dns_err != ESP_OK) attempt_count = 1;
    for (int attempt = 0; attempt < attempt_count; ++attempt) {
        bool use_fallback = dns_err != ESP_OK || attempt == 1;
        const char *attempt_url = use_fallback ? fallback_url : url;
        response->size = 0;
        esp_http_client_config_t config = {
            .url = attempt_url, .method = HTTP_METHOD_POST, .timeout_ms = 12000,
            .event_handler = http_event, .user_data = response,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 4096, .buffer_size_tx = 4096,
            .common_name = use_fallback ? DASHSCOPE_HOST : NULL,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return ESP_ERR_NO_MEM;
        char authorization[224];
        snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_JULIA_AI_API_KEY);
        esp_http_client_set_header(client, "Authorization", authorization);
        esp_http_client_set_header(client, "Content-Type", content_type);
        if (use_fallback) esp_http_client_set_header(client, "Host", DASHSCOPE_HOST);
        esp_http_client_set_post_field(client, body, body_size);
        int64_t started_ms = esp_timer_get_time() / 1000;
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Cloud POST route=%s status=%d bytes=%u elapsed=%lldms result=%s",
                 use_fallback ? "fallback-ip" : "dns", status, (unsigned)response->size,
                 esp_timer_get_time() / 1000 - started_ms, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (err == ESP_OK) break;
        if (!use_fallback && fallback_available) {
            ESP_LOGW(TAG, "Hostname route failed; trying DashScope IPv4 fallback %s",
                     CONFIG_JULIA_DASHSCOPE_FALLBACK_IP);
        }
    }
    if (err != ESP_OK) {
        s_cloud_retry_after_ms = esp_timer_get_time() / 1000 + CLOUD_FAILURE_COOLDOWN_MS;
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d: %.*s", status, (int)(response->size > 240 ? 240 : response->size),
                 response->data ? (char *)response->data : "");
        s_cloud_retry_after_ms = esp_timer_get_time() / 1000 + CLOUD_FAILURE_COOLDOWN_MS;
        return ESP_FAIL;
    }
    s_cloud_retry_after_ms = 0;
    return ESP_OK;
}

esp_err_t julia_speech_asr(const int16_t *pcm, size_t sample_count,
                           char *text, size_t text_size)
{
    if (!pcm || !sample_count || !text || text_size < 2) return ESP_ERR_INVALID_ARG;
    size_t wav_size = 44 + sample_count * sizeof(int16_t);
    uint8_t *wav = heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) return ESP_ERR_NO_MEM;
    write_wav_header(wav, sample_count * sizeof(int16_t));
    memcpy(wav + 44, pcm, sample_count * sizeof(int16_t));
    size_t encoded_size = 0;
    mbedtls_base64_encode(NULL, 0, &encoded_size, wav, wav_size);
    const char *prefix = "{\"model\":\"qwen-audio-turbo-latest\",\"input\":{\"messages\":[{\"role\":\"user\",\"content\":[{\"audio\":\"data:audio/wav;base64,";
    const char *suffix = "\"},{\"text\":\"请只输出这段录音中的中文内容，不要解释。\"}]}]},\"parameters\":{\"result_format\":\"message\"}}";
    size_t body_size = strlen(prefix) + encoded_size + strlen(suffix);
    uint8_t *body = heap_caps_malloc(body_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { free(wav); return ESP_ERR_NO_MEM; }
    memcpy(body, prefix, strlen(prefix));
    size_t written = 0;
    esp_err_t encode_err = mbedtls_base64_encode(body + strlen(prefix), encoded_size,
                                                  &written, wav, wav_size);
    free(wav);
    if (encode_err != 0) { free(body); return ESP_FAIL; }
    memcpy(body + strlen(prefix) + written, suffix, strlen(suffix));
    body_size = strlen(prefix) + written + strlen(suffix); body[body_size] = 0;
    response_t response = {0};
    esp_err_t err = perform(MULTIMODAL_URL, "application/json", body, body_size, &response);
    free(body);
    if (err != ESP_OK) { free(response.data); return err; }
    cJSON *root = cJSON_Parse((char *)response.data);
    cJSON *output = root ? cJSON_GetObjectItemCaseSensitive(root, "output") : NULL;
    cJSON *choices = output ? cJSON_GetObjectItemCaseSensitive(output, "choices") : NULL;
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    cJSON *part = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : NULL;
    cJSON *value = part ? cJSON_GetObjectItemCaseSensitive(part, "text") : NULL;
    if (!cJSON_IsString(value) && cJSON_IsString(content)) value = content;
    if (!cJSON_IsString(value) || !value->valuestring[0]) err = ESP_ERR_INVALID_RESPONSE;
    else strlcpy(text, value->valuestring, text_size);
    cJSON_Delete(root); free(response.data);
    return err;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t julia_speech_tts(const char *text, int16_t **pcm, size_t *sample_count)
{
    if (!text || !text[0] || !pcm || !sample_count) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "qwen3-tts-flash");
    cJSON *input = cJSON_AddObjectToObject(root, "input");
    cJSON_AddStringToObject(input, "text", text);
    cJSON_AddStringToObject(input, "voice", "Cherry");
    cJSON_AddStringToObject(input, "language_type", "Chinese");
    char *body = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    response_t response = {0};
    esp_err_t err = perform(MULTIMODAL_URL, "application/json", body, strlen(body), &response);
    cJSON_free(body);
    if (err != ESP_OK) { free(response.data); return err; }
    cJSON *reply = cJSON_Parse((char *)response.data);
    cJSON *output = reply ? cJSON_GetObjectItemCaseSensitive(reply, "output") : NULL;
    cJSON *audio = output ? cJSON_GetObjectItemCaseSensitive(output, "audio") : NULL;
    cJSON *url = audio ? cJSON_GetObjectItemCaseSensitive(audio, "url") : NULL;
    if (!cJSON_IsString(url)) { cJSON_Delete(reply); free(response.data); return ESP_ERR_INVALID_RESPONSE; }
    char *audio_url = strdup(url->valuestring);
    cJSON_Delete(reply); free(response.data); response = (response_t){0};
    esp_http_client_config_t download_config = {
        .url = audio_url, .method = HTTP_METHOD_GET, .timeout_ms = 30000,
        .event_handler = http_event, .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 4096,
    };
    esp_http_client_handle_t download = esp_http_client_init(&download_config);
    free(audio_url);
    if (!download) return ESP_ERR_NO_MEM;
    int64_t download_started_ms = esp_timer_get_time() / 1000;
    err = esp_http_client_perform(download);
    int status = esp_http_client_get_status_code(download);
    ESP_LOGI(TAG, "TTS download: status=%d bytes=%u elapsed=%lldms result=%s",
             status, (unsigned)response.size,
             esp_timer_get_time() / 1000 - download_started_ms, esp_err_to_name(err));
    esp_http_client_cleanup(download);
    if (err != ESP_OK || status < 200 || status >= 300 || response.size < 44 ||
        memcmp(response.data, "RIFF", 4) != 0) { free(response.data); return ESP_ERR_INVALID_RESPONSE; }
    size_t offset = 12;
    bool data_found = false;
    uint16_t audio_format = 0, channels = 0, bits_per_sample = 0;
    uint32_t source_rate = 0;
    size_t data_offset = 0, data_bytes = 0;
    while (offset <= response.size && response.size - offset >= 8) {
        uint32_t chunk_size = get_le32(response.data + offset + 4);
        size_t available = response.size - offset - 8;
        /* DashScope emits streaming WAV files whose RIFF/data sizes use the
         * 0x7fffffff sentinel. The HTTP body length is authoritative here. */
        if (memcmp(response.data + offset, "data", 4) == 0) {
            data_found = true;
            data_offset = offset + 8;
            data_bytes = chunk_size > available ? available : chunk_size;
            break;
        }
        if ((size_t)chunk_size > available) break;
        if (memcmp(response.data + offset, "fmt ", 4) == 0 && chunk_size >= 16) {
            const uint8_t *fmt = response.data + offset + 8;
            audio_format = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            channels = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
            source_rate = get_le32(fmt + 4);
            bits_per_sample = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
        }
        /* RIFF chunks are padded to an even byte boundary. Validate before
         * advancing so malformed/extended metadata cannot wrap size_t and
         * trap the voice task in an endless parser loop. */
        size_t padded_size = (size_t)chunk_size + (chunk_size & 1U);
        if (padded_size > response.size - offset - 8) break;
        offset += 8 + padded_size;
    }
    if (!data_found || audio_format != 1 || (channels != 1 && channels != 2) ||
        bits_per_sample != 16 || source_rate < 8000 || source_rate > 96000) {
        ESP_LOGE(TAG, "Unsupported WAV: fmt=%u channels=%u rate=%u bits=%u data=%u",
                 audio_format, channels, (unsigned)source_rate, bits_per_sample,
                 (unsigned)data_bytes);
        free(response.data);
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t source_frames = data_bytes / (sizeof(int16_t) * channels);
    size_t output_samples = source_frames * JULIA_AUDIO_SAMPLE_RATE / source_rate;
    if (!source_frames || !output_samples || output_samples > JULIA_AUDIO_SAMPLE_RATE * 120U) {
        free(response.data);
        return ESP_ERR_INVALID_SIZE;
    }
    int16_t *output_pcm = heap_caps_malloc(output_samples * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output_pcm)
        output_pcm = heap_caps_malloc(output_samples * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (!output_pcm) { free(response.data); return ESP_ERR_NO_MEM; }
    const int16_t *source = (const int16_t *)(response.data + data_offset);
    for (size_t i = 0; i < output_samples; ++i) {
        size_t frame = i * source_rate / JULIA_AUDIO_SAMPLE_RATE;
        if (frame >= source_frames) frame = source_frames - 1;
        if (channels == 1) output_pcm[i] = source[frame];
        else output_pcm[i] = (int16_t)(((int32_t)source[frame * 2] + source[frame * 2 + 1]) / 2);
    }
    ESP_LOGI(TAG, "TTS WAV decoded: %uHz %uch -> %u samples",
             (unsigned)source_rate, channels, (unsigned)output_samples);
    free(response.data);
    *pcm = output_pcm;
    *sample_count = output_samples;
    return ESP_OK;
}
