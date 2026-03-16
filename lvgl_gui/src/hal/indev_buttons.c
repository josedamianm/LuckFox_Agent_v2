#include "indev_buttons.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#define DEBOUNCE_MS 80

static const int gpio_pins[BTN_COUNT] = {
    57, 69, 65, 67, 55, 64, 68, 66, 54
};

static const char *btn_names[BTN_COUNT] = {
    "A", "B", "X", "Y", "UP", "DOWN", "LEFT", "RIGHT", "CTRL"
};

static int gpio_fds[BTN_COUNT];
static bool btn_state[BTN_COUNT];
static uint32_t last_change[BTN_COUNT];
static btn_event_cb_t g_event_cb = NULL;

static uint32_t millis(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static int gpio_open_input(int pin) {
    char path[64];
    int fd;

    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        char buf[8];
        int n = snprintf(buf, sizeof(buf), "%d", pin);
        write(fd, buf, n);
        close(fd);
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[btn] gpio%d direction open failed\n", pin);
    } else {
        write(fd, "in", 2);
        close(fd);
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        fprintf(stderr, "[btn] gpio%d value open failed\n", pin);
    else
        fprintf(stderr, "[btn] gpio%d OK (fd=%d)\n", pin, fd);
    return fd;
}

static bool gpio_read(int fd) {
    char c = '1';
    lseek(fd, 0, SEEK_SET);
    read(fd, &c, 1);
    return c == '0';
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    uint32_t now = millis();

    for (int i = 0; i < BTN_COUNT; i++) {
        if (gpio_fds[i] < 0) continue;
        bool raw = gpio_read(gpio_fds[i]);
        if (raw != btn_state[i] && (now - last_change[i]) > DEBOUNCE_MS) {
            btn_state[i] = raw;
            last_change[i] = now;
            fprintf(stderr, "[btn] %s %s\n", btn_names[i], raw ? "PRESSED" : "released");
            if (g_event_cb) g_event_cb((btn_id_t)i, raw);
        }
    }

    static const lv_key_t key_map[BTN_COUNT] = {
        LV_KEY_ENTER, LV_KEY_ESC, LV_KEY_PREV, LV_KEY_NEXT,
        LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT, LV_KEY_RIGHT,
        LV_KEY_HOME
    };

    data->state = LV_INDEV_STATE_RELEASED;
    for (int i = 0; i < BTN_COUNT; i++) {
        if (btn_state[i]) {
            data->key = key_map[i];
            data->state = LV_INDEV_STATE_PRESSED;
            break;
        }
    }
}

lv_indev_t *indev_buttons_init(void) {
    memset(btn_state, 0, sizeof(btn_state));
    memset(last_change, 0, sizeof(last_change));

    for (int i = 0; i < BTN_COUNT; i++)
        gpio_fds[i] = gpio_open_input(gpio_pins[i]);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, read_cb);

    lv_group_t *g = lv_group_create();
    lv_group_set_default(g);

    return indev;
}

void indev_buttons_deinit(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        if (gpio_fds[i] >= 0) { close(gpio_fds[i]); gpio_fds[i] = -1; }
    }
}

void indev_buttons_set_event_cb(btn_event_cb_t cb) {
    g_event_cb = cb;
}

bool indev_buttons_is_pressed(btn_id_t btn) {
    return (btn < BTN_COUNT) ? btn_state[btn] : false;
}
