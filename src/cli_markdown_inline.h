#ifndef OI_CLI_MARKDOWN_INLINE_H
#define OI_CLI_MARKDOWN_INLINE_H

#include <stddef.h>

#include "cli_bytebuf.h"
#include "cli_markdown.h"
#include "oi/status.h"

enum oi_cli_md_delim_kind { OI_CLI_MD_DELIM_STAR, OI_CLI_MD_DELIM_UNDERSCORE };

struct oi_cli_md_delim_run {
    size_t start;
    size_t len;
    enum oi_cli_md_delim_kind kind;
    int can_open;
    int can_close;
};

struct oi_cli_md_delim_list {
    struct oi_cli_md_delim_run *runs;
    size_t len;
    size_t cap;
};

void oi_cli_md_delim_list_init(struct oi_cli_md_delim_list *list);
oi_status oi_cli_md_delim_list_append(struct oi_cli_md_delim_list *list,
                                      size_t start, size_t len,
                                      enum oi_cli_md_delim_kind kind,
                                      int can_open, int can_close);
void oi_cli_md_delim_list_free(struct oi_cli_md_delim_list *list);

oi_status oi_cli_markdown_tokenize(const char *text, size_t len,
                                   struct oi_cli_md_delim_list *out_delims);

oi_status oi_cli_markdown_inline_parse(const char *text, size_t len,
                                       struct oi_cli_bytebuf *out_text,
                                       struct oi_cli_md_span_list *out_spans);

#endif
