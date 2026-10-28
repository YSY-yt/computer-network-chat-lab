CC       ?= cc
CPPFLAGS ?= -Iinclude -D_POSIX_C_SOURCE=200809L
CFLAGS   ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
LDLIBS   ?= -pthread
LDFLAGS  ?=

BUILD_DIR := build
BIN_DIR   := bin
COMMON    := $(BUILD_DIR)/chat_protocol.o $(BUILD_DIR)/net_util.o

.PHONY: all clean test test-unit test-integration

all: $(BIN_DIR)/chat_server $(BIN_DIR)/chat_client

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c $< -o $@

$(BUILD_DIR)/server.o: src/server.c include/chat_protocol.h include/net_util.h | $(BUILD_DIR)
$(BUILD_DIR)/client.o: src/client.c include/chat_protocol.h include/net_util.h | $(BUILD_DIR)
$(BUILD_DIR)/chat_protocol.o: src/chat_protocol.c include/chat_protocol.h | $(BUILD_DIR)
$(BUILD_DIR)/net_util.o: src/net_util.c include/net_util.h include/chat_protocol.h | $(BUILD_DIR)

$(BIN_DIR)/chat_server: $(BUILD_DIR)/server.o $(COMMON) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BIN_DIR)/chat_client: $(BUILD_DIR)/client.o $(COMMON) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build/protocol_test: tests/protocol_test.c $(BUILD_DIR)/chat_protocol.o | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $^ -o $@ $(LDLIBS)

test-unit: build/protocol_test
	./build/protocol_test

test-integration: all
	python3 tests/integration_test.py

test: test-unit test-integration

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
