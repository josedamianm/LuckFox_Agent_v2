#include "cmd_parser.h"
#include "ipc_server.h"
#include "../screens/scr_manager.h"
#include "../anim/eyes_anim.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *json_get_str(const char *json, const char *key, char *out, int out_sz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
    return NULL;
}

static int json_get_int(const char *json, const char *key, int def) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return def;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') p++;
    return atoi(p);
}

void cmd_parser_init(void) {
    ipc_server_set_handler(cmd_parser_handle);
}

void cmd_parser_handle(const char *json_line, int client_fd) {
    char cmd[32] = {0};
    char name[64] = {0};
    char text[256] = {0};
    char color[16] = {0};
    char path[128] = {0};

    json_get_str(json_line, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "screen") == 0) {
        json_get_str(json_line, "name", name, sizeof(name));

        char dir_str[16] = {0};
        json_get_str(json_line, "dir", dir_str, sizeof(dir_str));
        scr_dir_t dir = SCR_DIR_AUTO;
        if (strcmp(dir_str, "left")  == 0) dir = SCR_DIR_LEFT;
        else if (strcmp(dir_str, "right") == 0) dir = SCR_DIR_RIGHT;
        else if (strcmp(dir_str, "fade")  == 0) dir = SCR_DIR_FADE;

        if (strcmp(name, "status") == 0) scr_manager_switch_dir(SCR_STATUS, dir);
        else if (strcmp(name, "eyes") == 0) scr_manager_switch_dir(SCR_EYES, dir);
        else if (strcmp(name, "emoji") == 0) {
            json_get_str(json_line, "emoji", text, sizeof(text));
            scr_manager_set_emoji(text);
            scr_manager_switch_dir(SCR_EMOJI, dir);
        }
        else if (strcmp(name, "text") == 0) {
            json_get_str(json_line, "text", text, sizeof(text));
            json_get_str(json_line, "color", color, sizeof(color));
            int scale = json_get_int(json_line, "scale", 3);
            scr_manager_set_text(text, color, scale);
            scr_manager_switch_dir(SCR_TEXT, dir);
        }
        else if (strcmp(name, "image") == 0) scr_manager_switch_dir(SCR_IMAGE, dir);
        else if (strcmp(name, "menu") == 0) scr_manager_switch_dir(SCR_MENU, dir);
        else if (strcmp(name, "chat") == 0) scr_manager_switch_dir(SCR_CHAT, dir);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "text") == 0) {
        json_get_str(json_line, "text", text, sizeof(text));
        json_get_str(json_line, "color", color, sizeof(color));
        int scale = json_get_int(json_line, "scale", 3);
        scr_manager_set_text(text, color, scale);
        scr_manager_switch(SCR_TEXT);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "emoji") == 0) {
        json_get_str(json_line, "name", name, sizeof(name));
        scr_manager_set_emoji(name);
        scr_manager_switch(SCR_EMOJI);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "eyes_gaze") == 0) {
        int zone = json_get_int(json_line, "zone", 4);
        eyes_anim_set_gaze(zone);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "eyes_blink") == 0) {
        eyes_anim_blink();
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "image") == 0) {
        json_get_str(json_line, "path", path, sizeof(path));
        scr_manager_set_image(path);
        scr_manager_switch(SCR_IMAGE);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "gif_start") == 0) {
        int frame_count = json_get_int(json_line, "frame_count", 0);
        scr_manager_gif_start(frame_count);
        scr_manager_switch_dir(SCR_IMAGE, SCR_DIR_FADE);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "gif_frame") == 0) {
        int idx = json_get_int(json_line, "index", 0);
        int dur = json_get_int(json_line, "duration_ms", 50);
        json_get_str(json_line, "path", path, sizeof(path));
        scr_manager_gif_frame(idx, path, dur);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "chat_state") == 0) {
        int state = json_get_int(json_line, "state", 0);
        scr_manager_set_chat_state(state);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "chat_text") == 0) {
        json_get_str(json_line, "text", text, sizeof(text));
        scr_manager_set_chat_text(text);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    }
    else if (strcmp(cmd, "get_state") == 0) {
        char resp[128];
        const char *scr_name = scr_manager_current_name();
        snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"screen\":\"%s\"}", scr_name);
        ipc_server_send(client_fd, resp);
    }
    else {
        ipc_server_send(client_fd, "{\"status\":\"error\",\"msg\":\"unknown cmd\"}");
    }
}
