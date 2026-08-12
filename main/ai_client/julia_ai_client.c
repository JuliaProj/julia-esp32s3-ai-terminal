#include "julia_ai_client.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_network.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#define TAG "JULIA_AI"
#define HISTORY_ROUNDS 10
#define FUNCTION_COUNT 8
#define CHUNK_SIZE 256
#define SSE_LINE_SIZE 2048
#define RESPONSE_SIZE 8192
#define TOOL_ARGUMENT_SIZE 1536

typedef struct { char text[CHUNK_SIZE]; } ai_chunk_t;
typedef struct { char *user; char *assistant; } history_t;
typedef struct { char name[48]; julia_ai_function_cb_t callback; void *ctx; } function_t;
typedef struct { char *user; char *system; } request_t;

static char s_url[256];
static char s_key[192];
static QueueHandle_t s_chunks;
static SemaphoreHandle_t s_lock;
static history_t s_history[HISTORY_ROUNDS];
static size_t s_history_count;
static function_t s_functions[FUNCTION_COUNT];
static size_t s_function_count;
static volatile bool s_ready;
static volatile bool s_busy;
static volatile bool s_closing;
static esp_http_client_handle_t s_active_client;

typedef struct {
    char line[SSE_LINE_SIZE]; size_t line_len;
    char response[RESPONSE_SIZE]; size_t response_len;
    char tool_name[48]; char tool_args[TOOL_ARGUMENT_SIZE]; size_t tool_args_len;
} stream_ctx_t;

static void queue_text(const char *text)
{
    if (!text || !text[0]) return;
    while (*text) {
        ai_chunk_t item = {0};
        size_t len = strnlen(text, CHUNK_SIZE - 1);
        memcpy(item.text, text, len);
        xQueueSend(s_chunks, &item, pdMS_TO_TICKS(100));
        text += len;
    }
}

static void append_text(char *dest, size_t *used, size_t capacity, const char *part)
{
    if (!part || *used >= capacity - 1) return;
    size_t n = strnlen(part, capacity - 1 - *used);
    memcpy(dest + *used, part, n); *used += n; dest[*used] = '\0';
}

static void execute_tool(stream_ctx_t *ctx)
{
    if (!ctx->tool_name[0]) return;
    cJSON *arguments = cJSON_Parse(ctx->tool_args[0] ? ctx->tool_args : "{}");
    if (!arguments) {
        ESP_LOGE(TAG, "invalid arguments for tool %s", ctx->tool_name);
        return;
    }
    char *normalized = cJSON_PrintUnformatted(arguments);
    cJSON_Delete(arguments);
    for (size_t i = 0; i < s_function_count; ++i) {
        if (strcmp(s_functions[i].name, ctx->tool_name) == 0) {
            ESP_LOGI(TAG, "calling tool: %s", ctx->tool_name);
            esp_err_t result = s_functions[i].callback(normalized, s_functions[i].ctx);
            ESP_LOGI(TAG, "tool %s result: %s", ctx->tool_name, esp_err_to_name(result));
            cJSON_free(normalized);
            ctx->tool_name[0] = '\0'; ctx->tool_args_len = 0; ctx->tool_args[0] = '\0';
            return;
        }
    }
    ESP_LOGW(TAG, "unregistered tool: %s", ctx->tool_name);
    cJSON_free(normalized);
}

static void parse_sse_json(stream_ctx_t *ctx, const char *json_text)
{
    cJSON *root = cJSON_Parse(json_text);
    if (!root) { ESP_LOGW(TAG, "invalid SSE JSON"); return; }
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
        if (cJSON_IsString(message)) queue_text(message->valuestring);
        cJSON_Delete(root); return;
    }
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *delta = choice ? cJSON_GetObjectItemCaseSensitive(choice, "delta") : NULL;
    cJSON *content = delta ? cJSON_GetObjectItemCaseSensitive(delta, "content") : NULL;
    if (cJSON_IsString(content) && content->valuestring) {
        queue_text(content->valuestring);
        append_text(ctx->response, &ctx->response_len, sizeof(ctx->response), content->valuestring);
    }
    cJSON *tools = delta ? cJSON_GetObjectItemCaseSensitive(delta, "tool_calls") : NULL;
    cJSON *tool = cJSON_IsArray(tools) ? cJSON_GetArrayItem(tools, 0) : NULL;
    cJSON *function = tool ? cJSON_GetObjectItemCaseSensitive(tool, "function") : NULL;
    cJSON *name = function ? cJSON_GetObjectItemCaseSensitive(function, "name") : NULL;
    cJSON *arguments = function ? cJSON_GetObjectItemCaseSensitive(function, "arguments") : NULL;
    if (cJSON_IsString(name)) strlcpy(ctx->tool_name, name->valuestring, sizeof(ctx->tool_name));
    if (cJSON_IsString(arguments)) append_text(ctx->tool_args, &ctx->tool_args_len,
                                               sizeof(ctx->tool_args), arguments->valuestring);
    cJSON *finish = choice ? cJSON_GetObjectItemCaseSensitive(choice, "finish_reason") : NULL;
    if (cJSON_IsString(finish) && strcmp(finish->valuestring, "tool_calls") == 0) execute_tool(ctx);
    cJSON_Delete(root);
}

static void process_line(stream_ctx_t *ctx)
{
    ctx->line[ctx->line_len] = '\0';
    if (strncmp(ctx->line, "data:", 5) == 0) {
        const char *data = ctx->line + 5;
        while (*data == ' ') ++data;
        if (strcmp(data, "[DONE]") == 0) execute_tool(ctx);
        else parse_sse_json(ctx, data);
    }
    ctx->line_len = 0;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    stream_ctx_t *ctx = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        for (int i = 0; i < event->data_len; ++i) {
            char ch = ((char *)event->data)[i];
            if (ch == '\n') process_line(ctx);
            else if (ch != '\r' && ctx->line_len < sizeof(ctx->line) - 1) ctx->line[ctx->line_len++] = ch;
        }
    } else if (event->event_id == HTTP_EVENT_DISCONNECTED && ctx->line_len) process_line(ctx);
    return ESP_OK;
}

static cJSON *build_request(const request_t *request)
{
    cJSON *root = cJSON_CreateObject(); cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON_AddStringToObject(root, "model", CONFIG_JULIA_AI_MODEL);
    cJSON_AddBoolToObject(root, "stream", true);
    if (s_function_count) {
        cJSON *tools = cJSON_AddArrayToObject(root, "tools");
        for (size_t i = 0; i < s_function_count; ++i) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON *function = cJSON_AddObjectToObject(tool, "function");
            cJSON_AddStringToObject(function, "name", s_functions[i].name);
            cJSON_AddStringToObject(function, "description", "Julia device control function");
            cJSON *parameters = cJSON_AddObjectToObject(function, "parameters");
            cJSON_AddStringToObject(parameters, "type", "object");
            if (strcmp(s_functions[i].name, "control_home") == 0) {
                cJSON *properties = cJSON_AddObjectToObject(parameters, "properties");
                cJSON *action = cJSON_AddObjectToObject(properties, "action");
                cJSON_AddStringToObject(action, "type", "string");
                cJSON *actions = cJSON_AddArrayToObject(action, "enum");
                const char *action_names[] = {"light_on","light_off","ac_on","ac_off",
                                              "curtain_set","socket_on","socket_off"};
                for (size_t a = 0; a < sizeof(action_names)/sizeof(action_names[0]); ++a)
                    cJSON_AddItemToArray(actions, cJSON_CreateString(action_names[a]));
                cJSON *device = cJSON_AddObjectToObject(properties, "device");
                cJSON_AddStringToObject(device, "type", "string");
                cJSON *devices = cJSON_AddArrayToObject(device, "enum");
                const char *device_names[] = {"living_room","bedroom","study","kitchen"};
                for (size_t d = 0; d < sizeof(device_names)/sizeof(device_names[0]); ++d)
                    cJSON_AddItemToArray(devices, cJSON_CreateString(device_names[d]));
                const char *numbers[] = {"brightness","color_temp","mode","temp","fan_speed","percent"};
                for (size_t n = 0; n < sizeof(numbers)/sizeof(numbers[0]); ++n) {
                    cJSON *value = cJSON_AddObjectToObject(properties, numbers[n]);
                    cJSON_AddStringToObject(value, "type", "integer");
                }
                cJSON *confirmed = cJSON_AddObjectToObject(properties, "confirmed");
                cJSON_AddStringToObject(confirmed, "type", "boolean");
                cJSON *required = cJSON_AddArrayToObject(parameters, "required");
                cJSON_AddItemToArray(required, cJSON_CreateString("action"));
                cJSON_AddItemToArray(required, cJSON_CreateString("device"));
                cJSON_AddBoolToObject(parameters, "additionalProperties", false);
            } else {
                cJSON_AddBoolToObject(parameters, "additionalProperties", true);
            }
            cJSON_AddItemToArray(tools, tool);
        }
    }
    if (request->system && request->system[0]) {
        cJSON *m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "system");
        cJSON_AddStringToObject(m, "content", request->system); cJSON_AddItemToArray(messages, m);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_history_count; ++i) {
        cJSON *u=cJSON_CreateObject(); cJSON_AddStringToObject(u,"role","user"); cJSON_AddStringToObject(u,"content",s_history[i].user); cJSON_AddItemToArray(messages,u);
        cJSON *a=cJSON_CreateObject(); cJSON_AddStringToObject(a,"role","assistant"); cJSON_AddStringToObject(a,"content",s_history[i].assistant); cJSON_AddItemToArray(messages,a);
    }
    xSemaphoreGive(s_lock);
    cJSON *user=cJSON_CreateObject(); cJSON_AddStringToObject(user,"role","user"); cJSON_AddStringToObject(user,"content",request->user); cJSON_AddItemToArray(messages,user);
    return root;
}

static void save_history(const char *user, const char *assistant)
{
    if (!assistant[0]) return;
    char *user_copy = strdup(user ? user : "");
    char *assistant_copy = strdup(assistant);
    if (!user_copy || !assistant_copy) {
        free(user_copy);
        free(assistant_copy);
        ESP_LOGW(TAG, "history allocation failed; dropping completed turn");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_history_count == HISTORY_ROUNDS) {
        free(s_history[0].user); free(s_history[0].assistant);
        memmove(&s_history[0], &s_history[1], sizeof(history_t) * (HISTORY_ROUNDS - 1));
        --s_history_count;
    }
    s_history[s_history_count++] = (history_t){user_copy, assistant_copy};
    xSemaphoreGive(s_lock);
}

static void request_task(void *arg)
{
    request_t *request = arg;
    stream_ctx_t *stream = heap_caps_calloc(1, sizeof(*stream),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stream) {
        queue_text("AI memory allocation failed");
        free(request->user); free(request->system); free(request); s_busy = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    for (int i = 0; i < 30 && time(NULL) < 1704067200; ++i) {
        if (i == 0) ESP_LOGI(TAG, "Waiting for SNTP before AI TLS request");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (time(NULL) < 1704067200) {
        ESP_LOGE(TAG, "System clock is not synchronized");
        queue_text("AI service unavailable");
        ai_chunk_t done = {0}; xQueueSend(s_chunks, &done, pdMS_TO_TICKS(100));
        free(stream); free(request->user); free(request->system); free(request);
        s_busy = false; vTaskDeleteWithCaps(NULL); return;
    }
    if (julia_wifi_wait_cloud_ready(5000) != ESP_OK) {
        ESP_LOGE(TAG, "Cloud network is not stable: RSSI=%d dBm", julia_wifi_get_rssi());
        queue_text("网络不稳定，请稍后再试");
        ai_chunk_t done = {0}; xQueueSend(s_chunks, &done, pdMS_TO_TICKS(100));
        free(stream); free(request->user); free(request->system); free(request);
        s_busy = false; vTaskDeleteWithCaps(NULL); return;
    }
    cJSON *json = build_request(request);
    if (!json) {
        queue_text("AI请求内存不足");
        ai_chunk_t done = {0}; xQueueSend(s_chunks, &done, pdMS_TO_TICKS(100));
        free(stream); free(request->user); free(request->system); free(request);
        s_busy = false; vTaskDeleteWithCaps(NULL); return;
    }
    char *json_body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!json_body) {
        queue_text("AI memory allocation failed");
        ai_chunk_t done = {0}; xQueueSend(s_chunks, &done, pdMS_TO_TICKS(100));
        free(stream); free(request->user); free(request->system); free(request);
        s_busy = false; vTaskDeleteWithCaps(NULL); return;
    }
    size_t body_size = strlen(json_body);
    char *body = heap_caps_malloc(body_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body) memcpy(body, json_body, body_size + 1);
    cJSON_free(json_body);
    if (!body) {
        queue_text("AI memory allocation failed");
        ai_chunk_t done = {0}; xQueueSend(s_chunks, &done, pdMS_TO_TICKS(100));
        free(stream); free(request->user); free(request->system); free(request);
        s_busy = false; vTaskDeleteWithCaps(NULL); return;
    }
    esp_http_client_config_t cfg = {.url=s_url, .method=HTTP_METHOD_POST, .timeout_ms=30000,
        .event_handler=http_event, .user_data=stream, .crt_bundle_attach=esp_crt_bundle_attach,
        .buffer_size=2048, .buffer_size_tx=2048};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "failed to initialize HTTP client");
        queue_text("AI网络客户端初始化失败");
        ai_chunk_t done = {0};
        xQueueSend(s_chunks, &done, pdMS_TO_TICKS(100));
        free(stream);
        free(body);
        free(request->user);
        free(request->system);
        free(request);
        s_busy = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    s_active_client = client;
    char auth[224]; snprintf(auth,sizeof(auth),"Bearer %s",s_key);
    esp_http_client_set_header(client,"Authorization",auth);
    esp_http_client_set_header(client,"Content-Type","application/json");
    esp_http_client_set_header(client,"Accept","text/event-stream");
    esp_http_client_set_post_field(client,body,body_size);
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) { ESP_LOGE(TAG,"request failed: %s",esp_err_to_name(err)); queue_text("AI service unavailable"); }
    else if (esp_http_client_get_status_code(client) != 200) { ESP_LOGE(TAG,"HTTP status %d",esp_http_client_get_status_code(client)); }
    else save_history(request->user,stream->response);
    ai_chunk_t done = {0};
    xQueueSend(s_chunks, &done, pdMS_TO_TICKS(100));
    esp_http_client_cleanup(client); s_active_client=NULL; free(stream); free(body); free(request->user); free(request->system); free(request); s_busy=false;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t julia_ai_init(const char *api_url, const char *api_key)
{
    if (!api_url || !api_url[0] || !api_key || !api_key[0]) return ESP_ERR_INVALID_ARG;
    if (s_ready || s_chunks || s_lock) return ESP_ERR_INVALID_STATE;
    strlcpy(s_url,api_url,sizeof(s_url)); strlcpy(s_key,api_key,sizeof(s_key));
    /* Keep receiving SSE deltas while the voice task synthesizes and plays
     * the first sentence. Short model deltas can otherwise fill 16 slots and
     * silently drop the remainder of the answer. */
    s_chunks = xQueueCreate(64, sizeof(ai_chunk_t));
    if (!s_chunks) return ESP_ERR_NO_MEM;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        vQueueDelete(s_chunks);
        s_chunks = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t julia_ai_init_qwen(const char *api_key)
{
    return julia_ai_init(JULIA_QWEN_CHAT_COMPLETIONS_URL, api_key);
}
esp_err_t julia_ai_chat_start(void) { return s_ready ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t julia_ai_send_message(const char *user_text, const char *system_prompt)
{
    if (!s_ready || s_closing || !s_chunks || !s_lock) return ESP_ERR_INVALID_STATE;
    if (!user_text || !user_text[0]) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    request_t *r = calloc(1, sizeof(*r));
    if (!r) return ESP_ERR_NO_MEM;
    r->user=strdup(user_text); r->system=strdup(system_prompt?system_prompt:"");
    if(!r->user||!r->system){free(r->user);free(r->system);free(r);return ESP_ERR_NO_MEM;} s_busy=true; xQueueReset(s_chunks);
    if(xTaskCreateWithCaps(request_task,"ai_request",16384,r,5,NULL,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)!=pdPASS){s_busy=false;free(r->user);free(r->system);free(r);return ESP_ERR_NO_MEM;} return ESP_OK;
}
esp_err_t julia_ai_receive_chunk(char *buffer, size_t len)
{
    if (!buffer || len < 2) return ESP_ERR_INVALID_ARG;
    if (!s_ready || !s_chunks) return ESP_ERR_INVALID_STATE;
    ai_chunk_t item;
#if 0
    if(xQueueReceive(s_chunks,&item,pdMS_TO_TICKS(20000))!=pdTRUE){
        strlcpy(buffer,"网络有点慢",len); return ESP_ERR_TIMEOUT;
    }
    if (!item.text[0]) return ESP_ERR_NOT_FINISHED;
#endif
    if (xQueueReceive(s_chunks, &item, pdMS_TO_TICKS(20000)) != pdTRUE) {
        strlcpy(buffer, "AI request timeout", len);
        return ESP_ERR_TIMEOUT;
    }
    if (!item.text[0]) return ESP_ERR_NOT_FINISHED;
    strlcpy(buffer,item.text,len); return ESP_OK;
}
esp_err_t julia_ai_cancel_request(void)
{
    esp_http_client_handle_t client = s_active_client;
    if (!s_busy) return ESP_OK;
    if (!client) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "cancelling active AI request");
    return esp_http_client_close(client);
}
esp_err_t julia_ai_register_function(const char *name,julia_ai_function_cb_t cb,void *ctx)
{
    if(!name||!cb||s_function_count>=FUNCTION_COUNT)return ESP_ERR_INVALID_ARG;
    strlcpy(s_functions[s_function_count].name,name,sizeof(s_functions[0].name)); s_functions[s_function_count].callback=cb; s_functions[s_function_count++].ctx=ctx; return ESP_OK;
}
void julia_ai_close(void)
{
    s_closing=true; if(s_active_client)esp_http_client_close(s_active_client); s_ready=false;
    for(size_t i=0;i<s_history_count;++i){free(s_history[i].user);free(s_history[i].assistant);} s_history_count=0;
}
