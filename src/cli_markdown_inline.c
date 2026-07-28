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

/*
 * Resolves backtick code spans: a run of N backticks is closed by the
 * next run of exactly N backticks later in the text. An unmatched run is
 * literal and scanning continues after it. Marks both delimiter runs and
 * everything between them (inclusive) as `guard` -- excluded from
 * emphasis tokenizing -- and the interior alone as CODE-styled and
 * stripped-delimiter.
 */
static void resolve_code_spans(const char *text, size_t len,
                               unsigned char *stripped, unsigned *style,
                               unsigned char *guard) {
    size_t i = 0;

    while (i < len) {
        if (text[i] == '`') {
            size_t run_start = i;
            size_t run_len;
            size_t j;
            int found = 0;

            while (i < len && text[i] == '`') {
                i++;
            }
            run_len = i - run_start;

            j = i;
            while (j < len) {
                if (text[j] == '`') {
                    size_t close_start = j;
                    size_t close_len;

                    while (j < len && text[j] == '`') {
                        j++;
                    }
                    close_len = j - close_start;
                    if (close_len == run_len) {
                        size_t k;

                        for (k = run_start; k < run_start + run_len; k++) {
                            stripped[k] = 1;
                        }
                        for (k = close_start; k < close_start + run_len;
                             k++) {
                            stripped[k] = 1;
                        }
                        for (k = run_start; k < close_start + run_len; k++) {
                            guard[k] = 1;
                        }
                        for (k = run_start + run_len; k < close_start; k++) {
                            style[k] |= OI_CLI_MD_STYLE_CODE;
                        }
                        found = 1;
                        i = close_start + run_len;
                        break;
                    }
                } else {
                    j++;
                }
            }
            if (!found) {
                i = run_start + run_len;
            }
        } else {
            i++;
        }
    }
}

static oi_status stack_push(size_t **stack, size_t *stack_len,
                            size_t *stack_cap, size_t value) {
    if (*stack_len == *stack_cap) {
        size_t cap = *stack_cap == 0 ? 8 : *stack_cap;
        size_t *next;

        if (cap > SIZE_MAX / sizeof *next / 2) {
            return OI_ERR_NOMEM;
        }
        cap *= 2;
        next = realloc(*stack, cap * sizeof *next);
        if (next == NULL) {
            return OI_ERR_NOMEM;
        }
        *stack = next;
        *stack_cap = cap;
    }
    (*stack)[(*stack_len)++] = value;
    return OI_OK;
}

static int incompatible_multiple_of_three(size_t opener_len, size_t closer_len,
                                          int opener_both, int closer_both) {
    if (!opener_both && !closer_both) {
        return 0;
    }
    if ((opener_len + closer_len) % 3 != 0) {
        return 0;
    }
    return !(opener_len % 3 == 0 && closer_len % 3 == 0);
}

/*
 * Stack-based emphasis matcher (CommonMark's "process emphasis"): walks
 * filtered delimiter runs left to right, matching each closer against the
 * nearest compatible still-open opener, honoring the multiple-of-3 rule.
 * ORs BOLD/ITALIC onto `style` for the fixed interior byte range between
 * a matched pair (the range never changes across repeated partial
 * matches of the same pair, since run positions are fixed -- only how
 * many of a run's own characters are stripped changes), and marks each
 * run's final consumed characters in `stripped`.
 */
static oi_status match_emphasis(const struct oi_cli_md_delim_run *filtered,
                                size_t filtered_len, unsigned *style,
                                unsigned char *stripped) {
    size_t *remaining = NULL;
    size_t *consumed_opener = NULL;
    size_t *consumed_closer = NULL;
    size_t *stack = NULL;
    size_t stack_len = 0;
    size_t stack_cap = 0;
    oi_status status = OI_OK;
    size_t i;

    if (filtered_len == 0) {
        return OI_OK;
    }
    remaining = calloc(filtered_len, sizeof *remaining);
    consumed_opener = calloc(filtered_len, sizeof *consumed_opener);
    consumed_closer = calloc(filtered_len, sizeof *consumed_closer);
    if (remaining == NULL || consumed_opener == NULL ||
        consumed_closer == NULL) {
        status = OI_ERR_NOMEM;
        goto done;
    }
    for (i = 0; i < filtered_len; i++) {
        remaining[i] = filtered[i].len > 3 ? 0 : filtered[i].len;
    }

    for (i = 0; i < filtered_len; i++) {
        if (filtered[i].can_close) {
            while (remaining[i] > 0) {
                long found = -1;
                size_t s;

                for (s = stack_len; s > 0; s--) {
                    size_t idx = stack[s - 1];

                    if (filtered[idx].kind != filtered[i].kind ||
                        !filtered[idx].can_open || remaining[idx] == 0) {
                        continue;
                    }
                    if (incompatible_multiple_of_three(
                            filtered[idx].len, filtered[i].len,
                            filtered[idx].can_open && filtered[idx].can_close,
                            filtered[i].can_open && filtered[i].can_close)) {
                        continue;
                    }
                    found = (long)(s - 1);
                    break;
                }
                if (found < 0) {
                    break;
                }
                {
                    size_t opener_idx = stack[found];
                    size_t use = (remaining[opener_idx] >= 2 &&
                                 remaining[i] >= 2)
                                     ? 2
                                     : 1;
                    unsigned bit =
                        use == 2 ? OI_CLI_MD_STYLE_BOLD : OI_CLI_MD_STYLE_ITALIC;
                    size_t range_start =
                        filtered[opener_idx].start + filtered[opener_idx].len;
                    size_t range_end = filtered[i].start;
                    size_t k;

                    for (k = range_start; k < range_end; k++) {
                        style[k] |= bit;
                    }
                    remaining[opener_idx] -= use;
                    remaining[i] -= use;
                    consumed_opener[opener_idx] += use;
                    consumed_closer[i] += use;
                    stack_len =
                        (size_t)found + (remaining[opener_idx] > 0 ? 1 : 0);
                }
            }
            if (remaining[i] > 0 && filtered[i].can_open) {
                status = stack_push(&stack, &stack_len, &stack_cap, i);
                if (status != OI_OK) {
                    goto done;
                }
            }
        } else if (filtered[i].can_open) {
            status = stack_push(&stack, &stack_len, &stack_cap, i);
            if (status != OI_OK) {
                goto done;
            }
        }
    }

    for (i = 0; i < filtered_len; i++) {
        size_t k;

        for (k = 0; k < consumed_closer[i]; k++) {
            stripped[filtered[i].start + k] = 1;
        }
        for (k = 0; k < consumed_opener[i]; k++) {
            stripped[filtered[i].start + filtered[i].len - 1 - k] = 1;
        }
    }

done:
    free(stack);
    free(remaining);
    free(consumed_opener);
    free(consumed_closer);
    return status;
}

static oi_status compact(const char *text, size_t len,
                         const unsigned char *stripped, const unsigned *style,
                         struct oi_cli_bytebuf *out_text,
                         struct oi_cli_md_span_list *out_spans) {
    size_t i = 0;

    while (i < len) {
        size_t start;
        size_t out_start;
        unsigned bits;
        oi_status status;

        if (stripped[i]) {
            i++;
            continue;
        }
        start = i;
        bits = style[i];
        out_start = out_text->len;
        while (i < len && !stripped[i] && style[i] == bits) {
            i++;
        }
        status = oi_cli_bytebuf_append(out_text, text + start, i - start);
        if (status != OI_OK) {
            return status;
        }
        status = oi_cli_md_span_list_append(out_spans, out_start, i - start,
                                            bits);
        if (status != OI_OK) {
            return status;
        }
    }
    return OI_OK;
}

oi_status oi_cli_markdown_inline_parse(const char *text, size_t len,
                                       struct oi_cli_bytebuf *out_text,
                                       struct oi_cli_md_span_list *out_spans) {
    unsigned char *stripped = NULL;
    unsigned char *guard = NULL;
    unsigned *style = NULL;
    struct oi_cli_md_delim_run *filtered = NULL;
    struct oi_cli_md_delim_list delims;
    size_t filtered_len = 0;
    oi_status status;
    size_t i;

    if ((text == NULL && len != 0) || out_text == NULL || out_spans == NULL) {
        return OI_ERR_INVAL;
    }
    oi_cli_md_delim_list_init(&delims);
    if (len == 0) {
        return OI_OK;
    }

    stripped = calloc(len, 1);
    guard = calloc(len, 1);
    style = calloc(len, sizeof *style);
    if (stripped == NULL || guard == NULL || style == NULL) {
        status = OI_ERR_NOMEM;
        goto done;
    }

    resolve_code_spans(text, len, stripped, style, guard);

    status = oi_cli_markdown_tokenize(text, len, &delims);
    if (status != OI_OK) {
        goto done;
    }

    if (delims.len != 0) {
        filtered = malloc(delims.len * sizeof *filtered);
        if (filtered == NULL) {
            status = OI_ERR_NOMEM;
            goto done;
        }
        for (i = 0; i < delims.len; i++) {
            if (!guard[delims.runs[i].start]) {
                filtered[filtered_len++] = delims.runs[i];
            }
        }
    }

    status = match_emphasis(filtered, filtered_len, style, stripped);
    if (status != OI_OK) {
        goto done;
    }

    status = compact(text, len, stripped, style, out_text, out_spans);

done:
    free(filtered);
    free(stripped);
    free(guard);
    free(style);
    oi_cli_md_delim_list_free(&delims);
    return status;
}
