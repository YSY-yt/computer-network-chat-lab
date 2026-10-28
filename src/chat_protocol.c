#include "chat_protocol.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int type_is_valid(const char *type)
{
    size_t len = strlen(type);

    if (len == 0 || len >= CHAT_TYPE_MAX) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (type[i] < 'A' || type[i] > 'Z') {
            return 0;
        }
    }
    return 1;
}

static int has_forbidden_line_char(const char *text)
{
    return strchr(text, '\n') != NULL || strchr(text, '\r') != NULL;
}

void chat_now(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm local;

    if (cap == 0) {
        return;
    }
    if (localtime_r(&now, &local) == NULL ||
        strftime(out, cap, "%Y-%m-%d %H:%M:%S", &local) == 0) {
        snprintf(out, cap, "0000-00-00 00:00:00");
    }
}

int chat_make_frame(char *out, size_t cap, const char *type, uint64_t seq,
                    const char *time_text, const char *from, const char *body)
{
    int written;

    if (out == NULL || cap == 0 || !type_is_valid(type) || time_text == NULL ||
        from == NULL || body == NULL || from[0] == '\0' ||
        strlen(time_text) >= CHAT_TIME_MAX ||
        strlen(from) >= CHAT_FROM_MAX ||
        strlen(body) > CHAT_BODY_MAX ||
        has_forbidden_line_char(time_text) || has_forbidden_line_char(from) ||
        has_forbidden_line_char(body) || strchr(from, '|') != NULL ||
        strchr(time_text, '|') != NULL) {
        return -1;
    }

    written = snprintf(out, cap, "%s|%" PRIu64 "|%s|%s|%s\n",
                       type, seq, time_text, from, body);
    if (written < 0 || (size_t)written >= cap ||
        written > CHAT_FRAME_MAX) {
        return -1;
    }
    return written;
}

static int copy_field(const char **cursor, char delimiter, char *out,
                      size_t cap, size_t *field_len)
{
    const char *end = strchr(*cursor, delimiter);
    size_t len;

    if (end == NULL) {
        return 0;
    }
    len = (size_t)(end - *cursor);
    if (len >= cap) {
        return 0;
    }
    memcpy(out, *cursor, len);
    out[len] = '\0';
    *field_len = len;
    *cursor = end + 1;
    return 1;
}

int chat_parse_frame(const char *line, chat_message_t *msg)
{
    const char *cursor;
    char seq_text[32];
    size_t len;
    size_t body_len;
    char *endp;
    unsigned long long seq_value;

    if (line == NULL || msg == NULL) {
        return -1;
    }
    len = strlen(line);
    if (len > CHAT_FRAME_MAX - 1) {
        return -2;
    }
    if (len == 0 || strchr(line, '\n') != NULL || strchr(line, '\r') != NULL) {
        return -1;
    }

    memset(msg, 0, sizeof(*msg));
    cursor = line;

    if (!copy_field(&cursor, '|', msg->type, sizeof(msg->type), &len) ||
        len == 0 || !type_is_valid(msg->type)) {
        return -1;
    }
    if (!copy_field(&cursor, '|', seq_text, sizeof(seq_text), &len) ||
        len == 0) {
        return -1;
    }

    errno = 0;
    endp = NULL;
    seq_value = strtoull(seq_text, &endp, 10);
    if (errno != 0 || endp == seq_text || *endp != '\0' || seq_value == 0) {
        return -1;
    }
    msg->seq = (uint64_t)seq_value;

    if (!copy_field(&cursor, '|', msg->time, sizeof(msg->time), &len) ||
        len == 0 || !copy_field(&cursor, '|', msg->from, sizeof(msg->from),
                                &len) || len == 0) {
        return -1;
    }

    body_len = strlen(cursor);
    if (body_len > CHAT_BODY_MAX) {
        return -2;
    }
    memcpy(msg->body, cursor, body_len + 1);
    return 0;
}

int chat_valid_utf8(const char *text, size_t len, size_t *codepoints)
{
    size_t i = 0;
    size_t count = 0;

    if (text == NULL) {
        return 0;
    }

    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        uint32_t cp;
        size_t width;

        if (c <= 0x7F) {
            cp = c;
            width = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            unsigned char c1;
            if (i + 1 >= len) {
                return 0;
            }
            c1 = (unsigned char)text[i + 1];
            if ((c1 & 0xC0) != 0x80) {
                return 0;
            }
            cp = ((uint32_t)(c & 0x1F) << 6) | (c1 & 0x3F);
            width = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            unsigned char c1;
            unsigned char c2;
            if (i + 2 >= len) {
                return 0;
            }
            c1 = (unsigned char)text[i + 1];
            c2 = (unsigned char)text[i + 2];
            if ((c2 & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xE0) {
                if (c1 < 0xA0 || c1 > 0xBF) {
                    return 0;
                }
            } else if (c == 0xED) {
                if (c1 < 0x80 || c1 > 0x9F) { /* no UTF-16 surrogates */
                    return 0;
                }
            } else if (c1 < 0x80 || c1 > 0xBF) {
                return 0;
            }
            cp = ((uint32_t)(c & 0x0F) << 12) |
                 ((uint32_t)(c1 & 0x3F) << 6) | (c2 & 0x3F);
            width = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            unsigned char c1;
            unsigned char c2;
            unsigned char c3;
            if (i + 3 >= len) {
                return 0;
            }
            c1 = (unsigned char)text[i + 1];
            c2 = (unsigned char)text[i + 2];
            c3 = (unsigned char)text[i + 3];
            if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xF0) {
                if (c1 < 0x90 || c1 > 0xBF) {
                    return 0;
                }
            } else if (c == 0xF4) {
                if (c1 < 0x80 || c1 > 0x8F) {
                    return 0;
                }
            } else if (c1 < 0x80 || c1 > 0xBF) {
                return 0;
            }
            cp = ((uint32_t)(c & 0x07) << 18) |
                 ((uint32_t)(c1 & 0x3F) << 12) |
                 ((uint32_t)(c2 & 0x3F) << 6) | (c3 & 0x3F);
            width = 4;
        } else {
            return 0;
        }

        /* Reject C0, DEL and C1 control characters. */
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            return 0;
        }
        ++count;
        i += width;
    }

    if (codepoints != NULL) {
        *codepoints = count;
    }
    return 1;
}

int chat_text_is_blank(const char *text)
{
    if (text == NULL) {
        return 1;
    }
    for (; *text != '\0'; ++text) {
        if (!isspace((unsigned char)*text)) {
            return 0;
        }
    }
    return 1;
}

int chat_valid_nickname(const char *nickname)
{
    size_t len;
    size_t chars = 0;

    if (nickname == NULL) {
        return 0;
    }
    len = strlen(nickname);
    if (len == 0 || len > CHAT_NICK_MAX_BYTES) {
        return 0;
    }
    if (!chat_valid_utf8(nickname, len, &chars) ||
        chars == 0 || chars > CHAT_NICK_MAX_CHARS) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)nickname[i];
        if (c == '|' || c <= 0x20 || c == 0x7F) {
            return 0;
        }
    }
    return 1;
}

int chat_valid_text(const char *text)
{
    size_t len;

    if (text == NULL) {
        return 0;
    }
    len = strlen(text);
    if (len == 0 || len > CHAT_BODY_MAX || chat_text_is_blank(text)) {
        return 0;
    }
    return chat_valid_utf8(text, len, NULL);
}
