# oi.c

`oi.c` is a small C11 library and CLI for streaming OpenAI-compatible chat
responses, running approved subprocess tools, and persisting independent
agent sessions. Its core is a single-threaded reactor: network sockets,
tool pipes, child exits, and deadlines share one event loop without locks.

`oi` on a terminal is an interactive chat with durable sessions, slash
commands, and a Markdown renderer. Given a prompt argument or piped input it
is an ordinary one-shot command instead.

The implementation targets Linux, using epoll, timerfd, signalfd, and pidfd
(`pidfd_open` requires Linux 5.3 or newer). OpenSSL and POSIX threads are
the only external runtime dependencies. (A prior macOS/kqueue backend
lives on the `macos-support` branch rather than `main`.)

**Full command-line guide: [docs/CLI.md](docs/CLI.md)** — installation,
configuration, startup modes, key bindings, every slash command, session
storage, security, and troubleshooting.

## Install

You need a C11 compiler, GNU Make, and the OpenSSL development headers
(`pkg-config` is used when present). On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libssl-dev
```

Build and test:

```sh
make
make check
```

Artifacts are written under `build/`: `oi`, `liboi.a`, and `liboi.so`. The
CLI runs straight from there (`build/oi --help`), or install it:

```sh
sudo make PREFIX=/usr/local install     # system-wide
make PREFIX="$HOME/.local" install      # per-user, no sudo
```

This installs the CLI, both libraries, and public headers under
`$PREFIX/{bin,lib,include/oi}`. The shared library is installed as
`liboi.so.0.1.0` with `liboi.so.0` (runtime) and `liboi.so` (development)
symlinks. `make DESTDIR=/tmp/package-root PREFIX=/usr install` stages an
install for packaging, and `make PREFIX=... uninstall` removes exactly what
`install` created. `type -a oi` confirms which copy is on your `PATH`.

## First run

Set an API key. Keep it out of your shell history and out of `ps`: `oi`
reads `OI_API_KEY`, and a config file containing `api_key` is refused
outright.

```sh
install -d -m 700 "$HOME/.config/oi"              # private directory
install -m 600 /dev/null "$HOME/.config/oi/api-key"
$EDITOR "$HOME/.config/oi/api-key"                # paste the key
export OI_API_KEY="$(cat "$HOME/.config/oi/api-key")"
```

Start an interactive chat by running `oi` with no arguments on a terminal:

```console
$ oi
> What does the reactor in this repository do?
...
> /status
...
> /exit
```

Enter submits, `Ctrl+J` inserts a newline, `Up`/`Down` walk this session's
input history, `/` opens the command menu, and `Ctrl+D` on an empty prompt
exits. `/help` lists every command and key binding; `//text` sends a message
that really does start with a slash.

One-shot mode needs no extra flags — a prompt argument or redirected stdin
selects it:

```sh
oi "Explain this repository"
{ printf 'Review this diff:\n'; git diff; } | oi
oi --dry-run "hi"        # print the resolved request; no network, no key
```

A prompt argument wins over stdin outright — `git diff | oi "Review this"`
would send only the argument and silently drop the diff — so when you pipe,
put the instruction in the piped text as above.

Interactive runs are durable: a fresh session is created under
`$XDG_STATE_HOME/oi/sessions` (or `~/.local/state/oi/sessions`) on your first
message, with `0700` directories and `0600` files. One-shot runs are
ephemeral unless you pass `--session ID` explicitly. `/session` lists,
switches, renames, trashes, restores, imports, and — after an explicit
confirmation — permanently deletes sessions.

Configuration is resolved in this order:

1. built-in defaults;
2. an optional `key = value` config file (`--config PATH`, no `api_key`);
3. `OI_API_KEY`;
4. CLI flags.

`build/oi --help` lists every flag: endpoint, TLS, model, session,
deadline, and tool policy.

## Tools

The CLI advertises a built-in `shell` tool, which runs `sh -c <command>` in
the session working directory. That is arbitrary code execution requested by
a model, so the default policy is `ask`: interactive runs prompt before each
call, and a one-shot run refuses the call rather than prompting. Pass
`--allow-tools` deliberately for unattended tool use, or `--deny-tools` to
refuse everything. `oi` follows tool calls with their results until the model
returns a final answer or `--max-turns` (default 8) is reached.

Session files are append-only `.oilog` logs. They recover a torn trailing
record after a process crash. They intentionally do not call `fsync`, so
they do not promise persistence across power loss, and they are not
encrypted — tool output is stored verbatim.

## Verify

```sh
make quick     # routine edit-test loop
make check     # every unit and integration test
make verify    # pre-merge gate: gcc + clang, ABI, sanitizers, valgrind, fuzz
```

`make -j24 quick` and `make -j24 check` parallelize compilation and only the
audited pure test binaries; PTY, socket, fork, signal, and integration tests
remain serialized. `make timings` reports clean compilation, each tier, and
every test binary. See [docs/TESTING.md](docs/TESTING.md) for the workflow,
concurrency boundaries, and measured issue-#32 results. Clang can be
selected for the ordinary build with `make CC=clang check`.

The project follows semantic release versions and is currently `0.1.0`.
Its public C ABI is deliberately exported through an allowlist and carries
ABI major `0` in the SONAME. The API remains pre-stable: additions are
expected during `0.x`, but an incompatible ABI change must advance the
SONAME major. Patch releases preserve the current ABI; obsolete APIs should
be deprecated for at least one release before removal when practical.
`make abi-check` verifies the exported symbol set.

## Library layout

- `reactor`: fd readiness, one-shot timers, and stale-event protection;
- `llm`: TLS/TCP, HTTP response framing, SSE completion, and JSON deltas;
- `tool`: declaration/permission policy boundaries and subprocess I/O;
- `session` / `sesslog`: identity, fault containment, and persistence;
- `arena` / `json`: session-scoped allocation and incremental JSON;
- `config`: typed configuration parsing and range validation.

Public headers are in `include/oi`. Ownership and callback lifetime rules
are documented beside each API.

## License

Licensed under the MIT License. See [LICENSE](LICENSE).

The original architecture notes and rationale are in [PLAN.md](PLAN.md).
The interactive-mode design, module boundaries, and storage contract are in
[docs/REPL_PLAN.md](docs/REPL_PLAN.md); shared terminology is in
[CONTEXT.md](CONTEXT.md).
