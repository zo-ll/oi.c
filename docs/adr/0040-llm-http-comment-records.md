# llm_http: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/llm_http.c`, `src/llm_http.h`, `test/fuzz/fuzz_http.c`.

## `OI_HTTP_MAX_BODY_VALUE` (src/llm_http.c)

Upper bound on a Content-Length or chunk-size value. These are digit strings from an untrusted response header, and accumulating them unbounded overflows the signed accumulator -- undefined behavior, and in practice a negative `remaining` that makes the body reader treat the length as effectively unlimited. Anything above this is rejected as malformed instead; it is orders of magnitude beyond any real chat-completion response, so a legitimate server never trips it.

## `oi_llm_http_parser.destroyed_flag` (src/llm_http.c)

Set by oi_llm_http_parser_feed before invoking on_headers_done or on_body, so a reentrant oi_llm_http_parser_destroy() from within one of those (e.g. the caller cancels an in-flight request as soon as it sees enough data) can signal back to feed()'s still-running loop that `p` is gone.

## `finish_headers` (src/llm_http.c)

Neither chunked nor Content-Length: the body ends when the peer closes the connection (oi_llm_http_parser_close).

## `handle_header_line` (src/llm_http.c)

Optional white space around a field value is not part of it.

Checked before the multiply, so the accumulator never overflows rather than being detected after the fact.

## `handle_header_line.k` (src/llm_http.c)

A comma-separated token list; "close" in it ends the connection after this response.

## `oi_llm_http_parser_feed.avail` (src/llm_http.c)

ST_CHUNK_DATA / ST_CONTENT_BODY / ST_UNTIL_CLOSE_BODY: consume a run of raw bytes at once rather than one byte at a time.

## `oi_llm_http_parser` (src/llm_http.h)

Incremental HTTP/1.1 response parser: status line, headers, and a body delimited by chunks, by Content-Length, or (with neither header) by the close of the connection, fed byte-by-byte-safe from a non-blocking socket. Deliberately scoped to what an OpenAI-compatible API actually sends -- no chunk trailers beyond a single terminating CRLF. Knows nothing about SSE or JSON; it just hands raw body bytes to the caller.

## `oi_llm_http_parser_close` (src/llm_http.h)

Tells the parser that the peer closed the connection. OI_OK when that close is what ends the body (no Content-Length and not chunked): the body is then done. OI_ERR_CLOSED in every other state, where the close cut the response short. The transport cannot tell a clean close from a truncated one here, so the caller must check the body's own end marker.

## `oi_llm_http_parser_reusable` (src/llm_http.h)

True when the response is complete, ended by its own framing (a Content-Length, chunks, or a status with no body), and came from HTTP/1.1 with no "Connection: close": the connection can carry the next request.

## `file scope` (test/fuzz/fuzz_http.c)

libFuzzer harness for the HTTP/1.1 response parser (src/llm_http.h).

This is message framing fed straight from a socket, so the property that matters is the same one the JSON parser makes: the result must not depend on how the bytes were split across feed() calls. Each input is parsed once whole and once one byte at a time, and the two runs must agree on their terminal state, the reported status code, and the exact body bytes handed to the caller.

Chunked transfer-decoding lives behind this interface, so a framing bug that duplicates, drops, or reorders body bytes under one particular split shows up here as a body mismatch rather than as a silent corruption downstream.

## `on_body.grown` (test/fuzz/fuzz_http.c)

Keep the live pointer until realloc has succeeded, so a failure leaves the already-captured bytes intact.

## `assert` (test/fuzz/fuzz_http.c)

Headers can only complete once per response.
