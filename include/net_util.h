#ifndef NET_UTIL_H
#define NET_UTIL_H

#include <stddef.h>

#include "chat_protocol.h"

typedef struct {
    int    fd;
    char   buffer[CHAT_FRAME_MAX];
    size_t used;
} line_reader_t;

void reader_init(line_reader_t *reader, int fd);
/*
 * Read one '\n'-terminated line.  The trailing CR/LF is removed.
 * Returns 1 for a line, 0 for clean EOF, -1 for an I/O error,
 * -2 for an over-long frame, -3 for an embedded NUL and -4 for timeout.
 */
int reader_readline(line_reader_t *reader, char *out, size_t cap);

int send_all(int fd, const char *data, size_t len);

int tcp_listen(const char *port);
int tcp_connect(const char *host, const char *port);

void set_recv_timeout(int fd, int seconds);
void set_send_timeout(int fd, int seconds);
void set_keepalive(int fd);

#endif /* NET_UTIL_H */
