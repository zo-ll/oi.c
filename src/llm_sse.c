#include "llm_sse.h"

#include <stdlib.h>
#include <string.h>

#include "compat.h"

#define OI_SSE_MAX_LINE (1 << 20)

struct oi_llm_sse_parser {
    char *line_buf;
    size_t line_len;
    size_t line_cap;
    oi_llm_sse_event_cb on_event;
    void *user_data;

    /* Set by oi_llm_sse_parser_feed before invoking on_event, so a
     * reentrant oi_llm_sse_parser_destroy() from within that callback
     * (e.g. the caller cancels an in-flight request as soon as it sees
     * enough data) can signal back to feed()'s still-running loop that
     * `p` is gone. Same pattern as oi_llm_conn's destroyed_flag. */
    int *destroyed_flag;
};

oi_llm_sse_parser *oi_llm_sse_parser_create(oi_llm_sse_event_cb on_event,
                                             void *user_data) {
    oi_llm_sse_parser *p = malloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->line_cap = 256;
    p->line_buf = malloc(p->line_cap);
    if (p->line_buf == NULL) {
        free(p);
        return NULL;
    }
    p->line_len = 0;
    p->on_event = on_event;
    p->user_data = user_data;
    p->destroyed_flag = NULL;
    return p;
}

void oi_llm_sse_parser_destroy(oi_llm_sse_parser *p) {
    if (p == NULL) {
        return;
    }
    if (p->destroyed_flag) {
        *p->destroyed_flag = 1;
    }
    free(p->line_buf);
    free(p);
}

static oi_status line_append(oi_llm_sse_parser *p, char c) {
    if (p->line_len >= OI_SSE_MAX_LINE) {
        return OI_ERR_PARSE;
    }
    if (p->line_len == p->line_cap) {
        size_t new_cap = p->line_cap * 2;
        if (new_cap > OI_SSE_MAX_LINE) {
            new_cap = OI_SSE_MAX_LINE;
        }
        char *nb = realloc(p->line_buf, new_cap);
        if (nb == NULL) {
            return OI_ERR_NOMEM;
        }
        p->line_buf = nb;
        p->line_cap = new_cap;
    }
    p->line_buf[p->line_len++] = c;
    return OI_OK;
}

static void process_line(oi_llm_sse_parser *p) {
    if (p->line_len >= 5 && strncmp(p->line_buf, "data:", 5) == 0) {
        size_t start = 5;
        if (start < p->line_len && p->line_buf[start] == ' ') {
            start++;
        }
        if (p->on_event) {
            p->on_event(p->line_buf + start, p->line_len - start,
                        p->user_data);
        }
    }
}

oi_status oi_llm_sse_parser_feed(oi_llm_sse_parser *p, const void *bytes,
                                  size_t len) {
    if (p == NULL || (bytes == NULL && len > 0)) {
        return OI_ERR_INVAL;
    }

    int destroyed = 0;
OI_DIAG_PUSH_IGNORE_DANGLING
    p->destroyed_flag = &destroyed;
OI_DIAG_POP

    const unsigned char *buf = bytes;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c == '\n') {
            if (p->line_len > 0 && p->line_buf[p->line_len - 1] == '\r') {
                p->line_len--;
            }
            process_line(p);
            if (destroyed) {
                return OI_OK; /* `p` was freed by a reentrant destroy */
            }
            p->line_len = 0;
        } else {
            oi_status st = line_append(p, (char)c);
            if (st != OI_OK) {
                p->destroyed_flag = NULL;
                return st;
            }
        }
    }
    p->destroyed_flag = NULL;
    return OI_OK;
}

oi_status oi_llm_sse_parser_finish(oi_llm_sse_parser *p) {
    if (p == NULL) {
        return OI_ERR_INVAL;
    }
    if (p->line_len == 0) {
        return OI_OK;
    }

    int destroyed = 0;
OI_DIAG_PUSH_IGNORE_DANGLING
    p->destroyed_flag = &destroyed;
OI_DIAG_POP
    process_line(p);
    if (destroyed) {
        return OI_OK;
    }
    p->line_len = 0;
    p->destroyed_flag = NULL;
    return OI_OK;
}
