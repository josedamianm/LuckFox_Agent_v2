#include "lvgl.h"
#include "hal/disp_driver.h"
#include "hal/indev_buttons.h"
#include "ipc/ipc_server.h"
#include "ipc/cmd_parser.h"
#include "screens/scr_manager.h"
#include "theme/lf_theme.h"
#include <signal.h>
#include <unistd.h>
#include <stdio.h>

static volatile int g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void on_button(btn_id_t btn, bool pressed) {
    static const char *names[] = {
        "A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "CTRL"
    };
    if (btn < BTN_COUNT && pressed) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "{\"event\":\"button\",\"name\":\"%s\",\"state\":\"pressed\"}",
                 names[btn]);
        ipc_server_broadcast(msg);
    }
}

int main(void) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    lv_init();

    lv_display_t *disp  = disp_driver_init();
    lv_indev_t   *indev = indev_buttons_init();

    lf_theme_init(disp);
    lv_indev_set_group(indev, lv_group_get_default());

    scr_manager_init();
    scr_manager_switch(SCR_STATUS);

    ipc_server_init("/tmp/luckfox_gui.sock");
    cmd_parser_init();
    indev_buttons_set_event_cb(on_button);

    printf("luckfox_gui running\n");

    while (g_running) {
        lv_timer_handler();
        ipc_server_poll();
        usleep(5000);
    }

    ipc_server_deinit();
    indev_buttons_deinit();
    disp_driver_deinit();
    return 0;
}
