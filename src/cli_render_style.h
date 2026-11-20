#ifndef OI_CLI_RENDER_STYLE_H
#define OI_CLI_RENDER_STYLE_H

#include <stddef.h>
#include <stdio.h>

#include "cli_markdown.h"
#include "oi/status.h"

oi_status oi_cli_render_style_write_line(
    FILE *out, enum oi_cli_md_block_style block_style, int heading_level,
    const char *text, size_t text_len,
    const struct oi_cli_md_span_list *spans, int *style_active);

oi_status oi_cli_render_style_write_verbatim(FILE *out, const char *text,
                                             size_t len, int dim,
                                             int *style_active);

oi_status oi_cli_render_style_reset(FILE *out, int *style_active);

#endif
