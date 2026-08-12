#include "avatar_rle.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crc.h"
#include <string.h>

#define AVATAR_FRAME_PIXELS (360U * 360U)
#define BENCHMARK_ROUNDS 100
#define BENCHMARK_EXPECTED_DECODED_CRC 0xe57c30cbU

static const char *TAG = "AVATAR_RLE";

extern const uint8_t rle_bench_start[] asm("_binary_rle_bench_bin_start");
extern const uint8_t rle_bench_end[] asm("_binary_rle_bench_bin_end");

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

esp_err_t avatar_rle_decode_rgb565(const uint8_t *input, size_t input_size,
                                   uint16_t *output, size_t output_pixels)
{
    if (!input || !output) return ESP_ERR_INVALID_ARG;
    size_t source = 0, destination = 0;
    while (source + 2 <= input_size && destination < output_pixels) {
        uint16_t control = read_u16_le(input + source);
        source += 2;
        size_t count = (size_t)(control & 0x7fffU) + 1U;
        if (count > output_pixels - destination) return ESP_ERR_INVALID_SIZE;
        if (control & 0x8000U) {
            if (source + 2 > input_size) return ESP_ERR_INVALID_SIZE;
            uint16_t pixel = read_u16_le(input + source);
            source += 2;
            if ((destination & 1U) && count) {
                output[destination++] = pixel;
                --count;
            }
            uint32_t pair = (uint32_t)pixel | ((uint32_t)pixel << 16);
            uint32_t *wide = (uint32_t *)(output + destination);
            size_t pairs = count / 2U;
            for (size_t i = 0; i < pairs; ++i) wide[i] = pair;
            destination += pairs * 2U;
            if (count & 1U) output[destination++] = pixel;
        } else {
            size_t bytes = count * sizeof(uint16_t);
            if (source + bytes > input_size) return ESP_ERR_INVALID_SIZE;
            /* ESP32-S3与打包格式均为小端，原样段可直接复制到PSRAM。 */
            memcpy(output + destination, input + source, bytes);
            destination += count;
            source += bytes;
        }
    }
    return destination == output_pixels && source == input_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t avatar_rle_run_benchmark(void)
{
    uint16_t *frame = heap_caps_malloc(AVATAR_FRAME_PIXELS * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) return ESP_ERR_NO_MEM;
    size_t compressed_size = (size_t)(rle_bench_end - rle_bench_start);
    int64_t minimum_us = INT64_MAX, maximum_us = 0, total_us = 0;
    esp_err_t result = ESP_OK;
    for (int round = 0; round < BENCHMARK_ROUNDS; ++round) {
        int64_t started = esp_timer_get_time();
        result = avatar_rle_decode_rgb565(rle_bench_start, compressed_size,
                                          frame, AVATAR_FRAME_PIXELS);
        int64_t elapsed = esp_timer_get_time() - started;
        if (result != ESP_OK) break;
        uint32_t crc = esp_crc32_le(0, (const uint8_t *)frame,
                                    AVATAR_FRAME_PIXELS * sizeof(uint16_t));
        bool crc_ok = crc == BENCHMARK_EXPECTED_DECODED_CRC;
        ESP_LOGI(TAG, "CRC frame %03d: actual=%08lx expected=%08lx %s", round + 1,
                 (unsigned long)crc, (unsigned long)BENCHMARK_EXPECTED_DECODED_CRC,
                 crc_ok ? "PASS" : "FAIL");
        if (!crc_ok) { result = ESP_ERR_INVALID_CRC; break; }
        if (elapsed < minimum_us) minimum_us = elapsed;
        if (elapsed > maximum_us) maximum_us = elapsed;
        total_us += elapsed;
    }
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "RLE decode benchmark: rounds=%d compressed=%u raw=%u avg=%.3fms min=%.3fms max=%.3fms",
                 BENCHMARK_ROUNDS, (unsigned)compressed_size,
                 (unsigned)(AVATAR_FRAME_PIXELS * sizeof(uint16_t)),
                 (double)total_us / BENCHMARK_ROUNDS / 1000.0,
                 (double)minimum_us / 1000.0, (double)maximum_us / 1000.0);
    } else {
        ESP_LOGE(TAG, "RLE decode benchmark failed: %s", esp_err_to_name(result));
    }
    heap_caps_free(frame);
    return result;
}

esp_err_t avatar_rle_decode_embedded_frame(uint16_t *output, size_t output_pixels)
{
    return avatar_rle_decode_rgb565(rle_bench_start,
                                    (size_t)(rle_bench_end - rle_bench_start),
                                    output, output_pixels);
}
