# oi.c

`oi.c` is a small C11 library and CLI for streaming OpenAI-compatible chat
responses, running approved subprocess tools, and persisting independent
agent sessions. Its core is a single-threaded reactor: network sockets,
tool pipes, child exits, and deadlines share one event loop without locks.

The implementation targets Linux, using epoll, timerfd, and pidfd
(`pidfd_open` requires Linux 5.3 or newer). OpenSSL and POSIX threads are
the only external runtime dependencies. (A prior macOS/kqueue backend
lives on the `macos-support` branch rather than `main`.)

## Build and verify

Install a C compiler, GNU Make, and the OpenSSL development headers, then:

```sh
make
make check
```

Use `make quick` for the routine edit-test loop. It still builds the real CLI,
but runs only deterministic pure unit tests. `make check` runs every ordinary
unit and integration test, while `make verify` is the comprehensive pre-merge
and release gate:

```sh
make quick
make check
make verify
```

`make -j24 quick` and `make -j24 check` parallelize compilation and only the
audited pure test binaries; PTY, socket, fork, signal, and integration tests
remain serialized. `make timings` reports clean compilation, each tier, and
every test binary. See [docs/TESTING.md](docs/TESTING.md) for the workflow,
concurrency boundaries, and measured issue-#32 results.

Clang can be selected for the ordinary build with `make CC=clang check`.
Artifacts are written under `build/`: `oi`, `liboi.a`, and `liboi.so`.

To stage an installation:

```sh
make DESTDIR=/tmp/package-root PREFIX=/usr install
```

This installs the CLI, both libraries, and public headers under
`$PREFIX/{bin,lib,include/oi}`. The shared library is installed as
`liboi.so.0.1.0` with `liboi.so.0` (runtime) and `liboi.so` (development)
symlinks.

The project follows semantic release versions and is currently `0.1.0`.
Its public C ABI is deliberately exported through an allowlist and carries
ABI major `0` in the SONAME. The API remains pre-stable: additions are
expected during `0.x`, but an incompatible ABI change must advance the
SONAME major. Patch releases preserve the current ABI; obsolete APIs should
be deprecated for at least one release before removal when practical.
`make abi-check` verifies the exported symbol set.

## CLI

Set an API key and provide a prompt:

```sh
export OI_API_KEY='...'
build/oi "Explain this repository"
```

The CLI advertises a built-in `shell` tool. By default it asks on the
controlling terminal before each execution; use `--allow-tools` or
`--deny-tools` for explicit non-interactive policy. It follows tool calls
with their results until the model returns a final answer or `--max-turns`
is reached. Use `build/oi --help` for endpoint, TLS, model, session,
deadline, and tool-policy options. `--dry-run` prints the resolved request
without making a network connection or requiring an API key.

Configuration is resolved in this order:

1. built-in defaults;
2. an optional `key = value` config file;
3. `OI_API_KEY`;
4. CLI flags.

Session files are append-only `.oilog` logs. They recover a torn trailing
record after a process crash. They intentionally do not call `fsync`, so
they do not promise persistence across power loss.

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
