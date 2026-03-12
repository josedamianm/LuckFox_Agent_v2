#ifndef CMD_PARSER_H
#define CMD_PARSER_H

void cmd_parser_init(void);
void cmd_parser_handle(const char *json_line, int client_fd);

#endif
