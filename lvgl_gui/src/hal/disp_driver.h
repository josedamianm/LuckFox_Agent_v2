#ifndef DISP_DRIVER_H
#define DISP_DRIVER_H

#include "lvgl.h"

lv_display_t *disp_driver_init(void);
void          disp_driver_deinit(void);
void          disp_fill_color(uint8_t r, uint8_t g, uint8_t b);

#endif
