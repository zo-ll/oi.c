# oi.c — design plan

A minimal, modular, efficiency-first C agent harness. It drives an LLM tool-use loop and
supports running many independent agent sessions concurrently.

## Architecture

- **Scope**: drives a single LLM tool-use loop (send messages, parse tool calls, execute
  tools, feed results back) and supports multi-agent orchestration — running many such
  sessions concurrently.
- **Concurrency model**: single-threaded event loop / reactor (epoll on Linux, kqueue on
  macOS) multiplexing all agent sessions' I/O. Chosen over one-thread-per-agent and
  one-process-per-agent because the workload is I/O-bound (waiting on LLM API responses,
  subprocess pipes); a single-threaded reactor has no locks/races by construction, the
  lowest memory overhead, and the most predictable scheduling.
- **Platform target**: Linux (epoll, timerfd, pidfd) and macOS (kqueue with
  native timer and process-exit filters). Platform-specific behavior stays
  behind the reactor backend; the project does not claim broader portable
  POSIX support.
- **Interface**: a C library core (headers + .a/.so) exposing the reactor/session API, plus
  a thin CLI binary that links it for standalone use.

## LLM & tools

- **LLM API**: targets the OpenAI-compatible wire format as the single API shape (covers
  many backends: OpenAI itself, and any OpenAI-compatible hosted/local model). opencode
  (Go project) is a design reference for patterns, not a dependency.
- **Streaming**: LLM responses are streamed incrementally to the caller as chunks arrive
  (SSE/chunked parsing forwards partial content via callback/event), not buffered until a
  full turn completes.
- **Tool declaration**: a static tool registry (name, JSON schema, handler) set up at
  startup via the library API.
- **Tool permissions**: before executing a requested tool call, the harness invokes a
  caller-supplied permission callback (allow/deny/ask) — policy stays out of the harness
  core while still providing a hook.
- **Tool execution**: tools run as child processes (fork/exec); their stdin/stdout pipes
  are registered as non-blocking fds in the same reactor loop. No blocking calls anywhere.
- **Request concurrency / rate limiting**: none built in — capping in-flight LLM requests
  is the embedder's responsibility.

## Data & memory

- **Allocation strategy**: arena/pool allocator per session. Allocations within a turn are
  bump-allocated and freed in bulk at turn/session lifecycle boundaries. Avoids
  malloc/free overhead and fragmentation on the hot path.
- **JSON**: an in-house streaming JSON parser, written from scratch — no external or
  vendored dependencies, so it can parse incrementally as bytes arrive from a non-blocking
  socket.
- **Persistence**: a durable, append-only session log on disk so a restart can resume a
  session mid-conversation. Whole-record writes (single `write()` per record,
  length-prefixed or newline-delimited); no `fsync` by default (crash-safe against process
  crash, not against power loss). A truncated trailing record found on recovery is
  discarded. The on-disk log format carries a version tag from day one.
- **Session addressing**: sessions are created with a caller-chosen name/ID; the library
  maintains an internal registry so callers can look sessions up by ID rather than holding
  a raw handle — this also supports resuming a named session from its on-disk log after a
  restart.

## Reliability & faults

- **Fault containment**: a session-level fault (malformed API response, allocator failure,
  etc.) tears down that session's arena and fds and marks it failed; the reactor and all
  other sessions keep running. There is no process-level isolation between sessions
  (single trust domain — your own agents).
- **Multi-agent model**: sessions are fully independent. They share only the reactor and
  process-level resources; there is no inter-agent message-passing or shared task-queue
  primitive in the harness itself.

## Config

- **Config source**: secrets (API keys) come from environment variables; non-secret
  settings (endpoint URL, model, timeouts) come from an optional small config file, with
  CLI flags able to override either.

## Build & quality

- **Build system**: a plain, hand-written Makefile — no cmake/meson, no build-time
  dependency beyond a C toolchain.
- **Testing**: unit tests for pure logic (JSON parser, arena allocator, message framing);
  integration tests against a mock API server driving the full session loop; a fuzzing
  harness for the JSON parser and message framing (untrusted network input); the full test
  suite run under ASan/UBSan/TSan; valgrind as a secondary check.

## Meta

- No project name was settled during design; the repo is named `oi.c` as the concrete name
  chosen at repo-creation time.
- Private repository for now; license decision deferred.
