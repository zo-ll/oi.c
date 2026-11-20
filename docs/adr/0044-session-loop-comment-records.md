# session_loop: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `test/integration/test_session_loop.c`.

## `file scope` (test/integration/test_session_loop.c)

End-to-end integration tests: reactor + LLM client + tool execution + session lifecycle, driven against the local mock API server.

Where the unit tests exercise one module against hand-fed bytes, these drive the combination the way an embedder would -- a real socket, real HTTP/chunked/SSE framing, a real forked subprocess for the tool, and a real on-disk session log -- with only the model itself faked.

One structural note, because it shapes these tests: the library deliberately ships no turn-loop driver (see oi/session.h -- the session module owns identity, arena, and log, not conversation shape), so the loop below *is* the embedder. It builds each request body, consumes the stream, runs the tool, and feeds the result into the next turn.

## `msg` (test/integration/test_session_loop.c)

--- request building ---

The request body is assembled with oi/json.h's writer, which is the path that matters: the tool's output is untrusted bytes going back out over the wire, so its escaping has to be exercised for real rather than sprintf'd into place by the test.

## `REPLAY_MAX` (test/integration/test_session_loop.c)

Collects replayed log records so a test can assert on the whole history a restart would see.

## `full_tool_use_loop` (test/integration/test_session_loop.c)

Turn 1's stream carries content deltas *and* a tool_calls delta. The client documents that it forwards assistant text only and treats a tool-call-only chunk as "nothing to do" -- so this asserts the text arrives intact and the tool-call chunk neither errors nor produces a spurious delta.

That documented behavior is also why the tool arguments below are supplied by the test rather than read out of the stream: the client exposes no way for an embedder to see a tool_calls delta, so the model-to-tool hop cannot be closed through the public API today. Everything after that hop -- argument parsing, permission check, subprocess execution, logging, and feeding the result into turn 2 -- is the real code path.

The registry hands the schema back verbatim for the caller to put in the request's "tools" field.

## `full_tool_use_loop.turns` (test/integration/test_session_loop.c)

A deliberately small chunk size so SSE events straddle HTTP chunk boundaries -- the framing layers have to reassemble them.

## `full_tool_use_loop.captured` (test/integration/test_session_loop.c)

The mock captured what actually went over the wire: the second request must carry the tool's output, JSON-escaped.

## `denied_tool_never_executes` (test/integration/test_session_loop.c)

A denied tool must never reach fork/exec, and must report synchronously rather than through the completion callback.

An unregistered name is a distinct, equally synchronous failure.

## `denied_tool_never_executes.never` (test/integration/test_session_loop.c)

Give the reactor a chance to dispatch anything that was wrongly queued; nothing should have been.

## `deferred_permission_resolves` (test/integration/test_session_loop.c)

A deferred decision runs nothing until it is resolved.

## `api_error_fails_only_its_own_session` (test/integration/test_session_loop.c)

PLAN.md's claim is that a session-level fault tears down that session's resources and leaves the reactor and every other session running. This drives a real API error through the client, fails the session on it, and checks the neighbouring session is untouched.

## `session_resumes_from_its_log` (test/integration/test_session_loop.c)

The point of the durable log: a process that dies mid-conversation can reopen the same log by session ID and replay what it had.
