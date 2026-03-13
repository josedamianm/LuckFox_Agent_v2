#ifndef SCR_IMAGE_H
#define SCR_IMAGE_H

#include "lvgl.h"

void      scr_image_create(void);
lv_obj_t *scr_image_get(void);
void      scr_image_set(const char *path);          /* single PNG/image file */
void      scr_image_gif_start(int frame_count);     /* begin GIF session     */
void      scr_image_gif_frame(int idx, const char *path, int duration_ms);

#endif
