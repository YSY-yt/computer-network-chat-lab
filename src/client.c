#include "chat_protocol.h"
#include "net_util.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_sock = -1;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_quitting = 0;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_print_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_client_seq = 1;
static uint64_t g_expected_seq = 1;
static int g_show_seq = 0;
static char g_nickname[CHAT_NICK_MAX_BYTES + 1];

static void print_safe(const char *fmt, ...)
{
    va_list args;

    pthread_mutex_lock(&g_print_lock);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_lock);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [-s] <host> <port> <nickname>\n"
            "  -s, --show-seq  show server->client sequence numbers\n"
            "example: %s 127.0.0.1 8888 Alice\n",
            program, program);
}

static int send_client_frame(const char *type, const char *body)
{
    char frame[CHAT_FRAME_MAX + 1];
    char now[CHAT_TIME_MAX];
    uint64_t seq;
    int frame_len;
    int rc;

    pthread_mutex_lock(&g_send_lock);
    seq = g_client_seq++;
    chat_now(now, sizeof(now));
    frame_len = chat_make_frame(frame, sizeof(frame), type, seq, now,
                                g_nickname, body);
    if (frame_len < 0) {
        pthread_mutex_unlock(&g_send_lock);
        return -1;
    }
    rc = send_all(g_sock, frame, (size_t)frame_len);
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

static void display_frame(const chat_message_t *msg)
{
    char seq_prefix[32] = "";

    if (g_show_seq) {
        snprintf(seq_prefix, sizeof(seq_prefix), "#%" PRIu64 " ", msg->seq);
    }

    if (strcmp(msg->type, "CHAT") == 0) {
        print_safe("[%s] %s<%s> %s\n", msg->time, seq_prefix, msg->from,
                   msg->body);
    } else if (strcmp(msg->type, "ERROR") == 0) {
        print_safe("[%s] %s[!] %s\n", msg->time, seq_prefix, msg->body);
    } else if (strcmp(msg->type, "PONG") == 0) {
        print_safe("[%s] %s* 服务器响应 PONG (server is alive)\n",
                   msg->time, seq_prefix);
    } else if (strcmp(msg->type, "JOIN") == 0 ||
               strcmp(msg->type, "LEAVE") == 0 ||
               strcmp(msg->type, "INFO") == 0 ||
               strcmp(msg->type, "WELCOME") == 0) {
        print_safe("[%s] %s* %s\n", msg->time, seq_prefix, msg->body);
    } else {
        print_safe("[%s] %s* 未知消息 %s: %s\n", msg->time, seq_prefix,
                   msg->type, msg->body);
    }
}

static void *receiver_thread(void *arg)
{
    line_reader_t *reader = (line_reader_t *)arg;
    chat_message_t msg;
    char line[CHAT_FRAME_MAX + 1];

    while (g_running) {
        int rc = reader_readline(reader, line, sizeof(line));
        if (rc == 0) {
            if (g_running && !g_quitting) {
                print_safe("\n[client] 服务器关闭了连接 (connection closed)\n");
            }
            break;
        }
        if (rc == -4) {
            continue;
        }
        if (rc < 0) {
            print_safe("\n[client] 接收失败 (read error), rc=%d\n", rc);
            break;
        }
        if (chat_parse_frame(line, &msg) < 0) {
            print_safe("[client] 收到无法解析的服务器消息\n");
            continue;
        }

        if (msg.seq != g_expected_seq) {
            print_safe("[%s] [!] 序号不连续：收到 #%" PRIu64
                       "，期望 #%" PRIu64 "，疑似丢失或乱序\n",
                       msg.time, msg.seq, g_expected_seq);
        }
        g_expected_seq = msg.seq + 1;
        display_frame(&msg);
    }

    g_running = 0;
    if (g_sock >= 0) {
        shutdown(g_sock, SHUT_RDWR);
    }
    return NULL;
}

static void print_help(void)
{
    printf(
        "\n聊天命令:\n"
        "  /help          显示本帮助\n"
        "  /list          查看在线用户\n"
        "  /ping          测试服务器是否在线\n"
        "  /quit, /exit   退出聊天室\n"
        "直接输入中文或英文消息并按回车即可发送，单条最多 %d 字节。\n\n",
        CHAT_BODY_MAX);
}

int main(int argc, char **argv)
{
    int argi = 1;
    const char *host;
    const char *port;
    line_reader_t reader;
    chat_message_t welcome;
    char line[CHAT_FRAME_MAX + 1];
    pthread_t receiver;
    int rc;

    if (argc > 1 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "-s") == 0 ||
                     strcmp(argv[1], "--show-seq") == 0)) {
        g_show_seq = 1;
        ++argi;
    }
    if (argc - argi != 3) {
        usage(argv[0]);
        return 2;
    }

    host = argv[argi];
    port = argv[argi + 1];
    if (!chat_valid_nickname(argv[argi + 2])) {
        fprintf(stderr,
                "invalid nickname: 1-16 UTF-8 characters, no spaces or '|'\n");
        return 2;
    }
    snprintf(g_nickname, sizeof(g_nickname), "%s", argv[argi + 2]);

    signal(SIGPIPE, SIG_IGN);

    g_sock = tcp_connect(host, port);
    if (g_sock < 0) {
        return 1;
    }
    set_send_timeout(g_sock, 5);
    set_keepalive(g_sock);
    reader_init(&reader, g_sock);

    set_recv_timeout(g_sock, 10);
    if (send_client_frame("LOGIN", CHAT_PROTO_VERSION) < 0) {
        fprintf(stderr, "send LOGIN failed: %s\n", strerror(errno));
        close(g_sock);
        return 1;
    }

    rc = reader_readline(&reader, line, sizeof(line));
    if (rc != 1) {
        fprintf(stderr, "no WELCOME from server (rc=%d)\n", rc);
        close(g_sock);
        return 1;
    }
    if (chat_parse_frame(line, &welcome) < 0) {
        fprintf(stderr, "malformed WELCOME frame from server\n");
        close(g_sock);
        return 1;
    }
    if (strcmp(welcome.type, "ERROR") == 0) {
        fprintf(stderr, "[%s] login rejected: %s\n", welcome.time,
                welcome.body);
        close(g_sock);
        return 1;
    }
    if (strcmp(welcome.type, "WELCOME") != 0) {
        fprintf(stderr, "unexpected server reply: %s\n", welcome.type);
        close(g_sock);
        return 1;
    }

    g_expected_seq = welcome.seq + 1;
    set_recv_timeout(g_sock, 0);

    printf("=== 多用户聊天室 / Multi-user chat ===\n");
    printf("server: %s:%s   nickname: %s\n", host, port, g_nickname);
    if (g_show_seq) {
        printf("[%s] #%" PRIu64 " * %s\n", welcome.time, welcome.seq,
               welcome.body);
    } else {
        printf("[%s] * %s\n", welcome.time, welcome.body);
    }
    printf("输入 /help 查看命令，输入 /quit 退出。\n\n");
    fflush(stdout);

    rc = pthread_create(&receiver, NULL, receiver_thread, &reader);
    if (rc != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(rc));
        close(g_sock);
        return 1;
    }

    while (g_running) {
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;

        rc = poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (rc == 0) {
            continue;
        }
        if (!(pfd.revents & (POLLIN | POLLHUP))) {
            if (pfd.revents & (POLLERR | POLLNVAL)) {
                break;
            }
            continue;
        }
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
            g_quitting = 1;
            send_client_frame("QUIT", "");
            print_safe("[client] 已退出聊天室 (bye)\n");
            break;
        }
        if (strcmp(line, "/help") == 0) {
            print_help();
            continue;
        }
        if (strcmp(line, "/list") == 0) {
            if (send_client_frame("LIST", "") < 0) {
                print_safe("[client] 发送失败 (send failed)\n");
                break;
            }
            continue;
        }
        if (strcmp(line, "/ping") == 0) {
            if (send_client_frame("PING", "") < 0) {
                print_safe("[client] 发送失败 (send failed)\n");
                break;
            }
            continue;
        }
        if (line[0] == '/') {
            print_safe("[client] 未知命令 %s，输入 /help 查看帮助\n", line);
            continue;
        }
        if (!chat_valid_text(line)) {
            print_safe("[client] 消息为空、含控制字符或超过 %d 字节\n",
                       CHAT_BODY_MAX);
            continue;
        }
        if (send_client_frame("CHAT", line) < 0) {
            print_safe("[client] 发送失败 (send failed)\n");
            break;
        }
    }

    g_running = 0;
    g_quitting = 1;
    if (g_sock >= 0) {
        shutdown(g_sock, SHUT_RDWR);
    }
    pthread_join(receiver, NULL);
    if (g_sock >= 0) {
        close(g_sock);
    }
    return 0;
}
