# build: decision records from code comments

Date: 2026-11-20

These records were comments in the Makefile and the CI workflow. The project does not allow code comments, so the records are here. Each heading names the target or the variable that the record applies to. The record text is the original comment text.

Source files: `Makefile`, `.github/workflows/ci.yml`.

## `CFLAGS` (Makefile)

The default build is what `make install` ships, so it is optimized and hardened. _FORTIFY_SOURCE has no effect without optimization; -U first because some compilers predefine it, and a redefinition fails -Werror. The sanitizer tiers replace CFLAGS with their own -O1 flags.

## `PIE_CFLAGS` (Makefile)

PIE and full RELRO do not depend on CFLAGS or on compiler defaults, so a CFLAGS override still links: -pie needs every object built with -fPIE.

## `DEPFLAGS` (Makefile)

-MMD writes a <object>.d dependency file next to each object, so a header edit rebuilds every compile that includes it instead of silently linking stale objects against a new declaration (an ABI drift that the suite then fails with a hard-to-place crash). -MP adds phony per-header targets so a deleted header does not error the next run. The .d files are read back in at the bottom of this file via -include.

## `PIC_BUILD` (Makefile)

Shared library: built from separately-compiled -fPIC objects in their own subdirectory, so `make lib`'s default (non-PIC) objects for the static archive don't need -fPIC baked into every build (including the sanitizer variants below, which reuse CFLAGS/BUILD overrides and never touch the .so).

## `CLI_TEST_SRCS` (Makefile)

Private CLI implementation archive for unit/integration tests. Production still links its complete object list directly, so archive member selection cannot change the real executable's linkage. cli.c owns main(), while cli_loop.c and cli_repl.c are exercised through that real executable rather than linked into private-module tests; excluding all three also means a production-only edit does not relink every CLI test.

## `uninstall` (Makefile)

Removes exactly what `install` creates, and nothing else: the header directory is removed with rmdir rather than `rm -r`, so an INCLUDEDIR/oi holding anything this project did not install is left alone instead of being deleted along with it. Session data is never touched -- see docs/CLI.md for where it lives and how to remove it deliberately.

## `$(BUILD)/test_tool_exec` (Makefile)

test_tool_exec records the kill() calls that liboi makes.

## `$(BUILD)/test_reactor` (Makefile)

test_reactor counts the epoll_ctl calls that liboi makes.

## `$(BUILD)/test_llm_conn` (Makefile)

test_llm_conn gives one host name a dead address and a live address, counts the send() calls that carry MSG_NOSIGNAL, counts SSL_CTX_new, and moves the monotonic clock forward.

## `PURE_TESTS` (Makefile)

Tests that touch no PTY, socket, fork, signal, or other global process state, and so are safe to run as the routine edit-test loop (`make quick`).

Membership is spelled out rather than derived, deliberately: a test that grows a socket must be moved out of this list by someone who noticed, not silently reclassified by a pattern match. The `tier-audit` target below fails the build if this list plus IMPURE_TESTS stops covering every binary.

## `IMPURE_TESTS` (Makefile)

Tests that do touch that state: PTYs, sockets, forked helpers, or signals. Slower and ordering-sensitive, so they stay out of `quick` and are not candidates for blanket parallelism.

## `test` (Makefile)

Unit-test runner. A recursive make lets `make -j check` use its jobserver for the explicitly audited pure binaries. The impure binaries always run in one serial loop afterward, so PTYs, sockets, forks, signals, and other process-global behavior never overlap merely because the build was parallel.

## `quick` (Makefile)

The routine edit-test loop: runs deterministic unit tests only, but still builds $(CLI_BIN). Without that dependency this target passes while production-only sources (src/cli.c, src/cli_repl.c, src/cli_loop.c) no longer compile, since no pure test links them -- a fast tier that cannot notice a broken build is worse than no fast tier.

## `tier-audit` (Makefile)

Fails if PURE_TESTS + IMPURE_TESTS stops accounting for every test binary, so a newly added test cannot quietly belong to no tier and go unrun by everything except the full `check`.

Checks both halves of the invariant. Set coverage alone is not enough: a test named in both tiers still satisfies "every test is tiered" while actually being run twice by `check` and misfiled in `quick`, so exclusivity is checked separately.

Negative cases, each reproducible by editing the lists above and re-running:

- `missing`: delete a name from PURE_TESTS

- `nonexistent`: add a name matching no test/test_*.c

- `duplicate`: repeat a name within one tier

- `overlap`: add a PURE_TESTS name to IMPURE_TESTS as well

## `INTEGRATION_BUILD` (Makefile)

Integration tests get their own build subdir so their binaries don't collide with the unit tests' $(BUILD)/test_% pattern rule. Helper headers (mock_api.h) are picked up relative to the source file, so only test_*.c become binaries.

## `check` (Makefile)

The full automated suite. Sanitizer and valgrind targets run this rather than `test` alone, so instrumentation covers the integration tests too -- they are where the reactor, the socket paths, and the tool subprocesses actually run together.

## `verify` (Makefile)

The pre-merge and release gate: everything `check` covers, under both compilers and every instrumentation the project has, plus the ABI export check and a bounded fuzz smoke run.

The two compiler passes must use separate BUILD trees, and each tree must be deleted immediately before its pass. Three distinct hazards, all of which let this gate pass without a compiler having run:

- Make decides what to rebuild from timestamps, not from the value of CC. Running both passes against one build/ leaves the second reporting "up to date" and re-running the first compiler's binaries.

- Separate trees alone are not enough. A build-gcc/ left holding artifacts from some earlier run -- a stray `make BUILD=build-gcc CC=clang`, an interrupted pass -- is still "up to date", so the gcc pass compiles nothing and the stamp then certifies gcc for clang's objects. Deleting the tree first makes reuse impossible, which is the only thing that makes the stamp mean anything.

- `$(MAKE) ... check compiler-stamp` names two goals in one sub-make, which under -j may run concurrently, writing the stamp while compilation is still going. They are separate $(MAKE) lines instead: lines within a recipe run in order regardless of -j.

The rebuild cost is deliberate. verify is the pre-merge gate, not the edit loop; `check` and `quick` remain incremental.

Sequenced rather than parallel: each pass is a full rebuild, and running them concurrently competes for the cores the sanitized tests' own timing assumptions depend on.

## `compiler-stamp` (Makefile)

Records which compiler populated this BUILD tree.

The stamp names $(CC), which is only the *requested* compiler -- it says nothing about what produced the objects already in the tree. That is trustworthy only because verify deletes each tree immediately before its pass, so the tree it stamps was necessarily built from scratch by that compiler and nothing could have been reused. Do not call this against a tree that was not just rebuilt: it would happily certify someone else's artifacts.

## `timings` (Makefile)

Records wall time for the compile phase, every test binary, and each tier, so the effect of a change to any of them can be compared rather than guessed at. Writes a Markdown table on stdout. Timings are only meaningful for a build that actually succeeded and tests that actually passed, so a failure fails this target. Measurement output is still printed first -- the point is to see where the time went even on a bad

run -- but the exit status never reports success for an incomplete build.

Per-binary failures are recorded in a file rather than a shell variable because that loop is piped into sort, and a pipeline's exit status is the last command's.

## `tsan` (Makefile)

TSan is here to catch accidental concurrency assumptions (signal handlers, the forked tool subprocesses) creeping into what is meant to be a single-threaded reactor, not because the harness spawns threads.

On kernels that hand out more ASLR entropy than TSan's shadow mapping expects (recent Ubuntu, WSL2), it aborts at startup with "unexpected memory mapping". Either run `setarch -R make tsan`, or lower the entropy globally with `sudo sysctl -w vm.mmap_rnd_bits=28` (what CI does).

## `valgrind` (Makefile)

Valgrind runs against a plain (non-sanitizer) build -- ASan and valgrind's instrumentation don't coexist.

--trace-children=no keeps it off the tools' exec'd children (/bin/echo and friends), which are not ours to check and would only add noise.

## `FUZZ_CC` (Makefile)

libFuzzer is a clang feature -- gcc has no -fsanitize=fuzzer -- so the fuzz targets pin their own compiler rather than following CC, which the rest of the build (and CI's gcc matrix entry) is free to set elsewhere.

## `FUZZ_RUNS` (Makefile)

Iterations per harness for `make fuzz-run`, which exists to catch regressions in CI, not to find new bugs. Override it, or run a built harness directly, for an open-ended campaign.

## `$(FUZZ_BUILD)/%` (Makefile)

Harnesses link the library sources directly rather than $(LIB): the instrumentation must cover the code under test, and $(LIB) is built without it.

## `CLI_RENDER_STREAM_SRCS` (Makefile)

The CLI-private render/Markdown pipeline doesn't touch the public library, so this harness links only its own dependency chain (the generic pattern rule above pulls in $(LIB_SRCS), which it doesn't need).

## `CLI_HISTORY_REPLAY_FUZZ_SRCS` (Makefile)

Also CLI-private, plus the JSON parser/writer $(LIB_SRCS) itself needs for record encode/decode (oi_cli_history_record_encode is compiled in even though this harness never calls it, so the writer side is a real link dependency too, not just the parser).

## `fuzz-run` (Makefile)

Seed corpora are read-only inputs; newly discovered ones go to a scratch dir under $(FUZZ_BUILD) so a run never dirties the work tree.

## `-include` dependency files (Makefile)

Dependency files, read back in so header edits rebuild dependents. All build trees are defined above this point; a wildcard that matches nothing (a tree not built yet, or a tree wiped by `clean`) silently includes nothing.

## `test` (.github/workflows/ci.yml)

Both compilers: a GCC-only construct in the library would otherwise only surface on someone else's machine.

## `Narrow ASLR entropy for ThreadSanitizer` step (.github/workflows/ci.yml)

ThreadSanitizer's shadow mapping assumes fewer ASLR bits than recent kernels hand out; without this it aborts at startup with "unexpected memory mapping" before running a single test.

## `valgrind` (.github/workflows/ci.yml)

Secondary memory check: catches what ASan's redzone model doesn't, and runs against an uninstrumented build since the two allocators cannot coexist.

## `fuzz` (.github/workflows/ci.yml)

A short regression run over the checked-in seed corpora -- enough to catch a parser change that reintroduces a known crash, not a substitute for an open-ended campaign.

## `timeout-minutes` (.github/workflows/ci.yml)

A hung child must fail the job, not run the 6-hour default.
