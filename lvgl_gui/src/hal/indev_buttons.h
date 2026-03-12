#ifndef INDEV_BUTTONS_H
#define INDEV_BUTTONS_H

#include "lvgl.h"

typedef enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y,
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_CTRL,
    BTN_COUNT
} btn_id_t;

typedef void (*btn_event_cb_t)(btn_id_t btn, bool pressed);

lv_indev_t *indev_buttons_init(void);
void         indev_buttons_deinit(void);
void         indev_buttons_set_event_cb(btn_event_cb_t cb);
bool         indev_buttons_is_pressed(btn_id_t btn);

#endif
