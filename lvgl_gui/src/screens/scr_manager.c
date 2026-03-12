#include "scr_manager.h"
#include "scr_status.h"
#include "scr_eyes.h"
#include "scr_emoji.h"
#include "scr_text.h"
#include "scr_image.h"
#include "scr_menu.h"
#include "lvgl.h"
#include <string.h>

static scr_id_t current_scr = SCR_STATUS;

static const char *scr_names[SCR_COUNT] = {
    "status", "eyes", "emoji", "text", "image", "menu"
};

void scr_manager_init(void) {
    scr_status_create();
    scr_eyes_create();
    scr_emoji_create();
    scr_text_create();
    scr_image_create();
    scr_menu_create();
}

void scr_manager_switch(scr_id_t id) {
    if (id >= SCR_COUNT) return;
    current_scr = id;

    lv_obj_t *scr = NULL;
    switch (id) {
        case SCR_STATUS: scr = scr_status_get(); break;
        case SCR_EYES:   scr = scr_eyes_get();   break;
        case SCR_EMOJI:  scr = scr_emoji_get();  break;
        case SCR_TEXT:   scr = scr_text_get();   break;
        case SCR_IMAGE:  scr = scr_image_get();  break;
        case SCR_MENU:   scr = scr_menu_get();   break;
        default: return;
    }
    if (scr) lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

scr_id_t scr_manager_current(void) {
    return current_scr;
}

const char *scr_manager_current_name(void) {
    return scr_names[current_scr];
}

void scr_manager_set_text(const char *text, const char *color, int scale) {
    scr_text_set(text, color, scale);
}

void scr_manager_set_emoji(const char *name) {
    scr_emoji_set(name);
}

void scr_manager_set_image(const char *path) {
    scr_image_set(path);
}
