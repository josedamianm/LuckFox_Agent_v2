#include "ipc_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAX_CLIENTS 4
#define RX_BUF_SIZE 4096

typedef struct {
    int fd;
    char buf[RX_BUF_SIZE];
    int buf_len;
} client_t;

static int server_fd = -1;
static client_t clients[MAX_CLIENTS];
static ipc_cmd_handler_t g_handler = NULL;

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int ipc_server_init(const char *sock_path) {
    struct sockaddr_un addr;

    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    unlink(sock_path);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;

    set_nonblock(server_fd);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    listen(server_fd, MAX_CLIENTS);
    return 0;
}

static void accept_clients(void) {
    int fd = accept(server_fd, NULL, NULL);
    if (fd < 0) return;

    set_nonblock(fd);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            clients[i].fd = fd;
            clients[i].buf_len = 0;
            return;
        }
    }
    close(fd);
}

static void process_client(client_t *c) {
    int n = read(c->fd, c->buf + c->buf_len, RX_BUF_SIZE - c->buf_len - 1);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(c->fd);
            c->fd = -1;
            c->buf_len = 0;
        }
        return;
    }
    c->buf_len += n;
    c->buf[c->buf_len] = '\0';

    char *start = c->buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (g_handler && (nl - start) > 0)
            g_handler(start, c->fd);
        start = nl + 1;
    }

    int remain = c->buf_len - (int)(start - c->buf);
    if (remain > 0)
        memmove(c->buf, start, remain);
    c->buf_len = remain;
}

void ipc_server_poll(void) {
    if (server_fd < 0) return;
    accept_clients();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0)
            process_client(&clients[i]);
    }
}

void ipc_server_deinit(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) { close(clients[i].fd); clients[i].fd = -1; }
    }
    if (server_fd >= 0) { close(server_fd); server_fd = -1; }
}

void ipc_server_set_handler(ipc_cmd_handler_t handler) {
    g_handler = handler;
}

void ipc_server_send(int client_fd, const char *json_line) {
    if (client_fd < 0) return;
    write(client_fd, json_line, strlen(json_line));
    write(client_fd, "\n", 1);
}

void ipc_server_broadcast(const char *json_line) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0)
            ipc_server_send(clients[i].fd, json_line);
    }
}
