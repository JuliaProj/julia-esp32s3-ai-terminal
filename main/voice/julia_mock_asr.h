#pragma once

#include <stdbool.h>

void mock_asr_init(void);
void mock_asr_input(const char *text);
bool mock_asr_is_active(void);
const char *mock_asr_get_result(void);
void mock_asr_clear(void);
