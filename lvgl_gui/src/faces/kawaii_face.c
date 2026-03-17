#include "kawaii_face.h"
#include <math.h>
#include <string.h>

typedef struct {
    uint8_t left_eye;
    uint8_t right_eye;
    int8_t  mouth;
    int8_t  brow_l;
    int8_t  brow_r;
    int8_t  brow_h;
    uint8_t blush;
} emo_params_t;

static const emo_params_t EMO_TABLE[FACE_EMOTION_COUNT] = {
    [FACE_NEUTRAL]      = {100, 100,   0,   0,   0,   0,   0},
    [FACE_HAPPY]        = { 96,  96,  90,  -4,  -4,  -5,  82},
    [FACE_SURPRISED]    = {100, 100,  50,   0,   0, -10,  20},
    [FACE_CONFUSED]     = { 88,  75,  12, -18,   8,  -3,  15},
    [FACE_WORKING_HARD] = { 65,  65,   0,  22, -22,   4,  60},
    [FACE_SAD]          = { 60,  60, -75, -15,  15,   3,   0},
    [FACE_ANGRY]        = { 75,  75, -45,  25, -25,   5,  50},
    [FACE_SLEEPY]       = { 35,  35,  -5,  -5,   5,   8,  30},
    [FACE_EXCITED]      = {100, 100,  95,   8,   8,  -8,  85},
};

typedef struct {
    lv_obj_t *brow;
    lv_obj_t *sclera;
    lv_obj_t *iris;
    lv_obj_t *pupil;
    lv_obj_t *highlight;
    lv_obj_t *blush;
    int16_t   base_x;
    int16_t   base_y;
} eye_t;

typedef struct {
    lv_obj_t *container;
    eye_t     el;
    eye_t     er;
    lv_obj_t *mouth;

    int16_t   eye_w;
    int16_t   eye_h_max;
    int16_t   iris_w;
    int16_t   iris_h;
    int16_t   pupil_w;
    int16_t   pupil_h;
    int16_t   brow_w;
    int16_t   mouth_w;
    int16_t   mouth_h;
    int16_t   mouth_base_y;
    int16_t   face_sz;

    uint8_t  eye_l_open;
    uint8_t  eye_r_open;
    int8_t   mouth_curve;
    int8_t   brow_l_ang;
    int8_t   brow_r_ang;
    int8_t   brow_height;
    uint8_t  blush;
    int8_t   pupil_x;
    int8_t   pupil_y;
    int8_t   bounce;

    face_emotion_t current;
    face_emotion_t target;
    uint8_t        trans_pct;

    bool     blinking;
    uint8_t  blink_phase;
    uint32_t last_blink_ms;
    uint32_t blink_interval_ms;
    bool     auto_blink;

    uint32_t bounce_cnt;
    uint32_t pupil_cnt;

    lv_timer_t *timer;
    bool        inited;
} kf_state_t;

static kf_state_t S;

static void apply_emotion_instant(face_emotion_t e);
static void lerp_toward_target(void);
static void update_display(void);
static void update_single_eye(eye_t *e, uint8_t openness, bool is_left);
static void update_mouth(void);
static void timer_cb(lv_timer_t *t);

/* ------------------------------------------------------------------ */
/* Widget factory                                                       */
/* ------------------------------------------------------------------ */
static eye_t make_eye_widget(lv_obj_t *parent, int16_t x, int16_t y)
{
    eye_t e;
    memset(&e, 0, sizeof e);
    e.base_x = x;
    e.base_y = y;

    e.brow = lv_obj_create(parent);
    lv_obj_set_size(e.brow, S.brow_w, 5);
    lv_obj_set_pos(e.brow, x + (S.eye_w - S.brow_w) / 2, y - 12);
    lv_obj_set_style_bg_color(e.brow, lv_color_make(90, 65, 35), 0);
    lv_obj_set_style_bg_opa(e.brow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(e.brow, 0, 0);
    lv_obj_set_style_radius(e.brow, 3, 0);
    lv_obj_set_style_pad_all(e.brow, 0, 0);
    lv_obj_clear_flag(e.brow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(e.brow, S.brow_w / 2, 0);
    lv_obj_set_style_transform_pivot_y(e.brow, 2, 0);

    e.sclera = lv_obj_create(parent);
    lv_obj_set_size(e.sclera, S.eye_w, S.eye_h_max);
    lv_obj_set_pos(e.sclera, x, y);
    lv_obj_set_style_bg_color(e.sclera, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(e.sclera, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(e.sclera, lv_color_black(), 0);
    lv_obj_set_style_border_width(e.sclera, 3, 0);
    lv_obj_set_style_radius(e.sclera, S.eye_h_max / 2, 0);
    lv_obj_set_style_clip_corner(e.sclera, true, 0);
    lv_obj_set_style_pad_all(e.sclera, 0, 0);
    lv_obj_clear_flag(e.sclera, LV_OBJ_FLAG_SCROLLABLE);

    e.iris = lv_obj_create(e.sclera);
    lv_obj_set_size(e.iris, S.iris_w, S.iris_h);
    lv_obj_set_pos(e.iris, (S.eye_w - S.iris_w) / 2, (S.eye_h_max - S.iris_h) / 2);
    lv_obj_set_style_bg_color(e.iris, lv_color_make(50, 180, 255), 0);
    lv_obj_set_style_bg_opa(e.iris, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(e.iris, lv_color_make(30, 130, 200), 0);
    lv_obj_set_style_border_width(e.iris, 2, 0);
    lv_obj_set_style_radius(e.iris, S.iris_w / 2, 0);
    lv_obj_set_style_clip_corner(e.iris, true, 0);
    lv_obj_set_style_pad_all(e.iris, 0, 0);
    lv_obj_clear_flag(e.iris, LV_OBJ_FLAG_SCROLLABLE);

    e.pupil = lv_obj_create(e.iris);
    lv_obj_set_size(e.pupil, S.pupil_w, S.pupil_h);
    lv_obj_set_pos(e.pupil, (S.iris_w - S.pupil_w) / 2, (S.iris_h - S.pupil_h) / 2);
    lv_obj_set_style_bg_color(e.pupil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(e.pupil, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(e.pupil, 0, 0);
    lv_obj_set_style_radius(e.pupil, S.pupil_w / 2, 0);
    lv_obj_set_style_pad_all(e.pupil, 0, 0);
    lv_obj_clear_flag(e.pupil, LV_OBJ_FLAG_SCROLLABLE);

    int16_t hl = (S.pupil_w / 3 < 3) ? 3 : S.pupil_w / 3;
    e.highlight = lv_obj_create(e.iris);
    lv_obj_set_size(e.highlight, hl, hl);
    lv_obj_set_pos(e.highlight, (S.iris_w - S.pupil_w) / 2,
                                (S.iris_h - S.pupil_h) / 2);
    lv_obj_set_style_bg_color(e.highlight, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(e.highlight, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(e.highlight, 0, 0);
    lv_obj_set_style_radius(e.highlight, hl / 2, 0);
    lv_obj_set_style_pad_all(e.highlight, 0, 0);
    lv_obj_clear_flag(e.highlight, LV_OBJ_FLAG_SCROLLABLE);

    e.blush = lv_obj_create(parent);
    lv_obj_set_size(e.blush, S.eye_w - 4, 7);
    lv_obj_set_pos(e.blush, x + 2, y + S.eye_h_max + 4);
    lv_obj_set_style_bg_color(e.blush, lv_color_make(255, 100, 150), 0);
    lv_obj_set_style_bg_opa(e.blush, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(e.blush, 0, 0);
    lv_obj_set_style_radius(e.blush, 4, 0);
    lv_obj_set_style_pad_all(e.blush, 0, 0);
    lv_obj_clear_flag(e.blush, LV_OBJ_FLAG_SCROLLABLE);

    return e;
}

/* ------------------------------------------------------------------ */
/* Init / deinit                                                        */
/* ------------------------------------------------------------------ */
void kawaii_init(const kawaii_cfg_t *cfg)
{
    if (S.inited) return;
    memset(&S, 0, sizeof S);

    lv_obj_t *parent = (cfg && cfg->parent) ? cfg->parent : lv_screen_active();
    uint32_t anim_ms = (cfg && cfg->anim_ms) ? cfg->anim_ms : 30;
    S.blink_interval_ms = (cfg && cfg->blink_ms) ? cfg->blink_ms : 3000;
    S.auto_blink = cfg ? cfg->auto_blink : true;

    lv_obj_update_layout(parent);
    int32_t pw = lv_obj_get_width(parent);
    int32_t ph = lv_obj_get_height(parent);
    if (pw < 20) pw = 220;
    if (ph < 20) ph = 220;
    S.face_sz    = (int16_t)((pw < ph) ? pw : ph);

    S.eye_w      = (int16_t)(S.face_sz * 0.33f);
    S.eye_h_max  = (int16_t)(S.face_sz * 0.29f);
    S.iris_w     = (int16_t)(S.eye_w * 0.58f);
    S.iris_h     = (int16_t)(S.eye_h_max * 0.76f);
    if (S.iris_h > S.iris_w) S.iris_h = S.iris_w;
    S.pupil_w    = (int16_t)(S.iris_w * 0.46f);
    S.pupil_h    = (int16_t)(S.iris_h * 0.54f);
    S.brow_w     = (int16_t)(S.eye_w * 0.88f);
    S.mouth_w    = (int16_t)(S.face_sz * 0.42f);
    S.mouth_h    = (int16_t)(S.face_sz * 0.13f);
    S.mouth_base_y = (int16_t)(S.face_sz * 0.63f);

    S.container = lv_obj_create(parent);
    lv_obj_set_size(S.container, S.face_sz, S.face_sz);
    lv_obj_center(S.container);
    lv_obj_set_style_bg_color(S.container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(S.container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(S.container, 0, 0);
    lv_obj_set_style_pad_all(S.container, 0, 0);
    lv_obj_clear_flag(S.container, LV_OBJ_FLAG_SCROLLABLE);

    int16_t gap   = (int16_t)(S.face_sz * 0.08f);
    int16_t eye_y = (int16_t)(S.face_sz * 0.22f);
    int16_t lx    = S.face_sz / 2 - S.eye_w - gap / 2;
    int16_t rx    = S.face_sz / 2 + gap / 2;

    S.el = make_eye_widget(S.container, lx, eye_y);
    S.er = make_eye_widget(S.container, rx, eye_y);

    S.mouth = lv_obj_create(S.container);
    lv_obj_set_size(S.mouth, S.mouth_w, S.mouth_h);
    lv_obj_set_pos(S.mouth, S.face_sz / 2 - S.mouth_w / 2, S.mouth_base_y);
    lv_obj_set_style_bg_color(S.mouth, lv_color_make(200, 60, 80), 0);
    lv_obj_set_style_bg_opa(S.mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(S.mouth, lv_color_black(), 0);
    lv_obj_set_style_border_width(S.mouth, 2, 0);
    lv_obj_set_style_radius(S.mouth, S.mouth_h / 2, 0);
    lv_obj_set_style_pad_all(S.mouth, 0, 0);
    lv_obj_clear_flag(S.mouth, LV_OBJ_FLAG_SCROLLABLE);

    apply_emotion_instant(FACE_NEUTRAL);
    update_display();

    S.last_blink_ms = lv_tick_get();
    S.timer = lv_timer_create(timer_cb, anim_ms, NULL);
    S.inited = true;
}

void kawaii_deinit(void)
{
    if (!S.inited) return;
    if (S.timer)     lv_timer_delete(S.timer);
    if (S.container) lv_obj_delete(S.container);
    memset(&S, 0, sizeof S);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
void kawaii_set_emotion(face_emotion_t emotion, bool smooth)
{
    if (!S.inited || emotion >= FACE_EMOTION_COUNT) return;
    S.target = emotion;
    if (!smooth) {
        apply_emotion_instant(emotion);
        update_display();
    } else {
        S.trans_pct = 0;
    }
}

face_emotion_t kawaii_get_emotion(void) { return S.current; }

void kawaii_trigger_blink(void)
{
    if (!S.inited || S.blinking) return;
    S.blinking    = true;
    S.blink_phase = 0;
}

/* ------------------------------------------------------------------ */
/* Display update — widget positions/sizes, triggers auto-invalidate   */
/* ------------------------------------------------------------------ */
static void update_single_eye(eye_t *e, uint8_t openness, bool is_left)
{
    int16_t eh = (int16_t)(S.eye_h_max * openness / 100);
    if (eh < 3) eh = 3;

    lv_obj_set_pos(e->sclera, e->base_x, e->base_y + S.bounce);
    lv_obj_set_height(e->sclera, eh);
    lv_obj_set_style_radius(e->sclera, eh / 2 + 2, 0);

    int16_t iris_x = (S.eye_w - S.iris_w) / 2 + S.pupil_x;
    int16_t iris_y = (eh - S.iris_h) / 2 + S.pupil_y;
    if (iris_x < 0) iris_x = 0;
    if (iris_x + S.iris_w > S.eye_w) iris_x = S.eye_w - S.iris_w;
    lv_obj_set_pos(e->iris, iris_x, iris_y);

    int8_t  ang   = is_left ? S.brow_l_ang : S.brow_r_ang;
    int16_t brow_y = e->base_y + S.bounce - 12 + S.brow_height;
    lv_obj_set_pos(e->brow, e->base_x + (S.eye_w - S.brow_w) / 2, brow_y);
    lv_obj_set_style_transform_rotation(e->brow, (int16_t)(-ang * 25), 0);

    lv_opa_t bopa = (lv_opa_t)(S.blush * LV_OPA_COVER / 100);
    lv_obj_set_style_bg_opa(e->blush, bopa, 0);
    lv_obj_set_y(e->blush, e->base_y + S.bounce + eh + 3);
}

static void update_mouth(void)
{
    int8_t  c = S.mouth_curve;
    int16_t mw, mh, my;
    lv_color_t col;

    if (c > 50) {
        mw = S.mouth_w;
        mh = S.mouth_h + S.mouth_h / 2;
        my = S.mouth_base_y + S.bounce;
        col = lv_color_make(220, 55, 75);
    } else if (c > 10) {
        mw = (int16_t)(S.mouth_w * 0.65f);
        mh = S.mouth_h;
        my = S.mouth_base_y + S.bounce;
        col = lv_color_make(210, 65, 85);
    } else if (c < -20) {
        mw = (int16_t)(S.mouth_w * 0.72f);
        mh = S.mouth_h;
        my = S.mouth_base_y + S.bounce + 8;
        col = lv_color_make(170, 45, 65);
    } else {
        mw = (int16_t)(S.mouth_w * 0.52f);
        mh = (int16_t)(S.mouth_h * 0.55f);
        my = S.mouth_base_y + S.bounce;
        col = lv_color_make(190, 55, 75);
    }

    lv_obj_set_pos(S.mouth, S.face_sz / 2 - mw / 2, my);
    lv_obj_set_size(S.mouth, mw, mh);
    lv_obj_set_style_radius(S.mouth, mh / 2, 0);
    lv_obj_set_style_bg_color(S.mouth, col, 0);
}

static void update_display(void)
{
    update_single_eye(&S.el, S.eye_l_open, true);
    update_single_eye(&S.er, S.eye_r_open, false);
    update_mouth();
}

/* ------------------------------------------------------------------ */
/* Emotion helpers                                                      */
/* ------------------------------------------------------------------ */
static void apply_emotion_instant(face_emotion_t e)
{
    S.current   = e;
    S.target    = e;
    S.trans_pct = 100;
    const emo_params_t *p = &EMO_TABLE[e];
    S.eye_l_open  = p->left_eye;
    S.eye_r_open  = p->right_eye;
    S.mouth_curve = p->mouth;
    S.brow_l_ang  = p->brow_l;
    S.brow_r_ang  = p->brow_r;
    S.brow_height = p->brow_h;
    S.blush       = p->blush;
    S.pupil_x     = 0;
    S.pupil_y     = 0;
    S.bounce      = 0;
}

static void lerp_toward_target(void)
{
    const emo_params_t *cur = &EMO_TABLE[S.current];
    const emo_params_t *tgt = &EMO_TABLE[S.target];
    int pct = S.trans_pct;
#define LERP(a,b) ((a) + ((int)(b)-(int)(a)) * pct / 100)
    S.eye_l_open  = (uint8_t)LERP(cur->left_eye,  tgt->left_eye);
    S.eye_r_open  = (uint8_t)LERP(cur->right_eye, tgt->right_eye);
    S.mouth_curve = (int8_t) LERP(cur->mouth,      tgt->mouth);
    S.brow_l_ang  = (int8_t) LERP(cur->brow_l,     tgt->brow_l);
    S.brow_r_ang  = (int8_t) LERP(cur->brow_r,     tgt->brow_r);
    S.brow_height = (int8_t) LERP(cur->brow_h,     tgt->brow_h);
    S.blush       = (uint8_t)LERP(cur->blush,      tgt->blush);
#undef LERP
    S.trans_pct += 8;
    if (S.trans_pct >= 100) {
        S.trans_pct = 100;
        S.current   = S.target;
    }
}

/* ------------------------------------------------------------------ */
/* Timer — runs every anim_ms, updates widget props (auto-invalidates) */
/* ------------------------------------------------------------------ */
static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!S.inited) return;

    uint32_t now = lv_tick_get();
    bool redraw = false;

    if (S.blinking) {
        S.blink_phase += 20;
        if (S.blink_phase >= 100) {
            S.blink_phase = 0;
            S.blinking    = false;
            S.last_blink_ms = now;
            S.eye_l_open = EMO_TABLE[S.current].left_eye;
            S.eye_r_open = EMO_TABLE[S.current].right_eye;
        } else {
            uint8_t op = (S.blink_phase < 50)
                ? (uint8_t)(100 - S.blink_phase * 2)
                : (uint8_t)((S.blink_phase - 50) * 2);
            S.eye_l_open = op;
            S.eye_r_open = op;
        }
        redraw = true;
    } else if (S.auto_blink && (now - S.last_blink_ms) > S.blink_interval_ms) {
        kawaii_trigger_blink();
    }

    if (S.current != S.target) {
        lerp_toward_target();
        redraw = true;
    }

    S.bounce_cnt++;
    S.pupil_cnt++;

    switch (S.current) {
    case FACE_NEUTRAL:
        S.bounce = (int8_t)(2.f * sinf(S.bounce_cnt * 0.05f));
        {
            uint32_t gp = S.pupil_cnt % 420;
            if      (gp < 160) { S.pupil_x = 0; S.pupil_y = 0; }
            else if (gp < 195) { S.pupil_x = (int8_t)(6.f*(gp-160)/35.f); S.pupil_y = 0; }
            else if (gp < 240) { S.pupil_x = 6; S.pupil_y = 0; }
            else if (gp < 275) { S.pupil_x = (int8_t)(6.f*(1.f-(gp-240)/35.f)); S.pupil_y = 0; }
            else if (gp < 340) { S.pupil_x = 0; S.pupil_y = 0; }
            else if (gp < 368) { float q=(gp-340)/28.f; S.pupil_x=(int8_t)(-4*q); S.pupil_y=(int8_t)(4*q); }
            else if (gp < 390) { S.pupil_x = -4; S.pupil_y = 4; }
            else               { float q=(gp-390)/30.f; S.pupil_x=(int8_t)(-4*(1-q)); S.pupil_y=(int8_t)(4*(1-q)); }
        }
        if (S.bounce_cnt % 3 == 0) redraw = true;
        break;
    case FACE_HAPPY:
        S.bounce  = (int8_t)(3.f * sinf(S.bounce_cnt * 0.25f));
        S.pupil_x = (int8_t)(5.f * cosf(S.pupil_cnt * 0.08f));
        S.pupil_y = (int8_t)(3.f * sinf(S.pupil_cnt * 0.08f));
        S.blush   = (uint8_t)(72 + 18 * fabsf(sinf(S.bounce_cnt * 0.13f)));
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    case FACE_SURPRISED:
        S.bounce  = (int8_t)((S.bounce_cnt % 6) < 3 ? 1 : -1);
        S.pupil_x = 0; S.pupil_y = -6;
        if (S.bounce_cnt % 3 == 0) redraw = true;
        break;
    case FACE_CONFUSED:
        S.bounce  = (int8_t)(2.f*sinf(S.bounce_cnt*0.07f) + sinf(S.bounce_cnt*0.19f));
        S.pupil_x = (int8_t)(6.f * cosf(S.pupil_cnt * 0.04f));
        S.pupil_y = (int8_t)(4.f * sinf(S.pupil_cnt * 0.06f));
        if (S.bounce_cnt % 3 == 0) redraw = true;
        break;
    case FACE_WORKING_HARD:
        S.bounce  = (int8_t)((S.bounce_cnt % 8) < 4 ? 1 : -1);
        S.pupil_x = 0; S.pupil_y = 4;
        if (S.bounce_cnt % 4 == 0) redraw = true;
        break;
    case FACE_SAD:
        S.bounce  = (int8_t)(1.5f * sinf(S.bounce_cnt * 0.06f));
        S.pupil_y = (int8_t)(3 + 3*fabsf(sinf(S.bounce_cnt*0.08f)));
        if (S.bounce_cnt % 4 == 0) redraw = true;
        break;
    case FACE_ANGRY:
        S.blush  = (uint8_t)(40 + 28*fabsf(sinf(S.bounce_cnt*0.3f)));
        S.bounce = (int8_t)((S.bounce_cnt % 8) < 2 ? 1 : 0);
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    case FACE_SLEEPY:
        S.bounce = (int8_t)(3.f * sinf(S.bounce_cnt * 0.04f));
        if (!S.blinking) {
            int16_t droop = (int16_t)(20*fabsf(sinf(S.bounce_cnt*0.03f)));
            int16_t o = 35 - droop;
            S.eye_l_open = S.eye_r_open = (uint8_t)(o < 8 ? 8 : o);
        }
        if (S.bounce_cnt % 3 == 0) redraw = true;
        break;
    case FACE_EXCITED:
        S.bounce  = (int8_t)(4.f * sinf(S.bounce_cnt * 0.5f));
        S.pupil_x = (int8_t)(((S.pupil_cnt/3)%2) ? 7 : -7);
        S.pupil_y = (int8_t)(((S.pupil_cnt/5)%2) ? 6 : -6);
        S.blush   = (uint8_t)(75 + 20*fabsf(sinf(S.bounce_cnt*0.2f)));
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    default:
        if (S.bounce_cnt % 10 == 0) redraw = true;
        break;
    }

    if (redraw) update_display();
}
