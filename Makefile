CC ?= cc
CSTD = -std=c11 -D_POSIX_C_SOURCE=200809L
WARN = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
CFLAGS ?= -g -O0
INCLUDES = -Iinclude -Isrc
# openssl (libssl/libcrypto) is the one deliberate exception to "no
# build-time dependency beyond a C toolchain" -- see PLAN.md and the
# issue #4 commit that introduces the LLM client's TLS support.
LDLIBS = -pthread -lssl -lcrypto

BUILD = build
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
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

.PHONY: all lib so cli install test check asan ubsan tsan valgrind fuzz \
	fuzz-run test-integration clean

all: lib cli

lib: $(LIB) $(LIB_SO)

so: $(LIB_SO)

cli: $(CLI_BIN)

install: all
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBDIR)" \
		"$(DESTDIR)$(INCLUDEDIR)/oi"
	install -m 755 $(CLI_BIN) "$(DESTDIR)$(BINDIR)/oi"
	install -m 644 $(LIB) $(LIB_SO) "$(DESTDIR)$(LIBDIR)/"
	install -m 644 include/oi/*.h "$(DESTDIR)$(INCLUDEDIR)/oi/"

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

# Integration tests get their own build subdir so their binaries don't
# collide with the unit tests' $(BUILD)/test_% pattern rule. Helper
# headers (mock_api.h) are picked up relative to the source file, so
# only test_*.c become binaries.
INTEGRATION_BUILD = $(BUILD)/integration
INTEGRATION_SRCS = $(wildcard test/integration/test_*.c)
INTEGRATION_BINS = \
	$(INTEGRATION_SRCS:test/integration/%.c=$(INTEGRATION_BUILD)/%)

$(INTEGRATION_BUILD):
	mkdir -p $(INTEGRATION_BUILD)

$(INTEGRATION_BUILD)/%: test/integration/%.c $(LIB) | $(INTEGRATION_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@ $(LDLIBS)

test-integration: $(INTEGRATION_BINS)
	@if [ -z "$(INTEGRATION_BINS)" ]; then \
		echo "no integration tests yet (test/integration/test_*.c)"; \
		exit 0; \
	fi; \
	set -e; for t in $(INTEGRATION_BINS); do echo "== $$t =="; $$t; done

# The full automated suite. Sanitizer and valgrind targets run this
# rather than `test` alone, so instrumentation covers the integration
# tests too -- they are where the reactor, the socket paths, and the
# tool subprocesses actually run together.
check: test test-integration

# Sanitizer variants each recurse into a separate BUILD dir with
# overridden CFLAGS, reusing every rule above unchanged rather than
# duplicating them.
asan:
	$(MAKE) BUILD=build-asan CFLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" check

ubsan:
	$(MAKE) BUILD=build-ubsan CFLAGS="-g -O1 -fsanitize=undefined -fno-omit-frame-pointer" check

# TSan is here to catch accidental concurrency assumptions (signal
# handlers, the forked tool subprocesses) creeping into what is meant to
# be a single-threaded reactor, not because the harness spawns threads.
#
# On kernels that hand out more ASLR entropy than TSan's shadow mapping
# expects (recent Ubuntu, WSL2), it aborts at startup with "unexpected
# memory mapping". Either run `setarch -R make tsan`, or lower the
# entropy globally with `sudo sysctl -w vm.mmap_rnd_bits=28` (what CI
# does).
tsan:
	$(MAKE) BUILD=build-tsan CFLAGS="-g -O1 -fsanitize=thread -fno-omit-frame-pointer" check

# Valgrind runs against a plain (non-sanitizer) build -- ASan and
# valgrind's instrumentation don't coexist.
#
# --trace-children=no keeps it off the tools' exec'd children (/bin/echo
# and friends), which are not ours to check and would only add noise.
valgrind: $(TEST_BINS) $(INTEGRATION_BINS) $(CLI_BIN)
	@set -e; for t in $(TEST_BINS) $(INTEGRATION_BINS); do \
		echo "== valgrind $$t =="; \
		valgrind -q --error-exitcode=1 --leak-check=full \
			--trace-children=no $$t; \
	done

# libFuzzer is a clang feature -- gcc has no -fsanitize=fuzzer -- so the
# fuzz targets pin their own compiler rather than following CC, which the
# rest of the build (and CI's gcc matrix entry) is free to set elsewhere.
FUZZ_CC ?= clang
FUZZ_BUILD = build-fuzz
FUZZ_SRCS = $(wildcard test/fuzz/fuzz_*.c)
FUZZ_BINS = $(FUZZ_SRCS:test/fuzz/%.c=$(FUZZ_BUILD)/%)
# Iterations per harness for `make fuzz-run`, which exists to catch
# regressions in CI, not to find new bugs. Override it, or run a built
# harness directly, for an open-ended campaign.
FUZZ_RUNS ?= 20000

$(FUZZ_BUILD):
	mkdir -p $(FUZZ_BUILD)

# Harnesses link the library sources directly rather than $(LIB): the
# instrumentation must cover the code under test, and $(LIB) is built
# without it.
$(FUZZ_BUILD)/%: test/fuzz/%.c $(LIB_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(CSTD) $(WARN) -g -O1 \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $< $(LIB_SRCS) -o $@ $(LDLIBS)

fuzz: $(FUZZ_BINS)
	@if [ -z "$(FUZZ_BINS)" ]; then \
		echo "no fuzz harnesses yet (test/fuzz/fuzz_*.c)"; \
	fi

# Seed corpora are read-only inputs; newly discovered ones go to a
# scratch dir under $(FUZZ_BUILD) so a run never dirties the work tree.
fuzz-run: $(FUZZ_BINS)
	@set -e; for b in $(FUZZ_BINS); do \
		name=$$(basename $$b); \
		seeds=test/fuzz/corpus/$$name; \
		found=$(FUZZ_BUILD)/corpus/$$name; \
		mkdir -p $$found; \
		echo "== $$b (-runs=$(FUZZ_RUNS)) =="; \
		$$b -runs=$(FUZZ_RUNS) -artifact_prefix=$(FUZZ_BUILD)/ \
			$$found $$([ -d $$seeds ] && echo $$seeds); \
	done

clean:
	rm -rf $(BUILD) build-asan build-ubsan build-tsan $(FUZZ_BUILD)
