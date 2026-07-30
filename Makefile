CC ?= cc
CSTD = -std=c11 -D_POSIX_C_SOURCE=200809L
WARN = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
CFLAGS ?= -g -O0
OPENSSL_CFLAGS ?= $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS ?= $(shell pkg-config --libs openssl 2>/dev/null || echo -lssl -lcrypto)
INCLUDES = -Iinclude -Isrc $(OPENSSL_CFLAGS)
# openssl (libssl/libcrypto) is the one deliberate exception to "no
# build-time dependency beyond a C toolchain" -- see PLAN.md and the
# issue #4 commit that introduces the LLM client's TLS support.
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
LIB_SRCS = src/reactor.c $(REACTOR_BACKEND) src/arena.c src/json_value.c src/json_parse.c src/json_write.c src/llm_http.c src/llm_sse.c src/llm_conn.c src/llm.c src/tool_registry.c src/tool_exec.c src/sesslog.c src/session.c src/config.c
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
LIB_SO_SONAME = $(BUILD)/liboi.so.$(ABI_MAJOR)
LIB_SO_REAL = $(BUILD)/liboi.so.$(VERSION)
SHARED_DEPS = src/liboi.map
SHARED_FLAGS = -shared -Wl,-soname,liboi.so.$(ABI_MAJOR) \
	-Wl,--version-script=src/liboi.map

TEST_SRCS = $(wildcard test/test_*.c)
TEST_BINS = $(TEST_SRCS:test/%.c=$(BUILD)/%)
CLI_BIN = $(BUILD)/oi
CLI_SRCS = src/cli.c src/cli_loop.c src/cli_tools.c src/cli_message.c src/cli_history.c src/cli_history_codec.c src/cli_history_replay.c src/cli_history_repair.c src/cli_history_store.c src/cli_conversation.c src/cli_utf8.c src/cli_editor.c src/cli_input_history.c src/cli_terminal.c src/cli_input.c src/cli_prompt_state.c src/cli_render.c src/cli_selector.c src/cli_composer.c src/cli_present.c src/cli_repl.c src/cli_sessions.c src/cli_commands.c src/cli_command_dispatch.c src/cli_bytebuf.c src/cli_utf8_stream.c src/cli_render_sanitize.c src/cli_markdown.c src/cli_render_style.c src/cli_markdown_inline.c src/cli_markdown_block.c src/cli_render_stream.c src/cli_session_metadata.c src/cli_session_metadata_codec.c src/cli_session_metadata_store.c src/cli_tool_panel.c src/cli_compact.c src/cli_session_switch.c
CLI_OBJ_BUILD = $(BUILD)/cli
CLI_OBJS = $(CLI_SRCS:src/%.c=$(CLI_OBJ_BUILD)/%.o)

# Private CLI implementation archive for unit/integration tests. Production
# still links its complete object list directly, so archive member selection
# cannot change the real executable's linkage. cli.c owns main(), while
# cli_loop.c and cli_repl.c are exercised through that real executable rather
# than linked into private-module tests; excluding all three also means a
# production-only edit does not relink every CLI test.
CLI_TEST_SRCS = $(filter-out src/cli.c src/cli_loop.c src/cli_repl.c,$(CLI_SRCS))
CLI_TEST_OBJS = $(CLI_TEST_SRCS:src/%.c=$(CLI_OBJ_BUILD)/%.o)
CLI_TEST_LIB = $(BUILD)/liboi_cli_test.a

.PHONY: all lib so cli install test quick check verify verify-compilers \
	compiler-stamp tier-audit timings abi-check asan ubsan tsan valgrind \
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

$(BUILD):
	mkdir -p $(BUILD)

$(PIC_BUILD):
	mkdir -p $(PIC_BUILD)

$(CLI_OBJ_BUILD):
	mkdir -p $(CLI_OBJ_BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(CLI_OBJ_BUILD)/%.o: src/%.c | $(CLI_OBJ_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(PIC_BUILD)/%.o: src/%.c | $(PIC_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) -fPIC $(INCLUDES) -c $< -o $@

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

$(LIB_SO_REAL): $(PIC_OBJS) $(SHARED_DEPS)
	$(CC) $(SHARED_FLAGS) -o $@ $(PIC_OBJS) $(LDLIBS)

$(LIB_SO_SONAME): $(LIB_SO_REAL)
	ln -sfn "$(notdir $(LIB_SO_REAL))" $@

$(LIB_SO): $(LIB_SO_SONAME)
	ln -sfn "$(notdir $(LIB_SO_SONAME))" $@

$(CLI_TEST_LIB): $(CLI_TEST_OBJS)
	rm -f $@
	ar rcs $@ $^

$(CLI_BIN): $(CLI_OBJS) $(LIB) | $(BUILD)
	$(CC) $(CLI_OBJS) $(LIB) -o $@ $(LDLIBS)

$(BUILD)/test_%: test/test_%.c $(LIB) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) -DOI_CLI_BIN=\"$(CLI_BIN)\" $< $(LIB) -o $@ $(LDLIBS)

$(BUILD)/test_cli: test/test_cli.c $(LIB) $(CLI_BIN) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) -DOI_CLI_BIN=\"$(CLI_BIN)\" $< $(LIB) -o $@ $(LDLIBS) $(PTY_LIBS)

$(BUILD)/test_cli_%: test/test_cli_%.c $(CLI_TEST_LIB) $(LIB) | $(BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) $< $(CLI_TEST_LIB) $(LIB) -o $@ $(LDLIBS)

$(BUILD)/test_cli_terminal $(BUILD)/test_cli_composer: LDLIBS += $(PTY_LIBS)

# Tests that touch no PTY, socket, fork, signal, or other global process
# state, and so are safe to run as the routine edit-test loop (`make quick`).
#
# Membership is spelled out rather than derived, deliberately: a test that
# grows a socket must be moved out of this list by someone who noticed, not
# silently reclassified by a pattern match. The `tier-audit` target below
# fails the build if this list plus IMPURE_TESTS stops covering every binary.
PURE_TESTS = \
	test_arena test_cli_bytebuf test_cli_command_dispatch test_cli_commands \
	test_cli_compact test_cli_editor test_cli_history test_cli_history_codec \
	test_cli_history_repair test_cli_history_replay test_cli_history_store \
	test_cli_input test_cli_input_history test_cli_markdown \
	test_cli_markdown_block test_cli_markdown_inline test_cli_message \
	test_cli_present test_cli_prompt_state test_cli_render \
	test_cli_render_sanitize test_cli_render_stream test_cli_render_style \
	test_cli_selector test_cli_session_metadata \
	test_cli_session_metadata_codec test_cli_session_metadata_store \
	test_cli_tool_panel test_cli_tools test_cli_utf8 test_cli_utf8_stream \
	test_config test_json test_llm_http test_llm_sse test_reactor \
	test_session test_sesslog test_tool_registry

# Tests that do touch that state: PTYs, sockets, forked helpers, or signals.
# Slower and ordering-sensitive, so they stay out of `quick` and are not
# candidates for blanket parallelism.
IMPURE_TESTS = \
	test_cli test_cli_composer test_cli_sessions test_cli_terminal \
	test_llm test_llm_conn test_tool_exec

PURE_TEST_BINS = $(PURE_TESTS:%=$(BUILD)/%)

test: $(TEST_BINS) $(CLI_BIN)
	@set -e; for t in $(TEST_BINS); do echo "== $$t =="; $$t; done

# The routine edit-test loop: runs deterministic unit tests only, but still
# builds $(CLI_BIN). Without that dependency this target passes while
# production-only sources (src/cli.c, src/cli_repl.c, src/cli_loop.c) no
# longer compile, since no pure test links them -- a fast tier that cannot
# notice a broken build is worse than no fast tier.
quick: $(PURE_TEST_BINS) $(CLI_BIN)
	@set -e; for t in $(PURE_TEST_BINS); do echo "== $$t =="; $$t; done

# Fails if PURE_TESTS + IMPURE_TESTS stops accounting for every test binary,
# so a newly added test cannot quietly belong to no tier and go unrun by
# everything except the full `check`.
#
# Checks both halves of the invariant. Set coverage alone is not enough: a
# test named in both tiers still satisfies "every test is tiered" while
# actually being run twice by `check` and misfiled in `quick`, so exclusivity
# is checked separately.
#
# Negative cases, each reproducible by editing the lists above and re-running:
#   missing     -- delete a name from PURE_TESTS
#   nonexistent -- add a name matching no test/test_*.c
#   duplicate   -- repeat a name within one tier
#   overlap     -- add a PURE_TESTS name to IMPURE_TESTS as well
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

$(INTEGRATION_BUILD)/test_cli_conversation: test/integration/test_cli_conversation.c $(CLI_TEST_LIB) $(LIB) | $(INTEGRATION_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) $< $(CLI_TEST_LIB) $(LIB) -o $@ $(LDLIBS)

$(INTEGRATION_BUILD)/test_cli_compact: test/integration/test_cli_compact.c $(CLI_TEST_LIB) $(LIB) | $(INTEGRATION_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) $< $(CLI_TEST_LIB) $(LIB) -o $@ $(LDLIBS)

$(INTEGRATION_BUILD)/test_cli_session_switch: test/integration/test_cli_session_switch.c $(CLI_TEST_LIB) $(LIB) | $(INTEGRATION_BUILD)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INCLUDES) $< $(CLI_TEST_LIB) $(LIB) -o $@ $(LDLIBS)

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
check: tier-audit test test-integration

# The pre-merge and release gate: everything `check` covers, under both
# compilers and every instrumentation the project has, plus the ABI export
# check and a bounded fuzz smoke run.
#
# The two compiler passes must use separate BUILD trees, and each tree must be
# deleted immediately before its pass. Three distinct hazards, all of which
# let this gate pass without a compiler having run:
#
#  - Make decides what to rebuild from timestamps, not from the value of CC.
#    Running both passes against one build/ leaves the second reporting "up to
#    date" and re-running the first compiler's binaries.
#  - Separate trees alone are not enough. A build-gcc/ left holding artifacts
#    from some earlier run -- a stray `make BUILD=build-gcc CC=clang`, an
#    interrupted pass -- is still "up to date", so the gcc pass compiles
#    nothing and the stamp then certifies gcc for clang's objects. Deleting
#    the tree first makes reuse impossible, which is the only thing that makes
#    the stamp mean anything.
#  - `$(MAKE) ... check compiler-stamp` names two goals in one sub-make, which
#    under -j may run concurrently, writing the stamp while compilation is
#    still going. They are separate $(MAKE) lines instead: lines within a
#    recipe run in order regardless of -j.
#
# The rebuild cost is deliberate. verify is the pre-merge gate, not the edit
# loop; `check` and `quick` remain incremental.
#
# Sequenced rather than parallel: each pass is a full rebuild, and running
# them concurrently competes for the cores the sanitized tests' own timing
# assumptions depend on.
VERIFY_FUZZ_RUNS ?= 200000

# Records which compiler populated this BUILD tree.
#
# The stamp names $(CC), which is only the *requested* compiler -- it says
# nothing about what produced the objects already in the tree. That is
# trustworthy only because verify deletes each tree immediately before its
# pass, so the tree it stamps was necessarily built from scratch by that
# compiler and nothing could have been reused. Do not call this against a
# tree that was not just rebuilt: it would happily certify someone else's
# artifacts.
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

# Records wall time for the compile phase, every test binary, and each tier,
# so the effect of a change to any of them can be compared rather than
# guessed at. Writes a Markdown table on stdout.
# Timings are only meaningful for a build that actually succeeded and tests
# that actually passed, so a failure fails this target. Measurement output is
# still printed first -- the point is to see where the time went even on a bad
# run -- but the exit status never reports success for an incomplete build.
#
# Per-binary failures are recorded in a file rather than a shell variable
# because that loop is piped into sort, and a pipeline's exit status is the
# last command's.
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
	@nm -D --defined-only --format=posix $(LIB_SO_REAL) | \
		awk '$$1 ~ /^oi_/ { sub(/@.*/, "", $$1); print $$1 }' | \
		sort -u | diff -u test/abi_exports.txt -

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

# The CLI-private render/Markdown pipeline doesn't touch the public
# library, so this harness links only its own dependency chain (the
# generic pattern rule above pulls in $(LIB_SRCS), which it doesn't need).
CLI_RENDER_STREAM_SRCS = src/cli_render_stream.c src/cli_render_style.c \
	src/cli_markdown_block.c src/cli_markdown_inline.c src/cli_markdown.c \
	src/cli_render_sanitize.c src/cli_utf8_stream.c src/cli_utf8.c \
	src/cli_bytebuf.c

$(FUZZ_BUILD)/fuzz_cli_render_stream: test/fuzz/fuzz_cli_render_stream.c $(CLI_RENDER_STREAM_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(CSTD) $(WARN) -g -O1 \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $< $(CLI_RENDER_STREAM_SRCS) -o $@ $(LDLIBS)

# Also CLI-private, plus the JSON parser/writer $(LIB_SRCS) itself needs
# for record encode/decode (oi_cli_history_record_encode is compiled in
# even though this harness never calls it, so the writer side is a real
# link dependency too, not just the parser).
CLI_HISTORY_REPLAY_FUZZ_SRCS = src/cli_history.c src/cli_history_codec.c \
	src/cli_history_replay.c src/cli_message.c src/arena.c \
	src/json_value.c src/json_parse.c src/json_write.c

$(FUZZ_BUILD)/fuzz_cli_history_replay: test/fuzz/fuzz_cli_history_replay.c $(CLI_HISTORY_REPLAY_FUZZ_SRCS) | $(FUZZ_BUILD)
	$(FUZZ_CC) $(CSTD) $(WARN) -g -O1 \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $< $(CLI_HISTORY_REPLAY_FUZZ_SRCS) -o $@ $(LDLIBS)

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
	rm -rf $(BUILD) build-gcc build-clang build-asan build-ubsan \
		build-tsan $(FUZZ_BUILD)
