#include "lvgl.h"
#include "hal/disp_driver.h"
#include "hal/indev_buttons.h"
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

static volatile int g_running = 1;
static lv_obj_t *g_scr = NULL;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static void set_color(lv_color_t c, const char *name) {
    printf("Button %s\n", name);
    lv_obj_set_style_bg_color(g_scr, c, 0);
    lv_obj_invalidate(g_scr);
}

int main(void) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    lv_init();
    disp_driver_init();

    g_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr, lv_color_make(0x00, 0x00, 0x80), 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(g_scr);
    lv_label_set_text(lbl, "A=Red  B=Blue\nX=Yellow Y=Green");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    lv_scr_load(g_scr);

    /* open GPIO value files directly for A/B/X/Y */
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
    printf("color_test running\n");

    while (g_running) {
        lv_timer_handler();

        for (int i = 0; i < 4; i++) {
            if (btn_fds[i] < 0) continue;
            char c = '1';
            lseek(btn_fds[i], 0, SEEK_SET);
            read(btn_fds[i], &c, 1);
            bool pressed = (c == '0');
            if (pressed && !prev[i]) {
                switch (i) {
                    case 0: set_color(lv_color_make(0xFF,0x00,0x00), btn_names[i]); break;
                    case 1: set_color(lv_color_make(0x00,0x00,0xFF), btn_names[i]); break;
                    case 2: set_color(lv_color_make(0xFF,0xFF,0x00), btn_names[i]); break;
                    case 3: set_color(lv_color_make(0x00,0xFF,0x00), btn_names[i]); break;
                }
            }
            prev[i] = pressed;
        }

        usleep(10000);
    }

    for (int i = 0; i < 4; i++)
        if (btn_fds[i] >= 0) close(btn_fds[i]);
    disp_driver_deinit();
    return 0;
}
