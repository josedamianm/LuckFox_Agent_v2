#ifndef IPC_SERVER_H
#define IPC_SERVER_H

typedef void (*ipc_cmd_handler_t)(const char *json_line, int client_fd);

int  ipc_server_init(const char *sock_path);
void ipc_server_poll(void);
void ipc_server_deinit(void);
void ipc_server_set_handler(ipc_cmd_handler_t handler);
void ipc_server_send(int client_fd, const char *json_line);
void ipc_server_broadcast(const char *json_line);

#endif
