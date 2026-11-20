CC ?= cc
CSTD = -std=c11 -D_POSIX_C_SOURCE=200809L
WARN = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
CFLAGS ?= -g -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fstack-protector-strong
PIE_CFLAGS = -fPIE
RELRO_LDFLAGS = -Wl,-z,relro,-z,now
EXE_LDFLAGS = -pie $(RELRO_LDFLAGS) $(LDFLAGS)
DEPFLAGS = -MMD -MP
OPENSSL_CFLAGS ?= $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS ?= $(shell pkg-config --libs openssl 2>/dev/null || echo -lssl -lcrypto)
INCLUDES = -Iinclude -Isrc $(OPENSSL_CFLAGS)
LDLIBS = -pthread $(OPENSSL_LIBS)

BUILD = build
VERSION = 0.1.0
ABI_MAJOR = 0
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
REACTOR_BACKEND = src/reactor_epoll.c
PTY_LIBS = -lutil
LIB_SRCS = src/reactor.c $(REACTOR_BACKEND) src/arena.c src/json_value.c src/json_parse.c src/json_write.c src/llm_http.c src/llm_sse.c src/llm_conn.c src/llm_responses.c src/llm.c src/tool_registry.c src/tool_exec.c src/sesslog.c src/session.c src/config.c
LIB_OBJS = $(LIB_SRCS:src/%.c=$(BUILD)/%.o)
LIB = $(BUILD)/liboi.a

PIC_BUILD = $(BUILD)/pic
PIC_OBJS = $(LIB_SRCS:src/%.c=$(PIC_BUILD)/%.o)
LIB_SO = $(BUILD)/liboi.so
LIB_SO_SONAME = $(BUILD)/liboi.so.$(ABI_MAJOR)
LIB_SO_REAL = $(BUILD)/liboi.so.$(VERSION)
SHARED_DEPS = src/liboi.map
SHARED_FLAGS = -shared -Wl,-soname,liboi.so.$(ABI_MAJOR) \
	-Wl,--version-script=src/liboi.map

TEST_SRCS = $(wildcard test/test_*.c)
TEST_BINS = $(TEST_SRCS:test/%.c=$(BUILD)/%)
CLI_BIN = $(BUILD)/oi
CLI_SRCS = src/cli.c src/cli_loop.c src/cli_tools.c src/cli_tool_run.c src/cli_message.c src/cli_history.c src/cli_history_codec.c src/cli_history_replay.c src/cli_history_repair.c src/cli_history_store.c src/cli_conversation.c src/cli_utf8.c src/cli_editor.c src/cli_input_history.c src/cli_terminal.c src/cli_input.c src/cli_prompt_state.c src/cli_render.c src/cli_selector.c src/cli_composer.c src/cli_present.c src/cli_repl.c src/cli_sessions.c src/cli_commands.c src/cli_command_dispatch.c src/cli_bytebuf.c src/cli_utf8_stream.c src/cli_render_sanitize.c src/cli_markdown.c src/cli_render_style.c src/cli_markdown_inline.c src/cli_markdown_block.c src/cli_render_stream.c src/cli_session_metadata.c src/cli_session_metadata_codec.c src/cli_session_metadata_store.c src/cli_tool_panel.c src/cli_compact.c src/cli_session_switch.c src/cli_status.c src/cli_credentials.c src/cli_oauth.c src/cli_login.c
CLI_OBJ_BUILD = $(BUILD)/cli
CLI_OBJS = $(CLI_SRCS:src/%.c=$(CLI_OBJ_BUILD)/%.o)

CLI_TEST_SRCS = $(filter-out src/cli.c src/cli_loop.c src/cli_repl.c,$(CLI_SRCS))
CLI_TEST_OBJS = $(CLI_TEST_SRCS:src/%.c=$(CLI_OBJ_BUILD)/%.o)
CLI_TEST_LIB = $(BUILD)/liboi_cli_test.a

.PHONY: all lib so cli install uninstall test quick check verify \
	verify-compilers \
	compiler-stamp tier-audit comment-lint comment-ratchet timings abi-check asan ubsan tsan valgrind \
	fuzz fuzz-run test-integration clean

all: lib cli

lib: $(LIB) $(LIB_SO)

so: $(LIB_SO)

cli: $(CLI_BIN)

install: all
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBDIR)" \
		"$(DESTDIR)$(INCLUDEDIR)/oi"
	install -m 755 $(CLI_BIN) "$(DESTDIR)$(BINDIR)/oi"
	install -m 644 $(LIB) $(LIB_SO_REAL) "$(DESTDIR)$(LIBDIR)/"
	ln -sfn "$(notdir $(LIB_SO_REAL))" \
		"$(DESTDIR)$(LIBDIR)/$(notdir $(LIB_SO_SONAME))"
	ln -sfn "$(notdir $(LIB_SO_SONAME))" \
		"$(DESTDIR)$(LIBDIR)/$(notdir $(LIB_SO))"
	install -m 644 include/oi/*.h "$(DESTDIR)$(INCLUDEDIR)/oi/"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/oi"
	rm -f "$(DESTDIR)$(LIBDIR)/$(notdir $(LIB))" \
		"$(DESTDIR)$(LIBDIR)/$(notdir $(LIB_SO_REAL))" \
		"$(DESTDIR)$(LIBDIR)/$(notdir $(LIB_SO_SONAME))" \
		"$(DESTDIR)$(LIBDIR)/$(notdir $(LIB_SO))"
	rm -f $(patsubst include/oi/%,"$(DESTDIR)$(INCLUDEDIR)/oi/%",$(wildcard include/oi/*.h))
	-rmdir "$(DESTDIR)$(INCLUDEDIR)/oi"

$(BUILD):
	mkdir -p $(BUILD)

$(PIC_BUILD):
	mkdir -p $(PIC_BUILD)

$(CLI_OBJ_BUILD):
	mkdir -p $(CLI_OBJ_BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) -c $< -o $@

$(CLI_OBJ_BUILD)/%.o: src/%.c | $(CLI_OBJ_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) -c $< -o $@

$(PIC_BUILD)/%.o: src/%.c | $(PIC_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) -fPIC $(INCLUDES) $(DEPFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(LIB_SO_REAL): $(PIC_OBJS) $(SHARED_DEPS)
	$(CC) $(SHARED_FLAGS) $(RELRO_LDFLAGS) $(LDFLAGS) -o $@ $(PIC_OBJS) $(LDLIBS)

$(LIB_SO_SONAME): $(LIB_SO_REAL)
	ln -sfn "$(notdir $(LIB_SO_REAL))" $@

$(LIB_SO): $(LIB_SO_SONAME)
	ln -sfn "$(notdir $(LIB_SO_SONAME))" $@

$(CLI_TEST_LIB): $(CLI_TEST_OBJS)
	rm -f $@
	ar rcs $@ $^

$(CLI_BIN): $(CLI_OBJS) $(LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(EXE_LDFLAGS) $(CLI_OBJS) $(LIB) -o $@ $(LDLIBS)

$(BUILD)/test_%: test/test_%.c $(LIB) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) -DOI_CLI_BIN=\"$(CLI_BIN)\" $< $(LIB) $(EXE_LDFLAGS) -o $@ $(LDLIBS)

$(BUILD)/test_tool_exec: LDLIBS += -Wl,--wrap=kill
$(BUILD)/test_reactor: LDLIBS += -Wl,--wrap=epoll_ctl
$(BUILD)/test_llm_conn: LDLIBS += -Wl,--wrap=getaddrinfo -Wl,--wrap=send \
	-Wl,--wrap=clock_gettime \
	-Wl,--wrap=SSL_CTX_new

$(BUILD)/test_cli: test/test_cli.c $(LIB) $(CLI_BIN) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) -DOI_CLI_BIN=\"$(CLI_BIN)\" $< $(LIB) $(EXE_LDFLAGS) -o $@ $(LDLIBS) $(PTY_LIBS)

$(BUILD)/test_cli_%: test/test_cli_%.c $(CLI_TEST_LIB) $(LIB) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) $< $(CLI_TEST_LIB) $(LIB) $(EXE_LDFLAGS) -o $@ $(LDLIBS)

$(BUILD)/test_cli_terminal $(BUILD)/test_cli_composer $(BUILD)/test_cli_login: \
	LDLIBS += $(PTY_LIBS)

PURE_TESTS = \
	test_arena test_cli_bytebuf test_cli_command_dispatch test_cli_commands \
	test_cli_compact test_cli_docs test_cli_editor test_cli_history \
	test_cli_history_codec \
	test_cli_history_repair test_cli_history_replay test_cli_history_store \
	test_cli_input test_cli_input_history test_cli_markdown \
	test_cli_markdown_block test_cli_markdown_inline test_cli_message \
	test_cli_present test_cli_prompt_state test_cli_render \
	test_cli_render_sanitize test_cli_render_stream test_cli_render_style \
	test_cli_selector test_cli_session_metadata \
	test_cli_session_metadata_codec test_cli_session_metadata_store \
	test_cli_credentials test_cli_oauth test_cli_status test_cli_tool_panel test_cli_tool_run test_cli_tools test_cli_utf8 \
	test_cli_utf8_stream \
	test_config test_json test_llm_http test_llm_responses test_llm_sse \
	test_reactor test_session test_sesslog test_tool_registry

IMPURE_TESTS = \
	test_cli test_cli_composer test_cli_login test_cli_sessions \
	test_cli_terminal \
	test_llm test_llm_conn test_tool_exec

PURE_TEST_BINS = $(PURE_TESTS:%=$(BUILD)/%)
IMPURE_TEST_BINS = $(IMPURE_TESTS:%=$(BUILD)/%)
PURE_TEST_RUNS = $(PURE_TESTS:%=run-%)

.PHONY: $(PURE_TEST_RUNS)

$(PURE_TEST_RUNS): run-%: $(BUILD)/%
	@echo "== $(BUILD)/$* =="
	@$(BUILD)/$*

test: $(TEST_BINS) $(CLI_BIN)
	+@$(MAKE) --no-print-directory --output-sync=target $(PURE_TEST_RUNS)
	@set -e; for t in $(IMPURE_TEST_BINS); do echo "== $$t =="; $$t; done

quick: $(PURE_TEST_BINS) $(CLI_BIN)
	+@$(MAKE) --no-print-directory --output-sync=target $(PURE_TEST_RUNS)

COMMENT_BASELINE = tools/comment-baseline.txt
COMMENT_LINT_FILES = $(sort $(wildcard src/*.c src/*.h include/oi/*.h \
	test/*.c test/*.h test/integration/*.c test/integration/*.h \
	test/fuzz/*.c tools/*.c docs/issue-33/*.c)) Makefile \
	$(wildcard .github/workflows/*.yml)

$(BUILD)/comment_lint: tools/comment_lint.c | $(BUILD)
	$(CC) $(CSTD) $(WARN) -O2 $< -o $@

comment-lint: $(BUILD)/comment_lint
	@$(BUILD)/comment_lint check $(COMMENT_BASELINE) $(COMMENT_LINT_FILES)

comment-ratchet: $(BUILD)/comment_lint
	@$(BUILD)/comment_lint ratchet $(COMMENT_BASELINE) $(COMMENT_LINT_FILES)

tier-audit:
	@all=`echo $(TEST_SRCS) | tr ' ' '\n' | sed 's|test/||;s|\.c$$||'`; \
	pure=`echo $(PURE_TESTS) | tr ' ' '\n'`; \
	impure=`echo $(IMPURE_TESTS) | tr ' ' '\n'`; \
	tiers=`printf '%s\n' $$pure $$impure`; \
	missing=`printf '%s\n' $$all $$tiers $$tiers | sort | uniq -u`; \
	extra=`printf '%s\n' $$tiers $$all $$all | sort | uniq -u`; \
	pure_dupes=`printf '%s\n' $$pure | sort | uniq -d`; \
	impure_dupes=`printf '%s\n' $$impure | sort | uniq -d`; \
	overlap=`printf '%s\n' \`printf '%s\n' $$pure | sort -u\` \
		\`printf '%s\n' $$impure | sort -u\` | sort | uniq -d`; \
	status=0; \
	if [ -n "$$missing" ]; then \
		echo "tier-audit: test(s) belong to no tier:"; \
		echo "$$missing" | sed 's/^/  /'; status=1; \
	fi; \
	if [ -n "$$extra" ]; then \
		echo "tier-audit: tier lists name test(s) that do not exist:"; \
		echo "$$extra" | sed 's/^/  /'; status=1; \
	fi; \
	if [ -n "$$pure_dupes" ]; then \
		echo "tier-audit: PURE_TESTS lists the same test twice:"; \
		echo "$$pure_dupes" | sed 's/^/  /'; status=1; \
	fi; \
	if [ -n "$$impure_dupes" ]; then \
		echo "tier-audit: IMPURE_TESTS lists the same test twice:"; \
		echo "$$impure_dupes" | sed 's/^/  /'; status=1; \
	fi; \
	if [ -n "$$overlap" ]; then \
		echo "tier-audit: test(s) in both PURE_TESTS and IMPURE_TESTS:"; \
		echo "$$overlap" | sed 's/^/  /'; status=1; \
	fi; \
	if [ $$status -eq 0 ]; then \
		echo "tier-audit: `printf '%s\n' $$all | wc -l | tr -d ' '` test binaries, each in exactly one tier"; \
	fi; \
	exit $$status

INTEGRATION_BUILD = $(BUILD)/integration
INTEGRATION_SRCS = $(wildcard test/integration/test_*.c)
INTEGRATION_BINS = \
	$(INTEGRATION_SRCS:test/integration/%.c=$(INTEGRATION_BUILD)/%)

$(INTEGRATION_BUILD):
	mkdir -p $(INTEGRATION_BUILD)

$(INTEGRATION_BUILD)/%: test/integration/%.c $(LIB) | $(INTEGRATION_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) $< $(LIB) $(EXE_LDFLAGS) -o $@ $(LDLIBS)

$(INTEGRATION_BUILD)/test_cli_conversation: test/integration/test_cli_conversation.c $(CLI_TEST_LIB) $(LIB) $(CLI_BIN) | $(INTEGRATION_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) -DOI_CLI_BIN=\"$(CLI_BIN)\" $< $(CLI_TEST_LIB) $(LIB) $(EXE_LDFLAGS) -o $@ $(LDLIBS)

$(INTEGRATION_BUILD)/test_cli_compact: test/integration/test_cli_compact.c $(CLI_TEST_LIB) $(LIB) | $(INTEGRATION_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) $< $(CLI_TEST_LIB) $(LIB) $(EXE_LDFLAGS) -o $@ $(LDLIBS)

$(INTEGRATION_BUILD)/test_cli_session_switch: test/integration/test_cli_session_switch.c $(CLI_TEST_LIB) $(LIB) | $(INTEGRATION_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(PIE_CFLAGS) $(INCLUDES) $(DEPFLAGS) $< $(CLI_TEST_LIB) $(LIB) $(EXE_LDFLAGS) -o $@ $(LDLIBS)

test-integration: $(INTEGRATION_BINS)
	@if [ -z "$(INTEGRATION_BINS)" ]; then \
		echo "no integration tests yet (test/integration/test_*.c)"; \
		exit 0; \
	fi; \
	set -e; for t in $(INTEGRATION_BINS); do echo "== $$t =="; $$t; done

check: tier-audit comment-lint
	+@$(MAKE) --no-print-directory test
	+@$(MAKE) --no-print-directory test-integration

VERIFY_FUZZ_RUNS ?= 200000

compiler-stamp: | $(BUILD)
	@$(CC) --version | head -1 > $(BUILD)/compiler.txt

verify-compilers:
	@gcc_id=`cat build-gcc/compiler.txt 2>/dev/null`; \
	clang_id=`cat build-clang/compiler.txt 2>/dev/null`; \
	if [ -z "$$gcc_id" ] || [ -z "$$clang_id" ]; then \
		echo "verify: a compiler pass left no stamp"; exit 1; \
	fi; \
	if [ "$$gcc_id" = "$$clang_id" ]; then \
		echo "verify: both passes used the same compiler:"; \
		echo "  $$gcc_id"; exit 1; \
	fi; \
	case "$$gcc_id" in *gcc*|*GCC*) ;; \
		*) echo "verify: build-gcc was not built by gcc: $$gcc_id"; exit 1;; \
	esac; \
	case "$$clang_id" in *clang*|*Clang*) ;; \
		*) echo "verify: build-clang was not built by clang: $$clang_id"; \
		   exit 1;; \
	esac; \
	echo "verify: gcc   -> $$gcc_id"; \
	echo "verify: clang -> $$clang_id"

verify:
	@echo "== verify: gcc =="
	rm -rf build-gcc
	$(MAKE) CC=gcc BUILD=build-gcc check
	$(MAKE) CC=gcc BUILD=build-gcc compiler-stamp
	@echo "== verify: clang =="
	rm -rf build-clang
	$(MAKE) CC=clang BUILD=build-clang check
	$(MAKE) CC=clang BUILD=build-clang compiler-stamp
	@echo "== verify: compilers =="
	@$(MAKE) verify-compilers
	@echo "== verify: abi =="
	$(MAKE) abi-check
	@echo "== verify: asan =="
	$(MAKE) asan
	@echo "== verify: ubsan =="
	$(MAKE) ubsan
	@echo "== verify: tsan =="
	$(MAKE) tsan
	@echo "== verify: valgrind =="
	$(MAKE) valgrind
	@echo "== verify: fuzz =="
	$(MAKE) fuzz-run FUZZ_RUNS=$(VERIFY_FUZZ_RUNS)
	@echo "== verify: all gates passed =="

timings:
	@failures=`mktemp`; \
	echo "| Phase | Seconds |"; \
	echo "| --- | --- |"; \
	$(MAKE) clean >/dev/null 2>&1; \
	start=`date +%s.%N`; \
	if $(MAKE) $(TEST_BINS) $(INTEGRATION_BINS) $(CLI_BIN) \
			>$$failures.build 2>&1; then \
		end=`date +%s.%N`; \
		echo "| compile (serial, clean) | `echo "$$end - $$start" | bc` |"; \
	else \
		echo; \
		echo "timings: compilation failed; no timings are meaningful" >&2; \
		tail -20 $$failures.build >&2; \
		rm -f $$failures $$failures.build; \
		exit 1; \
	fi; \
	rm -f $$failures.build; \
	start=`date +%s.%N`; \
	for t in $(PURE_TEST_BINS); do \
		$$t >/dev/null 2>&1 || echo "$$t" >> $$failures; \
	done; \
	end=`date +%s.%N`; \
	echo "| quick tier (run only) | `echo "$$end - $$start" | bc` |"; \
	start=`date +%s.%N`; \
	for t in $(TEST_BINS) $(INTEGRATION_BINS); do \
		$$t >/dev/null 2>&1 || echo "$$t" >> $$failures; \
	done; \
	end=`date +%s.%N`; \
	echo "| check tier (run only) | `echo "$$end - $$start" | bc` |"; \
	echo; \
	echo "| Binary | Seconds |"; \
	echo "| --- | --- |"; \
	for t in $(TEST_BINS) $(INTEGRATION_BINS); do \
		start=`date +%s.%N`; \
		$$t >/dev/null 2>&1 || echo "$$t" >> $$failures; \
		end=`date +%s.%N`; \
		printf '| %s | %s |\n' "$$t" "`echo "$$end - $$start" | bc`"; \
	done | sort -t'|' -k3 -rn; \
	if [ -s $$failures ]; then \
		echo; \
		echo "timings: these binaries failed, so the report is not a pass:" >&2; \
		sort -u $$failures | sed 's/^/  /' >&2; \
		rm -f $$failures; \
		exit 1; \
	fi; \
	rm -f $$failures

abi-check: $(LIB_SO_REAL)
	@exports=`mktemp`; \
	nm -D --defined-only --format=posix $(LIB_SO_REAL) | \
		awk '$$1 ~ /^oi_/ { sub(/@.*/, "", $$1); print $$1 }' | \
		LC_ALL=C sort -u > $$exports; \
	status=0; \
	LC_ALL=C sort -u test/abi_exports.txt | diff -u - $$exports || status=$$?; \
	rm -f $$exports; \
	exit $$status

asan:
	$(MAKE) BUILD=build-asan CFLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" check

ubsan:
	$(MAKE) BUILD=build-ubsan CFLAGS="-g -O1 -fsanitize=undefined -fno-omit-frame-pointer" check

tsan:
	$(MAKE) BUILD=build-tsan CFLAGS="-g -O1 -fsanitize=thread -fno-omit-frame-pointer" check

valgrind: $(TEST_BINS) $(INTEGRATION_BINS) $(CLI_BIN)
	@set -e; for t in $(TEST_BINS) $(INTEGRATION_BINS); do \
		echo "== valgrind $$t =="; \
		valgrind -q --error-exitcode=1 --leak-check=full \
			--trace-children=no $$t; \
	done

FUZZ_CC ?= clang
FUZZ_BUILD = build-fuzz
FUZZ_SRCS = $(wildcard test/fuzz/fuzz_*.c)
FUZZ_BINS = $(FUZZ_SRCS:test/fuzz/%.c=$(FUZZ_BUILD)/%)
FUZZ_RUNS ?= 20000

$(FUZZ_BUILD):
	mkdir -p $(FUZZ_BUILD)

$(FUZZ_BUILD)/%: test/fuzz/%.c $(LIB_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(CSTD) $(WARN) -g -O1 \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $(DEPFLAGS) $< $(LIB_SRCS) -o $@ $(LDLIBS)

CLI_RENDER_STREAM_SRCS = src/cli_render_stream.c src/cli_render_style.c \
	src/cli_markdown_block.c src/cli_markdown_inline.c src/cli_markdown.c \
	src/cli_render_sanitize.c src/cli_utf8_stream.c src/cli_utf8.c \
	src/cli_bytebuf.c

$(FUZZ_BUILD)/fuzz_cli_render_stream: test/fuzz/fuzz_cli_render_stream.c $(CLI_RENDER_STREAM_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(CSTD) $(WARN) -g -O1 \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $(DEPFLAGS) $< $(CLI_RENDER_STREAM_SRCS) -o $@ $(LDLIBS)

CLI_HISTORY_REPLAY_FUZZ_SRCS = src/cli_history.c src/cli_history_codec.c \
	src/cli_history_replay.c src/cli_message.c src/arena.c \
	src/json_value.c src/json_parse.c src/json_write.c

$(FUZZ_BUILD)/fuzz_cli_history_replay: test/fuzz/fuzz_cli_history_replay.c $(CLI_HISTORY_REPLAY_FUZZ_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(CSTD) $(WARN) -g -O1 \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $(DEPFLAGS) $< $(CLI_HISTORY_REPLAY_FUZZ_SRCS) -o $@ $(LDLIBS)

fuzz: $(FUZZ_BINS)
	@if [ -z "$(FUZZ_BINS)" ]; then \
		echo "no fuzz harnesses yet (test/fuzz/fuzz_*.c)"; \
	fi

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

-include $(wildcard $(BUILD)/*.d $(CLI_OBJ_BUILD)/*.d $(PIC_BUILD)/*.d \
	$(INTEGRATION_BUILD)/*.d $(FUZZ_BUILD)/*.d)

clean:
	rm -rf $(BUILD) build-gcc build-clang build-asan build-ubsan \
		build-tsan $(FUZZ_BUILD)
