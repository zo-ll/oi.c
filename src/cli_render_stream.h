#ifndef OI_CLI_RENDER_STREAM_H
#define OI_CLI_RENDER_STREAM_H

#include <stddef.h>
#include <stdio.h>

#include "cli_bytebuf.h"
#include "cli_markdown_block.h"
#include "cli_render_sanitize.h"
#include "cli_utf8_stream.h"
#include "oi/status.h"

/*
 * Per-turn facade wiring the full untrusted-output pipeline: raw bytes ->
 * incremental UTF-8 fixup -> control/escape sanitization -> (styling
 * enabled ? Markdown block/inline parsing + SGR styling : verbatim
 * passthrough) -> `out`. This is the one thing cli_present.c calls for
 * streamed assistant text.
 *
 * Sanitization always applies. Markdown styling applies only when
 * `styling_enabled` was set at init (the interactive REPL path); one-shot
 * mode always passes 0, regardless of whether its stdout happens to be a
 * TTY, so its output stays plain and script-friendly.
 */
struct oi_cli_render_stream {
    FILE *out;
    int styling_enabled;
    struct oi_cli_utf8_stream utf8;
    struct oi_cli_sanitize_state sanitize;
    struct oi_cli_markdown_block markdown;
    struct oi_cli_bytebuf fixed;
    struct oi_cli_bytebuf sanitized;
    int style_active;
};

oi_status oi_cli_render_stream_init(struct oi_cli_render_stream *stream,
                                    FILE *out, int styling_enabled);

oi_status oi_cli_render_stream_feed(struct oi_cli_render_stream *stream,
                                    const char *data, size_t len);

/*
 * Flushes any carried UTF-8/sanitizer/Markdown state and guarantees a
 * trailing SGR reset if a style was left open -- so a turn that ends
 * (successfully or not) mid-bold/mid-fence can never leave the terminal
 * in a styled state. Idempotent: safe to call more than once.
 */
oi_status oi_cli_render_stream_finish(struct oi_cli_render_stream *stream);

void oi_cli_render_stream_free(struct oi_cli_render_stream *stream);

/*
 * Convenience for a complete, non-chunked untrusted string (a tool name,
 * a model-error body): runs the same UTF-8-fixup-then-sanitize pipeline
 * in one shot and writes the result to `out`. Never styled, regardless of
 * any cli_render_stream's styling_enabled -- this is not part of the
 * assistant-text stream. No persistent state to carry between calls.
 */
oi_status oi_cli_render_sanitize_write(FILE *out, const char *data,
                                       size_t len);

#endif
