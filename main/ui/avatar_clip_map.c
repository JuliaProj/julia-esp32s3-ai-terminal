#include "avatar_clip_map.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "avatar_anim_engine.h"
#include "avatar_clip_preload.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_sd.h"
#include "cJSON.h"
#include "transition_director.h"

#define CLIP_DIR JULIA_SD_MOUNT_POINT "/julia/clips"

typedef struct {
    avatar_clip_descriptor_t descriptor;
    const uint8_t *embedded_start;
    const uint8_t *embedded_end;
} installed_clip_t;

#define EMBED_SYMBOLS(id) \
    extern const uint8_t id##_start[] asm("_binary_" #id "_bin_start"); \
    extern const uint8_t id##_end[] asm("_binary_" #id "_bin_end")

EMBED_SYMBOLS(SLEEP);
EMBED_SYMBOLS(IDLE);
EMBED_SYMBOLS(ACTIVE);
EMBED_SYMBOLS(ALERT);
EMBED_SYMBOLS(DIALOG);
EMBED_SYMBOLS(QUIET);
EMBED_SYMBOLS(D_IDLE);
EMBED_SYMBOLS(LISTEN);
EMBED_SYMBOLS(THINK);
EMBED_SYMBOLS(SPEAK);
extern const uint8_t idle_loop_jsn_start[] asm("_binary_idle_loop_jsn_start");
extern const uint8_t idle_loop_jsn_end[] asm("_binary_idle_loop_jsn_end");
extern const uint8_t listening_loop_jsn_start[] asm("_binary_listening_loop_jsn_start");
extern const uint8_t listening_loop_jsn_end[] asm("_binary_listening_loop_jsn_end");
extern const uint8_t thinking_loop_jsn_start[] asm("_binary_thinking_loop_jsn_start");
extern const uint8_t thinking_loop_jsn_end[] asm("_binary_thinking_loop_jsn_end");
extern const uint8_t speaking_base_loop_jsn_start[] asm("_binary_speaking_base_loop_jsn_start");
extern const uint8_t speaking_base_loop_jsn_end[] asm("_binary_speaking_base_loop_jsn_end");
extern const uint8_t sleep_loop_jsn_start[] asm("_binary_sleep_loop_jsn_start");
extern const uint8_t sleep_loop_jsn_end[] asm("_binary_sleep_loop_jsn_end");

#define CLIP(name_, file_, size_, crc_, decoded_, mode_, h0_, h1_) \
    {{.name = #name_, .path = CLIP_DIR "/" file_, .compressed_crc32 = crc_, \
      .frame_count = 1, .fps = 20, .transition_ms = 250, .mode = mode_, \
      .contains_blink = false, .transition = AVATAR_TRANSITION_CROSS_FADE, .frame_offsets = {0}, \
      .frame_sizes = {size_}, .decoded_crc32 = {decoded_}, \
      .next_hints = {h0_, h1_, NULL}}, name_##_start, name_##_end}

static installed_clip_t s_clips[] = {
    CLIP(SLEEP,  "SLEEP.BIN",  78056,  0x3331ffb3, 0x881971bf, AVATAR_CLIP_LOOP, "IDLE", NULL),
    CLIP(IDLE,   "IDLE.BIN",   88450,  0x96e9727e, 0xe57c30cb, AVATAR_CLIP_LOOP, "LISTEN", "ACTIVE"),
    CLIP(ACTIVE, "ACTIVE.BIN", 88164,  0x2fafcb98, 0x380c8477, AVATAR_CLIP_LOOP, "LISTEN", "IDLE"),
    CLIP(ALERT,  "ALERT.BIN",  83598,  0x51d35050, 0x1227ec02, AVATAR_CLIP_ONCE_HOLD, "LISTEN", "THINK"),
    CLIP(DIALOG, "DIALOG.BIN", 88892,  0x32c6cc72, 0xb3b78a5a, AVATAR_CLIP_LOOP, "LISTEN", "THINK"),
    CLIP(QUIET,  "QUIET.BIN",  86134,  0xd9a3d067, 0xf716348b, AVATAR_CLIP_LOOP, "IDLE", NULL),
    CLIP(D_IDLE, "D_IDLE.BIN", 88892,  0x32c6cc72, 0xb3b78a5a, AVATAR_CLIP_LOOP, "LISTEN", NULL),
    CLIP(LISTEN, "LISTEN.BIN",105338,  0xf1498583, 0x122fda66, AVATAR_CLIP_LOOP, "THINK", NULL),
    CLIP(THINK,  "THINK.BIN",  85962,  0x7bffb366, 0x26dc6a30, AVATAR_CLIP_LOOP, "SPEAK", NULL),
    CLIP(SPEAK,  "SPEAK.BIN",  85872,  0xb805aedc, 0xe8a17787, AVATAR_CLIP_LOOP, "D_IDLE", NULL),
};

typedef struct {
    avatar_clip_descriptor_t descriptor;
    const uint8_t *json_start, *json_end;
} loop_clip_t;

static loop_clip_t s_loops[] = {
    {.descriptor = {.name="idle_loop", .path=CLIP_DIR "/idle/idle_loop.clip", .transition_ms=250}, .json_start=idle_loop_jsn_start, .json_end=idle_loop_jsn_end},
    {.descriptor = {.name="listening_loop", .path=CLIP_DIR "/listening/listening_loop.clip", .transition_ms=250}, .json_start=listening_loop_jsn_start, .json_end=listening_loop_jsn_end},
    {.descriptor = {.name="thinking_loop", .path=CLIP_DIR "/thinking/thinking_loop.clip", .transition_ms=250}, .json_start=thinking_loop_jsn_start, .json_end=thinking_loop_jsn_end},
    {.descriptor = {.name="speaking_base_loop", .path=CLIP_DIR "/speaking/speaking_base_loop.clip", .transition_ms=250}, .json_start=speaking_base_loop_jsn_start, .json_end=speaking_base_loop_jsn_end},
    {.descriptor = {.name="sleep_loop", .path=CLIP_DIR "/sleep/sleep_loop.clip", .transition_ms=250}, .json_start=sleep_loop_jsn_start, .json_end=sleep_loop_jsn_end},
};
static bool s_loop_mode = true;
static void request_context_clip(void);

static uint32_t hex_value(const cJSON *item)
{
    return cJSON_IsString(item) ? (uint32_t)strtoul(item->valuestring, NULL, 16) : 0;
}

static esp_err_t prepare_loop(loop_clip_t *loop)
{
    size_t json_size = (size_t)(loop->json_end - loop->json_start);
    cJSON *root = cJSON_ParseWithLength((const char *)loop->json_start, json_size);
    cJSON *frames = root ? cJSON_GetObjectItem(root, "frames") : NULL;
    cJSON *fps = root ? cJSON_GetObjectItem(root, "fps") : NULL;
    cJSON *mode = root ? cJSON_GetObjectItem(root, "loop") : NULL;
    cJSON *file_crc = root ? cJSON_GetObjectItem(root, "file_crc32") : NULL;
    int count = cJSON_IsArray(frames) ? cJSON_GetArraySize(frames) : 0;
    if (!root || count < 1 || count > 24 || !cJSON_IsNumber(fps)) { cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE; }
    loop->descriptor.frame_count = (uint16_t)count;
    loop->descriptor.fps = (uint16_t)fps->valueint;
    loop->descriptor.mode = cJSON_IsString(mode) && !strcmp(mode->valuestring, "ping-pong") ? AVATAR_CLIP_PING_PONG : AVATAR_CLIP_LOOP;
    loop->descriptor.contains_blink = cJSON_IsTrue(cJSON_GetObjectItem(root, "contains_blink"));
    loop->descriptor.transition = AVATAR_TRANSITION_CROSS_FADE;
    loop->descriptor.compressed_crc32 = hex_value(file_crc);
    for (int i = 0; i < count; ++i) {
        cJSON *frame = cJSON_GetArrayItem(frames, i);
        loop->descriptor.frame_offsets[i] = (uint32_t)cJSON_GetObjectItem(frame, "offset")->valuedouble;
        loop->descriptor.frame_sizes[i] = (uint32_t)cJSON_GetObjectItem(frame, "size")->valuedouble;
        loop->descriptor.decoded_crc32[i] = hex_value(cJSON_GetObjectItem(frame, "crc32"));
    }
    cJSON_Delete(root);
    const char *slash = strrchr(loop->descriptor.path, '/');
    char directory[96]; size_t length = (size_t)(slash - loop->descriptor.path);
    memcpy(directory, loop->descriptor.path, length); directory[length] = 0; mkdir(directory, 0775);
    return ESP_OK;
}

static const char *TAG = "AVATAR_MAP";
static uint8_t s_state = 3; /* S1.1 */
static uint8_t s_phase;
static uint8_t s_crc_block[4096];

static esp_err_t install_one(const installed_clip_t *clip)
{
    size_t embedded_size = (size_t)(clip->embedded_end - clip->embedded_start);
    if (embedded_size != clip->descriptor.frame_sizes[0]) return ESP_ERR_INVALID_SIZE;
    bool valid = false;
    struct stat info;
    if (julia_sd_lock(pdMS_TO_TICKS(1000))) {
        valid = stat(clip->descriptor.path, &info) == 0 && (size_t)info.st_size == embedded_size;
        julia_sd_unlock();
    }
    if (valid && julia_sd_lock(pdMS_TO_TICKS(1000))) {
        FILE *existing = fopen(clip->descriptor.path, "rb");
        julia_sd_unlock();
        uint32_t crc = 0;
        size_t total = 0;
        while (existing && total < embedded_size) {
            if (!julia_sd_lock(pdMS_TO_TICKS(1000))) { valid = false; break; }
            size_t got = fread(s_crc_block, 1, sizeof(s_crc_block), existing);
            julia_sd_unlock();
            if (!got) break;
            crc = esp_crc32_le(crc, s_crc_block, got);
            total += got;
            vTaskDelay(1);
        }
        if (existing) {
            if (julia_sd_lock(pdMS_TO_TICKS(1000))) {
                fclose(existing);
                julia_sd_unlock();
            } else fclose(existing);
        }
        valid = valid && total == embedded_size && crc == clip->descriptor.compressed_crc32;
    }
    if (valid) return ESP_OK;

    if (!julia_sd_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    FILE *file = fopen(clip->descriptor.path, "wb");
    julia_sd_unlock();
    if (!file) return ESP_FAIL;
    esp_err_t err = ESP_OK;
    for (size_t offset = 0; offset < embedded_size; offset += 8192) {
        size_t count = embedded_size - offset;
        if (count > 8192) count = 8192;
        if (!julia_sd_lock(pdMS_TO_TICKS(1000))) { err = ESP_ERR_TIMEOUT; break; }
        size_t written = fwrite(clip->embedded_start + offset, 1, count, file);
        julia_sd_unlock();
        if (written != count) { err = ESP_FAIL; break; }
        vTaskDelay(1);
    }
    if (julia_sd_lock(pdMS_TO_TICKS(1000))) {
        fflush(file);
        fclose(file);
        julia_sd_unlock();
    } else {
        fclose(file);
        err = ESP_ERR_TIMEOUT;
    }
    return err;
}

esp_err_t avatar_clip_map_init(void)
{
    if (!julia_sd_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!julia_sd_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    mkdir(JULIA_SD_MOUNT_POINT "/julia", 0775);
    int result = mkdir(CLIP_DIR, 0775);
    julia_sd_unlock();
    if (result != 0 && errno != EEXIST) return ESP_FAIL;

    for (size_t i = 0; i < sizeof(s_clips) / sizeof(s_clips[0]); ++i) {
        esp_err_t err = install_one(&s_clips[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "install failed clip=%s: %s", s_clips[i].descriptor.name,
                     esp_err_to_name(err));
            return err;
        }
    }
    for (size_t i = 0; i < sizeof(s_loops) / sizeof(s_loops[0]); ++i) {
        esp_err_t err = prepare_loop(&s_loops[i]);
        if (err != ESP_OK) ESP_LOGW(TAG, "loop unavailable=%s: %s", s_loops[i].descriptor.name, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "clip map ready: clips=%u state_groups=6 dialog_phases=4",
             (unsigned)(sizeof(s_clips) / sizeof(s_clips[0])));
    return ESP_OK;
}

const avatar_clip_descriptor_t *avatar_clip_map_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(s_clips) / sizeof(s_clips[0]); ++i) {
        if (strcmp(s_clips[i].descriptor.name, name) == 0) return &s_clips[i].descriptor;
    }
    for (size_t i = 0; i < sizeof(s_loops) / sizeof(s_loops[0]); ++i)
        if (strcmp(s_loops[i].descriptor.name, name) == 0) return &s_loops[i].descriptor;
    return transition_director_find_clip(name);
}

const avatar_clip_descriptor_t *avatar_clip_map_resolve(uint8_t state, uint8_t phase)
{
    if (s_loop_mode) {
        if (phase == 1) return avatar_clip_map_find("listening_loop");
        if (phase == 2) return avatar_clip_map_find("thinking_loop");
        if (phase == 3) return avatar_clip_map_find("speaking_base_loop");
        if (state <= 2) return avatar_clip_map_find("sleep_loop");
        if (state <= 5) return avatar_clip_map_find("idle_loop");
        if (state <= 8) return avatar_clip_map_find("listening_loop");
        if (state <= 12) return avatar_clip_map_find("thinking_loop");
        if (state <= 16) return avatar_clip_map_find("speaking_base_loop");
        return avatar_clip_map_find("sleep_loop");
    }
    if (phase == 1) return avatar_clip_map_find("LISTEN");
    if (phase == 2) return avatar_clip_map_find("THINK");
    if (phase == 3) return avatar_clip_map_find("SPEAK");
    if (state >= 13 && state <= 16) return avatar_clip_map_find("D_IDLE");
    if (state <= 2) return avatar_clip_map_find("SLEEP");
    if (state <= 5) return avatar_clip_map_find("IDLE");
    if (state <= 8) return avatar_clip_map_find("ACTIVE");
    if (state <= 12) return avatar_clip_map_find("ALERT");
    if (state <= 16) return avatar_clip_map_find("DIALOG");
    return avatar_clip_map_find("QUIET");
}

void avatar_clip_map_set_loop_mode(bool enabled) { s_loop_mode = enabled; request_context_clip(); }
bool avatar_clip_map_loop_mode(void) { return s_loop_mode; }

static void request_context_clip(void)
{
    const avatar_clip_descriptor_t *clip = avatar_clip_map_resolve(s_state, s_phase);
    if (clip) avatar_anim_engine_request_clip(clip, s_state, s_phase);
}

void avatar_clip_map_set_state(uint8_t state)
{
    s_state = state;
    if (state != 11 && (state < 13 || state > 16)) s_phase = 0;
    avatar_clip_preload_on_state(state);
    if (s_phase == 0) avatar_clip_preload_on_dialog_phase(0);
    request_context_clip();
}

void avatar_clip_map_set_dialog_phase(uint8_t phase)
{
    s_phase = phase;
    avatar_clip_preload_on_dialog_phase(phase);
    request_context_clip();
}

void avatar_clip_map_schedule_hints(const avatar_clip_descriptor_t *clip,
                                    uint8_t state, uint8_t phase)
{
    if (!clip) return;
    for (size_t i = 0; i < 3 && clip->next_hints[i]; ++i) {
        const avatar_clip_descriptor_t *hint = avatar_clip_map_find(clip->next_hints[i]);
        if (!hint) continue;
        avatar_clip_preload_register_hint(state, phase, hint->name, hint->path,
                                          hint->compressed_crc32);
        avatar_clip_preload_request(hint->name, hint->path, hint->compressed_crc32,
                                    AVATAR_PRELOAD_PREDICTED);
    }
}

void avatar_clip_map_get_context(uint8_t *state, uint8_t *phase)
{
    if (state) *state = s_state;
    if (phase) *phase = s_phase;
}
