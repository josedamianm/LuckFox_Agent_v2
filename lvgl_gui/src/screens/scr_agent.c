#include "scr_agent.h"
#include "lvgl.h"
#include "../faces/kawaii_face.h"
#include <string.h>

#define COLOR_BG        0x000000
#define COLOR_WHITE     0xFFFFFF
#define COLOR_GRAY      0x888888
#define COLOR_GREEN     0x00FF80
#define COLOR_ORANGE    0xFF8800
#define COLOR_CYAN      0x00CCFF
#define COLOR_RED       0xFF3333
#define COLOR_DIM       0x333333

#define BAR_COUNT       7
#define BAR_W           18
#define BAR_GAP         8
#define BAR_MIN_H       12
#define BAR_MAX_H       60
#define DOT_COUNT       3
#define DOT_SIZE        14
#define DOT_GAP         20

#define IDLE_PAGE_COUNT 3

static agent_state_t g_state     = AGENT_IDLE;
static uint32_t      g_tick      = 0;
static int           g_idle_page = 0;

static lv_obj_t *g_containers[5];
static lv_obj_t *g_idle_containers[IDLE_PAGE_COUNT];
static lv_obj_t *g_idle_page_dots[IDLE_PAGE_COUNT];

static lv_obj_t *g_speak_text;
static lv_obj_t *g_error_text;
static lv_obj_t *g_bars[BAR_COUNT];
static lv_obj_t *g_spinner;
static lv_obj_t *g_dots[DOT_COUNT];



static int tri_wave(int t, int period) {
    t = ((t % period) + period) % period;
    int half = period / 2;
    return (t < half) ? t : (period - 1 - t);
}

static lv_obj_t *make_container(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 240, 240);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

static lv_obj_t *make_idle_subpage(lv_obj_t *parent) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 240, 220);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t color,
                             int y, int width) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_x(l, (240 - width) / 2);
    lv_obj_set_y(l, y);
    return l;
}

static void build_idle_main(lv_obj_t *parent) {
    lv_obj_t *c = g_idle_containers[0] = make_idle_subpage(parent);
    make_label(c, "PepeBotL1", &lv_font_montserrat_32,
               lv_color_hex(COLOR_WHITE), 70, 220);
    make_label(c, "Press A to start", &lv_font_montserrat_16,
               lv_color_hex(COLOR_GRAY), 124, 220);
}

static void build_idle_status(lv_obj_t *parent) {
    lv_obj_t *c = g_idle_containers[1] = make_idle_subpage(parent);
    make_label(c, "STATUS", &lv_font_montserrat_16,
               lv_color_hex(COLOR_GRAY), 10, 220);
    make_label(c, "PepeBotL1", &lv_font_montserrat_24,
               lv_color_hex(COLOR_WHITE), 42, 220);
    make_label(c, "System Ready", &lv_font_montserrat_16,
               lv_color_hex(COLOR_GREEN), 84, 220);
    make_label(c, "Press A to listen", &lv_font_montserrat_12,
               lv_color_hex(COLOR_GRAY), 140, 220);
    make_label(c, "< LEFT / RIGHT >", &lv_font_montserrat_12,
               lv_color_hex(COLOR_DIM), 170, 220);
}

static void build_idle_eyes(lv_obj_t *parent) {
    lv_obj_t *c = g_idle_containers[2] = make_idle_subpage(parent);

    lv_obj_t *face_panel = lv_obj_create(c);
    lv_obj_set_size(face_panel, 200, 200);
    lv_obj_set_pos(face_panel, 20, 0);
    lv_obj_set_style_bg_opa(face_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_panel, 0, 0);
    lv_obj_set_style_pad_all(face_panel, 0, 0);
    lv_obj_clear_flag(face_panel, LV_OBJ_FLAG_SCROLLABLE);

    kawaii_cfg_t kfg = {
        .parent     = face_panel,
        .anim_ms    = 30,
        .blink_ms   = 3000,
        .auto_blink = true,
    };
    kawaii_init(&kfg);
    kawaii_set_emotion(FACE_NEUTRAL, false);

    make_label(c, "< LEFT / RIGHT >", &lv_font_montserrat_12,
               lv_color_hex(COLOR_DIM), 208, 220);
}

static void build_idle(lv_obj_t *parent) {
    lv_obj_t *c = g_containers[AGENT_IDLE] = make_container(parent);

    for (int i = 0; i < IDLE_PAGE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(c);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_GRAY), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 108 + i * 14, 228);
        g_idle_page_dots[i] = dot;
    }

    build_idle_main(c);
    build_idle_status(c);
    build_idle_eyes(c);

    g_idle_page = 0;
    lv_obj_remove_flag(g_idle_containers[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(g_idle_page_dots[0], lv_color_hex(COLOR_WHITE), 0);
}

static void build_listening(lv_obj_t *parent) {
    lv_obj_t *c = g_containers[AGENT_LISTENING] = make_container(parent);
    make_label(c, "Listening...", &lv_font_montserrat_24,
               lv_color_hex(COLOR_GREEN), 14, 220);
    int total_w = BAR_COUNT * BAR_W + (BAR_COUNT - 1) * BAR_GAP;
    int start_x = (240 - total_w) / 2;
    for (int i = 0; i < BAR_COUNT; i++) {
        lv_obj_t *bar = lv_obj_create(c);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_GREEN), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_set_size(bar, BAR_W, BAR_MIN_H);
        lv_obj_set_x(bar, start_x + i * (BAR_W + BAR_GAP));
        lv_obj_set_y(bar, 170 - BAR_MIN_H);
        g_bars[i] = bar;
    }
    make_label(c, "Release A to process", &lv_font_montserrat_12,
               lv_color_hex(COLOR_GRAY), 200, 220);
}

static void build_thinking(lv_obj_t *parent) {
    lv_obj_t *c = g_containers[AGENT_THINKING] = make_container(parent);
    lv_obj_t *arc = lv_arc_create(c);
    lv_obj_set_size(arc, 100, 100);
    lv_obj_set_pos(arc, 70, 70);
    lv_arc_set_range(arc, 0, 360);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_DIM), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    lv_obj_set_style_size(arc, 0, 0, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_start_angle(arc, 0);
    lv_arc_set_end_angle(arc, 90);
    g_spinner = arc;
    make_label(c, "Thinking...", &lv_font_montserrat_24,
               lv_color_hex(COLOR_ORANGE), 182, 220);
}

static void build_speaking(lv_obj_t *parent) {
    lv_obj_t *c = g_containers[AGENT_SPEAKING] = make_container(parent);
    make_label(c, "Speaking...", &lv_font_montserrat_24,
               lv_color_hex(COLOR_CYAN), 14, 220);
    lv_obj_t *txt = lv_label_create(c);
    lv_label_set_text(txt, "");
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(txt, 210);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_opa(txt, LV_OPA_TRANSP, 0);
    lv_obj_set_x(txt, 15);
    lv_obj_set_y(txt, 56);
    lv_obj_set_height(txt, 140);
    g_speak_text = txt;
    int dots_total = DOT_COUNT * DOT_SIZE + (DOT_COUNT - 1) * DOT_GAP;
    int dot_start_x = (240 - dots_total) / 2;
    for (int i = 0; i < DOT_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(c);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(dot, DOT_SIZE / 2, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_CYAN), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_x(dot, dot_start_x + i * (DOT_SIZE + DOT_GAP));
        lv_obj_set_y(dot, 210);
        g_dots[i] = dot;
    }
}

static void build_error(lv_obj_t *parent) {
    lv_obj_t *c = g_containers[AGENT_ERROR] = make_container(parent);
    make_label(c, "!", &lv_font_montserrat_48,
               lv_color_hex(COLOR_RED), 72, 220);
    lv_obj_t *msg = lv_label_create(c);
    lv_label_set_text(msg, "Unknown error");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(msg, 210);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_bg_opa(msg, LV_OPA_TRANSP, 0);
    lv_obj_set_x(msg, 15);
    lv_obj_set_y(msg, 148);
    g_error_text = msg;
}

void agent_screen_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_idle(scr);
    build_listening(scr);
    build_thinking(scr);
    build_speaking(scr);
    build_error(scr);

    agent_set_state(AGENT_IDLE, NULL);
}

void agent_set_state(agent_state_t state, const char *text) {
    g_state = state;
    g_tick  = 0;

    for (int i = 0; i < 5; i++)
        lv_obj_add_flag(g_containers[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_containers[state], LV_OBJ_FLAG_HIDDEN);

    if (state == AGENT_IDLE) {
        for (int i = 0; i < IDLE_PAGE_COUNT; i++) {
            lv_obj_add_flag(g_idle_containers[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(g_idle_page_dots[i], lv_color_hex(COLOR_GRAY), 0);
        }
        g_idle_page = 0;
        lv_obj_remove_flag(g_idle_containers[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(g_idle_page_dots[0], lv_color_hex(COLOR_WHITE), 0);
    }

    if (state == AGENT_SPEAKING && text && *text)
        lv_label_set_text(g_speak_text, text);
    else if (state == AGENT_SPEAKING)
        lv_label_set_text(g_speak_text, "");

    if (state == AGENT_ERROR && text && *text)
        lv_label_set_text(g_error_text, text);
    else if (state == AGENT_ERROR)
        lv_label_set_text(g_error_text, "Unknown error");

    switch (state) {
        case AGENT_IDLE:       kawaii_set_emotion(FACE_NEUTRAL,      true); break;
        case AGENT_LISTENING:  kawaii_set_emotion(FACE_SURPRISED,    true); break;
        case AGENT_THINKING:   kawaii_set_emotion(FACE_WORKING_HARD, true); break;
        case AGENT_SPEAKING:   kawaii_set_emotion(FACE_HAPPY,        true); break;
        case AGENT_ERROR:      kawaii_set_emotion(FACE_SAD,          true); break;
        default: break;
    }

    lv_refr_now(NULL);
}

void agent_idle_nav(int dir) {
    if (g_state != AGENT_IDLE) return;

    lv_obj_add_flag(g_idle_containers[g_idle_page], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(g_idle_page_dots[g_idle_page], lv_color_hex(COLOR_GRAY), 0);

    g_idle_page = (g_idle_page + dir + IDLE_PAGE_COUNT) % IDLE_PAGE_COUNT;

    lv_obj_remove_flag(g_idle_containers[g_idle_page], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(g_idle_page_dots[g_idle_page], lv_color_hex(COLOR_WHITE), 0);

    lv_refr_now(NULL);
}

void agent_tick(void) {
    g_tick++;

    if (g_state == AGENT_LISTENING) {
        int total_w = BAR_COUNT * BAR_W + (BAR_COUNT - 1) * BAR_GAP;
        int start_x = (240 - total_w) / 2;
        for (int i = 0; i < BAR_COUNT; i++) {
            int t = (int)g_tick + i * 5;
            int wave = tri_wave(t, 32);
            int h = BAR_MIN_H + (wave * (BAR_MAX_H - BAR_MIN_H)) / 15;
            int y = 170 - h;
            lv_obj_set_size(g_bars[i], BAR_W, h);
            lv_obj_set_x(g_bars[i], start_x + i * (BAR_W + BAR_GAP));
            lv_obj_set_y(g_bars[i], y);
        }
    } else if (g_state == AGENT_THINKING) {
        int start = (int)(g_tick * 4) % 360;
        int end   = (start + 90) % 360;
        lv_arc_set_start_angle(g_spinner, (uint16_t)start);
        lv_arc_set_end_angle(g_spinner, (uint16_t)end);
    } else if (g_state == AGENT_SPEAKING) {
        int active = (g_tick / 20) % DOT_COUNT;
        for (int i = 0; i < DOT_COUNT; i++)
            lv_obj_set_style_bg_opa(g_dots[i],
                i == active ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}
