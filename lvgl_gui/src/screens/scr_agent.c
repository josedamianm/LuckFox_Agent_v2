#include "scr_agent.h"
#include "lvgl.h"
#include "../faces/kawaii_face.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define COLOR_BG        0x000000
#define COLOR_WHITE     0xFFFFFF
#define COLOR_GRAY      0x888888
#define COLOR_GREEN     0x00FF80
#define COLOR_CYAN      0x00CCFF
#define COLOR_RED       0xFF3333
#define COLOR_DIM       0x444444

#define IDLE_PAGE_COUNT 2
#define DOT_COUNT       3
#define DOT_SIZE        14
#define DOT_GAP         20

static agent_state_t g_state     = AGENT_IDLE;
static uint32_t      g_tick      = 0;
static int           g_idle_page = 0;

static lv_obj_t *g_containers[5];
static lv_obj_t *g_idle_containers[IDLE_PAGE_COUNT];
static lv_obj_t *g_idle_page_dots[IDLE_PAGE_COUNT];

static lv_obj_t *g_speak_text;
static lv_obj_t *g_error_text;
static lv_obj_t *g_dots[DOT_COUNT];

static lv_obj_t *g_label_time;
static lv_obj_t *g_label_date;
static lv_obj_t *g_label_ip;

static uint32_t g_last_clock_ms = 0;

/* ------------------------------------------------------------------ */

static void get_private_ip(char *buf, size_t len)
{
    struct ifaddrs *ifaddr, *ifa;
    buf[0] = '\0';
    if (getifaddrs(&ifaddr) == -1) {
        snprintf(buf, len, "---");
        return;
    }
    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, buf, (socklen_t)len);
        break;
    }
    freeifaddrs(ifaddr);
    if (buf[0] == '\0') snprintf(buf, len, "---");
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char tbuf[12], dbuf[20], ipbuf[20];
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", tm);
    strftime(dbuf, sizeof dbuf, "%a %d %b %Y", tm);
    get_private_ip(ipbuf, sizeof ipbuf);
    lv_label_set_text(g_label_time, tbuf);
    lv_label_set_text(g_label_date, dbuf);
    lv_label_set_text(g_label_ip, ipbuf);
}

/* ------------------------------------------------------------------ */

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
    lv_obj_set_size(c, 240, 228);
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

/* ------------------------------------------------------------------ */
/* Page 0 — Status: Date, Time, IP                                     */
/* ------------------------------------------------------------------ */
static void build_idle_status(lv_obj_t *parent) {
    lv_obj_t *c = g_idle_containers[0] = make_idle_subpage(parent);

    make_label(c, "PepeBotL1", &lv_font_montserrat_24,
               lv_color_hex(COLOR_GREEN), 8, 220);

    make_label(c, "divider", &lv_font_montserrat_12,
               lv_color_hex(COLOR_DIM), 34, 220);

    g_label_time = make_label(c, "--:--:--", &lv_font_montserrat_48,
                              lv_color_hex(COLOR_WHITE), 48, 220);

    g_label_date = make_label(c, "--- -- --- ----", &lv_font_montserrat_16,
                              lv_color_hex(COLOR_GRAY), 108, 220);

    make_label(c, "IP", &lv_font_montserrat_12,
               lv_color_hex(COLOR_DIM), 138, 220);

    g_label_ip = make_label(c, "---", &lv_font_montserrat_24,
                            lv_color_hex(COLOR_CYAN), 155, 220);

    make_label(c, "< LEFT / RIGHT >", &lv_font_montserrat_12,
               lv_color_hex(COLOR_DIM), 210, 220);
}

/* ------------------------------------------------------------------ */
/* Page 1 — Kawaii eyes                                                */
/* ------------------------------------------------------------------ */
static void build_idle_eyes(lv_obj_t *parent) {
    lv_obj_t *c = g_idle_containers[1] = make_idle_subpage(parent);

    lv_obj_t *face_panel = lv_obj_create(c);
    lv_obj_set_size(face_panel, 220, 220);
    lv_obj_set_pos(face_panel, 10, 4);
    lv_obj_set_style_bg_color(face_panel, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(face_panel, LV_OPA_COVER, 0);
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
               lv_color_hex(COLOR_DIM), 212, 220);
}

/* ------------------------------------------------------------------ */
/* build_idle — container with 2 pages + dots                          */
/* ------------------------------------------------------------------ */
static void build_idle(lv_obj_t *parent) {
    lv_obj_t *c = g_containers[AGENT_IDLE] = make_container(parent);

    int dot_total_w = IDLE_PAGE_COUNT * 6 + (IDLE_PAGE_COUNT - 1) * 10;
    int dot_start   = (240 - dot_total_w) / 2;
    for (int i = 0; i < IDLE_PAGE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(c);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_GRAY), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, dot_start + i * 16, 230);
        g_idle_page_dots[i] = dot;
    }

    build_idle_status(c);
    build_idle_eyes(c);

    g_idle_page = 0;
    lv_obj_remove_flag(g_idle_containers[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(g_idle_page_dots[0], lv_color_hex(COLOR_WHITE), 0);
}

/* ------------------------------------------------------------------ */
/* Speaking                                                            */
/* ------------------------------------------------------------------ */
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
    int dots_total  = DOT_COUNT * DOT_SIZE + (DOT_COUNT - 1) * DOT_GAP;
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

/* ------------------------------------------------------------------ */
/* Error                                                               */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */

void agent_screen_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_idle(scr);
    g_containers[AGENT_LISTENING] = g_containers[AGENT_IDLE];
    g_containers[AGENT_THINKING]  = g_containers[AGENT_IDLE];
    build_speaking(scr);
    build_error(scr);

    g_last_clock_ms = lv_tick_get();
    clock_timer_cb(NULL);

    agent_set_state(AGENT_IDLE, NULL);
}

void agent_set_state(agent_state_t state, const char *text) {
    g_state = state;
    g_tick  = 0;

    lv_obj_add_flag(g_containers[AGENT_IDLE],     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_containers[AGENT_SPEAKING], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_containers[AGENT_ERROR],    LV_OBJ_FLAG_HIDDEN);

    if (state == AGENT_IDLE || state == AGENT_LISTENING || state == AGENT_THINKING) {
        lv_obj_remove_flag(g_containers[AGENT_IDLE], LV_OBJ_FLAG_HIDDEN);
        if (state == AGENT_LISTENING || state == AGENT_THINKING) {
            for (int i = 0; i < IDLE_PAGE_COUNT; i++) {
                lv_obj_add_flag(g_idle_containers[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(g_idle_page_dots[i], lv_color_hex(COLOR_GRAY), 0);
            }
            g_idle_page = 1;
            lv_obj_remove_flag(g_idle_containers[1], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(g_idle_page_dots[1], lv_color_hex(COLOR_WHITE), 0);
        } else {
            for (int i = 0; i < IDLE_PAGE_COUNT; i++) {
                lv_obj_add_flag(g_idle_containers[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(g_idle_page_dots[i], lv_color_hex(COLOR_GRAY), 0);
            }
            g_idle_page = 0;
            lv_obj_remove_flag(g_idle_containers[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(g_idle_page_dots[0], lv_color_hex(COLOR_WHITE), 0);
        }
    } else {
        lv_obj_remove_flag(g_containers[state], LV_OBJ_FLAG_HIDDEN);
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
    uint32_t now = lv_tick_get();

    kawaii_tick();

    if ((now - g_last_clock_ms) >= 1000u) {
        g_last_clock_ms = now;
        clock_timer_cb(NULL);
    }

    if (g_state == AGENT_SPEAKING) {
        int active = (g_tick / 20) % DOT_COUNT;
        for (int i = 0; i < DOT_COUNT; i++)
            lv_obj_set_style_bg_opa(g_dots[i],
                i == active ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}
