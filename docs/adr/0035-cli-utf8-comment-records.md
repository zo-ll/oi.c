# cli_utf8: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_utf8.c`, `src/cli_utf8.h`, `src/cli_utf8_stream.c`, `src/cli_utf8_stream.h`.

## `oi_cli_utf8_codepoint_width` (src/cli_utf8.c)

East Asian Wide/Fullwidth: a pared-down, documented subset covering common CJK/Hangul ranges -- not a full Unicode UAX#11 table.

## `oi_cli_utf8_lead_length` (src/cli_utf8.h)

Classifies `first` as an ASCII byte (1), or a multi-byte UTF-8 lead byte (2/3/4), without looking at or requiring any continuation bytes. Returns

OI_ERR_PARSE for any byte that can never begin a valid sequence (0x80-0xC1 stray continuation/overlong-lead, 0xF5-0xFF).

## `oi_cli_utf8_sequence_length` (src/cli_utf8.h)

Validates the UTF-8 sequence at the start of `data` and returns its byte length. Embedded U+0000 is valid here; policy about NUL belongs to callers.

## `oi_cli_utf8_next_boundary` (src/cli_utf8.h)

Moves from one code point boundary to the adjacent boundary. `offset` must be a boundary within the input. Moving beyond either end is invalid.

## `oi_cli_utf8_decode` (src/cli_utf8.h)

Decodes the UTF-8 sequence at `data` to its code point. Precondition: `len` is exactly the sequence length already validated by oi_cli_utf8_sequence_length -- like oi_cli_utf8_next_boundary's boundary precondition, this is not re-validated here.

## `oi_cli_utf8_codepoint_width` (src/cli_utf8.h)

Display-width policy for cursor/column math: 0 (combining marks and C0 control code points, which must not advance the column), 1 (the default), or 2 (wide code points -- CJK/Hangul/fullwidth ranges). A documented, testable subset of Unicode combining-mark and East Asian Width ranges, not a full UAX#11 implementation; multi-code-point grapheme clusters (ZWJ sequences, emoji modifiers) are out of scope, per REPL_PLAN.md's "full grapheme editing" exclusion.

## `feed_one` (src/cli_utf8_stream.c)

Consumes one byte. Sets *reprocess when the byte was not consumed (a pending sequence broke, and this byte must be reconsidered from a clean state) -- the caller must call this again with the same byte.

## `oi_cli_utf8_stream` (src/cli_utf8_stream.h)

Incremental UTF-8 fixer: carries a partial trailing multi-byte sequence across feed() calls and emits deterministic U+FFFD replacement for malformed or truncated input, without ever reading past the bytes it was given. Output is always well-formed UTF-8.

## `oi_cli_utf8_stream_feed` (src/cli_utf8_stream.h)

Appends well-formed UTF-8 to *out. A valid sequence split across calls is held in `pending` until it completes. A byte that can't extend what's currently held in `pending` causes one U+FFFD to be emitted for the held prefix; that new byte is then reprocessed from a clean state (it is not itself dropped unless it is independently invalid). A syntactically complete but illegal sequence (overlong/surrogate/out-of-range) also emits one U+FFFD, consuming all of its bytes.

## `oi_cli_utf8_stream_finish` (src/cli_utf8_stream.h)

End of input: if a sequence is still pending (truncated), emits exactly one U+FFFD and clears it. Safe to call more than once.
