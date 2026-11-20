# cli_present: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_present.c`, `src/cli_present.h`, `test/test_cli_present.c`.

## `bidi_control_at` (src/cli_present.c)

The byte length of the bidirectional control character at `text` (U+061C, U+200E-U+200F, U+202A-U+202E, U+2066-U+2069), else 0.

## `write_request_text` (src/cli_present.c)

Sanitized like other untrusted text, and with each bidirectional control as a visible <U+XXXX>: such a character can make the terminal show a command in a different order than the shell runs it.

## `write_permission_request` (src/cli_present.c)

Writes what the user approves, in full and before the selector: the decoded `command` string when there is one, else the raw arguments. The terminal wraps it and keeps it in scrollback, so no length or JSON escape can hide a part of it.

## `write_model_error.header_len` (src/cli_present.c)

http_status 0: no response arrived, and the body names the connection failure.

## `oi_cli_present_event` (src/cli_present.c)

Styled (REPL) output already ends with exactly one newline per line via cli_markdown_block, including a synthesized one for a final line the model didn't itself terminate -- an unconditional newline here would add a spurious blank line. One-shot's plain passthrough has no such per-line bookkeeping, so it still needs this to guarantee the response ends on its own line.

## `oi_cli_present_init` (src/cli_present.h)

`styling_enabled` gates Markdown/SGR styling for streamed assistant text (interactive REPL: 1; one-shot: always 0, regardless of stdout's TTY-ness, so one-shot output stays plain and script-friendly). Sanitization of untrusted bytes applies unconditionally either way.

## `response_done_emits_trailing_reset_for_an_unclosed_bold_span` (test/test_cli_present.c)

Unclosed "**bold" never matched, so it renders literal; the point of this test is that RESPONSE_DONE's render_stream_finish() ran (safe/no-op here since nothing was ever actually styled) before the trailing newline.

## `turn_done_without_response_done_still_flushes_render_state` (test/test_cli_present.c)

A turn that fails/cancels before RESPONSE_DONE ever fires must still flush any pending styled line, not leave it in the markdown module's buffer forever.
