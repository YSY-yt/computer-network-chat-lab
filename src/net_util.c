#include "net_util.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

void reader_init(line_reader_t *reader, int fd)
{
    reader->fd = fd;
    reader->used = 0;
}

int reader_readline(line_reader_t *reader, char *out, size_t cap)
{
    if (reader == NULL || out == NULL || cap == 0) {
        return -1;
    }

    for (;;) {
        char *newline = memchr(reader->buffer, '\n', reader->used);

        if (newline != NULL) {
            size_t line_len = (size_t)(newline - reader->buffer);
            size_t consumed = line_len + 1;
            size_t copy_len = line_len;

            if (line_len >= cap) {
                return -2;
            }
            if (copy_len > 0 && reader->buffer[copy_len - 1] == '\r') {
                --copy_len;
            }

            memcpy(out, reader->buffer, copy_len);
            out[copy_len] = '\0';

            memmove(reader->buffer, reader->buffer + consumed,
                    reader->used - consumed);
            reader->used -= consumed;

            /* The protocol is text-only; an embedded NUL is an error. */
            if (memchr(out, '\0', copy_len) != NULL ||
                strchr(out, '\r') != NULL) {
                return -3;
            }
            return 1;
        }

        if (reader->used == sizeof(reader->buffer)) {
            return -2;
        }

        ssize_t n = recv(reader->fd, reader->buffer + reader->used,
                         sizeof(reader->buffer) - reader->used, 0);
        if (n > 0) {
            reader->used += (size_t)n;
            continue;
        }
        if (n == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -4;
        }
        return -1;
    }
}

int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int try_bind(const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it;
    int listen_fd = -1;
    int one = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int rc = getaddrinfo(NULL, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        listen_fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listen_fd, it->ai_addr, it->ai_addrlen) == 0 &&
            listen(listen_fd, 64) == 0) {
            break;
        }
        close(listen_fd);
        listen_fd = -1;
    }

    int saved_errno = errno;
    freeaddrinfo(result);
    if (listen_fd < 0) {
        errno = saved_errno;
    }
    return listen_fd;
}

int tcp_listen(const char *port)
{
    int fd = try_bind(port);
    if (fd < 0) {
        fprintf(stderr, "cannot listen on port %s: %s\n", port,
                strerror(errno));
    }
    return fd;
}

int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it;
    int fd = -1;
    int last_errno = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(rc));
        return -1;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        last_errno = errno;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    if (fd < 0) {
        fprintf(stderr, "connect to %s:%s failed: %s\n", host, port,
                strerror(last_errno));
    }
    return fd;
}

void set_recv_timeout(int fd, int seconds)
{
    struct timeval timeout;

    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

void set_send_timeout(int fd, int seconds)
{
    struct timeval timeout;

    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

void set_keepalive(int fd)
{
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}
