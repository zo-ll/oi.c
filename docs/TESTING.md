# Testing workflow

The test suite has three verification levels:

- `make quick` builds the real CLI and runs the deterministic pure unit tests.
  Use it during routine editing.
- `make check` runs every ordinary unit and integration binary. Use it before
  committing.
- `make verify` is the pre-merge and release gate. It performs clean GCC and
  Clang checks with compiler-provenance validation, ABI validation, ASan,
  UBSan, TSan, Valgrind, and bounded fuzzing.

Add `-j` when parallel build/test capacity is useful:

```sh
make -j24 quick
make -j24 check
```

Only the explicit `PURE_TESTS` set runs concurrently. Those binaries use no
PTYs, sockets, forks, or signals. Their temporary paths include the process ID,
and their environment, current directory, and globals are process-local.
`IMPURE_TESTS` and every integration binary remain serialized even under
`make -j`, preventing the command from introducing overlap among PTYs, ports,
signals, child processes, and process-global state.

`make tier-audit` proves every unit-test binary belongs to exactly one tier.
It rejects missing, nonexistent, duplicated, and overlapping entries.
Individual binaries remain directly runnable, for example:

```sh
make build/test_cli_composer
build/test_cli_composer
```

Two tests guard the user-facing documentation against drift rather than
proving behavior. `test_cli_docs` requires every command name, usage string,
and description in `cli_commands.c`, plus every `Ctrl+key` binding `/help`
prints, to appear in [CLI.md](CLI.md). `test_cli`'s
`help_flags_are_documented_in_the_guide` does the same for every long flag
`oi --help` prints; it lives there because the usage text belongs to `cli.c`,
which is deliberately not in the private test archive. Adding or renaming a
command or flag therefore fails the suite until the guide is updated.

`make timings` performs a clean serial build, times the pure and complete
runtime tiers, and reports every binary separately. A build or test failure
makes the command fail rather than producing a successful-looking partial
report.

`docs/VERIFICATION_MATRIX.md` maps every REPL-plan verification gate to the
concrete test cases and CI jobs that prove it, and records the release
checklist with environment-only limitations (macOS kqueue on the
`macos-support` branch, the TSan ASLR workaround). Keep it in sync when a
gate gains or loses coverage.

## Build architecture

CLI sources compile once into `build/cli/*.o`. The production `build/oi`
executable links its complete object list directly. CLI-private unit and
integration tests link the non-installed `build/liboi_cli_test.a`, allowing
the linker to select only the internal modules each test references without
recompiling their source chains.

`cli.c`, `cli_loop.c`, and `cli_repl.c` are excluded from the private archive.
They are exercised through the real CLI, and a production-only edit therefore
does not relink every private-module test.

## Issue #32 measurements

Measurements were taken on 2026-07-30 on the same 24-core development machine.
The merged post-issue-#21 baseline was `df5058c`; final measurements use the
issue-#32 branch after the response synchronization, reusable CLI objects,
selector-resize fix, and collision-audited runner.

| Phase | Baseline | Final |
| --- | ---: | ---: |
| clean serial compilation | 19.2 s | 7.65 s |
| clean serial `make check` | 71.3 s | 38.07 s |
| clean `make -j24 check` | 52.7 s | 32.60 s |
| `quick` run only | 0.11 s | 0.14 s |
| `quick` after a CLI-private source edit | 4.5 s | 3.00 s |
| `quick` after a production-only `cli_repl.c` edit | not recorded | 0.28 s |
| `test_cli` | 34.4 s | 16.09 s |
| deep `make verify` | no unified baseline command | 689.82 s |

The isolated reusable-object comparison on the already-optimized branch was
10.67 s before the change and 7.65 s afterward for clean serial compilation,
a 28% reduction. The final deep-gate time includes 200,000 fuzz iterations for
each of six harnesses.

Final per-binary runtimes from `make timings`:

| Binary | Seconds |
| --- | ---: |
| `build/test_cli` | 16.092 |
| `build/test_tool_exec` | 5.673 |
| `build/test_llm` | 4.435 |
| `build/integration/test_session_loop` | 2.241 |
| `build/integration/test_cli_conversation` | 1.271 |
| `build/test_cli_sessions` | 0.310 |
| `build/test_cli_composer` | 0.132 |
| `build/integration/test_cli_compact` | 0.096 |
| `build/test_reactor` | 0.064 |
| `build/test_llm_conn` | 0.018 |
| `build/test_cli_editor` | 0.008 |
| `build/test_sesslog` | 0.005 |
| `build/integration/test_cli_session_switch` | 0.005 |
| `build/test_cli_terminal` | 0.003 |
| `build/test_cli_markdown_block` | 0.003 |
| `build/test_session` | 0.003 |
| `build/test_arena` | 0.003 |
| `build/test_cli_render_stream` | 0.003 |
| `build/test_cli_compact` | 0.003 |
| `build/test_cli_tools` | 0.003 |
| `build/test_tool_registry` | 0.003 |
| `build/test_llm_http` | 0.003 |
| `build/test_config` | 0.003 |
| `build/test_cli_history_repair` | 0.003 |
| `build/test_cli_render_sanitize` | 0.003 |
| `build/test_cli_command_dispatch` | 0.003 |
| `build/test_cli_input` | 0.003 |
| `build/test_cli_history` | 0.003 |
| `build/test_cli_markdown_inline` | 0.003 |
| `build/test_cli_tool_panel` | 0.003 |
| `build/test_cli_utf8` | 0.003 |
| `build/test_cli_session_metadata_store` | 0.003 |
| `build/test_llm_sse` | 0.003 |
| `build/test_cli_present` | 0.003 |
| `build/test_cli_history_store` | 0.003 |
| `build/test_json` | 0.003 |
| `build/test_cli_render` | 0.003 |
| `build/test_cli_message` | 0.003 |
| `build/test_cli_history_replay` | 0.003 |
| `build/test_cli_session_metadata_codec` | 0.003 |
| `build/test_cli_markdown` | 0.003 |
| `build/test_cli_bytebuf` | 0.002 |
| `build/test_cli_selector` | 0.002 |
| `build/test_cli_render_style` | 0.002 |
| `build/test_cli_history_codec` | 0.002 |
| `build/test_cli_commands` | 0.002 |
| `build/test_cli_input_history` | 0.002 |
| `build/test_cli_utf8_stream` | 0.002 |
| `build/test_cli_prompt_state` | 0.002 |
| `build/test_cli_session_metadata` | 0.002 |

Small values are rounded to the nearest millisecond; use `make timings` for
fresh nanosecond-resolution results on the current machine.
