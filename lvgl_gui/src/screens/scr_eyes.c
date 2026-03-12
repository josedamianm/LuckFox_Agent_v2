#include "scr_eyes.h"
#include "../anim/eyes_anim.h"

static lv_obj_t *scr;

void scr_eyes_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    eyes_anim_init(scr);
}

lv_obj_t *scr_eyes_get(void) {
    return scr;
}
