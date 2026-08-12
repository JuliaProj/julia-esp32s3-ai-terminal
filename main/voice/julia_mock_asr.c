#include "julia_mock_asr.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "julia_dialog_manager.h"

static SemaphoreHandle_t s_lock;
static char s_result[512];
static bool s_active;

void mock_asr_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

void mock_asr_input(const char *text)
{
    if (!s_lock) mock_asr_init();
    if (!s_lock || !text || !text[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_result, text, sizeof(s_result));
    s_active = true;
    xSemaphoreGive(s_lock);
    dialog_manager_input(text);
}

bool mock_asr_is_active(void) { return s_active; }
const char *mock_asr_get_result(void) { return s_result; }

void mock_asr_clear(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_result[0] = '\0';
    s_active = false;
    xSemaphoreGive(s_lock);
}
