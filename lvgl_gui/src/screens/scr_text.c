#include "scr_text.h"
#include <stdlib.h>
#include <string.h>

static lv_obj_t *scr;
static lv_obj_t *lbl;

static lv_color_t parse_hex_color(const char *hex) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7)
        return lv_color_white();
    unsigned long v = strtoul(hex + 1, NULL, 16);
    return lv_color_make((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void scr_text_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lbl = lv_label_create(scr);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 230);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl, "");
}

lv_obj_t *scr_text_get(void) {
    return scr;
}

void scr_text_set(const char *text, const char *hex_color, int scale) {
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, parse_hex_color(hex_color), 0);

    const lv_font_t *font = &lv_font_montserrat_24;
    if      (scale >= 5) font = &lv_font_montserrat_48;
    else if (scale >= 4) font = &lv_font_montserrat_32;
    else if (scale >= 3) font = &lv_font_montserrat_24;
    else if (scale >= 2) font = &lv_font_montserrat_16;
    else                 font = &lv_font_montserrat_12;
    lv_obj_set_style_text_font(lbl, font, 0);
}
