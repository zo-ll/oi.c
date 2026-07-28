#include "cli_markdown.h"

#include <stdint.h>
#include <stdlib.h>

void oi_cli_md_span_list_init(struct oi_cli_md_span_list *spans) {
    spans->runs = NULL;
    spans->len = 0;
    spans->cap = 0;
}

oi_status oi_cli_md_span_list_append(struct oi_cli_md_span_list *spans,
                                     size_t start, size_t len,
                                     unsigned style_bits) {
    size_t cap;
    struct oi_cli_md_run *next;

    if (spans->len == spans->cap) {
        cap = spans->cap == 0 ? 8 : spans->cap;
        if (cap > SIZE_MAX / sizeof *next / 2) {
            return OI_ERR_NOMEM;
        }
        cap *= 2;
        next = realloc(spans->runs, cap * sizeof *next);
        if (next == NULL) {
            return OI_ERR_NOMEM;
        }
        spans->runs = next;
        spans->cap = cap;
    }
    spans->runs[spans->len].start = start;
    spans->runs[spans->len].len = len;
    spans->runs[spans->len].style_bits = style_bits;
    spans->len++;
    return OI_OK;
}

void oi_cli_md_span_list_free(struct oi_cli_md_span_list *spans) {
    free(spans->runs);
    spans->runs = NULL;
    spans->len = 0;
    spans->cap = 0;
}

int oi_cli_md_span_list_covers(const struct oi_cli_md_span_list *spans,
                               size_t text_len) {
    size_t expected_start = 0;
    size_t i;

    if (spans == NULL) {
        return 0;
    }
    for (i = 0; i < spans->len; i++) {
        if (spans->runs[i].start != expected_start) {
            return 0;
        }
        if (spans->runs[i].len == 0) {
            return 0;
        }
        expected_start += spans->runs[i].len;
    }
    return expected_start == text_len;
}
