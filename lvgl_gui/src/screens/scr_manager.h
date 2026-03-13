#ifndef SCR_MANAGER_H
#define SCR_MANAGER_H

typedef enum {
    SCR_STATUS = 0,
    SCR_EYES,
    SCR_EMOJI,
    SCR_TEXT,
    SCR_IMAGE,
    SCR_MENU,
    SCR_CHAT,
    SCR_COUNT
} scr_id_t;

/* Slide direction. AUTO picks left/right based on screen index order. */
typedef enum {
    SCR_DIR_AUTO = 0,
    SCR_DIR_LEFT,
    SCR_DIR_RIGHT,
    SCR_DIR_FADE,
} scr_dir_t;

void scr_manager_init(void);
void scr_manager_switch(scr_id_t id);
void scr_manager_switch_dir(scr_id_t id, scr_dir_t dir);
scr_id_t scr_manager_current(void);
const char *scr_manager_current_name(void);
void scr_manager_set_text(const char *text, const char *color, int scale);
void scr_manager_set_emoji(const char *name);
void scr_manager_set_image(const char *path);
void scr_manager_gif_start(int frame_count);
void scr_manager_gif_frame(int idx, const char *path, int duration_ms);
void scr_manager_set_chat_state(int state);
void scr_manager_set_chat_text(const char *text);

#endif
