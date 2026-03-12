#include "eyes_anim.h"
#include <stdlib.h>

#define EYE_W       50
#define EYE_H       60
#define EYE_GAP     30
#define PUPIL_R     12
#define SCREEN_W    240
#define SCREEN_H    240
#define BLINK_MS    150

static lv_obj_t *eye_l, *eye_r;
static lv_obj_t *pupil_l, *pupil_r;
static lv_obj_t *lid_l, *lid_r;
static int gaze_x = 0, gaze_y = 0;
static bool is_blinking = false;

static lv_obj_t *create_eye(lv_obj_t *parent, int cx) {
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, EYE_W, EYE_H);
    lv_obj_set_pos(eye, cx - EYE_W / 2, SCREEN_H / 2 - EYE_H / 2);
    lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eye, EYE_W / 2, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    return eye;
}

static lv_obj_t *create_pupil(lv_obj_t *eye_obj) {
    lv_obj_t *p = lv_obj_create(eye_obj);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, PUPIL_R * 2, PUPIL_R * 2);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, PUPIL_R, 0);
    lv_obj_center(p);
    return p;
}

static lv_obj_t *create_lid(lv_obj_t *parent, int cx) {
    lv_obj_t *lid = lv_obj_create(parent);
    lv_obj_remove_style_all(lid);
    lv_obj_set_size(lid, EYE_W + 4, EYE_H + 4);
    lv_obj_set_pos(lid, cx - EYE_W / 2 - 2, SCREEN_H / 2 - EYE_H / 2 - 2 - EYE_H);
    lv_obj_set_style_bg_color(lid, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lid, 0, 0);
    return lid;
}

static void blink_anim_cb(void *var, int32_t val) {
    lv_obj_t *lid = (lv_obj_t *)var;
    int base_y = SCREEN_H / 2 - EYE_H / 2 - 2 - EYE_H;
    lv_obj_set_y(lid, base_y + val);
}

static void blink_ready_cb(lv_anim_t *a) {
    is_blinking = false;
}

static void auto_blink_cb(lv_timer_t *t) {
    (void)t;
    if (!is_blinking && (rand() % 100) < 15)
        eyes_anim_blink();
}

static void auto_gaze_cb(lv_timer_t *t) {
    (void)t;
    if ((rand() % 100) < 20) {
        int zone = rand() % 9;
        eyes_anim_set_gaze(zone);
    }
}

void eyes_anim_init(lv_obj_t *parent) {
    int center_l = SCREEN_W / 2 - EYE_GAP / 2 - EYE_W / 2;
    int center_r = SCREEN_W / 2 + EYE_GAP / 2 + EYE_W / 2;

    eye_l = create_eye(parent, center_l);
    eye_r = create_eye(parent, center_r);
    pupil_l = create_pupil(eye_l);
    pupil_r = create_pupil(eye_r);
    lid_l = create_lid(parent, center_l);
    lid_r = create_lid(parent, center_r);

    lv_timer_create(auto_blink_cb, 500, NULL);
    lv_timer_create(auto_gaze_cb, 2000, NULL);
}

void eyes_anim_set_gaze(int zone) {
    int dx = 0, dy = 0;
    int max_x = (EYE_W / 2 - PUPIL_R - 4);
    int max_y = (EYE_H / 2 - PUPIL_R - 4);

    switch (zone) {
        case 0: dx = -max_x; dy = -max_y; break;
        case 1: dx = 0;      dy = -max_y; break;
        case 2: dx = max_x;  dy = -max_y; break;
        case 3: dx = -max_x; dy = 0;      break;
        case 4: dx = 0;      dy = 0;      break;
        case 5: dx = max_x;  dy = 0;      break;
        case 6: dx = -max_x; dy = max_y;  break;
        case 7: dx = 0;      dy = max_y;  break;
        case 8: dx = max_x;  dy = max_y;  break;
    }

    gaze_x = dx;
    gaze_y = dy;

    int px = EYE_W / 2 - PUPIL_R + dx;
    int py = EYE_H / 2 - PUPIL_R + dy;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

    lv_anim_set_var(&a, pupil_l);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, lv_obj_get_x(pupil_l), px);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(pupil_l), py);
    lv_anim_start(&a);

    lv_anim_set_var(&a, pupil_r);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, lv_obj_get_x(pupil_r), px);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(pupil_r), py);
    lv_anim_start(&a);
}

void eyes_anim_blink(void) {
    if (is_blinking) return;
    is_blinking = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_duration(&a, BLINK_MS);
    lv_anim_set_values(&a, 0, EYE_H + 4);
    lv_anim_set_exec_cb(&a, blink_anim_cb);
    lv_anim_set_playback_duration(&a, BLINK_MS);
    lv_anim_set_ready_cb(&a, blink_ready_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

    lv_anim_set_var(&a, lid_l);
    lv_anim_start(&a);
    lv_anim_set_var(&a, lid_r);
    lv_anim_start(&a);
}
