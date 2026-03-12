#ifndef SCR_MANAGER_H
#define SCR_MANAGER_H

typedef enum {
    SCR_STATUS = 0,
    SCR_EYES,
    SCR_EMOJI,
    SCR_TEXT,
    SCR_IMAGE,
    SCR_MENU,
    SCR_COUNT
} scr_id_t;

void scr_manager_init(void);
void scr_manager_switch(scr_id_t id);
scr_id_t scr_manager_current(void);
const char *scr_manager_current_name(void);
void scr_manager_set_text(const char *text, const char *color, int scale);
void scr_manager_set_emoji(const char *name);
void scr_manager_set_image(const char *path);

#endif
