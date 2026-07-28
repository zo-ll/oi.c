#ifndef OI_CLI_MARKDOWN_INLINE_H
#define OI_CLI_MARKDOWN_INLINE_H

#include <stddef.h>

#include "cli_bytebuf.h"
#include "cli_markdown.h"
#include "oi/status.h"

/*
 * Delimiter-run tokenizer and CommonMark-accurate emphasis/code-span
 * matcher for one complete, bounded chunk of inline text (a paragraph, a
 * heading's text, or a list item's text -- never a fenced code block,
 * which has no inline interpretation at all).
 *
 * Deliberate, documented simplification: a delimiter run longer than 3
 * characters can never open or close emphasis (real-world model output
 * essentially never intends 4+ repeated '*'/'_' as nested emphasis; this
 * avoids implementing arbitrary partial-run consumption beyond the
 * 1/2/3-length cases CommonMark's own examples focus on).
 */
enum oi_cli_md_delim_kind { OI_CLI_MD_DELIM_STAR, OI_CLI_MD_DELIM_UNDERSCORE };

struct oi_cli_md_delim_run {
    size_t start;
    size_t len; /* original run length, including any inert (>3) tail */
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

/*
 * Tokenizes every maximal run of '*' or '_' in text[0,len) into a
 * left-to-right ordered oi_cli_md_delim_list, classifying each run's
 * can_open/can_close per CommonMark's left-/right-flanking rules and the
 * intraword-underscore exception. Does not know about code spans -- the
 * caller (oi_cli_markdown_inline_parse) is responsible for excluding runs
 * that fall inside a resolved code span before matching.
 */
oi_status oi_cli_markdown_tokenize(const char *text, size_t len,
                                   struct oi_cli_md_delim_list *out_delims);

#endif
