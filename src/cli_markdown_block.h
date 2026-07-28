#ifndef OI_CLI_MARKDOWN_BLOCK_H
#define OI_CLI_MARKDOWN_BLOCK_H

#include <stddef.h>
#include <stdio.h>

#include "cli_bytebuf.h"
#include "oi/status.h"

/*
 * Incremental block-level Markdown state machine: recognizes ATX
 * headings, single-level list items, and fenced code blocks at line
 * boundaries; ordinary lines are treated as one-line paragraphs. Consumes
 * already UTF-8-fixed-up, control-sanitized bytes (never raw/untrusted at
 * this layer) and drives cli_markdown_inline + cli_render_style to
 * produce the final styled bytes.
 *
 * Deliberate, documented simplification: a "paragraph" is exactly one
 * line, resolved as soon as its terminating '\n' arrives (or at cap/
 * end-of-turn for the last unterminated line) -- not accumulated across
 * multiple lines. This means an emphasis span that opens on one line and
 * closes on a later line (a real but rare CommonMark construct) is not
 * recognized; each line's inline content is matched independently. This
 * keeps chunk-boundary handling simple: no cross-line lookahead is ever
 * needed to decide whether a line continues a paragraph or starts a new
 * block.
 *
 * A line (of any kind, including fenced code content) is bounded at
 * OI_CLI_MARKDOWN_LINE_CAP bytes. Exceeding it flushes what's buffered as
 * literal, unstyled text and continues accumulating the rest of that same
 * line in a plain passthrough mode until its terminating '\n' -- bytes are
 * never dropped, only structured interpretation is skipped for the
 * overlong line.
 */
#define OI_CLI_MARKDOWN_LINE_CAP (8u * 1024u)

enum oi_cli_md_block_mode {
    OI_CLI_MD_BLOCK_NONE = 0,
    OI_CLI_MD_BLOCK_FENCE
};

struct oi_cli_markdown_block {
    struct oi_cli_bytebuf buffer;
    enum oi_cli_md_block_mode mode;
    char fence_char;
    size_t fence_len;
    int overflowed;
};

void oi_cli_markdown_block_init(struct oi_cli_markdown_block *block);
void oi_cli_markdown_block_free(struct oi_cli_markdown_block *block);

oi_status oi_cli_markdown_block_feed(struct oi_cli_markdown_block *block,
                                     const char *data, size_t len, FILE *out,
                                     int *style_active);

/* End of turn: resolves and emits any still-buffered, un-terminated final
 * line (paragraph, list item, heading, or fence content). Does not itself
 * force a trailing style reset -- that is cli_render_stream's job. */
oi_status oi_cli_markdown_block_finish(struct oi_cli_markdown_block *block,
                                       FILE *out, int *style_active);

#endif
