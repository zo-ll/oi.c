# cli: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli.c`, `test/test_cli.c`.

## `print_usage` (src/cli.c)

Thin standalone CLI: resolves config, opens (or resumes) a session, sends a prompt argument (or stdin) through the CLI-owned model/tool loop, and prints assistant replies as they stream. Configuration and session lifecycle stay here; loop state and built-in tool policy live in their own internal modules.

A URL, not the repository-relative docs/CLI.md: `make install` installs the binary, the libraries, and the headers, so an installed `oi` has no docs directory to point anyone at.

## `read_stdin_all` (src/cli.c)

ensure non-NULL even if EOF immediately

## `persistence_context.model` (src/cli.c)

owned; may be updated live by /model

## `persistence_context.session_path` (src/cli.c)

borrowed directory or explicit log path

## `persistence_context.metadata_path` (src/cli.c)

borrowed, stable for the process lifetime

## `persistence_context.session_id` (src/cli.c)

borrowed

## `session_admin_context` (src/cli.c)

Everything the /session lifecycle commands need in order to act on main()'s own locals. Those locals are private to this translation unit, so the policy lives in cli_sessions/cli_session_switch and only the adoption of a new session happens here.

## `session_admin_context.switched_metadata_path` (src/cli.c)

Owned slot holding the metadata path of the most recently switched-to session. persistence->metadata_path points into it after a switch, so it must outlive every use and is freed once at cleanup.

## `session_admin_context.switched_session_path` (src/cli.c)

Same ownership arrangement for the user-facing session directory.

## `admin_current_session` (src/cli.c)

Resolved live on every call rather than cached from startup: the cache can be removed or corrupted by something outside oi at any point in a long-running session, and a single bounded read is cheap.

## `oi_cli_session_trash` (src/cli.c)

Passing the active id is what makes "cannot trash the session you are in" enforceable down in the policy layer.

## `admin_switch_session` (src/cli.c)

Adopts a different session as the active one.

oi_cli_session_switch does all the risky work and touches nothing that is currently live, so by the time it returns OK the only thing left is an ownership handover. Destroying the old session here is safe precisely because cli_repl.c has already destroyed the conversation that was allocating from its arena -- the precondition documented on the callback.

Move, rather than copy: the result's copies are re-inited so freeing it afterwards cannot double-free what was handed over.

## `oi_session_destroy` (src/cli.c)

Commit point. NULL-safe: switching before the first message means there is no session or history to release yet.

## `oi_cli_session_restore_init` (src/cli.c)

Every automatic session is brand new: bare interactive startup always begins a fresh timestamped directory (docs/REPL_PLAN.md), so this always durably records its first-ever effective model/cwd.

## `persist_model_setting` (src/cli.c)

Durable persistence for a live /model or /cwd change -- cli_repl.c's controller only requests the change; cli.c owns what persisting it actually means (a durable history record plus a metadata.json refresh). NULL `store` means no durable session exists yet (nothing to persist to yet; the REPL's own live update already applied).

## `persist_queued_input` (src/cli.c)

Durable persistence for the one-slot queued-input mechanism. Unlike persist_model_setting/persist_cwd_setting, these append a plain history record directly (oi_cli_history_store_append), the same way persist_conversation_event does -- there is no metadata.json refresh involved. context->state->next_record_id/next_turn_id are exactly right for a QUEUED_INPUT record (cli_history_replay.c requires its turn_id to be the *next* turn, the one that hasn't started yet, not the currently active one tracked by context->turn_id); a successful append then has oi_cli_history_store_append itself replay context->state fresh, which is what populates pending_input_turn_id below for the matching QUEUE_RESOLVED record -- cli_repl.c never needs to track or pass a turn_id itself, only the opaque record id persist_queued_input hands back.

## `persist_checkpoint` (src/cli.c)

`prefix_message_count` is a count into cli_repl.c's own live message list, never a record id -- the source range recorded here is read directly off replay_state->context[0..prefix_message_count-1]'s own record_id fields (real, stored ids), never computed arithmetically from the count itself, since a live index and a durable record id diverge once any earlier checkpoint has already collapsed part of the history.

## `seed_pending_draft_from_replay` (src/cli.c)

Closes out a crash-recovery window found by replay (a QUEUED_INPUT record with no matching QUEUE_RESOLVED, replay_state->has_pending_input): persists QUEUE_RESOLVED(DISCARDED) for it immediately, before anything else can run, so a second crash right after this restart can't re-litigate it, then hands its exact text back as a plain editable draft -- never an auto-resubmit, since nothing should silently run on the user's behalf after a crash. A no-op (out_draft untouched) when there's nothing pending. Only ever reachable for an explicit --session restore of a pre-existing log: an automatic session's directory is always brand new (see prepare_automatic_session's own comment), so it can never have a queued item left over from before this process even started.

Copied before the append below: a successful append replays and replaces *state wholesale (see persist_queued_input's own comment), which frees state->pending_input as part of clearing has_pending_input back to 0.

## `status_checkpoint` (src/cli.c)

Reports the durable checkpoint /status names. Read live off the replay state rather than remembered from startup: oi_cli_history_store_append republishes that state after every append, so a checkpoint written by a /compact this run is visible here without any separate bookkeeping, and a /session switch's adopted state is too.

`applied_this_run` and `context_compacted` are deliberately left alone -- they are the REPL's to fill in, since only it knows what it spliced into a live conversation that may have no durable storage behind it at all.

## `persistence_has_failed` (src/cli.c)

Wired unconditionally into oi_cli_repl_config, even for an ephemeral session with no durable persistence at all: persist_conversation_event (the only writer of persistence->last_error) is then never registered as on_event, so last_error naturally, permanently stays OI_OK.

## `main.cli_use_tls` (src/cli.c)

-1 = not specified on the command line

## `main` (src/cli.c)

The ID becomes a file name under the session directory, so a separator or a traversal component must never reach it.

The same resolved values the client above was built from, so /status reports the endpoint requests actually go to. Only the non-secret fields are passed on: the API key and CA file stay in llm_cfg.

## `unsetenv` (src/cli.c)

Tool subprocesses inherit this environment; a key left here is one `echo` away from the model and the session log.

## `oi_wipe` (src/cli.c)

Other users can read the argv strings in /proc/PID/cmdline and in `ps` for as long as the process runs.

## `main.reactor` (src/cli.c)

All of oi_reactor_destroy/oi_session_registry_destroy/ oi_llm_client_destroy are documented NULL-safe, so a single cascading cleanup at the end handles every early-failure point below without each one needing its own partial-teardown code -- whatever wasn't reached yet is just NULL and skipped.

## `main.model_origin` (src/cli.c)

Refined below for an explicit --session, whose replayed history or metadata cache can outrank a plain default.

## `main.is_new_history` (src/cli.c)

Captured before this run appends anything (including its own transition record below), so it reflects whether this ID had any pre-existing records at all.

## `oi_llm_client_set_keep_alive` (src/cli.c)

cleanup destroys the client before the reactor, as keep-alive requires.

## `file scope` (test/test_cli.c)

Exercises the actual compiled `oi` binary (build/oi) as a subprocess rather than any library code directly -- this is the one test in the suite whose job is to prove the CLI wrapper itself works, not just the pieces it's built from. Assumes CWD is the repo root, matching how `make test` invokes every test binary.

## `slow_mock_turn.hold` (test/test_cli.c)

When set, the server accepts and drains the request, announces that on the control channel, and then holds the response until the test explicitly releases it. Tests that need a request to still be in flight -- every cancellation and queueing test here -- use this instead of guessing how long "in flight" lasts.

## `slow_mock_control.accepted_unreleased` (test/test_cli.c)

Held requests observed via slow_mock_wait_accepted that have not yet been paired with a slow_mock_release call. Unheld requests never enter this count, so every credit corresponds to exactly one response blocked on the release pipe.

## `slow_mock_wait_accepted` (test/test_cli.c)

Blocks until the server reports `count` more accepted requests.

This is the barrier that makes a held turn mean anything. Terminal output such as the "\r\n" a submission echoes only proves the CLI rendered something; it says nothing about whether the HTTP request has reached the server. Releasing before that point is a silent no-op -- the byte waits in the pipe and the server consumes it the moment it accepts, holding nothing -- so every held turn must be waited for here first.

Bounded, and fails the test rather than hanging the suite if the request never arrives.

Only held requests produce acceptance credits. The two completed ordinary turns cannot accidentally authorize this release; this credit belongs specifically to the in-flight compaction request.

## `slow_mock_release` (test/test_cli.c)

Lets one held turn send its response.

Requires a prior slow_mock_wait_accepted to have observed the request this release is meant to unblock. Without that ordering the release byte would simply sit in the pipe and be consumed the instant the server accepts, holding nothing -- so this asserts the credit exists instead of allowing that silent no-op.

## `SLOW_MOCK_EXIT_OK` (test/test_cli.c)

Distinct exit codes so a control-channel failure inside the forked server reaches the test as a real failure rather than a warning nobody reads.

## `slow_mock_await_release` (test/test_cli.c)

Waits for exactly one release byte. Fails closed: a timeout, an EOF (the parent closed its end, usually because the test died), a read error, or a short read all mean "do not send this response". Sending it anyway would turn a broken control channel into a passing test.

## `close` (test/test_cli.c)

Ends the child does not own. Without this the child would keep the release *writer* alive, so the parent closing its end would never produce EOF.

Ends the parent does not own, for the same reason.

## `slow_mock_server_expect_clean_exit` (test/test_cli.c)

Reaps a server that should have served every turn and exited on its own. A control-channel failure inside the child surfaces here.

## `slow_mock_server_terminate` (test/test_cli.c)

Reaps a server the test deliberately terminates during cleanup, typically because turns were left unserved. Dying by our own SIGTERM is expected and fine; a clean exit is only fine if it reports SLOW_MOCK_EXIT_OK. Any other outcome -- exiting with a failure code, or dying from a different signal such as SIGSEGV or SIGABRT because the child had already crashed -- is a real failure and must not be waved through just because the process is gone.

## `start_split_response_mock_server` (test/test_cli.c)

A single-turn mock server that writes `response` in two pieces (the first `split_at` bytes, a pause, then the rest) over the same connection -- unlike start_slow_mock_server's one-sleep-then-write-it- all shape, this puts a real mid-stream gap *inside* the SSE body itself, so a client is genuinely still assembling a partial assistant delta (not just waiting for the first byte) when the pause happens. Used to exercise typing concurrently with an in-flight, partially-delivered response.

## `interactive_wait_for.timeout` (test/test_cli.c)

Bounded, not a real deadline: a slower CI runner under asan/tsan can legitimately take longer than a quiet local machine for one round trip. Each read-to-read gap gets its own fresh window, so this only matters when data genuinely stalls.

## `start_interactive_cli_with_api_key` (test/test_cli.c)

Starts a tool-denying interactive CLI against a caller-chosen API key. Parameterized because the /status tests need a distinctive sentinel key to assert its absence from the terminal; the shared "test-key" below is too plausible a substring to prove anything.

A plain fork() inherits the test harness's own session/process group, not one attached to this pty -- a resize test's TIOCSWINSZ-triggered SIGWINCH has nowhere to go without a real controlling terminal established here first.

## `start_interactive_cli_allowing_tools` (test/test_cli.c)

Same as start_interactive_cli, but allows tools to run without prompting -- needed for tests that cancel a genuinely-running tool subprocess, where --deny-tools would never let one start at all.

## `start_interactive_cli_with_session` (test/test_cli.c)

Same as start_interactive_cli, but against an explicit --session ID rather than a fresh automatic session directory -- needed to actually resume the same durable log across two separate process lifetimes (an automatic session's directory is always brand new, so it can never be resumed this way).

## `run_cli_with_stdin` (test/test_cli.c)

Same as run_cli, but with `input` piped in as stdin -- the shape every documented `... | oi` example has, and the only way to exercise the read-all-of-stdin prompt path.

SIGPIPE is possible if the child never reads stdin (a prompt argument was given); ignoring the write result is deliberate, the child's own behavior is what the assertions are about.

## `help_flags_are_documented_in_the_guide` (test/test_cli.c)

Documentation drift guard for the flag list, the counterpart to test_cli_docs' registry check: every long flag `oi --help` prints must appear in docs/CLI.md, so adding a flag fails the suite until the guide lists it. Lives here rather than in test_cli_docs because the usage text belongs to cli.c, which is deliberately not linked into the private-module test archive -- running the real binary is the only way to read it.

The path is repository-relative, like OI_BIN itself.

Read to real EOF rather than into a fixed buffer: a silently truncated guide would make every strstr() below pass for the wrong reason, and a "did it fit" check after the fact cannot tell a file that ended from one that filled the buffer.

Guards against the loop silently matching nothing at all.

## `piped_prompt_carries_stdin_into_the_request` (test/test_cli.c)

Guards the documented piped-input shape, which is easy to get subtly wrong: a prompt argument wins over stdin outright, so `git diff | oi "Review this diff"` sends only the argument and silently drops the diff. README.md and docs/CLI.md therefore show `{ printf 'Review this diff:\n'; git diff; } | oi` instead, and this test asserts both halves of why -- the piped form carries the whole stream into the request, and the argument form does not read stdin at all.

## `api_key_flag_is_removed_from_the_command_line` (test/test_cli.c)

Other users can read /proc/PID/cmdline. When the request reaches the server, the key must be gone from it.

## `default_ask_policy_denies_safely_without_a_controlling_terminal.tool_sse` (test/test_cli.c)

The one-shot loop is only ever reached when stdin/stdout aren't both a tty (cli.c's own `interactive` gate) -- exactly the case this test exercises via run_cli's plain pipe/fork setup, with no --allow-tools/--deny-tools flag at all, so the default policy is `ask`. It must deny promptly rather than hang forever waiting for a decision nothing can ever supply.

## `tool_argument_larger_than_an_arena_block_runs` (test/test_cli.c)

The argument is larger than the 64 KiB default arena block.

## `sigint_cancels_an_in_flight_request_and_returns_to_the_prompt` (test/test_cli.c)

Wait for the "\r\n" oi_cli_composer_wait_submit's cleanup unconditionally emits (render_finish) right before it returns: without this, a SIGINT sent immediately after write_interactive can race the idle-prompt's own poll() loop, which still has signal_fd registered until the read actually returns -- if both input_fd and signal_fd become readable in the same poll() call, the signal branch is checked first and the submission is never even read. Raw mode now spans the whole session (no more per-cycle tcsetattr(TCSAFLUSH)), so once this is seen there is no further race and no further discard risk -- the mock server's 3s delay only needs to outlast the turn's own setup, not this wait.

## `sigint_cancels_a_running_tool_and_returns_to_the_prompt` (test/test_cli.c)

The interactive present layer only ever prints a "running tool" status line, never the tool's own raw stdout -- there is no terminal-visible marker for "the shell command has actually produced output" to wait for. Wait for the status line, then give the forked child a short, generous fixed window to actually start running (fork+exec completing in a few milliseconds, in practice) well before its own 3-second sleep would complete on its own.

## `recoverable_turn_error_returns_to_the_prompt.denied_sse` (test/test_cli.c)

--deny-tools rejects this tool call, producing OI_ERR_DENIED -- a genuine turn failure, but not a durable-storage failure, so the REPL must print a message and return to a working prompt rather than exit.

## `ctrl_d_during_a_turn_has_no_effect` (test/test_cli.c)

"\r\n" (render_finish, emitted right before oi_cli_composer_wait_submit returns) proves the turn has actually started before sending Ctrl+D mid-turn, matching the same reasoning used for SIGINT above.

If Ctrl+D had exited or otherwise broken the busy input loop, this submission would never be observed and the wait below would time out -- that is the observable consequence proving Ctrl+D was processed mid-turn rather than merely written.

The queued message runs immediately once the first turn finishes, with no further input from us -- confirming normal completion and queued-turn behavior, not just that the queue slot was reached.

## `typing_during_a_turn_does_not_corrupt_the_display.reply_sse` (test/test_cli.c)

The response is written in two pieces with a real pause between them (not just a delay before the first byte), so the client is still assembling a partial assistant delta -- with its own composer frame erased/redrawn around every reactor step in between (see cli_repl.c) -- when the second piece and further keystrokes both land in the same window.

## `typing_during_a_turn_does_not_corrupt_the_display.reply` (test/test_cli.c)

On the stack: the server child below holds no heap block of this test, which valgrind's leak check in that child would report.

## `typing_during_a_turn_does_not_corrupt_the_display` (test/test_cli.c)

Split partway through the SSE body (well past the HTTP header and first delta), so the first write already reached the client as a real in-flight, partially-received response.

The typed text is echoed as an uninterrupted run: if the composer's erase/redraw around each reactor step failed to coordinate with the turn's own streamed writes, the two would interleave and this exact substring would not appear intact.

Nothing submitted it (no Enter), so it carries over as the next prompt's draft exactly as typed -- clear it with Ctrl+C before exiting cleanly.

## `typed_ctrl_c_during_a_turn_cancels_it.reply_sse` (test/test_cli.c)

Unlike the existing sigint_cancels_* tests (an external kill(SIGINT) on the process), this sends the raw Ctrl+C byte (0x03) through the pty as a real keystroke, exercising the composer's own decoded- CTRL_C path in handle_turn_input rather than the signalfd path.

## `busy_submit_is_queued_and_a_second_one_is_refused` (test/test_cli.c)

/help and /status are read-only: they dispatch immediately regardless of the occupied slot, and don't themselves get queued. Checked before the refused submission below, since a refusal deliberately leaves its draft uncommitted -- typing more after it would land in the same still-open draft rather than starting fresh.

Second submission while the slot is still occupied: refused, not silently dropped and not overwriting the first.

"another" is still sitting in the draft (the refusal above left it uncommitted, and busy/idle share the same editor state) -- clear it before Ctrl+D, which otherwise deletes forward instead of exiting when the draft isn't empty. The turn has already finished by this point (recovered arrived), so Ctrl+C here only clears the draft, it has no turn left to cancel.

The accepted message ("world") was durably persisted as a QUEUED_INPUT record; the refused one ("another") never reached persist_queued_input at all.

oi_sesslog's own file format is a 12-byte binary header ("OISESLOG" + a little-endian u32 version) followed by a sequence of [4-byte little-endian length][4-byte CRC-32] [JSON bytes] records -- both the header and each record's own head prefix contain embedded NUL/non-ASCII bytes, so treating the whole file as one C string (strstr from the start) stops almost immediately. Walk the length-prefixed records individually instead, each of which *is* a plain, NUL-free JSON string safe to strstr on its own.

## `oilog_records` (test/test_cli.c)

Loads oi_sesslog's length-prefixed records (see the file-format comment in busy_submit_is_queued_and_a_second_one_is_refused above) as an array of NUL-terminated JSON strings, one per record -- for tests that need to check more than one record's presence, or the relative order/turn_ids between several of them.

## `oilog_find` (test/test_cli.c)

Finds the first record at or after start_index containing every needle; returns its index, or SIZE_MAX if none match.

## `oilog_record_turn_id` (test/test_cli.c)

Every history record's turn_id is written as a JSON string, e.g. "turn_id":"2" -- extracts its numeric value.

## `oilog_count` (test/test_cli.c)

Counts every record containing all of the given needles -- used where oilog_find's "first match" isn't enough, e.g. asserting a crash-recovery pass ran exactly once, not zero or twice.

## `queued_message_resumes_at_the_safe_boundary_with_correct_turn_ids` (test/test_cli.c)

The first turn's 2s delay keeps it reliably busy long enough to submit "world" while it's still in flight; the second turn (no delay) is the queued message's own resumed turn, run automatically once the first reaches its safe boundary.

The core correctness property: the queued item, the record that resolves it, and the turn it becomes must all share one turn_id, distinct from (and later than) the first turn's.

## `queued_command_while_busy_resolves_discarded_and_dispatches_live` (test/test_cli.c)

/model is not read-only (unlike /help and /status): it queues like any plain message while busy, per the schema-forced rule that a queued command can only ever resolve DISCARDED, then dispatch live.

Once the turn reaches its safe boundary, the queued command dispatches live -- its own confirmation text proves this ran through dispatch_model, not as a new conversation turn.

## `ctrl_c_with_a_pending_item_discards_it_and_restores_the_draft` (test/test_cli.c)

The composer only redraws once per reactor step, after any decoded SUBMIT has already been handled -- since handle_busy_submit commits and clears the draft before that redraw fires, "world" is never actually echoed while being typed here (the whole "world\r" is decoded and submitted within a single step). It only appears once it's restored below.

Ctrl+C cancels the turn AND discards the queued item -- restoring it as a live, editable draft rather than either silently running it or dropping it.

"world" is echoed here for the only time in this whole session, as the restored draft is redrawn -- proving it came back rather than being dropped.

The restored draft is a normal, still-open line: clear it (Ctrl+C is safe now, no turn left to cancel) before Ctrl+D, which otherwise deletes forward instead of exiting when the draft isn't empty.

The discarded item must never actually run as a real turn -- no matching user message record should exist anywhere in the log.

## `crash_with_a_pending_item_restores_it_as_a_startup_draft` (test/test_cli.c)

First "life": queue something, then simulate a hard crash (SIGKILL, no graceful shutdown at all) while it's still sitting unresolved.

Second "life": same session, fresh process -- the crash-recovery window must be closed durably and the queued text handed back as a plain, editable startup draft, never auto-run. No mock server is needed: nothing here ever submits a message.

Clear the restored draft (Ctrl+C is safe here -- no turn is active) before Ctrl+D, which otherwise deletes forward rather than exiting when the draft isn't empty.

Third "life": restart again immediately. The crash window is already closed (has_pending_input is now false), so this must not seed a draft again or append a second resolution record -- no duplication.

## `snprintf` (test/test_cli.c)

Two JSON layers: the SSE string holds the arguments JSON, whose command holds the A escape.

## `permission_ask_shows_the_full_decoded_command_and_enter_denies` (test/test_cli.c)

Only the approval request shows it: the command never ran.

## `permission_ask_allow_for_process_elevates_policy` (test/test_cli.c)

A second tool call in a later turn must not prompt again -- the process-wide policy is now allow.

## `permission_ask_ctrl_c_while_awaiting_denies_without_hanging` (test/test_cli.c)

Dismissing the selector via Ctrl+C must resolve as a deny, not hang waiting for a decision that will never come.

## `permission_ask_resize_redraws_the_selector` (test/test_cli.c)

A fresh "\x1b[J" appearing with no further keystroke proves the redraw was driven by the resize signal, not by typing -- same technique as the composer-level resize test.

## `permission_ask_typing_does_not_leak_into_the_draft` (test/test_cli.c)

Plain letters are not digit shortcuts -- they must have no visible effect on the selector and must never reach the editor.

Confirms at the default selection ("Deny"): if any of "xyz" had leaked into the editor instead, this "\r" would submit garbage as a new message rather than confirming the selector.

Back at an idle prompt: if "xyz" had leaked into the draft, Ctrl+D would delete forward instead of exiting (same technique used elsewhere in this file to prove a draft is empty).

## `permissions_allow_requires_confirmation_and_can_be_cancelled` (test/test_cli.c)

Policy is genuinely unchanged -- still `ask`, not silently elevated.

## `start_renewal_mock` (test/test_cli.c)

Serves each connection in its own process: the token endpoint answers with a new token after `token_delay_ms`; /v1/responses answers after 300 ms with "ok". Each request appends "<path> <Authorization value>" to `log_path`.

## `token_renewal_runs_beside_the_turn_and_the_next_run_uses_it.credentials_len` (test/test_cli.c)

Inside the 300 s renewal margin, but still usable for this turn.

## `token_renewal_runs_beside_the_turn_and_the_next_run_uses_it` (test/test_cli.c)

Run 1 renewed beside its turn, which kept the old token; run 2 read the renewed token from the file and did not renew again.

## `sent_renewal_survives_the_turn_end_and_finishes_while_idle` (test/test_cli.c)

The token server rotates the refresh token when it gets the request, and answers only after the turn has ended. The renewal must survive the turn end and save the rotated token, or the next renewal would send a refresh token that the server has already used. It must also finish while the prompt is idle, and so release the credentials lock.

The prompt stays idle. The idle wait runs the reactor, so the renewal finishes on its own: within 3 s after the server answers, the rotated token is saved and the lock is free.

## `ctrl_c_ends_the_renewal_wait` (test/test_cli.c)

A sent token renewal waits 10 s for its answer, at the exit or at /login. The wait is reported, Ctrl+C ends it in under 1 s, and the credentials file keeps the tokens it had.

## `ctrl_c_cancels_a_turn_that_waits_for_a_token_renewal` (test/test_cli.c)

A token renewal waits for a server that accepts and never answers. The turn that needs the renewal must still cancel at once on Ctrl+C.

The renewal itself can take 20 s to time out.

## `ctrl_c_cancels_a_turn_that_waits_for_a_token_renewal.listen_fd` (test/test_cli.c)

The listener never calls accept(): the kernel completes the TCP handshake from its backlog, and no byte ever comes back.

## `setenv` (test/test_cli.c)

The loopback test base also stops oi from opening a browser.

## `oauth_callback_with_a_wrong_state_does_not_end_the_login` (test/test_cli.c)

A client that sends nothing must not stop the keyboard: Esc during its read cancels the login.

A second attempt gets a new state; the first one is now wrong.

## `permissions_allow_confirmed_elevates_policy` (test/test_cli.c)

A subsequent tool call must run immediately -- no selector, no further prompt.

## `tool_panel_survives_malicious_escape_bytes_in_output` (test/test_cli.c)

The tool prints raw CSI-colored "red" -- sanitized down to plain text by the panel; the process must not hang or crash regardless.

## `tool_panel_coexists_with_resize_and_queued_input` (test/test_cli.c)

Resize while the tool panel is showing: must redraw cleanly, not crash or corrupt the display.

The tool finishing and steering ending the turn both happen within the same reactor step, so the turn loop's own "don't redraw once present.done is true" rule (needed to avoid corrupting a final streamed reply) means the panel's "completed" frame is never drawn here -- only its earlier "running" state was. The queued message resumes automatically at the safe boundary regardless.

## `sigterm_during_a_turn_terminates_cleanly` (test/test_cli.c)

Wait for "\r\n" (render_finish) before signalling, matching the SIGINT tests above: it proves oi_cli_composer_wait_submit has already returned and cli_repl.c's own turn setup (including registering signal_fd on the reactor) has already run, so the signal is guaranteed to be handled by the turn-time reactor callback rather than raced against the idle-prompt's own signal draining.

## `interactive_cwd_command_changes_the_process_directory` (test/test_cli.c)

/cwd and /status never submit a model message, so this server is never actually queried -- a harmless placeholder response.

The /cwd confirmation above already printed this exact text once; /status's own CWD line is the second occurrence. Waiting for count 1 here would be satisfied instantly without draining the PTY, leaving /status's real response unread and deadlocking the child once its output fills the PTY buffer.

No session was ever created: /cwd alone doesn't submit a model message, matching interactive_exit_before_submission_creates_no_session.

## `status_sentinel_api_key` (test/test_cli.c)

A distinctive sentinel, so "the key never reached the terminal" is a real assertion rather than a coincidence about a plausible substring.

## `status_reports_every_field_before_a_session_exists` (test/test_cli.c)

The pre-session report: every contracted field is present, the endpoint is the one actually configured, and the API key is nowhere in the output.

A process-scoped permission change is reflected immediately: the snapshot reads the live policy rather than one captured at startup. "ask" needs no confirmation, unlike an elevation to "allow". Waiting for the second report's last line drains it completely, so the count below sees both /permissions' own echo and the report's line.

## `status_reports_streaming_and_queue_state_during_a_turn` (test/test_cli.c)

The mid-turn report, which is the whole point of /status being read-only: it must answer while the model is streaming, must see a queued message, and must not itself take the single pending slot.

Waits for the report's *last* line before inspecting earlier ones. A needle in the middle only proves the bytes up to it arrived, so asserting on a later field right after it is a race that a slower build (a sanitizer run, a loaded machine) genuinely loses.

Queue a message, then ask again: the queue line changes, the slot is still held by "world" rather than by /status, and the turn reports that it is steering to a safe boundary. Waited for as one whole line, for the same reason.

## `status_reports_a_running_tool_then_returns_to_idle_after_a_cancel` (test/test_cli.c)

A tool is a distinct in-flight kind, and the report has to name it rather than collapse it into "streaming". Cancelling it then returns the report to idle: a user cancel is a fully-recovered outcome, so calling the prompt "failed" afterwards -- which the cancel's own OI_ERR_CLOSED would suggest -- would describe a working REPL as broken.

"running tool shell" is written from the synchronous TOOL_STARTING callback, and the REPL installs conversation->tool the moment that callback returns -- both inside one reactor step, before any terminal input can be read again. So seeing that line is already the barrier: no sleep is needed to know the tool is installed by the time /status below is processed.

Cancel the running tool, then ask again. The report above already proved the tool is installed, so this cancel needs no timing window of its own either.

## `oilog_record_u64_field` (test/test_cli.c)

Every history record's record_id (and the checkpoint-specific source_first_record_id/source_last_record_id) are written as JSON strings, e.g. "record_id":"3" -- extracts one's numeric value.

## `last_request_text` (test/test_cli.c)

Isolates the last captured HTTP request in a multi-turn capture file (start_mock_server_turns_capture appends every turn's raw request bytes back-to-back with no delimiter) by finding the last occurrence of the request line every chat-completion request starts with. `buffer` must outlive the returned pointer, which points into it.

## `compact_replaces_older_turns_with_a_checkpoint` (test/test_cli.c)

The report tracks the compaction: a durable checkpoint with its own source range, and an active context that now starts from a summary. The exact range is cross-checked against the log below.

/status reported that same range, read back off the replay state rather than recomputed -- so the report and the durable record cannot disagree.

## `compact_typed_while_a_turn_is_active_queues_and_runs_next` (test/test_cli.c)

The only real turn is the slow one, so this races the queue against the very first submission in the whole test -- avoiding any ambiguity from an earlier "\r\n" already sitting in the buffer (see queued_command_while_busy_resolves_discarded_and_dispatches_live's identical "hello\r" / wait-for-"\r\n" / queue-behind-it shape, which relies on the same "first ever submission" property).

Queues /compact while the only turn so far is still in flight -- once it completes there's exactly 1 turn, so /compact 0 (compact everything) is what actually has something to do.

## `compact_survives_restart_and_replay_shows_the_checkpoint` (test/test_cli.c)

Second "life": restart against the same session, then send one more message -- its outgoing request is the proof replay reconstructed the checkpoint correctly, not just that the live process remembered it.

## `resize_redraws_the_live_prompt_at_the_new_width` (test/test_cli.c)

Never submits a prompt, so the mock-server port is never actually connected to -- matching interactive_exit_before_submission_creates_ no_session's use of an arbitrary unused port below.

Resize mid-edit, without any further keystroke: a fresh "\x1b[J" clear sequence appearing proves the redraw was driven by the SIGWINCH/signalfd path added in cli_repl.c, not by continued typing.

## `termios_is_cooked` (test/test_cli.c)

The REPL puts the pty into raw mode and enables bracketed paste when it starts; every exit path must undo both before the process goes away, or a shell resuming on that terminal is left with echo and line editing off. test_cli_terminal.c proves the terminal module restores a single struct; this proves the real process reaches that restore on each exit path and emits the matching bracketed-paste disable, by keeping the parent's own slave descriptor open and inspecting the shared tty state after the child has gone. Raw mode clears ICANON|ECHO|ISIG (cli_terminal.c), so their presence after exit is the cooked-mode signal.

## `exit_path_restores_terminal` (test/test_cli.c)

Drain what the child flushed before exiting: the disable sequence was written before the process ended, so it is already in the master's buffer. The parent's open slave descriptor means the master never reaches EOF, so read nonblocking until EAGAIN rather than block.

## `repl_restores_terminal_state_on_every_exit_path` (test/test_cli.c)

Keep slave_fd: it is the parent's window onto the tty state once the child's descriptor is gone, and holds the pty alive so the drain above sees buffered output instead of EOF.

## `session_ids` (test/test_cli.c)

Returns the ids of every session directory under `root`, sorted, so a test can name a session created by an earlier process run. An automatic session always gets a fresh timestamped directory, so running `oi` twice is how a second switchable session comes to exist.

## `run_one_interactive_turn` (test/test_cli.c)

Runs one interactive session that sends `message`, waits for `reply_text` in the transcript, and exits cleanly. Used to leave a real session with real content behind for a later run to switch into.

## `interactive_session_list_and_current_describe_the_active_session` (test/test_cli.c)

An automatic session is created lazily on the first message, so before one is sent this process genuinely has no session -- worth asserting, since /session current has to say so rather than inventing one.

## `collect_session_ids` (test/test_cli.c)

Both sessions exist now. Ids embed a timestamp and counter, so the later one is this process's own -- which is the row that must carry the current marker.

## `run_one_interactive_tool_turn` (test/test_cli.c)

Session A, with a real tool-using exchange in it, from an earlier process. This also makes the visible replay prove that it does not collapse model-visible tool details into a mere count.

## `slow_mock_client_request` (test/test_cli.c)

Opens a connection to the mock server and sends a minimal request, so a test can exercise the hold/release handshake without a CLI in the way.

## `mock_control_release_before_acceptance_is_rejected` (test/test_cli.c)

Complete an unheld request first. It must not create a release credit: only requests whose responses are actually blocked can authorize a slow_mock_release call.

Prove the rejected call did not merely report a CHECK and then leak a release byte anyway: when a real request now arrives, its response must remain held until the parent observes acceptance and explicitly releases it.

## `mock_control_closing_the_release_end_fails_the_server_promptly` (test/test_cli.c)

A vanished parent must be noticed, not waited out. Closing the release end gives the server EOF rather than silence, so it fails closed immediately instead of burning the full 20 s timeout and then sending the response anyway.

Fail closed: the held response was never sent. The socket does become readable, because the server closed it -- so the check is that reading yields EOF rather than response bytes.
