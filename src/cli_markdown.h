#ifndef OI_CLI_MARKDOWN_H
#define OI_CLI_MARKDOWN_H

#include <stddef.h>

#include "oi/status.h"

/*
 * Flat inline style spans shared by the Markdown inline matcher, the block
 * state machine, and the SGR renderer. BOLD/ITALIC may combine; CODE is
 * mutually exclusive with both (a code span's interior is never further
 * interpreted for emphasis).
 */
enum oi_cli_md_style_bits {
    OI_CLI_MD_STYLE_BOLD = 1u << 0,
    OI_CLI_MD_STYLE_ITALIC = 1u << 1,
    OI_CLI_MD_STYLE_CODE = 1u << 2
};

struct oi_cli_md_run {
    size_t start;
    size_t len;
    unsigned style_bits;
};

struct oi_cli_md_span_list {
    struct oi_cli_md_run *runs;
    size_t len;
    size_t cap;
};

/*
 * Which block construct produced a line handed to the style renderer.
 * Shared between cli_markdown_block (the producer) and cli_render_style
 * (the consumer) so neither depends on the other's header.
 */
enum oi_cli_md_block_style {
    OI_CLI_MD_BLOCK_STYLE_NONE = 0,
    OI_CLI_MD_BLOCK_STYLE_HEADING,
    OI_CLI_MD_BLOCK_STYLE_LIST
};

void oi_cli_md_span_list_init(struct oi_cli_md_span_list *spans);
oi_status oi_cli_md_span_list_append(struct oi_cli_md_span_list *spans,
                                     size_t start, size_t len,
                                     unsigned style_bits);
void oi_cli_md_span_list_free(struct oi_cli_md_span_list *spans);

/*
 * Debug/test invariant check: true iff runs are sorted by start, mutually
 * non-overlapping, and together cover exactly [0, text_len) with no gaps.
 */
int oi_cli_md_span_list_covers(const struct oi_cli_md_span_list *spans,
                               size_t text_len);

#endif
