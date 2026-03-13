#include "scr_chat.h"
#include <string.h>

/* ── Layout constants (all in pixels, 240×240 display) ──────────────────
 *
 *  [ 4px top bar  — accent color per state          ]  y=0
 *  [ state label  — 12px centered                   ]  y=8
 *  [ "AI CHAT"    — 12px muted                      ]  y=24
 *  [ ─────────────────────────────────────────────  ]  y=38 (1px divider)
 *  [ content area — 240×190                         ]  y=40..230
 *  [ bottom margin                                  ]  y=230..240
 *
 * ──────────────────────────────────────────────────────────────────────── */
#define TOP_BAR_H      4
#define CONTENT_Y     40
#define CONTENT_H    190

/* Listening bars */
#define LISTEN_BARS    7
#define BAR_W         14
#define BAR_GAP        9
/* Total bar row width: 7*14 + 6*9 = 152px  →  start_x = (240-152)/2 = 44 */
#define BAR_ROW_X     44
#define BAR_BOTTOM   170    /* bar bottom y within content panel */
#define BAR_MIN_H     12
#define BAR_MAX_H     90

/* Thinking arc */
#define ARC_SIZE      90
#define ARC_X        ((240 - ARC_SIZE) / 2)   /* 75 */
#define ARC_Y        ((CONTENT_H - ARC_SIZE) / 2 - 16)  /* 29 */

/* ── Private storage ─────────────────────────────────────────────────── */
static lv_obj_t   *scr;
static chat_state_t cur_state = CHAT_IDLE;

static lv_obj_t   *top_bar;
static lv_obj_t   *lbl_state;

/* One panel per state — shown/hidden when state changes */
static lv_obj_t   *panel_idle;
static lv_obj_t   *panel_listen;
static lv_obj_t   *panel_think;
static lv_obj_t   *panel_speak;

/* Listening bar objects + animation handles */
static lv_obj_t   *listen_bars[LISTEN_BARS];
static lv_anim_t   bar_anims[LISTEN_BARS];

/* Thinking arc + animation handle */
static lv_obj_t   *think_arc;
static lv_anim_t   arc_anim;

/* Speaking text label */
static lv_obj_t   *speak_lbl;

/* ── Color per state ─────────────────────────────────────────────────── */
static lv_color_t state_color(chat_state_t s) {
    switch (s) {
        case CHAT_IDLE:      return lv_color_make(0x44, 0x44, 0x44);
        case CHAT_LISTENING: return lv_color_make(0x00, 0xFF, 0x80);
        case CHAT_THINKING:  return lv_color_make(0xFF, 0x88, 0x00);
        case CHAT_SPEAKING:  return lv_color_make(0x00, 0xCC, 0xFF);
        default:             return lv_color_make(0x44, 0x44, 0x44);
    }
}

static const char *state_label(chat_state_t s) {
    switch (s) {
        case CHAT_IDLE:      return "IDLE";
        case CHAT_LISTENING: return "LISTENING";
        case CHAT_THINKING:  return "THINKING";
        case CHAT_SPEAKING:  return "SPEAKING";
        default:             return "IDLE";
    }
}

/* ── Animation callbacks ─────────────────────────────────────────────── */

/* Bar: animate height, keep bottom fixed */
static void bar_height_cb(void *obj, int32_t h) {
    lv_obj_set_height((lv_obj_t *)obj, h);
    lv_obj_set_y((lv_obj_t *)obj, BAR_BOTTOM - h);
}

/* Arc: animate rotation (0.1° units, 3600 = 360°) */
static void arc_rot_cb(void *obj, int32_t v) {
    lv_obj_set_style_transform_rotation((lv_obj_t *)obj, v, 0);
}

/* ── Animation start/stop helpers ───────────────────────────────────── */

static void start_listen_anims(void) {
    static const uint32_t delays_ms[LISTEN_BARS] = {0, 100, 200, 300, 150, 50, 250};
    for (int i = 0; i < LISTEN_BARS; i++) {
        lv_anim_init(&bar_anims[i]);
        lv_anim_set_var(&bar_anims[i], listen_bars[i]);
        lv_anim_set_values(&bar_anims[i], BAR_MIN_H, BAR_MAX_H);
        lv_anim_set_duration(&bar_anims[i], 650);
        lv_anim_set_delay(&bar_anims[i], delays_ms[i]);
        lv_anim_set_exec_cb(&bar_anims[i], bar_height_cb);
        lv_anim_set_playback_duration(&bar_anims[i], 650);
        lv_anim_set_repeat_count(&bar_anims[i], LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&bar_anims[i]);
    }
}

static void stop_listen_anims(void) {
    for (int i = 0; i < LISTEN_BARS; i++) {
        lv_anim_delete(listen_bars[i], bar_height_cb);
        bar_height_cb(listen_bars[i], BAR_MIN_H);
    }
}

static void start_think_anim(void) {
    /* Set pivot to center of arc object */
    lv_obj_set_style_transform_pivot_x(think_arc, ARC_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(think_arc, ARC_SIZE / 2, 0);

    lv_anim_init(&arc_anim);
    lv_anim_set_var(&arc_anim, think_arc);
    lv_anim_set_values(&arc_anim, 0, 3600);
    lv_anim_set_duration(&arc_anim, 1200);
    lv_anim_set_exec_cb(&arc_anim, arc_rot_cb);
    lv_anim_set_repeat_count(&arc_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&arc_anim);
}

static void stop_think_anim(void) {
    lv_anim_delete(think_arc, arc_rot_cb);
    lv_obj_set_style_transform_rotation(think_arc, 0, 0);
}

/* ── Panel factory helpers ───────────────────────────────────────────── */

static lv_obj_t *make_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 240, CONTENT_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/* ── Build the IDLE panel ────────────────────────────────────────────── */
static void build_idle_panel(void) {
    panel_idle = make_panel(scr);
    lv_obj_set_pos(panel_idle, 0, CONTENT_Y);

    /* Large mic symbol */
    lv_obj_t *mic = lv_label_create(panel_idle);
    lv_label_set_text(mic, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(mic, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(mic, lv_color_make(0x44, 0x44, 0x44), 0);
    lv_obj_align(mic, LV_ALIGN_CENTER, 0, -28);

    /* Hint text */
    lv_obj_t *hint = lv_label_create(panel_idle);
    lv_label_set_text(hint, "HOLD BUTTON");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_make(0x44, 0x44, 0x44), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 38);

    lv_obj_t *hint2 = lv_label_create(panel_idle);
    lv_label_set_text(hint2, "TO TALK");
    lv_obj_set_style_text_font(hint2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint2, lv_color_make(0x2a, 0x2a, 0x2a), 0);
    lv_obj_align(hint2, LV_ALIGN_CENTER, 0, 60);
}

/* ── Build the LISTENING panel ───────────────────────────────────────── */
static void build_listen_panel(void) {
    panel_listen = make_panel(scr);
    lv_obj_set_pos(panel_listen, 0, CONTENT_Y);
    lv_obj_add_flag(panel_listen, LV_OBJ_FLAG_HIDDEN);

    lv_color_t green = lv_color_make(0x00, 0xFF, 0x80);

    for (int i = 0; i < LISTEN_BARS; i++) {
        lv_obj_t *b = lv_obj_create(panel_listen);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, BAR_W, BAR_MIN_H);
        lv_obj_set_pos(b, BAR_ROW_X + i * (BAR_W + BAR_GAP), BAR_BOTTOM - BAR_MIN_H);
        lv_obj_set_style_bg_color(b, green, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, BAR_W / 2, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        listen_bars[i] = b;
    }

    lv_obj_t *lbl = lv_label_create(panel_listen);
    lv_label_set_text(lbl, "LISTENING");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, green, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* ── Build the THINKING panel ────────────────────────────────────────── */
static void build_think_panel(void) {
    panel_think = make_panel(scr);
    lv_obj_set_pos(panel_think, 0, CONTENT_Y);
    lv_obj_add_flag(panel_think, LV_OBJ_FLAG_HIDDEN);

    lv_color_t orange = lv_color_make(0xFF, 0x88, 0x00);

    /* Spinning partial arc */
    think_arc = lv_arc_create(panel_think);
    lv_obj_set_size(think_arc, ARC_SIZE, ARC_SIZE);
    lv_obj_set_pos(think_arc, ARC_X, ARC_Y);
    lv_arc_set_bg_angles(think_arc, 0, 360);
    lv_arc_set_angles(think_arc, 0, 300);       /* 300° filled, 60° gap */
    lv_arc_set_range(think_arc, 0, 100);
    lv_arc_set_value(think_arc, 0);

    /* Style: colored arc, transparent bg, no knob */
    lv_obj_set_style_arc_color(think_arc, orange, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(think_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(think_arc, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_arc_width(think_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(think_arc, LV_OPA_TRANSP, 0);
    lv_obj_remove_style(think_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(think_arc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(panel_think);
    lv_label_set_text(lbl, "THINKING");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, orange, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* ── Build the SPEAKING panel ────────────────────────────────────────── */
static void build_speak_panel(void) {
    panel_speak = make_panel(scr);
    lv_obj_set_pos(panel_speak, 0, CONTENT_Y);
    lv_obj_add_flag(panel_speak, LV_OBJ_FLAG_HIDDEN);

    lv_color_t cyan = lv_color_make(0x00, 0xCC, 0xFF);

    /* Response text — wraps, cyan */
    speak_lbl = lv_label_create(panel_speak);
    lv_label_set_long_mode(speak_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(speak_lbl, 220);
    lv_obj_set_style_text_font(speak_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(speak_lbl, cyan, 0);
    lv_obj_set_style_text_align(speak_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(speak_lbl, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(speak_lbl, "");

    /* Three small indicator dots at bottom */
    lv_color_t dots_color = lv_color_make(0x00, 0x88, 0xAA);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *dot = lv_obj_create(panel_speak);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, 100 + i * 14, CONTENT_H - 18);
        lv_obj_set_style_bg_color(dot, dots_color, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void scr_chat_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Colored top bar (accent changes per state) */
    top_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(top_bar);
    lv_obj_set_size(top_bar, 240, TOP_BAR_H);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_make(0x44, 0x44, 0x44), 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* State label (e.g. "IDLE", "LISTENING") */
    lbl_state = lv_label_create(scr);
    lv_label_set_text(lbl_state, "IDLE");
    lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_state, lv_color_make(0x44, 0x44, 0x44), 0);
    lv_obj_align(lbl_state, LV_ALIGN_TOP_MID, 0, 8);

    /* "AI CHAT" fixed subtitle */
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "AI CHAT");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_make(0x22, 0x22, 0x22), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 24);

    /* Divider line */
    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 240, 1);
    lv_obj_set_pos(div, 0, 38);
    lv_obj_set_style_bg_color(div, lv_color_make(0x18, 0x18, 0x18), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    /* Build all panels (most start hidden) */
    build_idle_panel();
    build_listen_panel();
    build_think_panel();
    build_speak_panel();
}

lv_obj_t *scr_chat_get(void) {
    return scr;
}

void scr_chat_set_state(chat_state_t new_state) {
    if (new_state == cur_state) return;

    /* Stop animations from current state */
    if (cur_state == CHAT_LISTENING) stop_listen_anims();
    if (cur_state == CHAT_THINKING)  stop_think_anim();

    cur_state = new_state;
    lv_color_t col = state_color(new_state);

    /* Update top bar and state label */
    lv_obj_set_style_bg_color(top_bar, col, 0);
    lv_label_set_text(lbl_state, state_label(new_state));
    lv_obj_set_style_text_color(lbl_state, col, 0);

    /* Show the active panel, hide the others */
    lv_obj_t *panels[CHAT_STATE_COUNT] = {
        panel_idle, panel_listen, panel_think, panel_speak
    };
    for (int i = 0; i < CHAT_STATE_COUNT; i++) {
        if (i == (int)new_state) lv_obj_remove_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Start animations for new state */
    if (new_state == CHAT_LISTENING) start_listen_anims();
    if (new_state == CHAT_THINKING)  start_think_anim();
}

void scr_chat_set_text(const char *text) {
    if (speak_lbl && text) {
        lv_label_set_text(speak_lbl, text);
    }
}
