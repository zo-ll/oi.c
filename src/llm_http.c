#include "llm_http.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat.h"

#define OI_HTTP_MAX_LINE 8192

/*
 * Upper bound on a Content-Length or chunk-size value. These are digit
 * strings from an untrusted response header, and accumulating them
 * unbounded overflows the signed accumulator -- undefined behavior, and
 * in practice a negative `remaining` that makes the body reader treat
 * the length as effectively unlimited. Anything above this is rejected
 * as malformed instead; it is orders of magnitude beyond any real
 * chat-completion response, so a legitimate server never trips it.
 */
#define OI_HTTP_MAX_BODY_VALUE ((long)1 << 40) /* 1 TiB */

enum state {
    ST_STATUS_LINE,
    ST_HEADER_LINE,
    ST_CHUNK_SIZE_LINE,
    ST_CHUNK_DATA,
    ST_CHUNK_DATA_CRLF,
    ST_FINAL_CRLF,
    ST_CONTENT_BODY,
    ST_UNTIL_CLOSE_BODY,
    ST_DONE,
    ST_ERROR
};

struct oi_llm_http_parser {
    enum state state;

    char *line_buf;
    size_t line_len;
    size_t line_cap;

    int status_code;
    int keep_alive; /* HTTP/1.1 with no "Connection: close" */
    int framed;     /* the body ends by its own framing, not by a close */
    int is_chunked;
    long content_length; /* -1 = absent */
    long remaining;      /* bytes left in the current chunk / content body */

    oi_llm_http_headers_done_cb on_headers_done;
    oi_llm_http_body_cb on_body;
    void *user_data;

    /* Set by oi_llm_http_parser_feed before invoking on_headers_done or
     * on_body, so a reentrant oi_llm_http_parser_destroy() from within
     * one of those (e.g. the caller cancels an in-flight request as
     * soon as it sees enough data) can signal back to feed()'s
     * still-running loop that `p` is gone. */
    int *destroyed_flag;
};

oi_llm_http_parser *oi_llm_http_parser_create(
    oi_llm_http_headers_done_cb on_headers_done, oi_llm_http_body_cb on_body,
    void *user_data) {
    oi_llm_http_parser *p = malloc(sizeof *p);
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
    p->state = ST_STATUS_LINE;
    p->status_code = 0;
    p->keep_alive = 0;
    p->framed = 0;
    p->is_chunked = 0;
    p->content_length = -1;
    p->remaining = 0;
    p->on_headers_done = on_headers_done;
    p->on_body = on_body;
    p->user_data = user_data;
    p->destroyed_flag = NULL;
    return p;
}

void oi_llm_http_parser_destroy(oi_llm_http_parser *p) {
    if (p == NULL) {
        return;
    }
    if (p->destroyed_flag) {
        *p->destroyed_flag = 1;
    }
    free(p->line_buf);
    free(p);
}

oi_status oi_llm_http_parser_close(oi_llm_http_parser *p) {
    if (p == NULL || p->state != ST_UNTIL_CLOSE_BODY) {
        return OI_ERR_CLOSED;
    }
    p->state = ST_DONE;
    return OI_OK;
}

int oi_llm_http_parser_body_done(const oi_llm_http_parser *p) {
    return p != NULL && p->state == ST_DONE;
}

int oi_llm_http_parser_reusable(const oi_llm_http_parser *p) {
    return p != NULL && p->state == ST_DONE && p->framed && p->keep_alive;
}

int oi_llm_http_parser_failed(const oi_llm_http_parser *p) {
    return p != NULL && p->state == ST_ERROR;
}

static int is_digit_c(char c) { return c >= '0' && c <= '9'; }

static oi_status line_append(oi_llm_http_parser *p, char c) {
    if (p->line_len >= OI_HTTP_MAX_LINE) {
        p->state = ST_ERROR;
        return OI_ERR_PARSE;
    }
    if (p->line_len == p->line_cap) {
        size_t new_cap = p->line_cap * 2;
        if (new_cap > OI_HTTP_MAX_LINE) {
            new_cap = OI_HTTP_MAX_LINE;
        }
        char *nb = realloc(p->line_buf, new_cap);
        if (nb == NULL) {
            p->state = ST_ERROR;
            return OI_ERR_NOMEM;
        }
        p->line_buf = nb;
        p->line_cap = new_cap;
    }
    p->line_buf[p->line_len++] = c;
    return OI_OK;
}

static oi_status handle_status_line(oi_llm_http_parser *p) {
    if (p->line_len < 12 ||
        (memcmp(p->line_buf, "HTTP/1.1 ", 9) != 0 &&
         memcmp(p->line_buf, "HTTP/1.0 ", 9) != 0)) {
        p->state = ST_ERROR;
        return OI_ERR_PARSE;
    }
    size_t i = 9;
    if (!is_digit_c(p->line_buf[i]) ||
        !is_digit_c(p->line_buf[i + 1]) || !is_digit_c(p->line_buf[i + 2])) {
        p->state = ST_ERROR;
        return OI_ERR_PARSE;
    }
    if (i + 3 < p->line_len && p->line_buf[i + 3] != ' ') {
        p->state = ST_ERROR;
        return OI_ERR_PARSE;
    }
    p->status_code = (p->line_buf[i] - '0') * 100 +
                      (p->line_buf[i + 1] - '0') * 10 +
                      (p->line_buf[i + 2] - '0');
    p->keep_alive = memcmp(p->line_buf, "HTTP/1.1 ", 9) == 0;
    p->state = ST_HEADER_LINE;
    return OI_OK;
}

static oi_status finish_headers(oi_llm_http_parser *p) {
    if (p->is_chunked && p->content_length >= 0) {
        p->state = ST_ERROR;
        return OI_ERR_PARSE;
    }
    p->framed = 1;
    if (p->is_chunked) {
        p->state = ST_CHUNK_SIZE_LINE;
    } else if (p->content_length >= 0) {
        if (p->content_length == 0) {
            p->state = ST_DONE;
        } else {
            p->remaining = p->content_length;
            p->state = ST_CONTENT_BODY;
        }
    } else if (p->status_code < 200 || p->status_code == 204 ||
               p->status_code == 304) {
        p->state = ST_DONE; /* these statuses never carry a body */
    } else {
        /* Neither chunked nor Content-Length: the body ends when the
         * peer closes the connection (oi_llm_http_parser_close). */
        p->framed = 0;
        p->state = ST_UNTIL_CLOSE_BODY;
    }
    if (p->on_headers_done) {
        p->on_headers_done(p->status_code, p->user_data);
    }
    return OI_OK;
}

static oi_status handle_header_line(oi_llm_http_parser *p) {
    if (p->line_len == 0) {
        return finish_headers(p);
    }

    size_t colon = 0;
    while (colon < p->line_len && p->line_buf[colon] != ':') {
        colon++;
    }
    if (colon == 0 || colon >= p->line_len) {
        p->state = ST_ERROR;
        return OI_ERR_PARSE;
    }
    size_t name_len = colon;
    size_t vi = colon + 1;
    size_t value_end = p->line_len;
    /* Optional white space around a field value is not part of it. */
    while (vi < value_end &&
           (p->line_buf[vi] == ' ' || p->line_buf[vi] == '\t')) {
        vi++;
    }
    while (value_end > vi && (p->line_buf[value_end - 1] == ' ' ||
                              p->line_buf[value_end - 1] == '\t')) {
        value_end--;
    }
    size_t value_len = value_end - vi;

    if (name_len == 17 &&
        strncasecmp(p->line_buf, "Transfer-Encoding", 17) == 0) {
        if (value_len != 7 ||
            strncasecmp(p->line_buf + vi, "chunked", 7) != 0) {
            p->state = ST_ERROR;
            return OI_ERR_PARSE;
        }
        p->is_chunked = 1;
    } else if (name_len == 14 &&
               strncasecmp(p->line_buf, "Content-Length", 14) == 0) {
        if (value_len == 0) {
            p->state = ST_ERROR; /* header present but with no digits */
            return OI_ERR_PARSE;
        }
        long v = 0;
        for (size_t k = vi; k < value_end; k++) {
            if (!is_digit_c(p->line_buf[k])) {
                p->state = ST_ERROR;
                return OI_ERR_PARSE;
            }
            long digit = p->line_buf[k] - '0';
            /* Checked before the multiply, so the accumulator never
             * overflows rather than being detected after the fact. */
            if (v > (OI_HTTP_MAX_BODY_VALUE - digit) / 10) {
                p->state = ST_ERROR;
                return OI_ERR_PARSE;
            }
            v = v * 10 + digit;
        }
        if (p->content_length >= 0 && p->content_length != v) {
            p->state = ST_ERROR;
            return OI_ERR_PARSE;
        }
        p->content_length = v;
    } else if (name_len == 10 &&
               strncasecmp(p->line_buf, "Connection", 10) == 0) {
        /* A comma-separated token list; "close" in it ends the
         * connection after this response. */
        size_t k = vi;
        while (k < value_end) {
            size_t end = k;
            while (end < value_end && p->line_buf[end] != ',') {
                end++;
            }
            size_t a = k;
            size_t b = end;
            while (a < b && (p->line_buf[a] == ' ' || p->line_buf[a] == '\t')) {
                a++;
            }
            while (b > a &&
                   (p->line_buf[b - 1] == ' ' || p->line_buf[b - 1] == '\t')) {
                b--;
            }
            if (b - a == 5 && strncasecmp(p->line_buf + a, "close", 5) == 0) {
                p->keep_alive = 0;
            }
            k = end + 1;
        }
    }

    p->state = ST_HEADER_LINE;
    return OI_OK;
}

static oi_status handle_chunk_size_line(oi_llm_http_parser *p) {
    size_t i = 0;
    long size = 0;
    while (i < p->line_len && p->line_buf[i] != ';') {
        char c = p->line_buf[i];
        int v;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            v = 10 + (c - 'A');
        } else {
            p->state = ST_ERROR;
            return OI_ERR_PARSE;
        }
        if (size > (OI_HTTP_MAX_BODY_VALUE - v) / 16) {
            p->state = ST_ERROR;
            return OI_ERR_PARSE;
        }
        size = size * 16 + v;
        i++;
    }
    if (i == 0) {
        p->state = ST_ERROR; /* no hex digits at all */
        return OI_ERR_PARSE;
    }

    if (size == 0) {
        p->state = ST_FINAL_CRLF;
    } else {
        p->remaining = size;
        p->state = ST_CHUNK_DATA;
    }
    return OI_OK;
}

static oi_status handle_chunk_data_crlf_line(oi_llm_http_parser *p) {
    if (p->line_len != 0) {
        p->state = ST_ERROR;
        return OI_ERR_PARSE;
    }
    p->state = ST_CHUNK_SIZE_LINE;
    return OI_OK;
}

static oi_status handle_final_crlf_line(oi_llm_http_parser *p) {
    if (p->line_len != 0) { /* no trailer-header support */
        p->state = ST_ERROR;
        return OI_ERR_PARSE;
    }
    p->state = ST_DONE;
    return OI_OK;
}

static int is_line_state(enum state s) {
    return s == ST_STATUS_LINE || s == ST_HEADER_LINE ||
           s == ST_CHUNK_SIZE_LINE || s == ST_CHUNK_DATA_CRLF ||
           s == ST_FINAL_CRLF;
}

static oi_status on_line_complete(oi_llm_http_parser *p,
                                   const int *destroyed) {
    oi_status st;
    switch (p->state) {
    case ST_STATUS_LINE:
        st = handle_status_line(p);
        break;
    case ST_HEADER_LINE:
        st = handle_header_line(p);
        break;
    case ST_CHUNK_SIZE_LINE:
        st = handle_chunk_size_line(p);
        break;
    case ST_CHUNK_DATA_CRLF:
        st = handle_chunk_data_crlf_line(p);
        break;
    case ST_FINAL_CRLF:
        st = handle_final_crlf_line(p);
        break;
    default:
        p->state = ST_ERROR;
        st = OI_ERR_PARSE;
        break;
    }
    if (*destroyed) {
        return OI_OK;
    }
    p->line_len = 0;
    return st;
}

oi_status oi_llm_http_parser_feed(oi_llm_http_parser *p, const void *bytes,
                                   size_t len) {
    if (p == NULL || (bytes == NULL && len > 0)) {
        return OI_ERR_INVAL;
    }

    const unsigned char *buf = bytes;
    int destroyed = 0;
OI_DIAG_PUSH_IGNORE_DANGLING
    p->destroyed_flag = &destroyed;
OI_DIAG_POP

    size_t i = 0;
    while (i < len) {
        if (p->state == ST_ERROR) {
            p->destroyed_flag = NULL;
            return OI_ERR_PARSE;
        }
        if (p->state == ST_DONE) {
            p->destroyed_flag = NULL;
            return OI_OK; /* trailing bytes beyond the body are ignored */
        }

        if (is_line_state(p->state)) {
            unsigned char c = buf[i];
            i++;
            if (c == '\n') {
                if (p->line_len > 0 && p->line_buf[p->line_len - 1] == '\r') {
                    p->line_len--;
                }
                oi_status st = on_line_complete(p, &destroyed);
                if (destroyed) {
                    return OI_OK; /* `p` was freed by a reentrant destroy */
                }
                if (st != OI_OK) {
                    p->destroyed_flag = NULL;
                    return st;
                }
            } else {
                oi_status st = line_append(p, (char)c);
                if (st != OI_OK) {
                    p->destroyed_flag = NULL;
                    return st;
                }
            }
            continue;
        }

        /* ST_CHUNK_DATA / ST_CONTENT_BODY / ST_UNTIL_CLOSE_BODY: consume
         * a run of raw bytes at once rather than one byte at a time. */
        size_t avail = len - i;
        if (p->state == ST_UNTIL_CLOSE_BODY) {
            if (p->on_body) {
                p->on_body(buf + i, avail, p->user_data);
                if (destroyed) {
                    return OI_OK; /* `p` was freed by a reentrant destroy */
                }
            }
            break;
        }
        size_t take =
            (size_t)p->remaining < avail ? (size_t)p->remaining : avail;
        if (p->on_body) {
            p->on_body(buf + i, take, p->user_data);
            if (destroyed) {
                return OI_OK; /* `p` was freed by a reentrant destroy */
            }
        }
        i += take;
        p->remaining -= (long)take;
        if (p->remaining == 0) {
            p->state = (p->state == ST_CHUNK_DATA) ? ST_CHUNK_DATA_CRLF
                                                     : ST_DONE;
        }
    }
    p->destroyed_flag = NULL;
    return OI_OK;
}
