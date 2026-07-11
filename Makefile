CC ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Werror -O2 -g
CPPFLAGS ?= -Iinclude -Isrc
LDFLAGS ?=

BUILD_DIR := build
TARGET := $(BUILD_DIR)/rozetka2

SRCS := \
	src/a64_jit.c \
	src/decode.c \
	src/interp.c \
	src/launch.c \
	src/main.c \
	src/macho.c \
	src/util.c

OBJS := $(SRCS:src/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

test: all
	./tests/run_tests.sh $(TARGET)

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
