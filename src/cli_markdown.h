#ifndef OI_CLI_MARKDOWN_H
#define OI_CLI_MARKDOWN_H

#include <stddef.h>

#include "oi/status.h"

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

int oi_cli_md_span_list_covers(const struct oi_cli_md_span_list *spans,
                               size_t text_len);

#endif
