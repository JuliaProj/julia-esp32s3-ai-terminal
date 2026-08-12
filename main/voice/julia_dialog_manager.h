#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char history[10][256];
    uint8_t turn_count;
    bool in_conversation;
} julia_dialog_context_t;

void dialog_manager_init(void);
void dialog_manager_input(const char *user_text);
void dialog_manager_llm_response(const char *ai_text);
const char *dialog_manager_get_context(void);
void dialog_manager_reset(void);
bool dialog_manager_is_in_conversation(void);
void dialog_manager_get_status(julia_dialog_context_t *status);

