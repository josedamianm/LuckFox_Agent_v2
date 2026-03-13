#include "scr_manager.h"
#include "scr_status.h"
#include "scr_eyes.h"
#include "scr_emoji.h"
#include "scr_text.h"
#include "scr_image.h"
#include "scr_menu.h"
#include "scr_chat.h"
#include "lvgl.h"
#include <string.h>

static scr_id_t current_scr = SCR_STATUS;

static const char *scr_names[SCR_COUNT] = {
    "status", "eyes", "emoji", "text", "image", "menu", "chat"
};

void scr_manager_init(void) {
    scr_status_create();
    scr_eyes_create();
    scr_emoji_create();
    scr_text_create();
    scr_image_create();
    scr_menu_create();
    scr_chat_create();
}

void scr_manager_switch_dir(scr_id_t id, scr_dir_t dir) {
    if (id >= SCR_COUNT) return;

    lv_scr_load_anim_t anim;
    if (id == current_scr) {
        anim = LV_SCR_LOAD_ANIM_NONE;
    } else if (dir == SCR_DIR_LEFT) {
        anim = LV_SCR_LOAD_ANIM_MOVE_LEFT;
    } else if (dir == SCR_DIR_RIGHT) {
        anim = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
    } else if (dir == SCR_DIR_FADE) {
        anim = LV_SCR_LOAD_ANIM_FADE_IN;
    } else {
        /* SCR_DIR_AUTO: higher index → slide left (forward), lower → slide right (back) */
        anim = (id > current_scr) ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
    }

    current_scr = id;

    lv_obj_t *scr = NULL;
    switch (id) {
        case SCR_STATUS: scr = scr_status_get(); break;
        case SCR_EYES:   scr = scr_eyes_get();   break;
        case SCR_EMOJI:  scr = scr_emoji_get();  break;
        case SCR_TEXT:   scr = scr_text_get();   break;
        case SCR_IMAGE:  scr = scr_image_get();  break;
        case SCR_MENU:   scr = scr_menu_get();   break;
        case SCR_CHAT:   scr = scr_chat_get();   break;
        default: return;
    }
    if (scr) lv_screen_load_anim(scr, anim, 250, 0, false);
}

void scr_manager_switch(scr_id_t id) {
    scr_manager_switch_dir(id, SCR_DIR_AUTO);
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

void scr_manager_gif_start(int frame_count) {
    scr_image_gif_start(frame_count);
}

void scr_manager_gif_frame(int idx, const char *path, int duration_ms) {
    scr_image_gif_frame(idx, path, duration_ms);
}

void scr_manager_set_chat_state(int state) {
    scr_chat_set_state((chat_state_t)state);
}

void scr_manager_set_chat_text(const char *text) {
    scr_chat_set_text(text);
}
