#ifndef KAWAII_FACE_H
#define KAWAII_FACE_H

#include "lvgl.h"

typedef enum {
    FACE_NEUTRAL = 0,
    FACE_HAPPY,
    FACE_SURPRISED,
    FACE_CONFUSED,
    FACE_WORKING_HARD,
    FACE_SAD,
    FACE_ANGRY,
    FACE_SLEEPY,
    FACE_EXCITED,
    FACE_EMOTION_COUNT
} face_emotion_t;

typedef struct {
    lv_obj_t *parent;
    uint32_t  anim_ms;
    uint32_t  blink_ms;
    bool      auto_blink;
} kawaii_cfg_t;

void kawaii_init(const kawaii_cfg_t *cfg);
void kawaii_set_emotion(face_emotion_t emotion, bool smooth);
face_emotion_t kawaii_get_emotion(void);
void kawaii_trigger_blink(void);
void kawaii_deinit(void);

#endif
