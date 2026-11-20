#ifndef OI_CLI_MARKDOWN_BLOCK_H
#define OI_CLI_MARKDOWN_BLOCK_H

#include <stddef.h>
#include <stdio.h>

#include "cli_bytebuf.h"
#include "oi/status.h"

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

oi_status oi_cli_markdown_block_finish(struct oi_cli_markdown_block *block,
                                       FILE *out, int *style_active);

#endif
