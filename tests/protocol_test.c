#include "chat_protocol.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void test_frame_roundtrip(void)
{
    char frame[CHAT_FRAME_MAX + 1];
    chat_message_t msg;
    int len = chat_make_frame(frame, sizeof(frame), "CHAT", 42,
                              "2026-10-28 20:30:00", "小明", "你好|hello");
    CHECK(len > 0);
    CHECK(frame[len - 1] == '\n');
    frame[len - 1] = '\0';
    CHECK(chat_parse_frame(frame, &msg) == 0);
    CHECK(strcmp(msg.type, "CHAT") == 0);
    CHECK(msg.seq == 42);
    CHECK(strcmp(msg.time, "2026-10-28 20:30:00") == 0);
    CHECK(strcmp(msg.from, "小明") == 0);
    CHECK(strcmp(msg.body, "你好|hello") == 0);
}

static void test_frame_rejections(void)
{
    char frame[CHAT_FRAME_MAX + 1];
    chat_message_t msg;
    static char huge_body[CHAT_BODY_MAX + 2];

    CHECK(chat_make_frame(frame, sizeof(frame), "chat", 1,
                          "2026-10-28 20:30:00", "alice", "x") < 0);
    CHECK(chat_make_frame(frame, sizeof(frame), "CHAT", 1,
                          "2026-10-28 20:30:00", "a|b", "x") < 0);
    CHECK(chat_make_frame(frame, sizeof(frame), "CHAT", 1,
                          "2026-10-28 20:30:00", "alice", "a\nb") < 0);

    CHECK(chat_parse_frame("CHAT|1|time|alice", &msg) < 0);
    CHECK(chat_parse_frame("CHAT|0|time|alice|body", &msg) < 0);
    CHECK(chat_parse_frame("CHAT|x|time|alice|body", &msg) < 0);

    memset(huge_body, 'a', sizeof(huge_body) - 1);
    huge_body[sizeof(huge_body) - 1] = '\0';
    CHECK(chat_make_frame(frame, sizeof(frame), "CHAT", 1,
                          "2026-10-28 20:30:00", "alice",
                          huge_body) < 0);
}

static void test_utf8_and_validation(void)
{
    size_t chars = 0;
    char too_many[64];
    const char invalid_continuation[] = {(char)0xE4, (char)0xB8, 0x41, 0};
    const char overlong[] = {(char)0xC0, (char)0xAF, 0};
    const char surrogate[] = {(char)0xED, (char)0xA0, (char)0x80, 0};

    CHECK(chat_valid_utf8("hello 世界", strlen("hello 世界"), &chars));
    CHECK(chars == 8);
    CHECK(chat_valid_utf8("聊天 | 😀", strlen("聊天 | 😀"), &chars));
    CHECK(!chat_valid_utf8(invalid_continuation,
                           sizeof(invalid_continuation) - 1, NULL));
    CHECK(!chat_valid_utf8(overlong, sizeof(overlong) - 1, NULL));
    CHECK(!chat_valid_utf8(surrogate, sizeof(surrogate) - 1, NULL));

    CHECK(chat_valid_nickname("Alice"));
    CHECK(chat_valid_nickname("小明"));
    CHECK(!chat_valid_nickname("Alice Bob"));
    CHECK(!chat_valid_nickname("a|b"));
    CHECK(!chat_valid_nickname(""));

    memset(too_many, 'x', 17);
    too_many[17] = '\0';
    CHECK(!chat_valid_nickname(too_many));

    CHECK(chat_valid_text("Hello, 世界!"));
    CHECK(chat_valid_text("a|b"));
    CHECK(!chat_valid_text("   "));
    CHECK(!chat_valid_text("line\nbreak"));
}

int main(void)
{
    test_frame_roundtrip();
    test_frame_rejections();
    test_utf8_and_validation();

    if (failures != 0) {
        fprintf(stderr, "%d protocol test(s) failed\n", failures);
        return 1;
    }
    printf("protocol tests: all passed\n");
    return 0;
}
