#include "cli_markdown_inline.h"

#include <stdint.h>
#include <stdlib.h>

void oi_cli_md_delim_list_init(struct oi_cli_md_delim_list *list) {
    list->runs = NULL;
    list->len = 0;
    list->cap = 0;
}

oi_status oi_cli_md_delim_list_append(struct oi_cli_md_delim_list *list,
                                      size_t start, size_t len,
                                      enum oi_cli_md_delim_kind kind,
                                      int can_open, int can_close) {
    size_t cap;
    struct oi_cli_md_delim_run *next;

    if (list->len == list->cap) {
        cap = list->cap == 0 ? 8 : list->cap;
        if (cap > SIZE_MAX / sizeof *next / 2) {
            return OI_ERR_NOMEM;
        }
        cap *= 2;
        next = realloc(list->runs, cap * sizeof *next);
        if (next == NULL) {
            return OI_ERR_NOMEM;
        }
        list->runs = next;
        list->cap = cap;
    }
    list->runs[list->len].start = start;
    list->runs[list->len].len = len;
    list->runs[list->len].kind = kind;
    list->runs[list->len].can_open = can_open;
    list->runs[list->len].can_close = can_close;
    list->len++;
    return OI_OK;
}

void oi_cli_md_delim_list_free(struct oi_cli_md_delim_list *list) {
    free(list->runs);
    list->runs = NULL;
    list->len = 0;
    list->cap = 0;
}

enum char_class { CLASS_WHITESPACE, CLASS_PUNCTUATION, CLASS_OTHER };

/* ASCII whitespace and ASCII punctuation classification. Bytes >= 0x80
 * (any multi-byte UTF-8 sequence) classify as OTHER -- a documented
 * simplification of CommonMark's full Unicode whitespace/punctuation
 * tables, adequate for the agreed minimal subset. */
static enum char_class classify_byte(unsigned char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
        c == '\v') {
        return CLASS_WHITESPACE;
    }
    if ((c >= 0x21U && c <= 0x2fU) || (c >= 0x3aU && c <= 0x40U) ||
        (c >= 0x5bU && c <= 0x60U) || (c >= 0x7bU && c <= 0x7eU)) {
        return CLASS_PUNCTUATION;
    }
    return CLASS_OTHER;
}

/* Start/end of the text counts as whitespace, per CommonMark. */
static enum char_class classify_before(const char *text, size_t start) {
    if (start == 0) {
        return CLASS_WHITESPACE;
    }
    return classify_byte((unsigned char)text[start - 1]);
}

static enum char_class classify_after(const char *text, size_t len,
                                      size_t end) {
    if (end >= len) {
        return CLASS_WHITESPACE;
    }
    return classify_byte((unsigned char)text[end]);
}

oi_status oi_cli_markdown_tokenize(const char *text, size_t len,
                                   struct oi_cli_md_delim_list *out_delims) {
    size_t i = 0;

    if ((text == NULL && len != 0) || out_delims == NULL) {
        return OI_ERR_INVAL;
    }

    while (i < len) {
        unsigned char c = (unsigned char)text[i];

        if (c == '*' || c == '_') {
            size_t start = i;
            enum oi_cli_md_delim_kind kind =
                c == '*' ? OI_CLI_MD_DELIM_STAR : OI_CLI_MD_DELIM_UNDERSCORE;
            size_t run_len;
            enum char_class before, after;
            int preceded_ws, preceded_punct, followed_ws, followed_punct;
            int left_flank, right_flank, can_open, can_close;
            oi_status status;

            while (i < len && (unsigned char)text[i] == c) {
                i++;
            }
            run_len = i - start;

            before = classify_before(text, start);
            after = classify_after(text, len, start + run_len);
            preceded_ws = before == CLASS_WHITESPACE;
            preceded_punct = before == CLASS_PUNCTUATION;
            followed_ws = after == CLASS_WHITESPACE;
            followed_punct = after == CLASS_PUNCTUATION;

            left_flank = !followed_ws &&
                         (!followed_punct || preceded_ws || preceded_punct);
            right_flank = !preceded_ws &&
                          (!preceded_punct || followed_ws || followed_punct);

            if (run_len > 3) {
                can_open = 0;
                can_close = 0;
            } else if (kind == OI_CLI_MD_DELIM_STAR) {
                can_open = left_flank;
                can_close = right_flank;
            } else {
                can_open = left_flank && (!right_flank || preceded_punct);
                can_close = right_flank && (!left_flank || followed_punct);
            }

            status = oi_cli_md_delim_list_append(out_delims, start, run_len,
                                                 kind, can_open, can_close);
            if (status != OI_OK) {
                return status;
            }
        } else {
            i++;
        }
    }
    return OI_OK;
}
