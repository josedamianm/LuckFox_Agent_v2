#include "kawaii_face.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    uint8_t left_eye;
    uint8_t right_eye;
    int8_t  mouth;
    int8_t  brow_l;
    int8_t  brow_r;
    int8_t  brow_h;
    uint8_t blush;
    uint8_t sparkle;
} emo_params_t;

static const emo_params_t EMO_TABLE[FACE_EMOTION_COUNT] = {
[FACE_NEUTRAL]      = {100, 100,   0,   0,   0,   0,   0,   0},
[FACE_HAPPY]        = { 96,  96,  90,  -4,  -4,  -5,  82,  90},
[FACE_SURPRISED]    = {100, 100,  50,   0,   0, -10,  20,  60},
[FACE_CONFUSED]     = { 88,  75,  12, -18,   8,  -3,  15,   0},
[FACE_WORKING_HARD] = { 65,  65,   0,  22, -22,   4,  60,   0},
[FACE_SAD]          = { 60,  60, -75, -15,  15,   3,   0,   0},
[FACE_ANGRY]        = { 75,  75, -45,  25, -25,   5,  50,   0},
[FACE_SLEEPY]       = { 35,  35,  -5,  -5,   5,   8,  30,   0},
[FACE_EXCITED]      = {100, 100,  95,   8,   8,  -8,  85, 100},
};

typedef struct {
    lv_obj_t    *container;
    lv_obj_t    *eye_l_canvas;
    lv_obj_t    *eye_r_canvas;
    lv_obj_t    *mouth_canvas;
    lv_color_t  *eye_l_buf;
    lv_color_t  *eye_r_buf;
    lv_color_t  *mouth_buf;

    uint16_t face_sz;
    uint16_t eye_cw;
    uint16_t mouth_cw;
    uint16_t mouth_ch;

    uint8_t  eye_l_open;
    uint8_t  eye_r_open;
    int8_t   mouth_curve;
    int8_t   brow_l_ang;
    int8_t   brow_r_ang;
    int8_t   brow_height;
    uint8_t  blush;
    uint8_t  sparkle;
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

static void draw_eye(lv_obj_t *canvas, uint8_t openness, bool is_left);
static void draw_mouth(lv_obj_t *canvas, int8_t curve);
static void timer_cb(lv_timer_t *t);
static void apply_emotion_instant(face_emotion_t e);

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
    uint16_t fsz = (pw < ph) ? (uint16_t)pw : (uint16_t)ph;
    S.face_sz  = fsz;
    S.eye_cw   = (uint16_t)(fsz * 0.46f);
    S.mouth_cw = (uint16_t)(fsz * 0.46f);
    S.mouth_ch = (uint16_t)(fsz * 0.38f);

    S.container = lv_obj_create(parent);
    lv_obj_set_size(S.container, fsz, fsz);
    lv_obj_center(S.container);
    lv_obj_set_style_bg_opa(S.container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(S.container, 0, 0);
    lv_obj_set_style_pad_all(S.container, 0, 0);
    lv_obj_clear_flag(S.container, LV_OBJ_FLAG_SCROLLABLE);

    size_t eye_sz   = (size_t)S.eye_cw * S.eye_cw;
    size_t mouth_sz = (size_t)S.mouth_cw * S.mouth_ch;
    S.eye_l_buf = malloc(eye_sz   * sizeof(lv_color_t));
    S.eye_r_buf = malloc(eye_sz   * sizeof(lv_color_t));
    S.mouth_buf = malloc(mouth_sz * sizeof(lv_color_t));
    if (!S.eye_l_buf || !S.eye_r_buf || !S.mouth_buf) return;

    int16_t gap     = S.eye_cw / 4;
    int16_t eye_y   = (int16_t)(fsz * 0.10f);
    int16_t lx      = (int16_t)(fsz / 2) - S.eye_cw - gap / 2;
    int16_t rx      = (int16_t)(fsz / 2) + gap / 2;
    int16_t mouth_y = (int16_t)(fsz * 0.62f);
    int16_t mouth_x = (int16_t)(fsz / 2) - S.mouth_cw / 2;

    S.eye_l_canvas = lv_canvas_create(S.container);
    lv_canvas_set_buffer(S.eye_l_canvas, S.eye_l_buf,
                         S.eye_cw, S.eye_cw, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(S.eye_l_canvas, lx, eye_y);

    S.eye_r_canvas = lv_canvas_create(S.container);
    lv_canvas_set_buffer(S.eye_r_canvas, S.eye_r_buf,
                         S.eye_cw, S.eye_cw, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(S.eye_r_canvas, rx, eye_y);

    S.mouth_canvas = lv_canvas_create(S.container);
    lv_canvas_set_buffer(S.mouth_canvas, S.mouth_buf,
                         S.mouth_cw, S.mouth_ch, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(S.mouth_canvas, mouth_x, mouth_y);

    apply_emotion_instant(FACE_NEUTRAL);

    S.last_blink_ms = lv_tick_get();
    S.timer = lv_timer_create(timer_cb, anim_ms, NULL);
    S.inited = true;
}

void kawaii_deinit(void)
{
    if (!S.inited) return;
    if (S.timer)        { lv_timer_delete(S.timer); S.timer = NULL; }
    if (S.eye_l_canvas) lv_obj_delete(S.eye_l_canvas);
    if (S.eye_r_canvas) lv_obj_delete(S.eye_r_canvas);
    if (S.mouth_canvas) lv_obj_delete(S.mouth_canvas);
    if (S.container)    lv_obj_delete(S.container);
    free(S.eye_l_buf); free(S.eye_r_buf); free(S.mouth_buf);
    memset(&S, 0, sizeof S);
}

void kawaii_set_emotion(face_emotion_t emotion, bool smooth)
{
    if (!S.inited || emotion >= FACE_EMOTION_COUNT) return;
    S.target = emotion;
    if (!smooth) {
        apply_emotion_instant(emotion);
        draw_eye(S.eye_l_canvas, S.eye_l_open, true);
        draw_eye(S.eye_r_canvas, S.eye_r_open, false);
        draw_mouth(S.mouth_canvas, S.mouth_curve);
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

static void draw_eye(lv_obj_t *canvas, uint8_t openness, bool is_left)
{
    if (!canvas) return;
    uint16_t W = S.eye_cw, H = S.eye_cw;
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    int16_t ew = (int16_t)(W * 0.75f);
    int16_t eh = (int16_t)(ew * openness / 100);
    if (eh < 6) eh = 6;
    int16_t cx = W / 2;
    int16_t cy = (int16_t)(H * 0.58f) + S.bounce;

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_make(80, 60, 40);
    line.width = 4;
    line.opa   = LV_OPA_COVER;
    line.round_start = 1; line.round_end = 1;
    int8_t  ba  = is_left ? S.brow_l_ang : S.brow_r_ang;
    int16_t bw  = (int16_t)(ew * 0.88f);
    int16_t by  = cy - ew / 2 - 6 + S.brow_height;
    int16_t dy  = (int16_t)(bw * 0.25f * sinf(ba * 3.14159f / 180.f));
    if (is_left) {
        line.p1.x = cx - bw/2; line.p1.y = by - dy;
        line.p2.x = cx + bw/2; line.p2.y = by + dy;
    } else {
        line.p1.x = cx - bw/2; line.p1.y = by + dy;
        line.p2.x = cx + bw/2; line.p2.y = by - dy;
    }
    lv_draw_line(&layer, &line);

    if (S.blush > 0) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color     = lv_color_make(255, 130, 160);
        r.bg_opa       = (lv_opa_t)((S.blush * LV_OPA_COVER) / 100);
        r.radius       = 8;
        r.border_width = 0;
        lv_area_t a = { cx-10, cy + ew/2 + 2, cx+10, cy + ew/2 + 9 };
        lv_draw_rect(&layer, &r, &a);
    }

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);

    if (openness > 20) {
        r.bg_color     = lv_color_white();
        r.bg_opa       = LV_OPA_COVER;
        r.border_color = lv_color_black();
        r.border_width = 3;
        r.border_opa   = LV_OPA_COVER;
        r.radius       = 14;
        lv_area_t ea   = { cx-ew/2, cy-eh/2, cx+ew/2, cy+eh/2 };
        lv_draw_rect(&layer, &r, &ea);

        if (openness > 30 && eh > 16) {
            int16_t iw  = (int16_t)(ew * 0.55f);
            int16_t ih  = (int16_t)(eh * 0.75f);
            if (ih > iw) ih = iw;
            int16_t icx = cx + S.pupil_x;
            int16_t icy = cy + S.pupil_y;
            if (icx - iw/2 < cx - ew/2 + 3) icx = cx - ew/2 + iw/2 + 3;
            if (icx + iw/2 > cx + ew/2 - 3) icx = cx + ew/2 - iw/2 - 3;
            if (icy - ih/2 < cy - eh/2 + 3) icy = cy - eh/2 + ih/2 + 3;
            if (icy + ih/2 > cy + eh/2 - 3) icy = cy + eh/2 - ih/2 - 3;

            r.bg_color     = lv_color_make(50, 180, 255);
            r.border_color = lv_color_make(30, 140, 230);
            r.border_width = 2;
            r.radius       = 8;
            lv_area_t ia   = { icx-iw/2, icy-ih/2, icx+iw/2, icy+ih/2 };
            lv_draw_rect(&layer, &r, &ia);

            int16_t pw2 = iw/4, ph2 = ih/3;
            r.bg_color     = lv_color_black();
            r.border_width = 0;
            r.radius       = 6;
            lv_area_t pa   = { icx-pw2, icy-ph2, icx+pw2, icy+ph2 };
            lv_draw_rect(&layer, &r, &pa);

            int16_t hlw = pw2/2 < 3 ? 3 : pw2/2;
            int16_t hlh = ph2/2 < 3 ? 3 : ph2/2;
            r.bg_color = lv_color_white(); r.radius = 3;
            lv_area_t hl = {
                icx - pw2/2 - hlw/2, icy - ph2/2 - hlh/2,
                icx - pw2/2 + hlw/2, icy - ph2/2 + hlh/2
            };
            lv_draw_rect(&layer, &r, &hl);
        }
    } else {
        line.color = lv_color_black();
        line.width = 4;
        line.round_start = 1; line.round_end = 1;
        line.p1.x = cx - ew/2; line.p1.y = cy;
        line.p2.x = cx + ew/2; line.p2.y = cy;
        lv_draw_line(&layer, &line);
    }

    if (S.sparkle > 0) {
        r.bg_color     = lv_color_make(255, 255, 80);
        r.bg_opa       = (lv_opa_t)((S.sparkle * LV_OPA_COVER) / 100);
        r.border_width = 0; r.radius = 2;
        for (int i = 0; i < 3; i++) {
            float angle = (i * 120 + S.sparkle * 3.6f) * 3.14159f / 180.f;
            int16_t sx = cx + (int16_t)((ew / 2 + 8) * cosf(angle));
            int16_t sy = cy + (int16_t)((ew / 2 + 8) * sinf(angle));
            lv_area_t sa = { sx-2, sy-2, sx+2, sy+2 };
            lv_draw_rect(&layer, &r, &sa);
        }
    }

    lv_canvas_finish_layer(canvas, &layer);
}

static void draw_mouth(lv_obj_t *canvas, int8_t curve)
{
    if (!canvas) return;
    uint16_t W = S.mouth_cw, H = S.mouth_ch;
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    int16_t cx  = W / 2;
    int16_t mw  = (int16_t)(W * 0.85f);
    int16_t off = (int16_t)(H * curve / 140);
    int16_t cy  = H / 2 + S.bounce;
    if (cy + abs(off) + 6 > H - 4) cy = H - 4 - abs(off) - 6;
    if (cy - abs(off) - 6 < 4)     cy = 4 + abs(off) + 6;

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa       = LV_OPA_COVER;
    r.border_opa   = LV_OPA_COVER;
    r.border_color = lv_color_black();
    r.border_width = 3;

    if (curve > 50) {
        int16_t mh = (int16_t)(H * 0.48f);
        r.bg_color = lv_color_make(210, 60, 80);
        r.radius   = 12;
        lv_area_t a = { cx - mw/2, cy + off/2 - mh/2,
                        cx + mw/2, cy + off/2 + mh/2 };
        lv_draw_rect(&layer, &r, &a);
    } else if (curve > 15) {
        int16_t sh = (int16_t)(H * 0.26f);
        int16_t sw = (int16_t)(mw * 0.65f);
        r.bg_color = lv_color_make(210, 80, 100);
        r.radius   = 6;
        lv_area_t a = { cx - sw/2, cy + off, cx + sw/2, cy + off + sh };
        lv_draw_rect(&layer, &r, &a);
    } else if (curve < -20) {
        int16_t fh = (int16_t)(H * 0.28f);
        r.bg_color = lv_color_make(180, 50, 70);
        r.radius   = 8;
        lv_area_t a = { cx - mw/2, cy + off, cx + mw/2, cy + off + fh };
        lv_draw_rect(&layer, &r, &a);
    } else {
        int16_t lh = (int16_t)(H * 0.20f);
        int16_t lw = (int16_t)(mw * 0.55f);
        r.bg_color = lv_color_make(190, 60, 80);
        r.radius   = 5;
        lv_area_t a = { cx - lw/2, cy, cx + lw/2, cy + lh };
        lv_draw_rect(&layer, &r, &a);
    }

    lv_canvas_finish_layer(canvas, &layer);
}

static void apply_emotion_instant(face_emotion_t e)
{
    S.current    = e;
    S.trans_pct  = 100;
    const emo_params_t *p = &EMO_TABLE[e];
    S.eye_l_open  = p->left_eye;
    S.eye_r_open  = p->right_eye;
    S.mouth_curve = p->mouth;
    S.brow_l_ang  = p->brow_l;
    S.brow_r_ang  = p->brow_r;
    S.brow_height = p->brow_h;
    S.blush       = p->blush;
    S.sparkle     = p->sparkle;
}

static void lerp_toward_target(void)
{
    const emo_params_t *cur = &EMO_TABLE[S.current];
    const emo_params_t *tgt = &EMO_TABLE[S.target];
    int pct = S.trans_pct;
#define LERP(a,b) ((a) + ((int)(b) - (int)(a)) * pct / 100)
    S.eye_l_open  = (uint8_t)LERP(cur->left_eye,  tgt->left_eye);
    S.eye_r_open  = (uint8_t)LERP(cur->right_eye, tgt->right_eye);
    S.mouth_curve = (int8_t) LERP(cur->mouth,      tgt->mouth);
    S.brow_l_ang  = (int8_t) LERP(cur->brow_l,     tgt->brow_l);
    S.brow_r_ang  = (int8_t) LERP(cur->brow_r,     tgt->brow_r);
    S.brow_height = (int8_t) LERP(cur->brow_h,     tgt->brow_h);
    S.blush       = (uint8_t)LERP(cur->blush,      tgt->blush);
    S.sparkle     = (uint8_t)LERP(cur->sparkle,    tgt->sparkle);
#undef LERP
    S.trans_pct += 8;
    if (S.trans_pct >= 100) {
        S.trans_pct = 100;
        S.current   = S.target;
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!S.inited) return;

    bool redraw = false;
    uint32_t now = lv_tick_get();

    if (S.blinking) {
        S.blink_phase += 20;
        if (S.blink_phase >= 100) {
            S.blink_phase = 0; S.blinking = false;
            S.last_blink_ms = now;
        }
        uint8_t op = (S.blink_phase < 50)
                     ? (uint8_t)(100 - S.blink_phase * 2)
                     : (uint8_t)((S.blink_phase - 50) * 2);
        S.eye_l_open = op; S.eye_r_open = op;
        redraw = true;
    } else if (S.auto_blink && (now - S.last_blink_ms) > S.blink_interval_ms) {
        kawaii_trigger_blink();
    }

    if (S.current != S.target && S.trans_pct < 100) {
        lerp_toward_target();
        redraw = true;
    }

    S.bounce_cnt++;
    S.pupil_cnt++;

    switch (S.current) {
    case FACE_NEUTRAL:
    {
        S.bounce = (int8_t)(1.2f * sinf(S.bounce_cnt * 0.05f));
        uint32_t gp = S.pupil_cnt % 420;
        if      (gp < 160) { S.pupil_x = 0; S.pupil_y = 0; }
        else if (gp < 195) { S.pupil_x = (int8_t)(7.f*(gp-160)/35.f); S.pupil_y = 0; }
        else if (gp < 240) { S.pupil_x = 7; S.pupil_y = 0; }
        else if (gp < 275) { S.pupil_x = (int8_t)(7.f*(1.f-(gp-240)/35.f)); S.pupil_y = 0; }
        else if (gp < 340) { S.pupil_x = 0; S.pupil_y = 0; }
        else if (gp < 368) { float q=(gp-340)/28.f; S.pupil_x=(int8_t)(-5*q); S.pupil_y=(int8_t)(5*q); }
        else if (gp < 390) { S.pupil_x = -5; S.pupil_y = 5; }
        else               { float q=(gp-390)/30.f; S.pupil_x=(int8_t)(-5*(1-q)); S.pupil_y=(int8_t)(5*(1-q)); }
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    }
    case FACE_HAPPY:
    {
        float ha = (S.pupil_cnt % 80) * 0.1572f;
        S.pupil_x = (int8_t)(7.f * cosf(ha));
        S.pupil_y = (int8_t)(4.f * sinf(ha));
        S.bounce  = (int8_t)(3.5f * sinf(S.bounce_cnt * 0.28f));
        S.sparkle = (uint8_t)(65 + 35 * fabsf(sinf(S.bounce_cnt * 0.20f)));
        S.blush   = (uint8_t)(72 + 18 * fabsf(sinf(S.bounce_cnt * 0.13f)));
        if (S.trans_pct == 100)
            S.mouth_curve = (int8_t)(87 + 8 * fabsf(sinf(S.bounce_cnt * 0.28f)));
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    }
    case FACE_SURPRISED:
        S.bounce = (S.bounce_cnt % 4) - 2;
        S.pupil_x = 0; S.pupil_y = -8;
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    case FACE_CONFUSED:
        S.bounce  = (int8_t)(2.f*sinf(S.bounce_cnt*0.07f) + sinf(S.bounce_cnt*0.19f));
        S.pupil_x = (int8_t)(7.f * cosf(S.pupil_cnt * 0.03f));
        S.pupil_y = (int8_t)(5.f * sinf(S.pupil_cnt * 0.05f));
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    case FACE_WORKING_HARD:
        S.bounce  = (S.bounce_cnt % 6 < 3) ? 1 : -1;
        S.pupil_x = 0; S.pupil_y = 4;
        if (S.bounce_cnt % 6 == 0) redraw = true;
        break;
    case FACE_SAD:
        S.bounce  = (int8_t)(1.5f * sinf(S.bounce_cnt * 0.06f));
        S.pupil_y = (int8_t)(3 + 3*fabsf(sinf(S.bounce_cnt*0.08f)));
        if (S.bounce_cnt % 4 == 0) redraw = true;
        break;
    case FACE_ANGRY:
        S.blush  = (uint8_t)(40 + 28*fabsf(sinf(S.bounce_cnt*0.3f)));
        S.bounce = (S.bounce_cnt % 8 < 2) ? 1 : 0;
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    case FACE_SLEEPY:
        S.bounce = (int8_t)(3.f * sinf(S.bounce_cnt * 0.04f));
        if (S.trans_pct == 100 && !S.blinking) {
            int16_t droop = (int16_t)(20*fabsf(sinf(S.bounce_cnt*0.03f)));
            int16_t o = 35 - droop;
            S.eye_l_open = S.eye_r_open = (uint8_t)(o < 10 ? 10 : o);
        }
        if (S.bounce_cnt % 3 == 0) redraw = true;
        break;
    case FACE_EXCITED:
        S.bounce  = (int8_t)(3.5f * sinf(S.bounce_cnt * 0.55f));
        S.pupil_x = (int8_t)(((S.pupil_cnt/3)%2) ? 9 : -9);
        S.pupil_y = (int8_t)(((S.pupil_cnt/5)%2) ? 7 : -7);
        S.sparkle = (uint8_t)(80 + 20*fabsf(sinf(S.bounce_cnt*0.4f)));
        S.blush   = (uint8_t)(75 + 20*fabsf(sinf(S.bounce_cnt*0.2f)));
        if (S.bounce_cnt % 2 == 0) redraw = true;
        break;
    default:
        if (S.bounce_cnt % 10 == 0) redraw = true;
        break;
    }

    if (redraw) {
        draw_eye(S.eye_l_canvas, S.eye_l_open, true);
        draw_eye(S.eye_r_canvas, S.eye_r_open, false);
        draw_mouth(S.mouth_canvas, S.mouth_curve);
    }
}
