# oi command-line guide

A task-oriented reference for installing and using `oi`: one-shot prompts,
the interactive mode, every slash command, where sessions are stored, and
what to do when something goes wrong.

This document describes what the current build actually does. Behavior that
is planned but not implemented is listed under
[Not supported yet](#not-supported-yet) rather than described as working.

- [Install](#install)
- [API key and configuration](#api-key-and-configuration)
- [Startup modes](#startup-modes)
- [Interactive mode](#interactive-mode)
- [Key bindings](#key-bindings)
- [Slash commands](#slash-commands)
- [Sessions and storage](#sessions-and-storage)
- [Tools and security](#tools-and-security)
- [Compaction](#compaction)
- [Troubleshooting](#troubleshooting)
- [Developer verification](#developer-verification)
- [Not supported yet](#not-supported-yet)

## Install

### Prerequisites

- A C11 toolchain (GCC or Clang) and GNU Make.
- OpenSSL development headers (`libssl`/`libcrypto`), located with
  `pkg-config` when available and otherwise linked as `-lssl -lcrypto`.
- Linux. The event loop uses `epoll`, `timerfd`, `signalfd`, and `pidfd`
  (`pidfd_open` needs Linux 5.3 or newer). POSIX threads are linked in.

On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libssl-dev
```

On Fedora:

```sh
sudo dnf install gcc make pkgconf-pkg-config openssl-devel
```

macOS is not supported on `main`: an earlier kqueue backend lives on the
`macos-support` branch. Nothing in this guide is verified there. Where a
path is mentioned below, note that the code has no Darwin-specific branch —
it uses the same XDG-style state directory on every platform it builds on,
not `~/Library/Application Support`.

### Build

```sh
make
make check
```

`make` writes `build/oi`, `build/liboi.a`, and `build/liboi.so`. The CLI is
usable straight from the build tree:

```sh
build/oi --help
```

### Install to a prefix

`PREFIX` defaults to `/usr/local`; `BINDIR`, `LIBDIR`, and `INCLUDEDIR`
default to `$PREFIX/bin`, `$PREFIX/lib`, and `$PREFIX/include`. `DESTDIR`
is honored for staged/packaged installs.

```sh
# system-wide
sudo make PREFIX=/usr/local install

# per-user, no sudo (add ~/.local/bin to PATH)
make PREFIX="$HOME/.local" install

# staged into a package root
make DESTDIR=/tmp/package-root PREFIX=/usr install
```

That installs `oi`, `liboi.a`, `liboi.so.0.1.0` with its `liboi.so.0` and
`liboi.so` symlinks, and the public headers into `$INCLUDEDIR/oi`.

If you installed the shared library into a directory the dynamic loader
does not search by default (a `$HOME/.local` prefix, typically), either run
`sudo ldconfig` for a system prefix or set `LD_LIBRARY_PATH` for the
non-standard one. The `oi` executable itself links the static archive, so it
does not depend on `liboi.so` being findable.

### Confirm which `oi` you are running

More than one copy is easy to end up with — a build tree, a `~/.local`
prefix, and a system prefix:

```sh
type -a oi          # every oi on PATH, in order
command -v oi       # the one that would run
hash -r             # forget a stale shell lookup after installing
```

`oi` has no `--version` flag. To be certain a given file is the build you
expect, compare it against the build tree:

```sh
cmp build/oi "$(command -v oi)" && echo "same binary"
```

### Uninstall

```sh
sudo make PREFIX=/usr/local uninstall
```

`uninstall` removes exactly what `install` created — the binary, both
libraries with their symlinks, and the headers, plus `$INCLUDEDIR/oi` itself
if nothing else is left in it — and honors the same `DESTDIR`, `PREFIX`,
`BINDIR`, `LIBDIR`, and `INCLUDEDIR` variables, so pass the same ones you
installed with. It does not touch your sessions; see
[Sessions and storage](#sessions-and-storage) for where those live and
[`/session delete`](#session) for removing them deliberately.

## API key and configuration

Settings are resolved in this order, each layer overriding the one before:

1. built-in defaults;
2. the `key = value` config file named by `--config PATH`, if any;
3. the `OI_API_KEY` environment variable;
4. command-line flags.

### The API key

The key can only come from the environment or the command line. A config
file containing `api_key` is **refused** — the whole file fails to load with
`denied` — so a secret cannot be committed to a config file by accident.

Prefer the environment, loaded from a file only you can read:

```sh
install -d -m 700 "$HOME/.config/oi"                 # private, 0700
install -m 600 /dev/null "$HOME/.config/oi/api-key"  # create it empty, 0600
$EDITOR "$HOME/.config/oi/api-key"                   # paste the key
export OI_API_KEY="$(cat "$HOME/.config/oi/api-key")"
```

`install -d` creates the directory only if it is missing and leaves an
existing one alone, so this works on a fresh account and is safe to repeat.

Avoid typing the key inline. `export OI_API_KEY=sk-...` lands in your shell
history, and `--api-key sk-...` additionally shows up in `ps` output for
every user on the machine for as long as the process runs. `--api-key`
exists for scripts that already hold the secret in a variable, and even then
the environment variable is the safer channel.

`oi` never writes the key to a session file and never prints it: `/status`
has no field for a key, a CA file, or a request body.

Without a key, anything that would make a request fails immediately:

```
oi: no API key set (use OI_API_KEY or --api-key)
```

`--dry-run` needs no key.

### Config file

One `key = value` pair per line. Blank lines and `#` comments are ignored;
whitespace around keys and values is trimmed; a line without `=` is a parse
error that fails the load. Recognized keys, with their defaults:

| Key | Default | Flag |
| --- | --- | --- |
| `host` | `api.openai.com` | `--host` |
| `port` | `443` | `--port` |
| `path` | `/v1/chat/completions` | `--path` |
| `model` | `gpt-4o-mini` | `--model` |
| `use_tls` | `true` | `--tls` / `--no-tls` |
| `ca_file` | system trust store | `--ca-file` |
| `timeout_ms` | `60000` | `--timeout-ms` |
| `api_key` | — | `--api-key`, `OI_API_KEY` |

`use_tls` accepts `true`/`false`, `1`/`0`, `yes`/`no`. `port` must be
1-65535 and `timeout_ms` must be positive.

```sh
install -d -m 700 "$HOME/.config/oi"
cat > "$HOME/.config/oi/config" <<'EOF'
# no api_key here -- it would be refused
host = api.example.com
model = my-model
timeout_ms = 120000
EOF

oi --config "$HOME/.config/oi/config" "Summarize this repository"
```

There is no implicit config-file location: `--config` is the only way a file
is read.

### Other flags

| Flag | Meaning |
| --- | --- |
| `--session ID` | use a durable session log (see below) |
| `--session-dir DIR` | session storage root |
| `--max-turns N` | model steps per turn, 1-1000, default 8 |
| `--allow-tools` | run requested tools without asking |
| `--deny-tools` | refuse every requested tool |
| `--dry-run` | print the resolved request; no network, no key needed |
| `-h`, `--help` | usage summary |

`--timeout-ms` is both the end-to-end request deadline and the tool
execution deadline.

An unrecognized flag, a missing flag value, or a second prompt argument is a
usage error that exits 1 without contacting anything.

## Startup modes

What `oi` does is decided entirely by the arguments and by whether stdin and
stdout are terminals.

| Invocation | Mode | Persistence |
| --- | --- | --- |
| `oi` on a terminal | interactive | new session, created on the first message |
| `oi --session work` on a terminal | interactive | `./work.oilog`, opened at startup |
| `oi "prompt"` | one-shot | none (ephemeral) |
| `oi --session work "prompt"` | one-shot | appends to `./work.oilog` |
| `echo prompt \| oi` | one-shot | none unless `--session` |
| `oi "prompt" > out.txt` | one-shot | none unless `--session` |
| `oi --dry-run "prompt"` | prints the request and exits | none |

The rules behind that table:

- Interactive mode requires **both** stdin and stdout to be a terminal, no
  prompt argument, and no `--dry-run`. Redirect either stream and you get
  one-shot mode instead — which is what makes `oi ... > file` and
  `oi ... | tee` behave like ordinary commands.
- A prompt argument always means one-shot, on a terminal or not. Stdin is
  not read in that case — `git diff | oi "Review this diff"` sends only the
  argument and silently drops the diff. Put the instruction in the piped
  text instead:

  ```sh
  { printf 'Review this diff:\n'; git diff; } | oi
  ```
- With no prompt argument and no terminal, the whole of stdin is read as the
  prompt, with one trailing newline stripped. Empty input is an error:
  `oi: no prompt given (pass one as an argument or pipe it on stdin)`.
- One-shot runs are **ephemeral unless `--session` is given explicitly**.
  Nothing is written, nothing is restored.
- Interactive runs are durable. Without `--session`, a fresh private session
  is created lazily under the sessions root the first time you submit a
  message, so starting `oi` and pressing Ctrl+D leaves nothing behind.
- `--dry-run` never enters interactive mode. With no prompt argument it
  reads stdin, so on a terminal it would wait for EOF — always give it a
  prompt argument or pipe input. It prints the resolved host, port, TLS
  flag, path, model, and request body, then exits 0.

Tool permission differs by mode: a one-shot run never prompts. See
[Tools and security](#tools-and-security).

## Interactive mode

```
$ oi
> Explain the reactor in this repo
```

`> ` is the prompt. Type a message and press Enter to send it; assistant
output streams back as it arrives, rendered as incremental Markdown with
terminal escapes stripped. Tool calls appear in a small live panel with a
status line — `shell: running`, then `completed`, `failed`, `denied`, or
`cancelled` — that stays visible until the next tool call or the end of the
turn.

Anything that begins with `/` is a local command, not a message to the
model. `//text` sends a literal message starting with a single slash.

While a turn is running you can keep typing:

- `/help` and `/status` run immediately.
- Anything else — a message, or any other command including `/exit` — is
  held in a **single** pending slot and printed as
  `oi: queued -- will run once the current turn reaches a safe point`.
  Whatever is already in flight finishes (the current model response, a
  running tool), but no new tool call or model step starts ahead of your
  queued item.
- A second submission while the slot is occupied is refused without losing
  your draft: `oi: a message is already queued; ...`.
- Ctrl+C cancels the turn. A queued item is not silently run afterwards: it
  is discarded and handed back as your editable draft
  (`oi: queued input discarded and restored to your draft`).

Recoverable failures (request errors, protocol errors, timeouts, tool
failures) print `oi: turn failed: <reason>` and return you to the prompt.
Only a durable-storage or structural failure ends the run and marks the
session failed.

`SIGINT` cancels the current turn. `SIGTERM` and `SIGHUP` cancel any active
turn and exit cleanly, restoring the terminal. `SIGWINCH` redraws the
current frame at the new width, preserving your draft, cursor, and menu
selection.

## Key bindings

At the prompt:

| Key | Effect |
| --- | --- |
| `Enter` | submit; with the command menu open, complete the highlighted command |
| `Ctrl+J` | insert a newline (multi-line message) |
| `Left` / `Right` | move one code point |
| `Home` / `Ctrl+A` | start of the current line |
| `End` / `Ctrl+E` | end of the current line |
| `Backspace` / `Ctrl+H` | delete the code point before the cursor |
| `Delete` | delete the code point after the cursor |
| `Ctrl+D` | delete forward; on an **empty** draft, exit |
| `Up` / `Down` | input history; with the command menu open, move the selection |
| `Tab` | complete the highlighted command (no-op with no menu) |
| `Ctrl+C` | clear the draft |
| `Escape` | nothing at the prompt; cancels a selector |

Notes:

- **Enter versus Ctrl+J.** Enter submits, so a newline needs Ctrl+J. Pasted
  newlines are inserted, not submitted.
- **Bracketed paste** is enabled, so a multi-line paste is inserted verbatim
  and drawn once when it completes. NUL bytes in a paste are discarded.
- **Input history** is the current session's own prior user messages
  (rebuilt on `/session switch`), not a global history file. Up walks back,
  Down walks forward, and walking past the newest entry restores the draft
  you had. Consecutive duplicates are not stored. Bounded to 256 entries or
  4 MiB, oldest dropped first. A single draft is bounded at 1 MiB.
- **Ctrl+C** at the prompt clears the draft; during a turn it cancels the
  turn; in a selector it cancels the selector (which counts as declining).
- **Ctrl+D** exits only from an empty draft. Pressed during a turn it does
  nothing.
- A lone `Escape` is resolved after 40 ms, so it is not mistaken for the
  start of an arrow-key sequence.
- Cursor arithmetic covers a documented subset of Unicode: combining marks
  are zero width, common CJK/Hangul/fullwidth ranges are width 2, and a wide
  code point is never split across a row. It is not grapheme-cluster aware,
  so emoji ZWJ sequences and modifiers may draw imperfectly.

### Command menu

Typing `/` opens a filterable menu of commands. It is shown while the draft
starts with a single `/` and contains no whitespace — so it disappears once
you type an argument, and never appears for `//literal`. Up to 8 matches are
listed, filtered by prefix (`/s` shows `/session` and `/status`). Up/Down
cycles, Tab completes the highlighted command and adds a space, and Enter
completes it — pressing Enter again on the completed name runs it.

### Selectors

Confirmations and tool-permission prompts use the same selector: Up/Down to
move, Enter to choose, `1`-`9` to choose directly by position, and Escape or
Ctrl+C to cancel. Cancelling always picks the safe option — the first one
for confirmations, and *deny* for a tool permission prompt.

## Slash commands

The summary below is the command registry verbatim — the same usage strings
and descriptions `/help` prints, checked against it by `test_cli_docs`:

- `/help` — Show commands and key bindings
- `/exit` — Exit after the current safe boundary
- `/session [list|current|switch|...]` — List, switch, rename, trash,
  restore, or import sessions
- `/model [name]` — Show or change the session model
- `/permissions [ask|allow|deny]` — Show or change tool permissions
- `/status` — Show current runtime and session status
- `/compact [turns]` — Compact older model context
- `/cwd [path]` — Show or change the session working directory

An unrecognized command prints `oi: unknown command: /whatever` and is never
sent to the model. Commands are only available in interactive mode; one-shot
runs have no command layer.

"Persistence scope" below means: **session** — stored in the session and
restored when it is resumed; **process** — lasts only for this `oi` run;
**read-only** — changes nothing.

### `/help`

Read-only. Prints the command list above plus the key summary. Runs
immediately even while a turn is active.

### `/exit`

Process scope. Ends the run at a safe boundary; typed during a turn it takes
the pending slot and exits once the turn finishes. Ctrl+D on an empty draft
is the same thing.

### `/status`

Read-only, and safe to run mid-turn. One deterministic, secret-free block:

```
> /status
Session: 20260731-121500-4242-000
Model: gpt-4o-mini (startup default)
Endpoint: api.openai.com:443/v1/chat/completions (TLS on)
Permissions: ask
Request timeout: 60000 ms
Tool timeout: 60000 ms
CWD: /home/you/src/project
Conversation: idle
Queue: empty
Checkpoint: none
Context: not compacted
```

- `Session` is a session id, `(not created)` for an interactive session with
  no message submitted yet, `(ephemeral, not persisted)` when nothing will
  ever be persisted, or an id with `(durable storage failed)`.
- `Model` names where the value came from: startup default, command-line
  override, restored from session history, restored from session metadata, or
  changed with `/model`.
- `Conversation` is idle, model streaming, awaiting tool permission, tool
  running, cancelling, working, or failed with its cause.
- `Queue` is `empty` or `1 message`/`1 command queued (N bytes)`, plus
  `(steering to a safe boundary)` while steering.
- `Checkpoint` reports the durable checkpoint's source record range, and
  `Context` whether active context is compacted.

Every value that could come from a tampered log, a filesystem, or the
command line is repaired to well-formed UTF-8 and stripped of control bytes
and escape sequences, so one field can never forge another line.

### `/model [name]`

Session scope. With no argument, prints the model in use. With a name, it
changes the model for this and all future turns of the session and persists
it immediately. Names are bounded (256 bytes here, 4096 in storage); a
rejected name prints `oi: could not change the model` and changes nothing.

```
> /model
Model: gpt-4o-mini
> /model my-model
Model: my-model
```

### `/permissions [ask|allow|deny]`

Process scope — never persisted, so a resumed session starts from your
flags again. With no argument it prints the current policy.

Changing to `allow` from anything else requires an explicit confirmation
selector, because it disables every future tool prompt for the rest of the
run. Cancelling prints `oi: permissions unchanged`. `ask` and `deny` apply
immediately.

```
> /permissions
Permissions: ask
> /permissions deny
Permissions: deny
```

### `/cwd [path]`

Session scope. With no argument, prints the working directory. With a path,
it must be an existing directory; the path is resolved to its canonical
form, becomes the process working directory (so tools run there), and is
persisted. An unusable path prints `oi: could not change the working
directory` and leaves both the live and the stored value untouched.

### `/compact [turns]`

Session scope. See [Compaction](#compaction).

### `/session`

Session catalog management. Grammar, exactly:

```
/session
/session list
/session trash-list
/session current
/session switch ID
/session rename ID NAME
/session trash ID
/session restore ID
/session delete ID
/session import PATH
```

`/session` and `/session list` show selectable sessions, most recently
updated first:

```
> /session list
Sessions (3):
* 20260731-121500-4242-000  "refactor"  model gpt-4o-mini  updated 2026-07-31 12:20
  20260730-090301-3311-000  model gpt-4o-mini  updated 2026-07-30 09:15  [open elsewhere]
  20260729-174455-2210-000  model unknown  updated unknown  [metadata rebuilt]
```

`*` marks the active session. `[open elsewhere]` means another process holds
that session's log lock. `[metadata rebuilt]` means the row had to be
reconstructed by replaying that session's history because its cache was
missing or unreadable — the row is accurate, but nothing about it was
cached.

`/session trash-list` is the same listing for trashed sessions.

`/session current` shows the active session's id, its path, and whether its
metadata cache is healthy right now (re-checked on every call, not cached
from startup):

```
> /session current
Session: 20260731-121500-4242-000
Path: /home/you/.local/state/oi/sessions/20260731-121500-4242-000
Status: healthy
```

`Path` is the session directory for an automatically created session, and the
`.oilog` file itself when the session came from `--session`. `Status` is
`healthy` or `degraded (metadata will be rebuilt from history)`.

`/session switch ID` switches at an idle boundary only. It replays and
repairs the target, restores its model and working directory, rebuilds input
history from its user messages, and then offers to print the restored
conversation. A failed switch leaves the current session active and usable
and reports why: `already on that session`, `no such session`,
`session is open in another process`, `invalid session id`, or
`session could not be loaded`.

`/session rename ID NAME` sets a display name only. The id and directory
never change, because every derived path depends on the id. `NAME` may
contain spaces, is bounded to 256 bytes, and may not contain control bytes.
The name lives in the metadata cache, not in history.

```
> /session rename 20260731-121500-4242-000 refactor pass 2
Renamed session 20260731-121500-4242-000
```

`/session trash ID` moves a session into the recoverable trash. It refuses
the active session (`cannot trash the active session -- switch away from it
first`) and any session open in another process. `/session restore ID` moves
a trashed session back, refusing to overwrite a live session with the same
id.

`/session delete ID` **permanently** deletes an already-trashed session,
after a confirmation selector that defaults to Cancel. There is deliberately
no one-step delete of a live session: trash it first. A session that is not
in the trash reports `no trashed session with that id -- trash it first`.
Deletion cannot be undone and there is no backup:

```
> /session delete 20260729-174455-2210-000
> Cancel
  Permanently delete session 20260729-174455-2210-000 (cannot be undone)
```

`> ` marks the highlighted option, and Cancel starts highlighted; choose the
second option (Down then Enter, or `2`) to go through with it:

```
oi: deleted session 20260729-174455-2210-000
```

`/session import PATH` copies a legacy `.oilog` file into a new private
session after a confirmation. The copy is validated by replaying it, the
source file is never modified or moved, and symlinks, non-regular files, and
files already inside the sessions root are refused. On success it prints the
new id and how to open it:

```
> /session import ./oi-session.oilog
> Cancel
  Copy './oi-session.oilog' into a new session
oi: imported as session 20260731-124400-4242-000 (use /session switch 20260731-124400-4242-000 to open it)
```

`switch`, `delete`, and `import` need the interactive terminal; the other
subcommands do not.

## Sessions and storage

### Where sessions live

The sessions root is:

1. `$XDG_STATE_HOME/oi/sessions` when `XDG_STATE_HOME` is set to an absolute
   path;
2. otherwise `$HOME/.local/state/oi/sessions`.

That rule is the same on every platform this builds on — including macOS, if
you build the `macos-support` branch. There is no `~/Library/Application
Support` layout.

`--session-dir DIR` overrides the root. It also becomes the root that
`/session list` and the rest of the lifecycle commands operate on.

### Layout

An automatically created session is a private directory named
`YYYYMMDD-HHMMSS-<pid>-<seq>` containing:

| File | Role |
| --- | --- |
| `history.oilog` | authoritative append-only records |
| `metadata.json` | rebuildable selector cache (id, model, cwd, timestamps, display name) |
| `metadata.json.lock` | empty advisory lock serializing cache updates |

Directories are created `0700` and files `0600`, so sessions are readable
only by you. Session ids are bounded portable ASCII (`A-Z a-z 0-9 _ -`,
1-128 bytes) and contain no path separators, which is what makes them safe
to turn into paths.

`--session ID` uses a flat layout instead: `DIR/ID.oilog` alongside
`DIR/ID.metadata.json`, where `DIR` is `--session-dir` (default `.`). The
directory must already exist; `oi` will not create it, and reports
`failed to open session 'ID' at 'DIR/ID.oilog'` if it cannot.

### Authoritative history versus rebuildable cache

`history.oilog` is the only authoritative file. It holds versioned JSON
envelopes for user and assistant messages, tool calls and results, repairs,
setting changes, queued-input bookkeeping, and context checkpoints, each
with a stable monotonic record id. Legacy raw records from older versions
still replay, across an explicit schema-transition marker.

`metadata.json` is a cache. It is replaced atomically (temp file plus
`rename`), and if it is missing, corrupt, or belongs to another id, it is
silently rebuilt from replayed history on the next open, with a diagnostic on
stderr. That is why a rebuilt row is labelled `[metadata rebuilt]` in
`/session list`: the answer is correct, but it did not come from a cache.
A display name exists only in the cache, so losing the cache loses the name
but nothing else.

### Locks and concurrency

A running `oi` holds an exclusive `flock` on the session's log for its whole
lifetime, so one session cannot be opened by two processes. A read-only
probe of that lock is what produces `[open elsewhere]`, and it is why trash,
delete, and switch refuse a session that is in use. Cache updates take a
short lock on the sibling `metadata.json.lock` file so a rename and a
concurrent `/model` or `/cwd` refresh cannot discard each other's field.

### Lazy creation, repair, and recovery

- An interactive session without `--session` is created only when you submit
  your first message. Its initial model and working directory are recorded
  durably at creation, so the cache can always be rebuilt exactly.
- A torn trailing record from a killed process is detected and recovered at
  the next open.
- A turn that ended without a final assistant reply is repaired into valid
  context: a placeholder closes the missing reply, and each dangling tool
  call gets an outcome-unknown or not-executed result. Partial assistant
  text stays in history for audit and replay but is excluded from the context
  sent to the model.
- If the process died with a message queued, that text is handed back as an
  editable draft on the next open of that session — it is never auto-sent.
  (Only reachable for `--session`; a fresh automatic session has no history
  to recover.)

Limits worth knowing before you rely on any of this:

- **No `fsync`.** Records survive a process crash, not a power loss or a
  filesystem that loses recent writes.
- **No encryption.** History is plain JSON on disk, including tool output.
  File permissions are the only protection.
- **Ids are permanent.** `rename` changes the display name only.
- **Deletion is permanent** and requires trashing first. There is no undo
  and no backup copy.
- A history file that is corrupt beyond a torn trailing record cannot be
  repaired by `oi`. Such a session is still listed and can still be trashed;
  `/session switch` reports `session could not be loaded`.

### Trash

Trashed sessions move whole into a `.trash` subdirectory of the sessions
root by a single `rename(2)`. Because the trash is inside the same root, the
move is same-filesystem and atomic: there is no half-trashed state. A
cross-device failure is reported as
`the trash directory is on a different filesystem` rather than a generic I/O
error. `.trash` starts with a dot, so it can never be mistaken for a session
id.

## Tools and security

`oi` advertises one built-in tool, `shell`, which takes a single `command`
string and runs it as `sh -c <command>` in the session working directory,
with the environment `oi` itself was started with.

**That is arbitrary code execution, requested by a model.** Tool arguments
are produced from model output, which can be influenced by anything in
context — a web page a previous tool fetched, a file it read, a tool result.
Treat `--allow-tools` and the *Allow for process* option as "I trust
whatever ends up in this conversation to run commands as me".

The policy is process-wide and one of:

| Policy | How | Behavior |
| --- | --- | --- |
| `ask` | default | interactive: prompt per call. one-shot: refuse |
| `allow` | `--allow-tools`, `/permissions allow` | run without asking |
| `deny` | `--deny-tools`, `/permissions deny` | refuse every call |

In interactive mode, `ask` shows a selector before the call runs:

```
Tool: shell
Args: {"command":"ls -la"}
> Allow once
  Allow for process (skip future prompts)
  Deny
```

*Allow once* starts highlighted, so a reflexive Enter allows that one call —
read the `Args:` line before pressing it. `1`, `2`, and `3` pick the options
directly. Escape or Ctrl+C dismisses the selector as a *deny*, never leaving
the call hanging. Choosing *Allow for process* is itself the elevation (no
second confirmation) and prints
`oi: tool policy set to allow for the rest of this process`.

The tool name and the argument summary (truncated to 200 bytes for display)
are sanitized before being drawn, so model output cannot inject escape
sequences into the prompt.

In one-shot mode there is nothing to ask, so `ask` **refuses**: the run
prints `oi: tool permission denied` rather than hanging. Non-interactive
tool use therefore requires an explicit `--allow-tools`, which is the
deliberate choice you want it to be:

```sh
# runs shell commands the model asks for, unattended -- use with care
oi --allow-tools "Find the largest files in this checkout"
```

Other boundaries:

- `--max-turns N` (default 8) caps the model steps in one turn, so a
  tool-calling loop cannot run forever.
- `--timeout-ms` bounds each tool execution as well as the request.
- Live tool output is bounded in the display (about 4 KiB across at most 6
  lines); the full result still goes to the model and to history.
- All assistant and tool output is stripped of control bytes and escape
  sequences before it reaches your terminal.
- Tool output is stored verbatim in the session log. If a tool prints a
  secret, that secret is now on disk in `history.oilog`.
- TLS is on by default and verifies against the system trust store, or
  `--ca-file` if given. `--no-tls` exists for local mock servers; do not
  point it at a real endpoint, since the API key would go out in clear text.

## Compaction

`/compact [turns]` summarizes the completed turns older than the most recent
`turns` (default 8) into a single durable checkpoint, and replaces that
prefix of active context with the summary.

```
> /compact 4
oi: compacted 6 turns into a checkpoint (kept last 4)
```

Semantics:

- It runs only at an idle boundary. Typed during a turn, it waits in the
  pending slot like any other state-changing command.
- The summarization request is a dedicated, bounded request that treats
  prior turns as data to summarize, never as instructions, so an adversarial
  earlier tool result or model reply cannot hijack it.
- Nothing is compacted if there is nothing older than the kept turns:
  `oi: nothing to compact (3 turns, keeping 8)`. Before the first message,
  `oi: nothing to compact yet`.
- The durable checkpoint must be written successfully before the live
  conversation is spliced. A failed request (`oi: compaction failed: ...`) or
  a Ctrl+C (`oi: compaction cancelled`) leaves both the log and the live
  conversation completely untouched.
- **Nothing is deleted.** The original records stay in `history.oilog`, and
  the checkpoint records the exact source record range it replaced. Full
  history remains available for audit and replay; only what is *sent to the
  model* shrinks. `/status` reflects this as `Checkpoint: records A-B` and
  `Context: compacted`.
- Compaction is manual. There is no automatic compaction.

## Troubleshooting

**`oi: no API key set (use OI_API_KEY or --api-key)`** — export
`OI_API_KEY`, or pass `--api-key`. Remember a config file cannot hold the
key. `oi --dry-run "hi"` verifies everything else without a key.

**`oi: failed to load config file '...': denied`** — the file contains
`api_key`. Remove that line; supply the key through the environment.

**The wrong `oi` runs.** `type -a oi` lists every candidate, `hash -r`
clears a stale shell lookup after installing, and
`cmp build/oi "$(command -v oi)"` proves whether the installed file is the
build you just made.

**TLS or CA failures.** Check the endpoint and port first with
`oi --dry-run "hi"`. For a private CA, pass `--ca-file /path/to/ca.pem`. If
your distribution stores trust anchors somewhere OpenSSL is not configured
for, `SSL_CERT_FILE`/`SSL_CERT_DIR` apply as usual, since verification is
OpenSSL's. `--no-tls` is for local mock servers only.

**`session is open in another process` / `[open elsewhere]`.** Another `oi`
holds that session's lock. Exit it. If a process was killed with `SIGKILL`,
the lock dies with it, so a stale lock is not something you have to clear by
hand; a lingering report means the process is still alive.

**`session could not be loaded` / `[metadata rebuilt]`.** A rebuilt cache is
harmless and self-healing. A session that will not load has a damaged
`history.oilog`; `oi` will not rewrite it. You can still list and trash it.
The file is a 12-byte `OISESLOG` header followed by length-prefixed records
whose payloads are JSON, so `strings history.oilog` recovers the text of a
conversation even when the framing is too damaged to replay.

**Terminal left in a strange state.** `oi` restores the original terminal
modes and disables bracketed paste on exit, including on Ctrl+D, `/exit`,
`SIGTERM`, and `SIGHUP`. Only `SIGKILL` (or a crash) can skip that; run
`stty sane` or `reset` afterwards.

**Unsupported terminal.** Interactive mode needs an ANSI-capable terminal;
there is no fallback renderer. If your terminal cannot handle it, use
one-shot mode — redirecting stdout is enough to select it:
`oi "prompt" > out.txt`.

**Interactive mode never starts.** It requires stdin *and* stdout to be
terminals, no prompt argument, and no `--dry-run`. Under a pipe, a
redirection, or a CI runner, `oi` is a one-shot command by design.

**Bug reports** go to <https://github.com/zo-ll/oi.c/issues>. Include the
exact command line, whether you were in interactive or one-shot mode, the
message `oi` printed, and your platform and OpenSSL version. Never paste an
API key or a session log you have not reviewed — logs contain full tool
output.

## Developer verification

```sh
make quick     # routine edit-test loop: builds the CLI, runs pure unit tests
make check     # every unit and integration test
make verify    # pre-merge gate: gcc + clang, ABI, ASan, UBSan, TSan,
               # Valgrind, bounded fuzzing
```

Other useful targets: `make tier-audit` (every test binary belongs to
exactly one tier), `make timings` (per-phase and per-binary wall time),
`make abi-check` (exported symbol set), `make CC=clang check`,
`make -j24 check`. See [TESTING.md](TESTING.md) for the workflow and the
concurrency boundaries, and [REPL_PLAN.md](REPL_PLAN.md) for the module
boundaries and the storage contract.

Platform notes for contributors: the reactor backend is
`src/reactor_epoll.c`; `signalfd` and `pidfd_open` mean Linux 5.3+; PTY
tests link `-lutil`; TSan may need `setarch -R make tsan` on kernels with
more ASLR entropy than its shadow mapping expects.

## Not supported yet

Documented here so nothing above implies more than exists:

- macOS and any non-Linux platform on `main` (kqueue backend lives on
  `macos-support`).
- Durability across power loss (no `fsync`), and encryption at rest.
- Automatic compaction; `/compact` is manual.
- Semantic/vector retrieval over history. Checkpoints record their source
  ranges so an index can be added later without changing session files.
- Full grapheme-cluster editing (ZWJ sequences, emoji modifiers).
- A non-ANSI terminal fallback.
- More than one queued item while a turn runs; the slot holds exactly one.
- Any built-in tool other than `shell`.
- A `--version` flag, an implicit config-file location, and any way to put
  the API key in a config file.
