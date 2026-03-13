#include "scr_menu.h"
#include "scr_manager.h"
#include "scr_chat.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Card definitions — one card per navigable screen, in swipe order.
 * ----------------------------------------------------------------------- */
#define CARD_COUNT 6

typedef struct {
    scr_id_t    target;
    const char *title;
    uint32_t    accent_hex;
    const char *symbol;       /* LVGL built-in symbol or single UTF-8 char */
} card_def_t;

static const card_def_t CARDS[CARD_COUNT] = {
    { SCR_EYES,   "Eyes",    0x00CCFF, LV_SYMBOL_EYE_OPEN },
    { SCR_STATUS, "Status",  0x4488FF, LV_SYMBOL_HOME     },
    { SCR_EMOJI,  "Emoji",   0xFFCC00, LV_SYMBOL_IMAGE    },
    { SCR_TEXT,   "Text",    0x00CC80, LV_SYMBOL_EDIT     },
    { SCR_IMAGE,  "Image",   0xFF4080, LV_SYMBOL_FILE     },
    { SCR_CHAT,   "AI Chat", 0xCC44FF, LV_SYMBOL_AUDIO    },
};

static lv_obj_t *scr;
static lv_obj_t *tv;
static lv_obj_t *tiles[CARD_COUNT];
static lv_obj_t *dots[CARD_COUNT];
static int        cur_idx = 0;

/* ----------------------------------------------------------------------- */

static void update_dots(int idx) {
    for (int i = 0; i < CARD_COUNT; i++) {
        lv_color_t c = (i == idx)
            ? lv_color_hex(CARDS[i].accent_hex)
            : lv_color_make(0x28, 0x28, 0x28);
        lv_obj_set_style_bg_color(dots[i], c, 0);
        /* Active dot is wider (pill shape) */
        lv_obj_set_width(dots[i], i == idx ? 14 : 7);
    }
}

/* Fires when the tileview snaps to a new tile. */
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

/* Hardware button navigation: LEFT/RIGHT swipe, ENTER enters screen. */
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
        scr_manager_switch_dir(CARDS[cur_idx].target, SCR_DIR_LEFT);
    }
}

/* ----------------------------------------------------------------------- */

/*
 * Card layout (240×240 display, tile area = 240×224, dot bar = 16px):
 *
 *   ┌────────────────────────┐
 *   │                        │  ← subtle accent tint (5% opacity)
 *   │                        │
 *   │      [symbol 48px]     │  ← y ≈ center - 32
 *   │                        │
 *   │       Title  32px      │  ← y ≈ center + 28
 *   │                        │
 *   ├────────────────────────┤  ← 4px accent bottom line
 *   │   ● ● ● ● ●            │  ← dot bar  (16px)
 *   └────────────────────────┘
 */
void scr_menu_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Tileview: 240×224, leaving 16px strip at bottom for dots. */
    tv = lv_tileview_create(scr);
    lv_obj_set_size(tv, 240, 224);
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
        lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        /* Subtle accent background tint */
        lv_obj_t *tint = lv_obj_create(tile);
        lv_obj_set_size(tint, 240, 224);
        lv_obj_set_pos(tint, 0, 0);
        lv_obj_set_style_bg_color(tint, accent, 0);
        lv_obj_set_style_bg_opa(tint, LV_OPA_5, 0);
        lv_obj_set_style_border_width(tint, 0, 0);
        lv_obj_set_style_radius(tint, 0, 0);
        lv_obj_clear_flag(tint, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* Accent bottom line (4px) */
        lv_obj_t *line = lv_obj_create(tile);
        lv_obj_set_size(line, 240, 4);
        lv_obj_set_pos(line, 0, 220);
        lv_obj_set_style_bg_color(line, accent, 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_60, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* Large symbol — primary visual element */
        lv_obj_t *sym = lv_label_create(tile);
        lv_label_set_text(sym, CARDS[i].symbol);
        lv_obj_set_style_text_color(sym, accent, 0);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_48, 0);
        lv_obj_align(sym, LV_ALIGN_CENTER, 0, -32);

        /* Title — large, white */
        lv_obj_t *title = lv_label_create(tile);
        lv_label_set_text(title, CARDS[i].title);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 36);

        /* Card index hint (tiny, muted) */
        lv_obj_t *idx_lbl = lv_label_create(tile);
        lv_label_set_text_fmt(idx_lbl, "%d / %d", i + 1, CARD_COUNT);
        lv_obj_set_style_text_color(idx_lbl, lv_color_make(0x22, 0x22, 0x22), 0);
        lv_obj_set_style_text_font(idx_lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(idx_lbl, LV_ALIGN_BOTTOM_RIGHT, -10, -20);
    }

    /* --- Dot page indicators (pill-style, accent-colored active dot) ---
     *
     * 5 dots: active = 14px wide, inactive = 7px, gap = 6px
     * Total width at rest: 5×7 + 4×6 = 59px → start_x ≈ 91
     * Strip: y 224–240, center at y=232 → dot top = 228
     */
    int dot_x = 84;
    for (int i = 0; i < CARD_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, i == 0 ? 14 : 7, 7);
        lv_obj_set_pos(dot, dot_x, 229);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot,
            i == 0 ? lv_color_hex(CARDS[0].accent_hex)
                   : lv_color_make(0x28, 0x28, 0x28), 0);
        dots[i] = dot;
        dot_x += (i == 0 ? 14 : 7) + 6;
    }

    /* Register tileview in the default keypad group so it receives keys. */
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_add_obj(g, tv);
}

lv_obj_t *scr_menu_get(void) {
    return scr;
}
