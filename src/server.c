#include "chat_protocol.h"
#include "net_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT   "8888"
#define MAX_CLIENTS    64
#define LOGIN_TIMEOUT  10

typedef struct client {
    int              fd;
    int              linked;
    int              active;
    int              logged_in;
    uint64_t         out_seq;
    char             nickname[CHAT_NICK_MAX_BYTES + 1];
    char             peer[64];
    struct client   *next;
} client_t;

typedef struct {
    pthread_mutex_t lock;
    client_t       *head;
    size_t          count;
} server_t;

static server_t g_server;
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static int nickname_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

/* All send functions below must be called with g_server.lock held. */
static int send_frame_locked(client_t *client, const char *type,
                             const char *time_text, const char *from,
                             const char *body)
{
    char frame[CHAT_FRAME_MAX + 1];
    uint64_t seq = client->out_seq + 1;
    int frame_len;

    frame_len = chat_make_frame(frame, sizeof(frame), type, seq, time_text,
                                from, body);
    if (frame_len < 0) {
        fprintf(stderr, "server: cannot build %s frame for %s\n",
                type, client->nickname[0] != '\0' ? client->nickname : "?");
        return -1;
    }
    if (send_all(client->fd, frame, (size_t)frame_len) < 0) {
        client->active = 0;
        shutdown(client->fd, SHUT_RDWR);
        return -1;
    }
    client->out_seq = seq;
    return 0;
}

static void broadcast_locked(const char *type, const char *time_text,
                             const char *from, const char *body)
{
    for (client_t *client = g_server.head; client != NULL;
         client = client->next) {
        if (client->active) {
            send_frame_locked(client, type, time_text, from, body);
        }
    }
}

static void send_error_locked(client_t *client, const char *message)
{
    char now[CHAT_TIME_MAX];
    chat_now(now, sizeof(now));
    send_frame_locked(client, "ERROR", now, "server", message);
}

/* Send a rejection before the client has been inserted into the list. */
static void reject_and_close(int fd, const char *message)
{
    char frame[CHAT_FRAME_MAX + 1];
    char now[CHAT_TIME_MAX];
    int len;

    set_send_timeout(fd, 3);
    chat_now(now, sizeof(now));
    len = chat_make_frame(frame, sizeof(frame), "ERROR", 1, now, "server",
                          message);
    if (len > 0) {
        send_all(fd, frame, (size_t)len);
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

static int is_nickname_taken_locked(const char *nickname)
{
    for (client_t *client = g_server.head; client != NULL;
         client = client->next) {
        if (client->logged_in && nickname_equal(client->nickname, nickname)) {
            return 1;
        }
    }
    return 0;
}

static void unlink_client_locked(client_t *client)
{
    client_t **cursor = &g_server.head;

    while (*cursor != NULL && *cursor != client) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == client) {
        *cursor = client->next;
        client->linked = 0;
        if (g_server.count > 0) {
            --g_server.count;
        }
    }
}

static void build_user_list_locked(char *body, size_t cap)
{
    size_t visible_count = 0;
    size_t used;
    int first = 1;

    for (client_t *client = g_server.head; client != NULL;
         client = client->next) {
        if (client->active && client->logged_in) {
            ++visible_count;
        }
    }

    used = (size_t)snprintf(body, cap, "在线用户（%zu）：", visible_count);
    if (used >= cap) {
        body[cap - 1] = '\0';
        return;
    }

    for (client_t *client = g_server.head; client != NULL;
         client = client->next) {
        int written;
        if (!client->active || !client->logged_in) {
            continue;
        }
        written = snprintf(body + used, cap - used, "%s%s",
                           first ? "" : ", ", client->nickname);
        if (written < 0 || (size_t)written >= cap - used) {
            if (cap > 4) {
                strcpy(body + cap - 4, "...");
            }
            return;
        }
        used += (size_t)written;
        first = 0;
    }
}

static void *client_thread(void *arg)
{
    server_t *server = &g_server;
    client_t *client = (client_t *)arg;
    line_reader_t reader;
    chat_message_t message;
    char line[CHAT_FRAME_MAX + 1];
    char now[CHAT_TIME_MAX];
    char body[512];
    char list_body[CHAT_BODY_MAX + 1];
    int rc;

    set_recv_timeout(client->fd, LOGIN_TIMEOUT);
    reader_init(&reader, client->fd);

    rc = reader_readline(&reader, line, sizeof(line));
    if (rc != 1) {
        if (rc == -4) {
            reject_and_close(client->fd, "LOGIN_TIMEOUT: no LOGIN received");
            client->fd = -1;
        } else if (rc == -2) {
            reject_and_close(client->fd, "FRAME_TOO_LONG: LOGIN frame");
            client->fd = -1;
        } else {
            shutdown(client->fd, SHUT_RDWR);
            close(client->fd);
            client->fd = -1;
        }
        free(client);
        return NULL;
    }

    if (chat_parse_frame(line, &message) < 0 ||
        strcmp(message.type, "LOGIN") != 0 ||
        strcmp(message.body, CHAT_PROTO_VERSION) != 0) {
        reject_and_close(client->fd,
                         "BAD_LOGIN: expected LOGIN|1|TIME|nick|CHAT/1");
        client->fd = -1;
        free(client);
        return NULL;
    }
    if (!chat_valid_nickname(message.from)) {
        reject_and_close(client->fd,
                         "BAD_NICKNAME: 1-16 characters, no spaces or '|'");
        client->fd = -1;
        free(client);
        return NULL;
    }

    memcpy(client->nickname, message.from, strlen(message.from) + 1);

    pthread_mutex_lock(&server->lock);
    if (g_stop) {
        pthread_mutex_unlock(&server->lock);
        reject_and_close(client->fd, "SERVER_STOPPING");
        client->fd = -1;
        free(client);
        return NULL;
    }
    if (server->count >= MAX_CLIENTS) {
        pthread_mutex_unlock(&server->lock);
        reject_and_close(client->fd, "SERVER_FULL: too many clients");
        client->fd = -1;
        free(client);
        return NULL;
    }
    if (is_nickname_taken_locked(client->nickname)) {
        pthread_mutex_unlock(&server->lock);
        reject_and_close(client->fd,
                         "NICK_TAKEN: this nickname is already online");
        client->fd = -1;
        free(client);
        return NULL;
    }

    client->linked = 1;
    client->active = 1;
    client->out_seq = 0;
    client->next = server->head;
    server->head = client;
    ++server->count;

    chat_now(now, sizeof(now));
    snprintf(body, sizeof(body),
             "欢迎 %s！输入 /help 查看命令。Welcome! Online users: %zu",
             client->nickname, server->count);
    if (send_frame_locked(client, "WELCOME", now, "server", body) == 0) {
        client->logged_in = 1;
        snprintf(body, sizeof(body), "%s 加入了聊天室（joined the chat）",
                 client->nickname);
        broadcast_locked("JOIN", now, "server", body);
    }

    size_t online = server->count;
    char peer[sizeof(client->peer)];
    snprintf(peer, sizeof(peer), "%s", client->peer);
    pthread_mutex_unlock(&server->lock);

    if (!client->logged_in) {
        fprintf(stderr, "server: WELCOME to %s failed\n", peer);
    } else {
        printf("[%s] %s joined as \"%s\" (online=%zu)\n", now, peer,
               client->nickname, online);
        fflush(stdout);
    }

    /* The handshake is over; an idle connection is normal. */
    set_recv_timeout(client->fd, 0);

    while (!g_stop) {
        rc = reader_readline(&reader, line, sizeof(line));
        if (rc == 0) {
            break;
        }
        if (rc == -2) {
            pthread_mutex_lock(&server->lock);
            send_error_locked(client, "FRAME_TOO_LONG: closing connection");
            pthread_mutex_unlock(&server->lock);
            break;
        }
        if (rc < 0) {
            break;
        }

        int parse_rc = chat_parse_frame(line, &message);
        if (parse_rc < 0) {
            pthread_mutex_lock(&server->lock);
            send_error_locked(client,
                              "BAD_FRAME: malformed protocol message");
            pthread_mutex_unlock(&server->lock);
            if (parse_rc == -2) {
                break;
            }
            continue;
        }

        if (strcmp(message.type, "CHAT") == 0) {
            if (!chat_valid_text(message.body)) {
                pthread_mutex_lock(&server->lock);
                send_error_locked(client,
                                  "BAD_TEXT: 1-3000 UTF-8 bytes, not blank");
                pthread_mutex_unlock(&server->lock);
                continue;
            }
            pthread_mutex_lock(&server->lock);
            chat_now(now, sizeof(now));
            broadcast_locked("CHAT", now, client->nickname, message.body);
            pthread_mutex_unlock(&server->lock);
        } else if (strcmp(message.type, "PING") == 0) {
            pthread_mutex_lock(&server->lock);
            chat_now(now, sizeof(now));
            send_frame_locked(client, "PONG", now, "server", "pong");
            pthread_mutex_unlock(&server->lock);
        } else if (strcmp(message.type, "LIST") == 0) {
            pthread_mutex_lock(&server->lock);
            build_user_list_locked(list_body, sizeof(list_body));
            chat_now(now, sizeof(now));
            send_frame_locked(client, "INFO", now, "server", list_body);
            pthread_mutex_unlock(&server->lock);
        } else if (strcmp(message.type, "QUIT") == 0) {
            break;
        } else {
            pthread_mutex_lock(&server->lock);
            send_error_locked(client, "UNKNOWN_TYPE: unsupported command");
            pthread_mutex_unlock(&server->lock);
        }
    }

    pthread_mutex_lock(&server->lock);
    int announce = client->logged_in;
    client->active = 0;
    if (announce) {
        chat_now(now, sizeof(now));
        snprintf(body, sizeof(body), "%s 离开了聊天室（left the chat）",
                 client->nickname);
        broadcast_locked("LEAVE", now, "server", body);
    }
    unlink_client_locked(client);
    online = server->count;
    pthread_mutex_unlock(&server->lock);

    if (announce) {
        printf("[%s] %s left (online=%zu)\n", now, client->peer, online);
        fflush(stdout);
    }

    if (client->fd >= 0) {
        shutdown(client->fd, SHUT_RDWR);
        close(client->fd);
    }
    free(client);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *port = DEFAULT_PORT;
    pthread_attr_t attr;
    int rc;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        port = argv[1];
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    memset(&g_server, 0, sizeof(g_server));
    pthread_mutex_init(&g_server.lock, NULL);
    int listen_fd = tcp_listen(port);
    if (listen_fd < 0) {
        pthread_mutex_destroy(&g_server.lock);
        return 1;
    }

    printf("Chat server listening on 0.0.0.0:%s\n", port);
    printf("Protocol: %s, max clients: %d. Press Ctrl+C to stop.\n",
           CHAT_PROTO_VERSION, MAX_CLIENTS);
    fflush(stdout);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    while (!g_stop) {
        struct pollfd pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        rc = poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }
        if (rc == 0) {
            continue;
        }

        struct sockaddr_in address;
        socklen_t address_len = sizeof(address);
        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&address, &address_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        client_t *client = calloc(1, sizeof(*client));
        if (client == NULL) {
            reject_and_close(client_fd, "SERVER_ERROR: out of memory");
            continue;
        }
        client->fd = client_fd;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
        snprintf(client->peer, sizeof(client->peer), "%s:%u", ip,
                 (unsigned)ntohs(address.sin_port));
        set_send_timeout(client_fd, 5);
        set_keepalive(client_fd);

        pthread_t thread;
        rc = pthread_create(&thread, &attr, client_thread, client);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            close(client_fd);
            free(client);
            continue;
        }
    }

    pthread_attr_destroy(&attr);
    printf("\nStopping server...\n");
    fflush(stdout);

    pthread_mutex_lock(&g_server.lock);
    for (client_t *client = g_server.head; client != NULL;
         client = client->next) {
        client->active = 0;
        shutdown(client->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_server.lock);

    close(listen_fd);

    for (int i = 0; i < 50; ++i) {
        pthread_mutex_lock(&g_server.lock);
        size_t remaining = g_server.count;
        pthread_mutex_unlock(&g_server.lock);
        if (remaining == 0) {
            break;
        }
        struct timespec pause = {0, 20000000L};
        nanosleep(&pause, NULL);
    }

    printf("Server stopped.\n");
    return 0;
}
