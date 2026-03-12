#include "scr_menu.h"
#include "scr_manager.h"

static lv_obj_t *scr;
static lv_obj_t *list;

static void menu_btn_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch (idx) {
        case 0: scr_manager_switch(SCR_STATUS); break;
        case 1: scr_manager_switch(SCR_EYES);   break;
        case 2: scr_manager_switch(SCR_EMOJI);   break;
        case 3: scr_manager_switch(SCR_TEXT);    break;
        default: break;
    }
}

void scr_menu_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Menu");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    list = lv_list_create(scr);
    lv_obj_set_size(list, 220, 190);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(list, lv_color_make(0x10, 0x10, 0x10), 0);
    lv_obj_set_style_border_width(list, 0, 0);

    static const char *items[] = {"Status", "Eyes", "Emoji", "Text"};
    lv_group_t *g = lv_group_get_default();

    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, items[i]);
        lv_obj_add_event_cb(btn, menu_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_style_bg_color(btn, lv_color_make(0x20, 0x20, 0x20), 0);
        lv_obj_set_style_text_color(btn, lv_color_white(), 0);
        if (g) lv_group_add_obj(g, btn);
    }
}

lv_obj_t *scr_menu_get(void) {
    return scr;
}
