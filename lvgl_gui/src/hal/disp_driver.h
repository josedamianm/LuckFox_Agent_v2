#ifndef DISP_DRIVER_H
#define DISP_DRIVER_H

#include "lvgl.h"

lv_display_t *disp_driver_init(void);
void          disp_driver_deinit(void);

#endif
