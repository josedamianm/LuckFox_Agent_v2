#ifndef DISP_DRIVER_H
#define DISP_DRIVER_H

#include "lvgl.h"
#include <stdint.h>

lv_display_t *disp_driver_init(void);
void          disp_driver_deinit(void);
void          disp_fill_color(uint8_t r, uint8_t g, uint8_t b);

void          disp_set_capture_buf(uint8_t *buf);
void          disp_send_raw_frame(const uint8_t *frame_bytes);

#endif
