#include "lf_theme.h"

void lf_theme_init(lv_display_t *disp) {
    lv_theme_t *th = lv_theme_default_init(
        disp,
        lv_color_make(0x00, 0xCC, 0xFF),
        lv_color_make(0xFF, 0x40, 0x80),
        true,
        &lv_font_montserrat_16
    );
    lv_display_set_theme(disp, th);
}
