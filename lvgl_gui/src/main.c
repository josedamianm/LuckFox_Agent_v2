#include "lvgl.h"
#include "hal/disp_driver.h"
#include "hal/indev_buttons.h"
#include <signal.h>
#include <unistd.h>
#include <stdio.h>

static volatile int g_running = 1;
static lv_obj_t *g_scr = NULL;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void on_button(btn_id_t btn, bool pressed) {
    if (!pressed || !g_scr) return;
    lv_color_t c;
    const char *name;
    switch (btn) {
        case BTN_A: c = lv_color_make(0xFF, 0x00, 0x00); name = "A - RED";    break;
        case BTN_B: c = lv_color_make(0x00, 0x00, 0xFF); name = "B - BLUE";   break;
        case BTN_X: c = lv_color_make(0xFF, 0xFF, 0x00); name = "X - YELLOW"; break;
        case BTN_Y: c = lv_color_make(0x00, 0xFF, 0x00); name = "Y - GREEN";  break;
        default:    return;
    }
    printf("Button %s\n", name);
    lv_obj_set_style_bg_color(g_scr, c, 0);
    lv_obj_invalidate(g_scr);
}

int main(void) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    lv_init();

    disp_driver_init();
    lv_indev_t *indev = indev_buttons_init();
    (void)indev;

    g_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr, lv_color_make(0x00, 0x00, 0x80), 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(g_scr);
    lv_label_set_text(lbl, "A=Red  B=Blue\nX=Yellow Y=Green");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    lv_scr_load(g_scr);

    indev_buttons_set_event_cb(on_button);

    printf("color_test running\n");

    while (g_running) {
        lv_timer_handler();
        usleep(5000);
    }

    indev_buttons_deinit();
    disp_driver_deinit();
    return 0;
}
