#ifndef OI_CLI_TOOL_PANEL_H
#define OI_CLI_TOOL_PANEL_H

#include <stddef.h>

#include "cli_bytebuf.h"
#include "cli_render.h"
#include "cli_render_sanitize.h"
#include "cli_utf8_stream.h"
#include "oi/status.h"

/* Bound on the sanitized output tail kept for display -- once exceeded,
 * whole lines are trimmed from the front (never mid-line) until back
 * under bound. Independent of the raw/audit copy kept elsewhere
 * (cli_history's has_raw_tool_output field), which is never bounded or
 * sanitized -- this bound applies only to what's ever put on screen. */
#define OI_CLI_TOOL_PANEL_MAX_BYTES 4096u
/* Total render_line entries oi_cli_tool_panel_lines ever returns,
 * including the header. */
#define OI_CLI_TOOL_PANEL_MAX_LINES 6u
#define OI_CLI_TOOL_PANEL_NAME_MAX 128u
#define OI_CLI_TOOL_PANEL_HEADER_MAX 160u

enum oi_cli_tool_panel_status {
    OI_CLI_TOOL_PANEL_RUNNING = 0,
    OI_CLI_TOOL_PANEL_COMPLETED,
    OI_CLI_TOOL_PANEL_FAILED,
    OI_CLI_TOOL_PANEL_DENIED,
    OI_CLI_TOOL_PANEL_CANCELLED
};

/*
 * Bounded, sanitized presentation state for one tool call's live output --
 * owned by the REPL turn context, rebuilt (via _start) each time a new
 * call begins. Raw bytes are threaded through oi_cli_utf8_stream
 * (incremental UTF-8 fixup) then oi_cli_sanitize_feed (the same
 * control-byte/escape-sequence stripper write_tool_start already uses)
 * before ever reaching `tail`, so untrusted tool output can never inject
 * terminal escapes into the display -- this is the "malicious terminal
 * bytes in tool output" defense the render layer relies on.
 */
struct oi_cli_tool_panel {
    char name[OI_CLI_TOOL_PANEL_NAME_MAX];
    size_t name_len;
    enum oi_cli_tool_panel_status status;
    struct oi_cli_utf8_stream utf8;
    struct oi_cli_sanitize_state sanitize;
    struct oi_cli_bytebuf tail;
    /* Rebuilt fresh by oi_cli_tool_panel_lines on every call (entirely
     * pull-based) -- backing storage for the header line it returns. */
    char header[OI_CLI_TOOL_PANEL_HEADER_MAX];
    size_t header_len;
    /* False = nothing to draw (no call in flight, and _clear was called
     * since the last one finished). */
    int active;
};

void oi_cli_tool_panel_init(struct oi_cli_tool_panel *panel);
void oi_cli_tool_panel_free(struct oi_cli_tool_panel *panel);

/* Starts fresh presentation state for one call, sanitizing `name`
 * immediately (bounded to OI_CLI_TOOL_PANEL_NAME_MAX) since it's
 * untrusted, model-supplied text. */
void oi_cli_tool_panel_start(struct oi_cli_tool_panel *panel,
                             const char *name, size_t name_len);
oi_status oi_cli_tool_panel_feed(struct oi_cli_tool_panel *panel,
                                 const void *data, size_t len);
void oi_cli_tool_panel_finish(struct oi_cli_tool_panel *panel,
                              enum oi_cli_tool_panel_status status);
/* Resets to inactive/empty, ready for oi_cli_tool_panel_start to reuse --
 * called once a finished panel's frame is no longer needed on screen
 * (e.g. at the next turn boundary). */
void oi_cli_tool_panel_clear(struct oi_cli_tool_panel *panel);

/*
 * Writes up to `max_lines` render_line entries into `out_lines`: a
 * "<name>: <status>" header first, then as many of the most recent
 * sanitized output lines as fit -- returns the count actually written
 * (always >= 1 while active, since the header alone always fits).
 * Backing storage (panel->header, panel->tail.data) is owned by `panel`
 * and stays valid only until the next mutating call on it.
 */
size_t oi_cli_tool_panel_lines(struct oi_cli_tool_panel *panel,
                               struct oi_cli_render_line *out_lines,
                               size_t max_lines);

#endif
