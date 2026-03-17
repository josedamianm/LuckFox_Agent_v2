#ifndef SCR_AGENT_H
#define SCR_AGENT_H

typedef enum {
    AGENT_IDLE = 0,
    AGENT_LISTENING,
    AGENT_THINKING,
    AGENT_SPEAKING,
    AGENT_ERROR
} agent_state_t;

void agent_screen_init(void);
void agent_set_state(agent_state_t state, const char *text);
void agent_tick(void);

#endif
