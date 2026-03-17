#include "lvgl.h"
#include "hal/disp_driver.h"
#include "screens/scr_agent.h"
#include "ipc/ipc_server.h"
#include "ipc/cmd_parser.h"
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>

#define IPC_SOCK_PATH "/tmp/luckfox_gui.sock"

#define BTN_COUNT  9
#define BTN_A      0
#define BTN_LEFT   6
#define BTN_RIGHT  7

static const int  btn_gpios[BTN_COUNT] = {57, 69, 65, 67, 55, 64, 68, 66, 54};
static const char *btn_names[BTN_COUNT] = {"A","B","X","Y","UP","DOWN","LEFT","RIGHT","CTRL"};

static volatile int g_running = 1;
static void sig_handler(int sig) { (void)sig; g_running = 0; }

static int btn_fds[BTN_COUNT];
static bool btn_prev[BTN_COUNT];

static void gpio_buttons_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        char path[64];
        int fd;
        snprintf(path, sizeof(path), "%d", btn_gpios[i]);
        fd = open("/sys/class/gpio/export", O_WRONLY);
        if (fd >= 0) { write(fd, path, strlen(path)); close(fd); }
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", btn_gpios[i]);
        fd = open(path, O_WRONLY);
        if (fd >= 0) { write(fd, "in", 2); close(fd); }
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", btn_gpios[i]);
        btn_fds[i] = open(path, O_RDONLY);
        fprintf(stderr, "[gpio] gpio%d fd=%d\n", btn_gpios[i], btn_fds[i]);
    }
    for (int i = 0; i < BTN_COUNT; i++) {
        char c = '1';
        if (btn_fds[i] >= 0) { read(btn_fds[i], &c, 1); lseek(btn_fds[i], 0, SEEK_SET); }
        btn_prev[i] = (c == '0');
    }
}

static void gpio_buttons_deinit(void) {
    for (int i = 0; i < BTN_COUNT; i++)
        if (btn_fds[i] >= 0) { close(btn_fds[i]); btn_fds[i] = -1; }
}

static void gpio_poll_buttons(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        if (btn_fds[i] < 0) continue;
        char c = '1';
        lseek(btn_fds[i], 0, SEEK_SET);
        read(btn_fds[i], &c, 1);
        bool pressed = (c == '0');

        if (pressed != btn_prev[i]) {
            if (pressed && i == BTN_LEFT)  agent_idle_nav(-1);
            if (pressed && i == BTN_RIGHT) agent_idle_nav(+1);

            char msg[128];
            snprintf(msg, sizeof(msg),
                     "{\"event\":\"button\",\"name\":\"%s\",\"state\":\"%s\"}",
                     btn_names[i], pressed ? "pressed" : "released");
            fprintf(stderr, "[btn] %s\n", msg);
            ipc_server_broadcast(msg);
        }
        btn_prev[i] = pressed;
    }
}

int main(void) {
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    lv_init();
    disp_driver_init();
    agent_screen_init();

    if (ipc_server_init(IPC_SOCK_PATH) < 0)
        fprintf(stderr, "[ipc] failed to bind %s\n", IPC_SOCK_PATH);
    cmd_parser_init();

    gpio_buttons_init();

    fprintf(stderr, "[main] agent running\n");

    while (g_running) {
        ipc_server_poll();
        gpio_poll_buttons();
        agent_tick();
        lv_timer_handler();
        lv_refr_now(NULL);
        usleep(10000);
    }

    gpio_buttons_deinit();
    ipc_server_deinit();
    disp_driver_deinit();
    return 0;
}
