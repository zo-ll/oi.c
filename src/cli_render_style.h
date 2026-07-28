#ifndef OI_CLI_RENDER_STYLE_H
#define OI_CLI_RENDER_STYLE_H

#include <stddef.h>
#include <stdio.h>

#include "cli_markdown.h"
#include "oi/status.h"

/*
 * Turns already-inline-parsed text (delimiters already stripped by
 * cli_markdown_inline; `spans` describes style runs over `text`) into SGR-
 * styled bytes. The only module allowed to emit new style escapes -- never
 * relies on paired on/off codes (22/23/24 interact awkwardly with each
 * other and with dim); every transition into a non-default style emits a
 * full reset (\x1b[0m) then the complete codes for the new combination.
 *
 * SGR mapping (single source of truth):
 *   bold             = 1
 *   italic           = 3
 *   bold + italic    = 1;3
 *   code span        = 36 (cyan)
 *   heading level<=2 = adds bold + underline (4)
 *   heading level>=3 = adds bold only
 *   list             = no extra wrapping beyond each span's own style
 *
 * *style_active tracks whether a non-default SGR state is currently open
 * on `out`, threaded across calls so a caller (cli_render_stream) knows
 * whether a trailing oi_cli_render_style_reset is needed.
 */
oi_status oi_cli_render_style_write_line(
    FILE *out, enum oi_cli_md_block_style block_style, int heading_level,
    const char *text, size_t text_len,
    const struct oi_cli_md_span_list *spans, int *style_active);

/* Fenced code content: dim (SGR 2) while the fence is open when dim != 0,
 * plain bytes otherwise -- no inline parsing, no other styling. */
oi_status oi_cli_render_style_write_verbatim(FILE *out, const char *text,
                                             size_t len, int dim,
                                             int *style_active);

/* Emits \x1b[0m iff *style_active != 0, then clears it. Idempotent. */
oi_status oi_cli_render_style_reset(FILE *out, int *style_active);

#endif
