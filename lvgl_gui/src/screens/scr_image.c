#include "scr_image.h"
#include <string.h>
#include <stdio.h>

static lv_obj_t *scr;
static lv_obj_t *img;
static char current_path[128];

void scr_image_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    img = lv_image_create(scr);
    lv_obj_center(img);
}

lv_obj_t *scr_image_get(void) {
    return scr;
}

void scr_image_set(const char *path) {
    if (!path) return;
    if (path[0] == '/') {
        snprintf(current_path, sizeof(current_path), "%s", path);
    } else {
        snprintf(current_path, sizeof(current_path), "S:%s", path);
    }
    lv_image_set_src(img, current_path);
    lv_obj_center(img);
}
