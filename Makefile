CC ?= cc
CSTD = -std=c11 -D_POSIX_C_SOURCE=200809L
WARN = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
CFLAGS ?= -g -O0
INCLUDES = -Iinclude -Isrc
# openssl (libssl/libcrypto) is the one deliberate exception to "no
# build-time dependency beyond a C toolchain" -- see PLAN.md and the
# issue #4 commit that introduces the LLM client's TLS support.
LDLIBS = -lssl -lcrypto

BUILD = build
LIB_SRCS = src/reactor.c src/reactor_epoll.c src/arena.c src/json_value.c src/json_parse.c src/json_write.c src/llm_http.c src/llm_sse.c src/llm_conn.c src/llm.c
LIB_OBJS = $(LIB_SRCS:src/%.c=$(BUILD)/%.o)
LIB = $(BUILD)/liboi.a

TEST_SRCS = $(wildcard test/test_*.c)
TEST_BINS = $(TEST_SRCS:test/%.c=$(BUILD)/%)

.PHONY: all lib test clean

all: lib

lib: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(BUILD)/test_%: test/test_%.c $(LIB) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t =="; $$t; done

clean:
	rm -rf $(BUILD)
