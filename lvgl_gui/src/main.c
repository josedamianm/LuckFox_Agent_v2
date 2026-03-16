#include "lvgl.h"
#include "hal/disp_driver.h"
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdbool.h>

static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static lv_obj_t *g_label = NULL;

static void set_screen_color(lv_color_t bg, const char *text) {
    lv_obj_set_style_bg_color(lv_screen_active(), bg, 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    if (g_label) lv_label_set_text(g_label, text);
}

int main(void) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    lv_init();
    disp_driver_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x001040), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    g_label = lv_label_create(scr);
    lv_label_set_text(g_label, "A=Red  B=Blue\nX=Yellow  Y=Green");
    lv_obj_set_style_text_color(g_label, lv_color_white(), 0);
    lv_obj_center(g_label);
    lv_label_set_long_mode(g_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_label, 220);
    lv_obj_set_style_text_align(g_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_timer_handler();

    static const int btn_gpios[4] = {57, 69, 65, 67};
    static const char *btn_names[4] = {"A", "B", "X", "Y"};
    int btn_fds[4];
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", btn_gpios[i]);
        btn_fds[i] = open(path, O_RDONLY);
        fprintf(stderr, "[main] gpio%d fd=%d\n", btn_gpios[i], btn_fds[i]);
    }

    bool prev[4] = {false, false, false, false};
    fprintf(stderr, "color_test running\n");

    while (g_running) {
        for (int i = 0; i < 4; i++) {
            if (btn_fds[i] < 0) continue;
            char c = '1';
            lseek(btn_fds[i], 0, SEEK_SET);
            read(btn_fds[i], &c, 1);
            bool pressed = (c == '0');
            if (pressed && !prev[i]) {
                fprintf(stderr, "Button %s\n", btn_names[i]);
                switch (i) {
                    case 0: set_screen_color(lv_color_hex(0xFF0000), "RED\n\nA=Red  B=Blue\nX=Yellow  Y=Green"); break;
                    case 1: set_screen_color(lv_color_hex(0x0000FF), "BLUE\n\nA=Red  B=Blue\nX=Yellow  Y=Green"); break;
                    case 2: set_screen_color(lv_color_hex(0xFFFF00), "YELLOW\n\nA=Red  B=Blue\nX=Yellow  Y=Green"); break;
                    case 3: set_screen_color(lv_color_hex(0x00FF00), "GREEN\n\nA=Red  B=Blue\nX=Yellow  Y=Green"); break;
                }
                lv_refr_now(NULL);
            }
            prev[i] = pressed;
        }
        lv_timer_handler();
        usleep(10000);
    }

    for (int i = 0; i < 4; i++)
        if (btn_fds[i] >= 0) close(btn_fds[i]);
    disp_driver_deinit();
    return 0;
}
