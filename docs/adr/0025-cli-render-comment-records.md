# cli_render: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_render.c`, `src/cli_render.h`, `src/cli_render_sanitize.c`, `src/cli_render_sanitize.h`, `src/cli_render_stream.h`, `src/cli_render_style.h`, `test/test_cli_render.c`, `test/test_cli_render_stream.c`.

## `advance_codepoint` (src/cli_render.c)

Advances by one code point's display width, treating it as atomic: a wide (width 2) glyph is never split across a row boundary -- if it doesn't fully fit in the columns remaining, wrap to a fresh row first, then place it there. Unlike advance_columns (used for the "> " prompt's byte-run, where splitting plain ASCII across a boundary is harmless), this must never leave half a glyph's cells on one row and half on the next.

## `render_draw` (src/cli_render.c)

The repositioning above always leaves the physical cursor at `cursor.row`, not `end.row` (which can be further down whenever a command menu is showing, or the edit cursor sits before wrapped trailing content) -- previous_rows must track where the cursor actually ends up, or the next draw's clear step overshoots past this frame's top row and erases real prior terminal content. header_count is a uniform offset added to every row position build_frame computed relative to the first editor row, so it's added in only here, once.

## `oi_cli_render_draw_selector` (src/cli_render.c)

No trailing "\r\n" after the last option: the physical cursor must end up on the frame's last row (row_count - 1 once this loop finishes), matching what previous_rows below claims, so the next erase's cursor-up count is correct.

## `oi_cli_render` (src/cli_render.h)

ANSI prompt renderer. It borrows the output descriptor and remembers only the number of rows occupied by its previous frame.

## `oi_cli_render_line` (src/cli_render.h)

One pre-formatted, single-row line of text -- the caller is responsible for bounding/wrapping/sanitizing it before handing it here (the same simplifying assumption oi_cli_render_draw_commands already makes for its own menu entries).

## `oi_cli_render_draw_panel` (src/cli_render.h)

Like oi_cli_render_draw_commands, but with `header_lines` drawn above the prompt/editor content -- e.g. a live tool-execution panel. Row-count bookkeeping: header rows are written before the normal frame (which is unaware of them and computes cursor positions exactly as it does today) and simply added into the final previous_rows: the cursor-repositioning math only ever depends on the *delta* between the edit cursor's row and the frame's last row, which a uniform header offset added to both doesn't change.

## `oi_cli_render_draw_selector` (src/cli_render.h)

A self-contained modal frame -- header_lines followed by selectable option_lines (selected_option marked "> ", others "  ", mirroring oi_cli_render_draw_commands' marker convention) -- with no oi_cli_editor involved at all: nothing being edited is drawn or implied while this is showing. Composes with oi_cli_render_erase exactly like the other draw functions (same previous_rows bookkeeping); a later oi_cli_render_draw/_draw_panel call restores the caller's in-progress draft untouched, since this never reads or writes any editor state.

## `oi_cli_render_erase` (src/cli_render.h)

Clears the previously drawn frame (if any) without drawing a new one, leaving the physical cursor at the frame's top row and previous_rows at 0 -- the erase half of oi_cli_render_draw, split out so a caller that needs to write other content to the same fd between "clear the old frame" and "draw a new one" (e.g. a turn's streamed output landing between keystrokes) can do so without the frame and that other content corrupting each other's row-count assumptions. A no-op if no frame is currently drawn (previous_rows == 0).

## `feed_one` (src/cli_render_sanitize.c)

Consumes one byte. Sets *reprocess when the byte was not consumed (a held 0xC2 turned out not to be a C1 lead) -- the caller must call this again with the same byte, now that state has advanced.

## `oi_cli_sanitize_phase` (src/cli_render_sanitize.h)

Default-deny terminal control-byte/escape-sequence stripper. Assumes input is already valid UTF-8 (run oi_cli_utf8_stream first). Strips C0 controls other than '\n'/'\t' (including bare '\r' and NUL), the C1 control range encoded as UTF-8 (0xC2 0x80-0xC2 0x9F), and every ESC-introduced sequence (CSI, OSC/DCS/SOS/PM/APC control strings, and simple two-byte escapes) unconditionally -- never an allowlist of "safe" subtypes. Everything else passes through unchanged.

## `OI_CLI_SANITIZE_MAX_SEQUENCE` (src/cli_render_sanitize.h)

Bound on how many bytes an in-flight escape/control-string sequence may swallow before it is abandoned and scanning resumes in NORMAL. Guards against a malformed or never-terminated sequence forcing unbounded suppression of subsequent legitimate text.

## `oi_cli_sanitize_finish` (src/cli_render_sanitize.h)

End of input: any in-flight, never-forwarded sequence (including a held 0xC2 lead byte awaiting its C1 lookahead) is simply abandoned; nothing is appended. Safe to call more than once.

## `oi_cli_render_stream` (src/cli_render_stream.h)

Per-turn facade wiring the full untrusted-output pipeline: raw bytes -> incremental UTF-8 fixup -> control/escape sanitization -> (styling enabled ? Markdown block/inline parsing + SGR styling : verbatim passthrough) -> `out`. This is the one thing cli_present.c calls for streamed assistant text.

Sanitization always applies. Markdown styling applies only when `styling_enabled` was set at init (the interactive REPL path); one-shot mode always passes 0, regardless of whether its stdout happens to be a TTY, so its output stays plain and script-friendly.

## `oi_cli_render_stream_finish` (src/cli_render_stream.h)

Flushes any carried UTF-8/sanitizer/Markdown state and guarantees a trailing SGR reset if a style was left open -- so a turn that ends (successfully or not) mid-bold/mid-fence can never leave the terminal in a styled state. Idempotent: safe to call more than once.

## `oi_cli_render_sanitize_write` (src/cli_render_stream.h)

Convenience for a complete, non-chunked untrusted string (a tool name, a model-error body): runs the same UTF-8-fixup-then-sanitize pipeline in one shot and writes the result to `out`. Never styled, regardless of any cli_render_stream's styling_enabled -- this is not part of the assistant-text stream. No persistent state to carry between calls.

## `oi_cli_render_style_write_line` (src/cli_render_style.h)

Turns already-inline-parsed text (delimiters already stripped by cli_markdown_inline; `spans` describes style runs over `text`) into SGR- styled bytes. The only module allowed to emit new style escapes -- never relies on paired on/off codes (22/23/24 interact awkwardly with each other and with dim); every transition into a non-default style emits a full reset (\x1b[0m) then the complete codes for the new combination.

SGR mapping (single source of truth): bold             = 1 italic           = 3 bold + italic    = 1;3 code span        = 36 (cyan) heading level<=2 = adds bold + underline (4) heading level>=3 = adds bold only list             = no extra wrapping beyond each span's own style

*style_active tracks whether a non-default SGR state is currently open on `out`, threaded across calls so a caller (cli_render_stream) knows whether a trailing oi_cli_render_style_reset is needed.

## `oi_cli_render_style_write_verbatim` (src/cli_render_style.h)

Fenced code content: dim (SGR 2) while the fence is open when dim != 0, plain bytes otherwise -- no inline parsing, no other styling.

## `ascii_wraps_at_the_exact_column_boundary.expected` (test/test_cli_render.c)

columns=4: "> " occupies col 0-1, leaving 2 columns of content per row. "abc" fills "a","b" on row 0 (cols 2,3) and wraps "c" to row 1 (col 0) purely via the terminal's own line-wrap -- no explicit escape is emitted for a natural wrap, only the internal cursor bookkeeping changes.

## `a_wide_code_point_wraps_whole_rather_than_splitting.expected` (test/test_cli_render.c)

columns=5: "> " leaves 3 content columns on row 0. "a","b" use 2 of them; the wide (2-column) CJK glyph U+4E2D only has 1 column left, so it must wrap to row 1 whole rather than splitting its cells across the boundary.

## `command_menu_is_rendered_below_the_prompt` (test/test_cli_render.c)

The physical cursor is repositioned back up to the prompt row (row 0 here) before this call returns, not left on the menu's last row -- previous_rows must track where the cursor actually ends up, not the frame's total height, or the next draw's clear step overshoots.

## `second_draw_after_menu_does_not_overshoot_the_clear` (test/test_cli_render.c)

previous_rows was 1 (not > 1) after the first draw, so this clear prefix must be exactly "\r" + "\x1b[J" -- no cursor-up escape at all. Before the previous_rows fix, the first draw would have (wrongly) stored 3, and this clear would have emitted "\r\x1b[2A\x1b[J", moving two rows above this frame's actual top and erasing whatever preceded it on a real terminal.

## `erase_clears_a_multiline_frame_and_resets_previous_rows` (test/test_cli_render.c)

Erasing again immediately (nothing redrawn in between) is a no-op -- previous_rows is already 0, matching the "already erased" case a caller hits if a turn produces no output between two reactor steps.

## `unclosed_bold_at_finish_is_literal_plus_safety_reset` (test/test_cli_render_stream.c)

No closing "**" ever arrived: the whole thing stays literal text, finish() still guarantees a trailing reset call happened (a no-op here since nothing was ever actually styled).
