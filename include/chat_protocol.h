#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Application-layer protocol used by both the server and the client.
 *
 * Wire format (one UTF-8 frame per line):
 *     TYPE|SEQ|TIME|FROM|BODY\n
 *
 * Only the first four '|' characters are delimiters, therefore BODY may
 * contain '|'.  A frame must not contain an embedded CR or LF.
 */
#define CHAT_PROTO_VERSION "CHAT/1"

#define CHAT_FRAME_MAX        4096
#define CHAT_BODY_MAX         3000
#define CHAT_NICK_MAX_BYTES   48
#define CHAT_NICK_MAX_CHARS   16
#define CHAT_TYPE_MAX         16
#define CHAT_TIME_MAX         32
#define CHAT_FROM_MAX         64

typedef struct {
    char     type[CHAT_TYPE_MAX];
    uint64_t seq;
    char     time[CHAT_TIME_MAX];
    char     from[CHAT_FROM_MAX];
    char     body[CHAT_BODY_MAX + 1];
} chat_message_t;

/* Write local time as "YYYY-MM-DD HH:MM:SS". */
void chat_now(char *out, size_t cap);

/*
 * Build a frame.  The returned value is the number of bytes written
 * (including '\n'), or -1 when the arguments cannot form a valid frame.
 */
int chat_make_frame(char *out, size_t cap, const char *type, uint64_t seq,
                    const char *time_text, const char *from, const char *body);

/* Return 0 on success, -1 on a malformed frame or -2 when too long. */
int chat_parse_frame(const char *line, chat_message_t *msg);

/* Strict UTF-8 validation.  Returns 1 when valid, 0 otherwise. */
int chat_valid_utf8(const char *text, size_t len, size_t *codepoints);

int chat_valid_nickname(const char *nickname);
int chat_valid_text(const char *text);
int chat_text_is_blank(const char *text);

#endif /* CHAT_PROTOCOL_H */
