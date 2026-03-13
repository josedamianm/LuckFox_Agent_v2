#include "scr_image.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Layout ─────────────────────────────────────────────────────────────
 * Two child widgets sit on the screen:
 *   img    – lv_image for PNG / file-path images (lv_image_set_src)
 *   canvas – lv_canvas for raw RGB565 GIF frames (malloc'd buffer)
 * Only one is visible at a time.
 * ───────────────────────────────────────────────────────────────────── */
static lv_obj_t *scr;
static lv_obj_t *img;
static lv_obj_t *canvas;

/* Canvas pixel buffer — allocated from the OS heap (not LVGL's 48 KB pool)
 * so it doesn't exhaust LVGL's internal memory.
 * 240 × 240 × 2 bytes = 115 200 bytes ≈ 112 KB.                        */
static uint8_t      *frame_pixels = NULL;
static lv_draw_buf_t frame_draw_buf;

/* ── GIF state ──────────────────────────────────────────────────────── */
#define GIF_MAX_FRAMES 30

static char       gif_paths[GIF_MAX_FRAMES][128];
static uint32_t   gif_durations[GIF_MAX_FRAMES]; /* ms per frame */
static int        gif_frame_count  = 0;
static int        gif_frames_ready = 0;           /* loaded so far */
static int        gif_cur          = 0;
static uint32_t   gif_frame_start  = 0;           /* lv_tick_get() stamp */
static lv_timer_t *gif_timer       = NULL;

/* ── helpers ─────────────────────────────────────────────────────────── */

static void show_canvas(bool show) {
    if (show) {
        lv_obj_add_flag(img,    LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(img,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Read one raw RGB565 frame (little-endian) from disk into the canvas. */
static void load_canvas_frame(int idx) {
    FILE *f = fopen(gif_paths[idx], "rb");
    if (!f) return;
    fread(frame_pixels, 1, 240 * 240 * 2, f);
    fclose(f);
    lv_obj_invalidate(canvas);
}

/* ── GIF timer callback ─────────────────────────────────────────────── */
static void gif_tick(lv_timer_t *t) {
    (void)t;
    if (gif_frame_count == 0) return;

    uint32_t now = lv_tick_get();
    if ((now - gif_frame_start) < gif_durations[gif_cur]) return;

    gif_cur = (gif_cur + 1) % gif_frame_count;
    gif_frame_start = now;
    load_canvas_frame(gif_cur);
}

/* ── Public API ─────────────────────────────────────────────────────── */

void scr_image_create(void) {
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Static image widget (PNG / LVGL-format files) */
    img = lv_image_create(scr);
    lv_obj_center(img);

    /* Canvas for raw RGB565 GIF frames */
    frame_pixels = malloc(240 * 240 * 2);
    if (frame_pixels) {
        lv_draw_buf_init(&frame_draw_buf, 240, 240, LV_COLOR_FORMAT_RGB565,
                         240 * 2 /* stride: bytes/row */, frame_pixels, 240 * 240 * 2);
        canvas = lv_canvas_create(scr);
        lv_canvas_set_draw_buf(canvas, &frame_draw_buf);
        lv_obj_center(canvas);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Fallback: no canvas if alloc fails */
        canvas = lv_obj_create(scr);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *scr_image_get(void) {
    return scr;
}

void scr_image_set(const char *path) {
    if (!path) return;

    /* Stop any running GIF */
    if (gif_timer) {
        lv_timer_delete(gif_timer);
        gif_timer = NULL;
    }
    gif_frame_count = 0;

    show_canvas(false);

    char lvpath[132];
    if (path[0] == '/') {
        snprintf(lvpath, sizeof(lvpath), "%s", path);
    } else {
        snprintf(lvpath, sizeof(lvpath), "S:%s", path);
    }
    lv_image_set_src(img, lvpath);
    lv_obj_center(img);
}

void scr_image_gif_start(int frame_count) {
    /* Clamp to max */
    gif_frame_count  = frame_count > GIF_MAX_FRAMES ? GIF_MAX_FRAMES : frame_count;
    gif_frames_ready = 0;
    gif_cur          = 0;
    gif_frame_start  = 0;

    /* Clear any previous timer */
    if (gif_timer) {
        lv_timer_delete(gif_timer);
        gif_timer = NULL;
    }

    show_canvas(true);
}

void scr_image_gif_frame(int idx, const char *path, int duration_ms) {
    if (idx < 0 || idx >= GIF_MAX_FRAMES || !path) return;

    strncpy(gif_paths[idx], path, sizeof(gif_paths[0]) - 1);
    gif_paths[idx][sizeof(gif_paths[0]) - 1] = '\0';
    gif_durations[idx] = (uint32_t)(duration_ms > 0 ? duration_ms : 50);
    gif_frames_ready++;

    /* Display the first frame immediately so the screen isn't blank. */
    if (idx == 0) {
        load_canvas_frame(0);
        gif_frame_start = lv_tick_get();
    }

    /* Start the animation timer once all frames are ready. */
    if (gif_frames_ready >= gif_frame_count && gif_timer == NULL && gif_frame_count > 1) {
        gif_timer = lv_timer_create(gif_tick, 20, NULL); /* 50 Hz poll */
    }
}
