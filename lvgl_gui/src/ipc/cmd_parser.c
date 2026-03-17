#include "cmd_parser.h"
#include "ipc_server.h"
#include "../screens/scr_agent.h"
#include <string.h>
#include <stdio.h>

static const char *json_get_str(const char *json, const char *key, char *out, int out_sz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_sz - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return out;
}

void cmd_parser_init(void) {
    ipc_server_set_handler(cmd_parser_handle);
}

void cmd_parser_handle(const char *json_line, int client_fd) {
    char cmd[32]   = {0};
    char state[16] = {0};
    char text[256] = {0};

    json_get_str(json_line, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "set_state") == 0) {
        json_get_str(json_line, "state", state, sizeof(state));
        json_get_str(json_line, "text",  text,  sizeof(text));

        agent_state_t s = AGENT_IDLE;
        if      (strcmp(state, "listening") == 0) s = AGENT_LISTENING;
        else if (strcmp(state, "thinking")  == 0) s = AGENT_THINKING;
        else if (strcmp(state, "speaking")  == 0) s = AGENT_SPEAKING;
        else if (strcmp(state, "error")     == 0) s = AGENT_ERROR;

        agent_set_state(s, text[0] ? text : NULL);
        ipc_server_send(client_fd, "{\"status\":\"ok\"}");
    } else {
        ipc_server_send(client_fd, "{\"status\":\"error\",\"msg\":\"unknown cmd\"}");
    }
}
