# llm_sse: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/llm_sse.c`, `src/llm_sse.h`, `test/fuzz/fuzz_sse.c`.

## `oi_llm_sse_parser.destroyed_flag` (src/llm_sse.c)

Set by oi_llm_sse_parser_feed before invoking on_event, so a reentrant oi_llm_sse_parser_destroy() from within that callback (e.g. the caller cancels an in-flight request as soon as it sees enough data) can signal back to feed()'s still-running loop that `p` is gone. Same pattern as oi_llm_conn's destroyed_flag.

## `oi_llm_sse_parser` (src/llm_sse.h)

Splits a byte stream (an HTTP response body) into Server-Sent-Events "data:" payloads. Scoped to the single-data-line-per-event form used by OpenAI-compatible streaming APIs -- not the full SSE spec (no multi-line data-field joining, no id:/event:/retry: fields). Anything that isn't a "data:" line (comments, blank event separators, other fields) is silently ignored, matching SSE's own fault-tolerant design.

## `oi_llm_sse_parser_finish` (src/llm_sse.h)

Confirms end-of-input and dispatches a final non-empty line even when it was not newline-terminated. Safe to call after any number of feeds.

## `file scope` (test/fuzz/fuzz_sse.c)

libFuzzer harness for the SSE event splitter (src/llm_sse.h).

The last framing layer between raw socket bytes and the JSON parser, and the one most exposed to arbitrary server output. As with the HTTP and JSON harnesses, the invariant under test is chunk-independence: the same bytes split differently across feed() calls must yield the same sequence of events.

Events are captured length-prefixed so the comparison covers event *boundaries*, not just the concatenated payload -- splitting one event into two (or merging two into one) is exactly the kind of framing bug a boundary-blind comparison would let through.
