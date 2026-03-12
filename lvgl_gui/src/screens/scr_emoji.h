#ifndef SCR_EMOJI_H
#define SCR_EMOJI_H

#include "lvgl.h"

void      scr_emoji_create(void);
lv_obj_t *scr_emoji_get(void);
void      scr_emoji_set(const char *name);

#endif
