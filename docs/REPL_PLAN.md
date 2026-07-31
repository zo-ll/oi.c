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
  empty prompt, and `/exit` waits for a safe boundary; `Ctrl+D` sent while a
  turn is active has no effect (the terminal stays in cooked mode for the
  whole turn and nothing reads input until the next prompt, whose
  raw-mode `tcsetattr` discards it before it is ever read, same as any other
  byte typed mid-turn). `SIGTERM`/`SIGHUP` cancel an active turn (or, if none
  is active, the idle prompt) and terminate the process cleanly.
- Recoverable request, protocol, timeout, and tool errors return to the prompt.
  Durable-storage or structural failures mark the session failed.

## Commands

- `/help`: show commands and key bindings.
- `/exit`: request a graceful exit.
- `/session`: manage the session catalog. `switch`, `delete`, and `import` are
  handled by `cli_repl` rather than `cli_command_dispatch`, since they need
  the live conversation or the composer's confirmation flow; the rest are
  ordinary dispatch subcommands. The grammar is:
  - `/session` or `/session list`: list selectable sessions, most recently
    updated first. Marks the active one, shows a display name when set, and
    labels both sessions open in another process and sessions whose selector
    metadata had to be rebuilt — a rebuilt row must not read as though its
    cache had been authoritative.
  - `/session trash-list`: the same listing for trashed sessions.
  - `/session current`: show the active session's ID, path, and whether its
    selector metadata is healthy. Health is resolved live on every call rather
    than cached from startup, since metadata can be removed or corrupted by
    something outside `oi` at any point in a long-running session.
  - `/session switch ID`: switch only at an idle safe boundary. Replays and
    repairs the target, restores its model and working directory, rebuilds
    input history from its user messages, and then offers a full visible
    replay. A failed switch leaves the original session active and usable.
  - `/session rename ID NAME`: set a display name. Never moves the directory:
    the safe ID is permanent, because every derived path depends on it. `NAME`
    may contain spaces, is bounded, and may not contain control bytes, since
    it is echoed to the terminal unescaped.
  - `/session trash ID`: move a session into the recoverable trash. Refuses
    the active session and any session open in another process.
  - `/session restore ID`: move a trashed session back, refusing to overwrite
    a live session of the same ID.
  - `/session delete ID`: permanently delete an already-trashed session, after
    an explicit confirmation. There is deliberately no one-step delete of a
    live session — that is what makes refusing the active session, and one
    open elsewhere, structural rather than merely checked.
  - `/session import PATH`: after confirmation, validate a legacy `.oilog` and
    copy it into a new private session. The source is never modified.

  Enumeration accepts only bounded, portable IDs (`[A-Za-z0-9_-]`, 1-128
  bytes) that are plain directories. Excluding `.` and `/` from that charset
  is what rejects traversal attempts, dotfiles, the `.trash` subdirectory, and
  planted symlinks under one rule, so no separate traversal check exists.
  Listing a healthy session never opens its log; only a session whose cache is
  missing or malformed is replayed, and then only its own log.
- `/model`: edit the durable session model.
- `/permissions`: inspect or change the process-scoped `ask`, `allow`, or `deny`
  policy; elevation to `allow` requires confirmation.
- `/status`: report the whole runtime in one deterministic, secret-free
  block: durable session state (not created, ephemeral, active id, or
  durable-storage failure), the active model and where it came from
  (startup default, `--model` override, replayed history, metadata cache,
  or `/model`), the endpoint as host/port/path plus TLS on/off, the
  permission policy, the request and tool timeouts, the effective working
  directory, what the conversation is doing (idle, model streaming,
  awaiting tool permission, tool running, cancelling, working, or failed
  with its cause), the pending queue as a kind and a byte count, and the
  latest durable checkpoint's source range plus whether active context is
  compacted.

  Assembled as one typed borrowed snapshot (`cli_status`) rather than read
  field by field: the facts belong to four different owners, and a report
  stitched from several sources can contradict itself with no way to tell
  which half was right. `cli_command_dispatch` renders that snapshot and
  reaches into no private conversation or history struct to do it. Every
  string in the snapshot is borrowed and valid only for the one dispatch
  that reads it. Secrets are structurally absent — there is no field for an
  API key, an authorization header, a CA file, or a request body, so no
  assembler can pass one in.

  Every field has an explicit unknown, established by
  `oi_cli_status_snapshot_init` rather than by zeroing: zero would otherwise
  claim a pile of real states (no session, `ask`, disabled deadlines, idle,
  empty queue) that no assembler ever asserted. A callback that fills in
  nothing therefore produces a report that claims nothing.

  The borrowed strings are untrusted — a model name can come from a
  tampered log, a working directory can hold any byte a filesystem accepts,
  a host or session id is whatever the command line said — so the renderer
  repairs each one to well-formed UTF-8, runs it through the shared
  `cli_render_sanitize` escape/control stripper, and then flattens the two
  control bytes that pass deliberately preserves (`\n` and `\t`) into
  U+FFFD. That last step is what makes "one line per field" structural: no
  value can forge an extra status line, shift itself into another column, or
  emit CSI/OSC (including OSC 52 clipboard writes). Ordinary UTF-8 survives
  unchanged, and each field is bounded so no single value can produce an
  unbounded line.
- `/compact [N]`: summarize the completed turns older than the most recent `N`
  (default 8) into one durable checkpoint. Only runs at an idle safe boundary
  (the same one queued commands drain into); the summarization request is a
  dedicated, bounded request that treats the prior turns as data to
  summarize, never as instructions, so a prior adversarial tool result or
  model response can't hijack it. The durable checkpoint append must fully
  succeed before the live conversation is spliced to match it, so a failed or
  `Ctrl+C`/`SIGINT`-cancelled request leaves both completely untouched.
- `/cwd`: change and persist the session working directory.

`//text` sends a literal model message beginning with `/`. While a turn is
active, `/help` and `/status` may run immediately; state-changing commands
occupy the single pending slot and wait until idle.

## Private module boundaries

- `cli_repl`: owns interactive lifecycle, state transitions, signals (blocks
  `SIGWINCH`/`SIGINT`/`SIGTERM`/`SIGHUP` and owns the resulting `signalfd`'s
  lifecycle -- threaded into `oi_cli_prompt_read` while idle-prompting, and
  registered directly on the reactor for the duration of an active turn, so
  either polling context sees the same pending signal with no delivery gap
  at the handoff between them; degrades gracefully to no live-resize/signal
  support if setup fails, never a hard REPL-start failure). `SIGINT` during a
  turn cancels it and returns to a fresh prompt; `SIGTERM`/`SIGHUP` (idle or
  mid-turn) cancel any active turn and terminate the process cleanly.
  Command dispatch and the single pending item are owned here too.
- `cli_editor`: owns terminal modes, input buffers, cursor movement, multiline
  layout, input-history navigation, bracketed paste, and command selection.
- `cli_render`: owns control-byte sanitization, incremental Markdown state,
  terminal styling, and clear/redraw coordination, including resizing: a
  pending `SIGWINCH` re-reads `TIOCGWINSZ` and redraws the current editor/menu
  frame in place, preserving the edit buffer, cursor, and command-menu
  selection. Now that concurrent composition has landed, the editor frame and
  a turn's streamed output do share the screen, so redraw is bracketed rather
  than merely sequential: `cli_repl`'s turn loop erases the composer frame
  before every reactor step and redraws it only on a step that actually
  touched the composer (decoded input, or a pending resize), which is what
  keeps the editor's raw ANSI frame out of the middle of a multi-chunk reply
  streaming through `cli_present`/`cli_render_stream`. Resize therefore
  arrives from two polling contexts that both read the same `signalfd`: the
  idle prompt's private `poll()` in `cli_composer`, and — for the duration of
  a turn — a plain `oi_reactor_add` registration in `cli_repl`.
- `cli_conversation`: owns active messages, model/tool step sequencing,
  safe-boundary steering, cancellation, and recoverable turn failures. Any
  turn that ends before a normal, fully-resolved completion (cancellation,
  tool denial, a mid-loop failure) is repaired in place before the next
  turn starts: an `[assistant turn interrupted before completion]`
  placeholder closes out the missing assistant reply, and dangling tool
  calls each get an `OUTCOME_UNKNOWN` (may have already run) or
  `NOT_EXECUTED` (never started) placeholder result -- the same shape
  `cli_history`'s own crash-time replay repair produces, just applied live
  instead of at the next process start.
- `cli_history`: owns JSON record envelopes, stable record IDs, legacy replay,
  repair records, active-context reconstruction, partial responses, and
  checkpoints.
- `cli_sessions`: owns platform state paths, private per-session directories,
  atomic metadata caches, working directories, import, rename, selection, and
  trash. Also owns safe-ID validation and every filesystem policy decision
  behind the `/session` lifecycle: enumeration, the `.trash` convention,
  permanent deletion, and the read-only lock probe that reports a session as
  busy. It never enumerates on behalf of replay, and the replay/repair modules
  never enumerate directories.
- `cli_tools`: continues to own built-in tool definitions and permission
  decisions, using REPL-owned presentation callbacks instead of direct
  `/dev/tty` reads.
- `cli_status`: owns the `/status` snapshot type, its unknown-establishing
  initializer, and its renderer, and nothing else. It reads no live state
  itself — each owning module fills in its own fields — which is what keeps
  the report a pure function of the snapshot and so identical on a TTY and
  in a redirected stream. It composes `cli_utf8_stream` and
  `cli_render_sanitize` for untrusted values rather than carrying an
  escaping scheme of its own; the only thing it adds is line flattening,
  which those two deliberately do not do.

No module other than `cli_editor` and `cli_render` emits terminal-control
sequences. No terminal module interprets conversation records or drives model
requests.

## Storage model

Each durable session is a private directory containing:

- `history.oilog`: authoritative append-only records;
- `metadata.json`: atomically replaced, rebuildable selector metadata;
- `metadata.json.lock`: empty advisory-lock file serializing read-modify-write
  of the cache, so a rename and a concurrent model/CWD refresh cannot discard
  each other's field. A sibling file rather than the cache itself, because the
  cache is replaced via temp+`rename()` and so has no stable inode to lock;
  the session log's own lock cannot be reused, since the owning process holds
  it for its whole lifetime;
- future derived indexes, which are never authoritative.

Linux uses `$XDG_STATE_HOME/oi/sessions` or `~/.local/state/oi/sessions`.
Existing project-local `.oilog` files are imported only after selection
and confirmation; import validates a copy by replaying it before adopting it,
and never modifies the source.

Trashed sessions live in a `.trash` subdirectory of the sessions root, moved
there whole by a single `rename(2)`. Because the trash sits inside the same
root, that rename is same-filesystem and therefore atomic — no partially
trashed state can exist — and a cross-device failure is reported distinctly
rather than as a generic I/O error. The `.trash` name is itself excluded by
the safe-ID rule, so a trashed session simply is not visible to enumeration
and needs no filtering.

Selector metadata is schema-versioned. Version 2 adds an optional
`display_name`; version 1 files still decode, and are upgraded on the next
write rather than by any migration pass. The decoder checks an exact field
count per version, so an unknown or misspelled key remains a parse error —
and a parse error is never fatal, since the cache rebuilds from history.
A display name lives only in the cache and is never recorded in history, so
every path that refreshes the cache must carry the existing name forward.

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

Cursor/column math uses a documented, testable subset of the Unicode
combining-mark and East Asian Width ranges (`oi_cli_utf8_codepoint_width`):
combining marks and C0 controls are zero-width, a pared-down set of common
CJK/Hangul/fullwidth ranges is width 2, everything else is width 1. This is
not a full UAX#11 implementation and is not grapheme-cluster aware (ZWJ
sequences, emoji modifiers) — consistent with "full grapheme editing" being
out of scope above. A wide code point is treated as atomic for wrapping: it
is never split across a row boundary, wrapping whole to a fresh row instead.

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
