#ifndef EYES_ANIM_H
#define EYES_ANIM_H

#include "lvgl.h"

void eyes_anim_init(lv_obj_t *parent);
void eyes_anim_set_gaze(int zone);
void eyes_anim_blink(void);

#endif
