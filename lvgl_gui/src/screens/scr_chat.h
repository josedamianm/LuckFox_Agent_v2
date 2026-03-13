#ifndef SCR_CHAT_H
#define SCR_CHAT_H

#include "lvgl.h"

typedef enum {
    CHAT_IDLE = 0,
    CHAT_LISTENING,
    CHAT_THINKING,
    CHAT_SPEAKING,
    CHAT_STATE_COUNT
} chat_state_t;

void       scr_chat_create(void);
lv_obj_t  *scr_chat_get(void);
void       scr_chat_set_state(chat_state_t state);
void       scr_chat_set_text(const char *text);

#endif
