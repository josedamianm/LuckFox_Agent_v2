#include "hal/disp_driver.h"
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

int main(void) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    disp_driver_init();

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
    fprintf(stderr, "color_test running - no LVGL\n");

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
                    case 0: disp_fill_color(0xFF, 0x00, 0x00); break;
                    case 1: disp_fill_color(0x00, 0x00, 0xFF); break;
                    case 2: disp_fill_color(0xFF, 0xFF, 0x00); break;
                    case 3: disp_fill_color(0x00, 0xFF, 0x00); break;
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
