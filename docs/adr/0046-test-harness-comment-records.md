# test_harness: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `test/integration/mock_api.h`, `test/test.h`.

## `file scope` (test/integration/mock_api.h)

A scripted, local, OpenAI-compatible mock API server for integration tests -- header-only, in the same spirit as test.h.

It runs in a forked child so the test process can drive its own reactor loop against a real socket rather than a stubbed transport: everything from connect() through HTTP framing, chunked transfer-decoding, and SSE splitting is exercised for real, and only the model behind it is fake.

Each request the client makes is served the next scripted turn, and the raw bytes of every request are captured to a file so the parent can assert on what the harness actually sent -- which is how a test verifies that a tool result made it back into the next turn.

The client opens one connection per request ("Connection: close"), so one scripted turn is consumed per connection.

## `MOCK_MAX_REQUEST` (test/integration/mock_api.h)

Refuses to buffer a request larger than this; no test sends one.

## `mock_turn.chunk_size` (test/integration/mock_api.h)

Bytes per HTTP chunk, so a test can force event boundaries to straddle chunks. 0 sends the whole body as one chunk.

## `mock_read_request` (test/integration/mock_api.h)

Reads one complete HTTP request (headers plus any Content-Length body). Returns its length and, via *out, a malloc'd buffer the caller owns; -1 on failure with *out untouched.

## `mock_capture` (test/integration/mock_api.h)

Capture format: each request stored as a decimal length, a newline, then that many raw bytes.

## `mock_api_start` (test/integration/mock_api.h)

Returns 1 on success. On success the caller must call mock_api_stop to reap the child and remove the capture file.

## `signal` (test/integration/mock_api.h)

A client that gives up mid-response must not kill the mock.

## `mock_api_request` (test/integration/mock_api.h)

Returns request number `index` (0-based) as a malloc'd, NUL-terminated buffer the caller owns, or NULL if the mock never received that many. Only meaningful after the child has finished serving them, i.e. after the request that triggered it has completed.

## `file scope` (test/test.h)

Minimal header-only test harness. No external framework: a test file defines TEST(name) blocks, each executed by RUN(name) from main(), with CHECK/CHECK_EQ recording failures without aborting the run.

## `oi_test_deadline_seconds` (test/test.h)

Optional per-process watchdog for tests that fork children, own PTYs, or wait on sockets. A child that regresses into a silent hang must fail the run with the name of the test that was executing rather than stall CI until the platform's own job timeout. Opt-in: call oi_test_set_deadline(seconds) from main() before any RUN(). Pure tests never arm it; the deadline is deliberately generous so slow instrumented runs (valgrind, tsan) do not false-positive.

## `oi_test_deadline_handler` (test/test.h)

Unused by pure tests, which never arm the watchdog; the attribute keeps -Werror happy under both supported compilers without forcing a fake use.
