#ifndef SCR_TEXT_H
#define SCR_TEXT_H

#include "lvgl.h"

void      scr_text_create(void);
lv_obj_t *scr_text_get(void);
void      scr_text_set(const char *text, const char *hex_color, int scale);

#endif
