#ifndef OI_CLI_RENDER_STREAM_H
#define OI_CLI_RENDER_STREAM_H

#include <stddef.h>
#include <stdio.h>

#include "cli_bytebuf.h"
#include "cli_markdown_block.h"
#include "cli_render_sanitize.h"
#include "cli_utf8_stream.h"
#include "oi/status.h"

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

oi_status oi_cli_render_stream_finish(struct oi_cli_render_stream *stream);

void oi_cli_render_stream_free(struct oi_cli_render_stream *stream);

oi_status oi_cli_render_sanitize_write(FILE *out, const char *data,
                                       size_t len);

#endif
