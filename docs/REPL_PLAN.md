# REPL implementation plan

Date: 2026-07-28

## Outcome

Add a polished interactive chat mode to `oi` while preserving script-friendly
one-shot behavior. Bare `oi` on a supported stdin/stdout TTY creates a new
timestamped session; prompt arguments and piped input remain one-shot and are
ephemeral unless `--session` is explicit.

The implementation stays private to the CLI. The public library continues to
provide generic reactor, LLM, tool, session, and durable-record primitives
without learning terminal UI or conversation-record semantics.

## Product contract

- A session restores durable model context, including user messages, assistant
  messages, tool calls, tool results, repairs, and context checkpoints.
- Completed messages are appended as versioned JSON envelopes. Legacy raw
  user/assistant records remain readable across an explicit schema-transition
  marker.
- Interrupted turns are repaired into protocol-valid context. Partial assistant
  output remains available for audit and full replay but is excluded from active
  context.
- The editor supports UTF-8 code-point movement, session-derived input history,
  Enter to submit, `Ctrl+J` for a newline, bracketed multiline paste, and a
  filterable slash-command menu.
- Assistant output is sanitized and rendered as incremental Markdown. Tool
  invocations and bounded output stream live with clear status delimiters.
- One user message may be queued while a turn is active. Model streaming and a
  currently running tool reach a safe boundary; newly requested tools do not
  start ahead of queued input.
- `Ctrl+C` cancels an active turn or clears edited input. `Ctrl+D` exits from an
  empty prompt, and `/exit` waits for a safe boundary.
- Recoverable request, protocol, timeout, and tool errors return to the prompt.
  Durable-storage or structural failures mark the session failed.

## Commands

- `/help`: show commands and key bindings.
- `/exit`: request a graceful exit.
- `/session`: select, switch with optional full replay, rename, trash, restore,
  or permanently delete sessions.
- `/model`: edit the durable session model.
- `/permissions`: inspect or change the process-scoped `ask`, `allow`, or `deny`
  policy; elevation to `allow` requires confirmation.
- `/status`: show session ID, model, endpoint, permission policy, timeout,
  working directory, queue state, and checkpoint state without secrets.
- `/compact [N]`: summarize older active context while retaining eight recent
  completed turns by default.
- `/cwd`: change and persist the session working directory.

`//text` sends a literal model message beginning with `/`. While a turn is
active, `/help` and `/status` may run immediately; state-changing commands
occupy the single pending slot and wait until idle.

## Private module boundaries

- `cli_repl`: owns interactive lifecycle, state transitions, signals, command
  dispatch, and the single pending item.
- `cli_editor`: owns terminal modes, input buffers, cursor movement, multiline
  layout, input-history navigation, bracketed paste, and command selection.
- `cli_render`: owns control-byte sanitization, incremental Markdown state,
  terminal styling, and clear/redraw coordination.
- `cli_conversation`: owns active messages, model/tool step sequencing,
  safe-boundary steering, cancellation, and recoverable turn failures.
- `cli_history`: owns JSON record envelopes, stable record IDs, legacy replay,
  repair records, active-context reconstruction, partial responses, and
  checkpoints.
- `cli_sessions`: owns platform state paths, private per-session directories,
  atomic metadata caches, working directories, import, rename, selection, and
  trash.
- `cli_tools`: continues to own built-in tool definitions and permission
  decisions, using REPL-owned presentation callbacks instead of direct
  `/dev/tty` reads.

No module other than `cli_editor` and `cli_render` emits terminal-control
sequences. No terminal module interprets conversation records or drives model
requests.

## Storage model

Each durable session is a private directory containing:

- `history.oilog`: authoritative append-only records;
- `metadata.json`: atomically replaced, rebuildable selector metadata;
- future derived indexes, which are never authoritative.

Linux uses `$XDG_STATE_HOME/oi/sessions` or `~/.local/state/oi/sessions`.
Existing project-local `.oilog` files are imported only after selection
and confirmation.

A private per-session directory holds exactly one session, so its metadata
cache is always the literal `metadata.json`. The flat `--session-dir` layout
can hold multiple `<id>.oilog` files in one directory, so there the cache is
named `<id>.metadata.json` instead, derived by replacing the `.oilog` suffix.
Either way the cache is a rebuildable, atomically-replaced (temp file +
`rename()`) snapshot: `history.oilog` remains authoritative, and a missing or
corrupt cache silently rebuilds from replayed history on the next open, with
a diagnostic printed to the session's error stream.

New interactive sessions are created lazily on the first submitted message.
Session IDs are bounded portable ASCII filename stems. Session working
directories and models are durable; explicit `--model` overrides and updates
the stored model. A brand-new session durably records its initial effective
model and working directory as soon as it is created, even if `/model` and
`/cwd` are never used, so a lost metadata cache can always be rebuilt exactly
rather than falling back to the current run's defaults. `/model` and `/cwd`
apply live and persist immediately; other runtime settings remain
process-scoped.

Records receive stable monotonic IDs. Checkpoints record their exact source
range so a future vector index can be rebuilt without changing session files.
Semantic retrieval, encryption, automatic compaction, full grapheme editing,
and non-ANSI terminal fallback are outside this milestone.

## Delivery sequence

Each step should be a focused commit with its own tests and a green ordinary
suite before moving on.

1. Define private conversation record types and a versioned JSON codec.
2. Add legacy transition, typed replay, active-context reconstruction, and
   interrupted-turn repair tests.
3. Add per-session directory storage, atomic metadata, platform paths, safe IDs,
   working-directory restoration, import, and trash operations.
4. Refactor the current one-turn CLI loop into a reusable private stateful
   conversation object; route one-shot mode through it without changing stdout
   behavior.
5. Add the terminal editor core with pure buffer/cursor tests, then POSIX raw
   mode and PTY integration tests on Linux.
6. Add sanitization, incremental Markdown, and redraw coordination with chunk
   boundary, malformed UTF-8, resize, and control-sequence tests.
7. Add the REPL controller, startup mode detection, prompt lifecycle, signals,
   input history, and lazy session creation.
8. Add the command registry and menu, then implement `/status`, `/model`,
   `/permissions`, `/cwd`, `/session`, and `/exit`.
9. Add concurrent composition, the single pending item, safe-boundary steering,
   permission selectors, and live tool rendering.
10. Add `/compact [N]`, source-range checkpoints, recovery, and failure-atomic
    persistence.
11. Update help, README, configuration documentation, and installation examples.

## Verification gates

- Strict GCC and Clang builds with the existing warning policy.
- Unit tests for codecs, state machines, UTF-8 editing, command filtering,
  Markdown chunks, sanitization, paths, metadata recovery, and compaction.
- PTY tests for raw-mode restoration, multiline editing, resize, concurrent
  redraw, command selection, `Ctrl+C`, `Ctrl+D`, and redirected streams.
- Mock-server integration tests for multi-turn replay, tool history, steering,
  cancellation, queued-input recovery, session switching, and one-shot stdout.
- Existing ABI export check remains unchanged because the feature is CLI-private.
- ASan, UBSan, TSan, Valgrind, and fuzzers pass before the milestone is
  considered complete.
