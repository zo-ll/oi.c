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
LIB_SRCS = src/reactor.c src/reactor_epoll.c src/arena.c src/json_value.c src/json_parse.c src/json_write.c src/llm_http.c src/llm_sse.c src/llm_conn.c src/llm.c src/tool_registry.c src/tool_exec.c src/sesslog.c src/session.c src/config.c
LIB_OBJS = $(LIB_SRCS:src/%.c=$(BUILD)/%.o)
LIB = $(BUILD)/liboi.a

# Shared library: built from separately-compiled -fPIC objects in their
# own subdirectory, so `make lib`'s default (non-PIC) objects for the
# static archive don't need -fPIC baked into every build (including the
# sanitizer variants below, which reuse CFLAGS/BUILD overrides and never
# touch the .so).
PIC_BUILD = $(BUILD)/pic
PIC_OBJS = $(LIB_SRCS:src/%.c=$(PIC_BUILD)/%.o)
LIB_SO = $(BUILD)/liboi.so

TEST_SRCS = $(wildcard test/test_*.c)
TEST_BINS = $(TEST_SRCS:test/%.c=$(BUILD)/%)
CLI_BIN = $(BUILD)/oi

.PHONY: all lib so cli test asan ubsan tsan valgrind fuzz test-integration clean

all: lib cli

lib: $(LIB) $(LIB_SO)

so: $(LIB_SO)

cli: $(CLI_BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(PIC_BUILD):
	mkdir -p $(PIC_BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(PIC_BUILD)/%.o: src/%.c | $(PIC_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) -fPIC $(INCLUDES) -c $< -o $@

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(LIB_SO): $(PIC_OBJS)
	$(CC) -shared -o $@ $^ $(LDLIBS)

$(CLI_BIN): src/cli.c $(LIB) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

$(BUILD)/test_%: test/test_%.c $(LIB) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) -DOI_CLI_BIN=\"$(CLI_BIN)\" $< $(LIB) -o $@ $(LDLIBS)

test: $(TEST_BINS) $(CLI_BIN)
	@set -e; for t in $(TEST_BINS); do echo "== $$t =="; $$t; done

# Sanitizer variants each recurse into a separate BUILD dir with
# overridden CFLAGS, reusing every rule above unchanged rather than
# duplicating them -- `make test` under each variant covers lib + CLI +
# the full unit test suite.
asan:
	$(MAKE) BUILD=build-asan CFLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" test

ubsan:
	$(MAKE) BUILD=build-ubsan CFLAGS="-g -O1 -fsanitize=undefined -fno-omit-frame-pointer" test

tsan:
	$(MAKE) BUILD=build-tsan CFLAGS="-g -O1 -fsanitize=thread -fno-omit-frame-pointer" test

# Valgrind runs against a plain (non-sanitizer) build -- ASan and
# valgrind's instrumentation don't coexist.
valgrind: $(TEST_BINS) $(CLI_BIN)
	@set -e; for t in $(TEST_BINS); do \
		echo "== valgrind $$t =="; \
		valgrind -q --error-exitcode=1 --leak-check=full $$t; \
	done

# Wired by wildcard so issues #11 (integration tests) and #12 (fuzzing)
# slot in without further Makefile changes -- a no-op until those add
# sources under test/integration/ or test/fuzz/.
test-integration:
	@srcs="$(wildcard test/integration/*.c)"; \
	if [ -z "$$srcs" ]; then \
		echo "no integration tests yet (test/integration/*.c)"; exit 0; \
	fi; \
	set -e; for f in $$srcs; do \
		bin=$(BUILD)/$$(basename $$f .c); \
		$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) $$f $(LIB) -o $$bin $(LDLIBS); \
		echo "== $$bin =="; $$bin; \
	done

fuzz:
	@srcs="$(wildcard test/fuzz/*.c)"; \
	if [ -z "$$srcs" ]; then \
		echo "no fuzz harnesses yet (test/fuzz/*.c)"; exit 0; \
	fi; \
	mkdir -p build-fuzz; \
	for f in $$srcs; do \
		bin=build-fuzz/$$(basename $$f .c); \
		$(CC) $(CSTD) $(WARN) -g -O1 -fsanitize=fuzzer,address,undefined $(INCLUDES) $$f $(LIB_SRCS) -o $$bin $(LDLIBS); \
	done

clean:
	rm -rf $(BUILD) build-asan build-ubsan build-tsan build-fuzz
