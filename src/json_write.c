#include "oi/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct frame {
    int is_object;
    size_t count;         /* members/elements already fully written */
    int awaiting_value;   /* object only: key written, value still due */
};

struct oi_json_writer {
    char *buf;
    size_t len;
    size_t cap;

    struct frame *frames;
    size_t frame_count;
    size_t frame_cap;

    int have_root; /* top-level value fully written */
    int failed;    /* sticky: a structural-misuse error occurred */
};

oi_json_writer *oi_json_writer_create(void) {
    oi_json_writer *w = malloc(sizeof *w);
    if (w == NULL) {
        return NULL;
    }

    w->cap = 64;
    w->buf = malloc(w->cap);
    if (w->buf == NULL) {
        free(w);
        return NULL;
    }
    w->buf[0] = '\0';
    w->len = 0;

    w->frame_cap = 8;
    w->frames = malloc(w->frame_cap * sizeof *w->frames);
    if (w->frames == NULL) {
        free(w->buf);
        free(w);
        return NULL;
    }
    w->frame_count = 0;
    w->have_root = 0;
    w->failed = 0;
    return w;
}

void oi_json_writer_destroy(oi_json_writer *w) {
    if (w == NULL) {
        return;
    }
    free(w->buf);
    free(w->frames);
    free(w);
}

static oi_status ensure_buf(oi_json_writer *w, size_t additional) {
    size_t needed = w->len + additional + 1; /* +1 for trailing NUL */
    if (needed <= w->cap) {
        return OI_OK;
    }
    size_t new_cap = w->cap;
    while (new_cap < needed) {
        if (new_cap > (size_t)-1 / 2) {
            return OI_ERR_NOMEM;
        }
        new_cap *= 2;
    }
    char *new_buf = realloc(w->buf, new_cap);
    if (new_buf == NULL) {
        return OI_ERR_NOMEM;
    }
    w->buf = new_buf;
    w->cap = new_cap;
    return OI_OK;
}

static oi_status append_bytes(oi_json_writer *w, const char *bytes,
                               size_t n) {
    oi_status st = ensure_buf(w, n);
    if (st != OI_OK) {
        return st;
    }
    memcpy(w->buf + w->len, bytes, n);
    w->len += n;
    w->buf[w->len] = '\0';
    return OI_OK;
}

static oi_status append_str(oi_json_writer *w, const char *s) {
    return append_bytes(w, s, strlen(s));
}

static oi_status push_frame(oi_json_writer *w, int is_object) {
    if (w->frame_count == w->frame_cap) {
        if (w->frame_cap > (size_t)-1 / 2 / sizeof *w->frames) {
            return OI_ERR_NOMEM;
        }
        size_t new_cap = w->frame_cap * 2;
        struct frame *new_frames = realloc(w->frames, new_cap * sizeof *w->frames);
        if (new_frames == NULL) {
            return OI_ERR_NOMEM;
        }
        w->frames = new_frames;
        w->frame_cap = new_cap;
    }
    w->frames[w->frame_count].is_object = is_object;
    w->frames[w->frame_count].count = 0;
    w->frames[w->frame_count].awaiting_value = 0;
    w->frame_count++;
    return OI_OK;
}

/* Validates/updates state for the start of any value (scalar or the
 * opening bracket of a container), including comma insertion for array
 * elements. Object members insert their own comma in
 * oi_json_write_object_key, since it precedes the key, not the value. */
static oi_status begin_value(oi_json_writer *w) {
    if (w->failed) {
        return OI_ERR_INVAL;
    }
    if (w->frame_count == 0) {
        if (w->have_root) {
            w->failed = 1;
            return OI_ERR_INVAL;
        }
        return OI_OK;
    }

    struct frame *top = &w->frames[w->frame_count - 1];
    if (top->is_object) {
        if (!top->awaiting_value) {
            w->failed = 1;
            return OI_ERR_INVAL;
        }
        return OI_OK;
    }

    if (top->count > 0) {
        return append_bytes(w, ",", 1);
    }
    return OI_OK;
}

/* Marks the value just written as complete: bumps the parent frame's
 * element count (or, at top level, marks the whole document done). */
static void end_value(oi_json_writer *w) {
    if (w->frame_count == 0) {
        w->have_root = 1;
        return;
    }
    struct frame *top = &w->frames[w->frame_count - 1];
    top->count++;
    top->awaiting_value = 0;
}

oi_status oi_json_write_null(oi_json_writer *w) {
    oi_status st = begin_value(w);
    if (st != OI_OK) {
        return st;
    }
    st = append_str(w, "null");
    if (st != OI_OK) {
        return st;
    }
    end_value(w);
    return OI_OK;
}

oi_status oi_json_write_bool(oi_json_writer *w, int value) {
    oi_status st = begin_value(w);
    if (st != OI_OK) {
        return st;
    }
    st = append_str(w, value ? "true" : "false");
    if (st != OI_OK) {
        return st;
    }
    end_value(w);
    return OI_OK;
}

oi_status oi_json_write_number(oi_json_writer *w, double value) {
    if (!isfinite(value)) {
        return OI_ERR_INVAL; /* JSON has no NaN/Infinity literal */
    }
    oi_status st = begin_value(w);
    if (st != OI_OK) {
        return st;
    }

    char tmp[64];
    int n = snprintf(tmp, sizeof tmp, "%.17g", value);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return OI_ERR_INVAL;
    }
    st = append_bytes(w, tmp, (size_t)n);
    if (st != OI_OK) {
        return st;
    }
    end_value(w);
    return OI_OK;
}

static oi_status append_escaped_string(oi_json_writer *w, const char *s,
                                        size_t len) {
    oi_status st = append_bytes(w, "\"", 1);
    if (st != OI_OK) {
        return st;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        const char *out = NULL;
        size_t out_len = 0;

        switch (c) {
        case '"':
            out = "\\\"";
            out_len = 2;
            break;
        case '\\':
            out = "\\\\";
            out_len = 2;
            break;
        case '\b':
            out = "\\b";
            out_len = 2;
            break;
        case '\f':
            out = "\\f";
            out_len = 2;
            break;
        case '\n':
            out = "\\n";
            out_len = 2;
            break;
        case '\r':
            out = "\\r";
            out_len = 2;
            break;
        case '\t':
            out = "\\t";
            out_len = 2;
            break;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", c);
                out = esc;
                out_len = 6;
            } else {
                out = (const char *)&s[i];
                out_len = 1;
            }
            break;
        }

        st = append_bytes(w, out, out_len);
        if (st != OI_OK) {
            return st;
        }
    }

    return append_bytes(w, "\"", 1);
}

oi_status oi_json_write_string(oi_json_writer *w, const char *s, size_t len) {
    oi_status st = begin_value(w);
    if (st != OI_OK) {
        return st;
    }
    st = append_escaped_string(w, s, len);
    if (st != OI_OK) {
        return st;
    }
    end_value(w);
    return OI_OK;
}

static oi_status begin_container(oi_json_writer *w, int is_object) {
    oi_status st = begin_value(w);
    if (st != OI_OK) {
        return st;
    }
    st = append_bytes(w, is_object ? "{" : "[", 1);
    if (st != OI_OK) {
        return st;
    }
    end_value(w); /* the container occupies its parent's value slot */
    return push_frame(w, is_object);
}

static oi_status end_container(oi_json_writer *w, int is_object) {
    if (w->failed) {
        return OI_ERR_INVAL;
    }
    if (w->frame_count == 0) {
        w->failed = 1;
        return OI_ERR_INVAL;
    }
    struct frame *top = &w->frames[w->frame_count - 1];
    if (top->is_object != is_object || (top->is_object && top->awaiting_value)) {
        w->failed = 1;
        return OI_ERR_INVAL;
    }

    oi_status st = append_bytes(w, is_object ? "}" : "]", 1);
    if (st != OI_OK) {
        return st;
    }
    w->frame_count--;
    end_value(w);
    return OI_OK;
}

oi_status oi_json_write_array_begin(oi_json_writer *w) {
    return begin_container(w, 0);
}

oi_status oi_json_write_array_end(oi_json_writer *w) {
    return end_container(w, 0);
}

oi_status oi_json_write_object_begin(oi_json_writer *w) {
    return begin_container(w, 1);
}

oi_status oi_json_write_object_end(oi_json_writer *w) {
    return end_container(w, 1);
}

oi_status oi_json_write_object_key(oi_json_writer *w, const char *key,
                                    size_t len) {
    if (w->failed) {
        return OI_ERR_INVAL;
    }
    if (w->frame_count == 0) {
        w->failed = 1;
        return OI_ERR_INVAL;
    }
    struct frame *top = &w->frames[w->frame_count - 1];
    if (!top->is_object || top->awaiting_value) {
        w->failed = 1;
        return OI_ERR_INVAL;
    }

    if (top->count > 0) {
        oi_status st = append_bytes(w, ",", 1);
        if (st != OI_OK) {
            return st;
        }
    }
    oi_status st = append_escaped_string(w, key, len);
    if (st != OI_OK) {
        return st;
    }
    st = append_bytes(w, ":", 1);
    if (st != OI_OK) {
        return st;
    }
    top->awaiting_value = 1;
    return OI_OK;
}

const char *oi_json_writer_data(const oi_json_writer *w, size_t *out_len) {
    if (out_len != NULL) {
        *out_len = w->len;
    }
    return w->buf;
}
