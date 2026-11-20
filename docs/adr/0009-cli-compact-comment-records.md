# cli_compact: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_compact.c`, `src/cli_compact.h`, `test/integration/test_cli_compact.c`, `test/test_cli_compact.c`.

## `system_text` (src/cli_compact.c)

Framing for the one-off summarization request: states plainly that the transcript is data to summarize, never instructions, so a prior adversarial tool result or model response embedded in it can't hijack the summarizer.

## `compact_on_signal` (src/cli_compact.c)

on_done already fired for this request in the same reactor step (both fds became ready together) -- drain only, don't relabel an already-finished request as cancelled.

## `oi_cli_compact_parse_turns` (src/cli_compact.h)

Parses the argument text of "/compact [turns]" -- `arguments` is exactly what oi_cli_command_parse_text produces (leading/trailing whitespace already trimmed). An empty `arguments` (len == 0) means no value was given -- *out_has_value is set to 0 and the caller applies its own default; *out_turns is untouched. Otherwise `arguments` must be one or more decimal digits and nothing else (no sign, no surrounding or embedded whitespace, no other trailing text); anything else, or a value that overflows size_t, returns OI_ERR_PARSE.

## `oi_cli_compact_select_prefix` (src/cli_compact.h)

Scans `messages` for turn boundaries -- each OI_CLI_MESSAGE_USER message starts a new turn; assistant/tool messages continue the current one, and any leading messages before the first USER message (e.g. an existing checkpoint's synthesized summary) belong to no turn and are always eligible to be swept into a new checkpoint's prefix -- to determine how large a leading prefix must be consumed to leave exactly the `keep_turns` most recent turns untouched.

*out_total_turns is always set to the number of USER-started turns found. If *out_total_turns <= keep_turns there is nothing to compact: *out_prefix_count is set to 0 (callers must treat a zero prefix as "nothing to do" and must never call oi_cli_conversation_apply_checkpoint with it). Otherwise *out_prefix_count is the number of leading messages (never zero) that must be consumed to leave exactly `keep_turns` turns behind; `keep_turns == 0` consumes the entire list.

OI_ERR_INVAL if `messages` or either output pointer is NULL.

## `oi_cli_compact_build_request` (src/cli_compact.h)

Builds a standalone chat-completion request body summarizing the leading `prefix_count` messages of `messages` -- a one-off admin request, entirely independent of any oi_cli_conversation. Carries no "tools" key (the model being asked to summarize has nothing to call). The transcript is wrapped in explicit <<<TRANSCRIPT>>>/<<<END_TRANSCRIPT>>> markers inside one ordinary JSON string, framed by a system message that states everything between those markers is data to summarize, never instructions to follow -- normal JSON string escaping already prevents any message content from breaking out of that string, so this is a hygiene framing, not a new escaping mechanism.

On success `*out_writer` is a caller-owned oi_json_writer (destroy with oi_json_writer_destroy once its data has been copied/sent).

OI_ERR_INVAL if `messages`/`model`/`out_writer` is NULL, `model_len` is 0, `prefix_count` is 0, or `prefix_count` exceeds `messages->len`.

## `oi_cli_compact_result.status` (src/cli_compact.h)

Meaningful only when outcome == OI_CLI_COMPACT_FAILED: the underlying oi_llm_request_start failure (OI_ERR_IO/_PARSE/_TIMEOUT/ _NOMEM) and HTTP status, if any was received.

## `oi_cli_compact_result.terminate_signal` (src/cli_compact.h)

0, or the SIGTERM/SIGHUP that caused OI_CLI_COMPACT_CANCELLED -- mirrors repl_turn_signal_context.terminate_signal's convention so the caller can tell "user hit Escape/Ctrl+C" (0) apart from "the whole process is unwinding" (nonzero) and act accordingly. A plain SIGINT cancels with this left at 0.

## `oi_cli_compact_result.summary` (src/cli_compact.h)

Heap-owned, NUL-terminated; valid only when outcome == OI_CLI_COMPACT_OK. Free with oi_cli_compact_result_free.

## `oi_cli_compact_run` (src/cli_compact.h)

Drives one summarization request (`body`/`body_len`, as built by oi_cli_compact_build_request) to completion via its own bounded reactor-stepping loop -- this is a synchronous, blocking call from the caller's point of view, matching cli_loop.c's one-shot loop shape.

If `signal_fd` is >= 0 (a signalfd already configured for SIGINT/ SIGTERM/SIGHUP, e.g. the REPL's own), it is registered with `reactor` for the duration of this call and removed again before returning (regardless of outcome); delivery of any of those three signals cancels the request and returns OI_CLI_COMPACT_CANCELLED rather than hanging or propagating the signal. Pass -1 to run without signal handling.

Returns OI_ERR_INVAL for invalid arguments (nothing started). Otherwise returns OI_OK and reports the actual outcome via `*out_result` -- a failed or cancelled request is not itself an error return, since the caller must always inspect `*out_result` to decide what to do next.

## `silent_server` (test/integration/test_cli_compact.c)

A listening server that accepts one connection and then goes silent -- used to hold a request in flight indefinitely so a test can control exactly when (if ever) cancellation fires, with no reliance on real response timing.

## `append_user` (test/test_cli_compact.c)

Embeds quotes, a backslash, and the literal closing delimiter -- none of this must be able to break out of the JSON string it's written into.
