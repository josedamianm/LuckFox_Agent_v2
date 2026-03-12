#include "scr_emoji.h"
#include <stdio.h>

static lv_obj_t *scr;
static lv_obj_t *img;
static char current_path[128];

void scr_emoji_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    img = lv_image_create(scr);
    lv_obj_center(img);
}

lv_obj_t *scr_emoji_get(void) {
    return scr;
}

void scr_emoji_set(const char *name) {
    snprintf(current_path, sizeof(current_path), "S:emoji/%s.png", name);
    lv_image_set_src(img, current_path);
    lv_obj_center(img);
}
