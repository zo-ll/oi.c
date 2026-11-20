# llm: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/llm.c`, `test/test_llm.c`.

## `OI_LLM_HOOK_RETRY_MS` (src/llm.c)

How often a request asks a before_request hook that returned

OI_ERR_AGAIN again.

## `OI_LLM_MAX_TOOL_CALLS` (src/llm.c)

Bounds on what one stream can make the client hold. A model that goes past them gets OI_ERR_PARSE instead of unbounded memory.

## `oi_llm_client.idle_conn` (src/llm.c)

With keep_alive: the connection of the last complete response, on the reactor of that request, ready for the next one.

## `oi_llm_request.scratch` (src/llm.c)

Holds the JSON tree of one SSE event; reset before the next parse, so a long stream does not grow memory with its event count.

## `oi_llm_request.body_cb_failed` (src/llm.c)

Set by req_on_body/handle_sse_event when a sub-step fails; since oi_llm_http_body_cb is void-returning, this is how that failure gets back to req_on_data, which is the only place actually driving completion.

## `oi_llm_request.destroyed_flag` (src/llm.c)

Set by req_on_data before invoking oi_llm_http_parser_feed (which may synchronously call all the way down through the SSE/JSON layers into the caller's on_delta), so a reentrant oi_llm_request_cancel() from within on_delta can signal back that `req` itself was freed. Mirrors the same pattern already used in oi_llm_conn, oi_llm_http_parser, and oi_llm_sse_parser.

## `close_idle_conn` (src/llm.c)

before the cache that the connection borrows

## `oi_llm_client_reconfigure` (src/llm.c)

Same trust store: keep the TLS context and its session, so a token renewal does not cost a CA load and a full handshake.

## `idle_on_connected` (src/llm.c)

An idle connection expects nothing: data or a close ends it.

## `park_connection` (src/llm.c)

Keeps the connection of a complete response for the next request. It runs before on_done, which can start that request at once.

## `append_fragment` (src/llm.c)

Keeps `*buf` NUL-terminated. The capacity doubles, so a value that arrives in many small fragments costs a logarithmic count of reallocs.

## `record_tool_fragment` (src/llm.c)

A NULL field was absent from this fragment. Returns OI_ERR_CLOSED when the event callback cancelled (and freed) `req`.

## `validate_tool_calls` (src/llm.c)

Returns OI_ERR_CLOSED when the event callback cancelled (and freed) `req`.

Some OpenAI-compatible servers send "" for a tool that takes no arguments. Consumers append fragments, so the empty object goes out as one more fragment.

## `handle_responses_arguments` (src/llm.c)

`key` names the member holding the text: fragments arrive under "delta", and the closing event repeats the whole value under "arguments".

## `handle_sse_event` (src/llm.c)

sentinel, not JSON; body_done drives actual completion

A server can report a failure inside a 200 stream. The event is the only description of it, so it becomes the error body.

## `send_request.header` (src/llm.c)

OAuth access tokens are JWTs, which run to several kilobytes.

## `req_on_data` (src/llm.c)

`req` (and everything it owns) was freed by a reentrant oi_llm_request_cancel() from within a nested on_delta call.

## `req_on_error` (src/llm.c)

A kept connection that the server had closed fails before any response byte: send the request once more, on a new connection. `c` closes itself after this callback.

A body with no length ends at the close. A close that cut the stream short still fails: complete_body requires the end marker.

## `req_on_error.cause` (src/llm.c)

With no response body to show, the cause names the failure.

## `finish` (src/llm.c)

If already finished (body_done already drove completion in an earlier on_data call), the connection closing now is just the expected teardown, not a new failure.

## `request_begin` (src/llm.c)

The part of a start that reads the client: the body in the client's wire format, the parsers, and the connection. It runs only after the before_request hook returned OI_OK, so a hook may reconfigure the client while its request waits. On failure, the caller tears `req` down.

## `request_start.st` (src/llm.c)

The deadline covers a wait for the hook as well.

## `mock_response` (test/test_llm.c)

The response of the server child. A file-scope pointer keeps the caller's heap block reachable in that child until _exit, so valgrind's leak check there does not report it; volatile makes the store happen before the fork.

## `waiting_hook` (test/test_llm.c)

A hook that answers OI_ERR_AGAIN `waits` times, then `final`. Before `final`, it can point the client at `new_port`, as a credential renewal does.

## `start_keep_alive_server` (test/test_llm.c)

Serves up to `connections` connections, one after another, and each request on them, with a Content-Length response. With `close_after`, it waits 200 ms after each response and closes without reading more, so a request sent in that window gets no answer. Each accept writes one byte to *out_accepts_fd.
