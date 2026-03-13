#include "scr_menu.h"
#include "scr_manager.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Card definitions — one card per mode, in navigation order.
 * accent_hex is 0xRRGGBB converted to lv_color_t at build time.
 * ----------------------------------------------------------------------- */
#define CARD_COUNT 5

typedef struct {
    scr_id_t    target;
    const char *title;
    const char *subtitle;
    uint32_t    accent_hex;
    const char *symbol;
} card_def_t;

static const card_def_t CARDS[CARD_COUNT] = {
    { SCR_EYES,   "Eyes",   "Animated face",   0x00CCFF, LV_SYMBOL_EYE_OPEN },
    { SCR_STATUS, "Status", "System info",      0x4488FF, LV_SYMBOL_HOME     },
    { SCR_EMOJI,  "Emoji",  "Quick icons",      0xFFCC00, LV_SYMBOL_IMAGE    },
    { SCR_TEXT,   "Text",   "Show message",     0x00CC80, LV_SYMBOL_EDIT     },
    { SCR_IMAGE,  "Image",  "Display picture",  0xFF4080, LV_SYMBOL_FILE     },
};

static lv_obj_t *scr;
static lv_obj_t *tv;
static lv_obj_t *tiles[CARD_COUNT];
static lv_obj_t *dots[CARD_COUNT];
static int        cur_idx = 0;

/* ----------------------------------------------------------------------- */

static void update_dots(int idx) {
    for (int i = 0; i < CARD_COUNT; i++) {
        lv_obj_set_style_bg_color(dots[i],
            i == idx ? lv_color_white() : lv_color_make(0x44, 0x44, 0x44), 0);
    }
}

/* Fires when the tileview snaps to a new tile (scroll ends). */
static void tile_changed_cb(lv_event_t *e) {
    (void)e;
    lv_obj_t *tile = lv_tileview_get_tile_act(tv);
    for (int i = 0; i < CARD_COUNT; i++) {
        if (tiles[i] == tile) {
            cur_idx = i;
            update_dots(i);
            break;
        }
    }
}

/* Hardware button → LEFT/RIGHT: navigate cards; ENTER: enter screen. */
static void key_cb(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);

    if (key == LV_KEY_LEFT && cur_idx > 0) {
        cur_idx--;
        lv_obj_scroll_to_view(tiles[cur_idx], LV_ANIM_ON);
        update_dots(cur_idx);
    } else if (key == LV_KEY_RIGHT && cur_idx < CARD_COUNT - 1) {
        cur_idx++;
        lv_obj_scroll_to_view(tiles[cur_idx], LV_ANIM_ON);
        update_dots(cur_idx);
    } else if (key == LV_KEY_ENTER) {
        /* Always slide left when entering a screen from the menu. */
        scr_manager_switch_dir(CARDS[cur_idx].target, SCR_DIR_LEFT);
    }
}

/* ----------------------------------------------------------------------- */

void scr_menu_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Tileview: 240×220, leaves 20px strip at bottom for dots. */
    tv = lv_tileview_create(scr);
    lv_obj_set_size(tv, 240, 220);
    lv_obj_set_pos(tv, 0, 0);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(tv, key_cb, LV_EVENT_KEY, NULL);

    /* --- Build one card tile per mode --- */
    for (int i = 0; i < CARD_COUNT; i++) {
        lv_color_t accent = lv_color_hex(CARDS[i].accent_hex);

        lv_obj_t *tile = lv_tileview_add_tile(tv, i, 0, LV_DIR_HOR);
        tiles[i] = tile;
        lv_obj_set_style_bg_color(tile, lv_color_make(0x0A, 0x0A, 0x0A), 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        /* Accent circle with symbol */
        lv_obj_t *circle = lv_obj_create(tile);
        lv_obj_set_size(circle, 90, 90);
        lv_obj_align(circle, LV_ALIGN_CENTER, 0, -40);
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(circle, accent, 0);
        lv_obj_set_style_bg_opa(circle, LV_OPA_20, 0);
        lv_obj_set_style_border_color(circle, accent, 0);
        lv_obj_set_style_border_width(circle, 2, 0);
        lv_obj_set_style_pad_all(circle, 0, 0);
        lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sym = lv_label_create(circle);
        lv_label_set_text(sym, CARDS[i].symbol);
        lv_obj_set_style_text_color(sym, accent, 0);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_32, 0);
        lv_obj_center(sym);

        /* Title */
        lv_obj_t *title = lv_label_create(tile);
        lv_label_set_text(title, CARDS[i].title);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 45);

        /* Subtitle */
        lv_obj_t *sub = lv_label_create(tile);
        lv_label_set_text(sub, CARDS[i].subtitle);
        lv_obj_set_style_text_color(sub, lv_color_make(0x88, 0x88, 0x88), 0);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_align(sub, LV_ALIGN_CENTER, 0, 72);
    }

    /* --- Dot page indicators ---
     * 5 dots × 8px + 4 gaps × 6px = 64px total → start_x = 88
     * Vertically centered in the 20px strip (y 220-240) → y = 226
     */
    for (int i = 0; i < CARD_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, 88 + i * 14, 226);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot,
            i == 0 ? lv_color_white() : lv_color_make(0x44, 0x44, 0x44), 0);
        dots[i] = dot;
    }

    /* Register tileview in the default keypad group so it receives keys. */
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_add_obj(g, tv);
}

lv_obj_t *scr_menu_get(void) {
    return scr;
}
