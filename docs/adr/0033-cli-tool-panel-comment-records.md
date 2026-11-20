# cli_tool_panel: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_tool_panel.c`, `src/cli_tool_panel.h`.

## `trim_tail_to_bound` (src/cli_tool_panel.c)

No newline anywhere in the excess -- one pathologically long line; drop it all rather than leave something still over the bound.

## `oi_cli_tool_panel_lines` (src/cli_tool_panel.c)

Find the start of the last content_lines_wanted lines by scanning backward for that many newlines -- excluding a bare trailing '\n' from consideration first (the common case, since every fed chunk typically ends with one): it terminates the last line rather than separating it from a line after it, so counting it here would wrongly stop one line short of the one actually wanted.

## `OI_CLI_TOOL_PANEL_MAX_BYTES` (src/cli_tool_panel.h)

Bound on the sanitized output tail kept for display -- once exceeded, whole lines are trimmed from the front (never mid-line) until back under bound. Independent of the raw/audit copy kept elsewhere (cli_history's has_raw_tool_output field), which is never bounded or sanitized -- this bound applies only to what's ever put on screen.

## `OI_CLI_TOOL_PANEL_MAX_LINES` (src/cli_tool_panel.h)

Total render_line entries oi_cli_tool_panel_lines ever returns, including the header.

## `oi_cli_tool_panel` (src/cli_tool_panel.h)

Bounded, sanitized presentation state for one tool call's live output -- owned by the REPL turn context, rebuilt (via _start) each time a new call begins. Raw bytes are threaded through oi_cli_utf8_stream (incremental UTF-8 fixup) then oi_cli_sanitize_feed (the same control-byte/escape-sequence stripper write_tool_start already uses) before ever reaching `tail`, so untrusted tool output can never inject terminal escapes into the display -- this is the "malicious terminal bytes in tool output" defense the render layer relies on.

## `oi_cli_tool_panel.tool_call_id` (src/cli_tool_panel.h)

Exact, non-display identity of the call currently represented. Tool-result events are matched against it so a skipped sibling call cannot overwrite the status of the panel for the call that ran.

## `oi_cli_tool_panel.header` (src/cli_tool_panel.h)

Rebuilt fresh by oi_cli_tool_panel_lines on every call (entirely pull-based) -- backing storage for the header line it returns.

## `oi_cli_tool_panel.active` (src/cli_tool_panel.h)

False = nothing to draw (no call in flight, and _clear was called since the last one finished).

## `oi_cli_tool_panel_start` (src/cli_tool_panel.h)

Starts fresh presentation state for one call, sanitizing `name` immediately (bounded to OI_CLI_TOOL_PANEL_NAME_MAX) since it's untrusted, model-supplied text.

## `oi_cli_tool_panel_clear` (src/cli_tool_panel.h)

Resets to inactive/empty, ready for oi_cli_tool_panel_start to reuse -- called once a finished panel's frame is no longer needed on screen (e.g. at the next turn boundary).

## `oi_cli_tool_panel_lines` (src/cli_tool_panel.h)

Writes up to `max_lines` render_line entries into `out_lines`: a "<name>: <status>" header first, then as many of the most recent sanitized output lines as fit -- returns the count actually written (always >= 1 while active, since the header alone always fits). Backing storage (panel->header, panel->tail.data) is owned by `panel` and stays valid only until the next mutating call on it.
