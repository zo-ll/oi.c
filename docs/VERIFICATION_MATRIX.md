# REPL verification matrix

Issue #30's release gate requires every item in the REPL plan's
[Verification gates](REPL_PLAN.md) section to map to a concrete test binary,
test case, or CI job, and the combined suite to pass under both compilers and
every instrumentation. This file is that mapping, kept current with the tests
it names. When a gate gains coverage, update the map here in the same change.

The gates themselves are the six bullets at `docs/REPL_PLAN.md` ("Verification
gates"). Each row below names the artifact that proves it.

## Gate 1 — Strict GCC and Clang builds

- `make verify` runs a clean `make check` under `CC=gcc` into `build-gcc` and
  under `CC=clang` into `build-clang`, deleting each tree immediately before
  its pass so a stale tree cannot certify the wrong compiler, then checks the
  `compiler.txt` stamps (`verify-compilers`).
- CI: `.github/workflows/ci.yml` `test (gcc)` and `test (clang)` jobs run
  `make check` and `make abi-check` per compiler. Every job has
  `timeout-minutes: 60` so a hung child fails the job instead of running the
  platform's 6-hour default.
- Every compile uses `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
  -Werror`.

## Gate 2 — Unit tests for codecs, state machines, UTF-8, filtering, Markdown, sanitization, paths, recovery, compaction

| Coverage | Tests |
| --- | --- |
| history codec (encode/decode) | `test_cli_history_codec` (11) |
| session-metadata codec | `test_cli_session_metadata_codec` |
| history replay state machine | `test_cli_history_replay` (13) |
| command dispatch/grammar | `test_cli_command_dispatch`, `test_cli_commands` |
| UTF-8 editing | `test_cli_editor` (7), `test_cli_utf8` (10), `test_cli_utf8_stream` (9) |
| command filtering/selector | `test_cli_selector` (10) |
| Markdown chunks | `test_cli_markdown` (4), `test_cli_markdown_block` (15), `test_cli_markdown_inline` (22), `test_cli_render_stream` (12) |
| sanitization | `test_cli_render_sanitize` (16) |
| session paths / metadata | `test_cli_session_metadata` (8) |
| metadata store: atomic replace, lock, stale caches | `test_cli_session_metadata_store` (8) |
| history repair / recovery | `test_cli_history_repair` (3) |
| compaction unit behavior | `test_cli_compact` (20) |
| CLI message/tool plumbing | `test_cli_message`, `test_cli_tools`, `test_cli_prompt_state`, `test_cli_input`, `test_cli_input_history`, `test_cli_bytebuf`, `test_cli_present`, `test_cli_render`, `test_cli_render_style`, `test_cli_tool_panel`, `test_cli_status`, `test_cli_composer` |

Fuzz harnesses exercise the untrusted-input edges of the same modules:
`test/fuzz/fuzz_json.c`, `fuzz_http.c`, `fuzz_sse.c`, `fuzz_sesslog.c`,
`fuzz_cli_history_replay.c`, `fuzz_cli_render_stream.c` (corpora under
`test/fuzz/corpus/`).

## Gate 3 — PTY tests

| Coverage | Test case (in `test/test_cli.c` unless noted) |
| --- | --- |
| raw-mode enable + module restore | `test_cli_terminal` (3): `enable_sets_raw_mode_and_restore_reinstates_it`, `non_terminal_descriptors_are_refused`, `isig_can_be_toggled_without_leaving_raw_mode` |
| raw-mode/bracketed-paste restoration on every exit path | `repl_restores_terminal_state_on_every_exit_path` (covers `/exit`, `Ctrl+D`, and `SIGTERM`; asserts cooked termios on the parent-held slave fd and the `ESC[?2004l` disable) |
| multiline editing | `test_cli_composer`, `test_cli_input` (unit) |
| resize / concurrent redraw | `resize_redraws_the_live_prompt_at_the_new_width`, `permission_ask_resize_redraws_the_selector`, `tool_panel_coexists_with_resize_and_queued_input`, `typing_during_a_turn_does_not_corrupt_the_display` |
| `Ctrl+C` cancellation | `sigint_cancels_an_in_flight_request_and_returns_to_the_prompt`, `sigint_cancels_a_running_tool_and_returns_to_the_prompt`, `typed_ctrl_c_during_a_turn_cancels_it`, `ctrl_c_with_a_pending_item_discards_it_and_restores_the_draft`, `typed_ctrl_c_cancels_compaction_and_returns_to_the_prompt`, `permission_ask_ctrl_c_while_awaiting_denies_without_hanging` |
| `Ctrl+D` | `interactive_exit_before_submission_creates_no_session`, `ctrl_d_during_a_turn_has_no_effect` |
| external termination | `sigterm_during_a_turn_terminates_cleanly`, `mock_control_closing_the_release_end_fails_the_server_promptly` |
| permission selectors | `permission_ask_allow_once_lets_the_tool_run`, `permission_ask_deny_ends_the_turn`, `permission_ask_allow_for_process_elevates_policy`, `permission_ask_ctrl_c_while_awaiting_denies_without_hanging`, `permissions_allow_*` |
| tool panels | `tool_panel_shows_live_output_and_failed_status`, `tool_panel_survives_malicious_escape_bytes_in_output` |
| redirected stdin / non-TTY | `piped_prompt_carries_stdin_into_the_request`, `default_ask_policy_denies_safely_without_a_controlling_terminal`, `missing_api_key_fails` (all one-shot mode), plus `non_terminal_descriptors_are_refused` |
| session lifecycle under the real CLI | `interactive_session_*`, `interactive_session_trash_restore_and_confirmed_delete`, `interactive_session_import_requires_confirmation_and_keeps_the_source` |
| compaction under the real CLI | `compact_replaces_older_turns_with_a_checkpoint` and the other `compact_*` PTY cases |

PTY timing: every child wait uses `waitpid` with no unbounded loop, and every
read against a live descriptor uses `select`/`poll` with a timeout
(`interactive_wait_for` and `slow_mock_*`). A stalled child therefore fails
with a diagnostic instead of hanging CI.

Process-level watchdog: every impure and integration binary arms
oi_test_set_deadline(900) (test.h), a SIGALRM deadline that prints the name
of the test that was executing and exits 124 if the whole binary runs longer
than 15 minutes. This closes the one unbounded vector — a CLI child that
regresses into a silent hang — which per-read timeouts cannot catch. Mock
servers additionally bound each accept with a 20 s select and exit without a
client (a failed exec or missing connection fails in seconds, not forever).
Pure tests never arm the deadline; it is generous so valgrind/tsan runs do
not false-positive.

## Gate 4 — Mock-server integration

| Coverage | Tests |
| --- | --- |
| multi-turn context/tool replay | `test/integration/test_cli_conversation`: `start_is_event_driven_and_preserves_context`, `tool_start_boundary_precedes_process_output`; `test/test_cli.c`: `resume_replays_prior_exchange` |
| tool history | `test/integration/test_session_loop`: `full_tool_use_loop`, `denied_tool_never_executes`, `deferred_permission_resolves` |
| steering | `test/integration/test_cli_conversation`: `steer_after_a_tool_completes_skips_the_next_one`, `steer_after_the_only_tool_prevents_a_second_model_round`, `steer_before_any_tool_starts_skips_them_all` |
| cancellation / recoverable failure | `test/integration/test_cli_conversation`: `cancel_while_awaiting_permission_repairs_as_not_executed`, `cancel_while_streaming_needs_no_repair`, `cancel_while_tool_running_repairs_messages`, `cancel_from_within_tool_starting_event`; `test/test_cli.c`: `recoverable_turn_error_returns_to_the_prompt` |
| queued-input recovery | `test/test_cli.c`: `busy_submit_is_queued_and_a_second_one_is_refused`, `queued_message_resumes_at_the_safe_boundary_with_correct_turn_ids`, `queued_command_while_busy_resolves_discarded_and_dispatches_live`, `crash_with_a_pending_item_restores_it_as_a_startup_draft` |
| session switching/import/repair | `test/integration/test_cli_session_switch` (8); `test/test_cli.c`: `interactive_session_switch_*` |
| compaction/checkpoint restart | `test/integration/test_cli_compact` (6), including `compact_run_cancels_on_sigint_*`; `test/test_cli.c`: `compact_survives_restart_and_replay_shows_the_checkpoint`, `compact_failure_leaves_durable_and_live_state_untouched` |
| one-shot stdout compatibility | `end_to_end_streaming_reply`, `tool_loop_executes_and_returns_result`, `dry_run_reports_resolved_config` |
| session isolation on API failure | `api_error_fails_only_its_own_session` |
| mock-server control discipline | `mock_control_holds_a_response_until_it_is_released`, `mock_control_release_before_acceptance_is_rejected`, `mock_control_closing_the_release_end_fails_the_server_promptly`; `mock_cleanup_stops_unused_server` |

## Gate 5 — ABI export check

- `make abi-check` diffs the `oi_*` dynamic exports of `liboi.so` against
  `test/abi_exports.txt`. The REPL feature is CLI-private, so the file must
  not change for it; a deliberate public API change is the only way it may.
- CI runs it under both compilers.

## Gate 6 — Sanitizers, Valgrind, fuzzers

| Instrumentation | Target | CI job |
| --- | --- | --- |
| ASan + UBSan | `make asan` (`-fsanitize=address,undefined`, `build-asan`) | `sanitizers (asan)` |
| UBSan alone | `make ubsan` (`build-ubsan`) | `sanitizers (ubsan)` |
| TSan | `make tsan` (`build-tsan`) | `sanitizers (tsan)` |
| Valgrind | `make valgrind` (leak-check full, `--trace-children=no`, plain build) | `valgrind` (60 min) |
| Fuzz regression | `make fuzz-run` (clang libFuzzer, ASan+UBSan) | `fuzz` (`FUZZ_RUNS=200000`, crash artifacts uploaded on failure) (60 min) |

## Release checklist

Run before declaring the REPL milestone complete. Code-failure items must be
fixed; environment-only limitations are recorded, not fixed, but must be
noted in the release notes.

1. `make verify` — clean GCC pass, clean Clang pass, compiler stamps,
   `abi-check`, ASan, UBSan, TSan, Valgrind, and 200,000-iteration fuzz runs
   all green. This is the whole gate in one command.
2. Confirm `main` is clean and the remote CI jobs (test gcc/clang,
   sanitizers asan/ubsan/tsan, valgrind, fuzz) are all green on the head
   commit.
3. macOS: the kqueue reactor is maintained on the `macos-support` branch,
   not `main` (see `PLAN.md`); there is no native-macOS CI job. A macOS
   verification pass must build that branch manually. This is a recorded
   environment limitation, not a code failure.
4. TSan on hosts that hand out more ASLR entropy than its shadow mapping
   expects (recent Ubuntu, WSL2) aborts at startup with "unexpected memory
   mapping". CI works around it with `sysctl -w vm.mmap_rnd_bits=28`; local
   runs use `setarch -R make tsan`. A TSan shadow-map abort with a recorded
   workaround is an environment limitation, not a code failure.
5. Fuzz runs are regression smoke runs; an open-ended campaign is an
   explicit non-goal of the release gate.

Known gaps tracked against this matrix (update as they close):

- Native macOS PTY/kqueue verification (gate 3 on macOS) has no automated
  job; it depends on the `macos-support` branch (see item 3).
