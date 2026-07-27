#include "json_internal.h"

#include <stdlib.h>
#include <string.h>

/* Hard cap on container nesting depth: without one, a tiny malicious
 * input of a million '[' characters would grow the frame stack (and,
 * via oi_json_new_array, the arena) without bound. */
#define OI_JSON_MAX_DEPTH 1000
/* Hard cap on a single number literal's raw byte length. Real doubles
 * never need more than a few dozen digits; this just stops a
 * pathological "111...1" token from growing unbounded. */
#define OI_JSON_MAX_NUMBER_LEN 512

enum ps_state {
    PS_VALUE,           /* between tokens: scanning for the next byte */
    PS_STRING,
    PS_STRING_ESCAPE,
    PS_STRING_UNICODE,
    PS_NUMBER,
    PS_LITERAL,         /* matching true/false/null */
    PS_DONE,            /* top-level value complete */
    PS_ERROR
};

enum frame_kind { FRAME_ARRAY, FRAME_OBJECT };

enum frame_expect {
    EXPECT_ELEM_OR_CLOSE, /* just opened: a value/key, or the matching close */
    EXPECT_ELEM,          /* after a comma: a value/key is required */
    EXPECT_COMMA_OR_CLOSE,
    EXPECT_COLON,         /* object only, after a key */
    EXPECT_VALUE          /* object only, after a colon */
};

struct frame {
    enum frame_kind kind;
    oi_json_value *container;
    char *pending_key;
    size_t pending_key_len;
    enum frame_expect expect;
};

struct oi_json_parser {
    oi_arena *arena;

    enum ps_state state;
    oi_json_value *root;

    struct frame *frames;
    size_t depth;
    size_t frame_cap;

    /* string accumulation, shared by keys and string values */
    char *str_buf;
    size_t str_len;
    size_t str_cap;
    int parsing_key;
    int pending_high_surrogate;
    unsigned high_surrogate_value;
    unsigned unicode_value;
    int unicode_hex_count;

    /* number accumulation */
    char num_buf[OI_JSON_MAX_NUMBER_LEN];
    size_t num_len;

    /* true/false/null matching */
    const char *literal;
    int literal_index;
};

static int is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static int is_number_continuation(unsigned char c) {
    return is_digit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' ||
           c == '-';
}

static void reset_transient(oi_json_parser *p) {
    p->state = PS_VALUE;
    p->root = NULL;
    p->depth = 0;
    p->str_len = 0;
    p->parsing_key = 0;
    p->pending_high_surrogate = 0;
    p->high_surrogate_value = 0;
    p->unicode_value = 0;
    p->unicode_hex_count = 0;
    p->num_len = 0;
    p->literal = NULL;
    p->literal_index = 0;
}

oi_json_parser *oi_json_parser_create(oi_arena *arena) {
    if (arena == NULL) {
        return NULL;
    }

    oi_json_parser *p = malloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    p->arena = arena;

    p->frame_cap = 16;
    p->frames = malloc(p->frame_cap * sizeof *p->frames);
    if (p->frames == NULL) {
        free(p);
        return NULL;
    }

    p->str_cap = 64;
    p->str_buf = malloc(p->str_cap);
    if (p->str_buf == NULL) {
        free(p->frames);
        free(p);
        return NULL;
    }

    reset_transient(p);
    return p;
}

void oi_json_parser_destroy(oi_json_parser *p) {
    if (p == NULL) {
        return;
    }
    free(p->frames);
    free(p->str_buf);
    free(p);
}

void oi_json_parser_reset(oi_json_parser *p) {
    if (p == NULL) {
        return;
    }
    reset_transient(p);
}

int oi_json_parser_done(const oi_json_parser *p) {
    return p != NULL && p->state == PS_DONE;
}

int oi_json_parser_failed(const oi_json_parser *p) {
    return p != NULL && p->state == PS_ERROR;
}

oi_json_value *oi_json_parser_root(const oi_json_parser *p) {
    if (p == NULL || p->state != PS_DONE) {
        return NULL;
    }
    return p->root;
}

/* --- scratch buffer growth --- */

static oi_status str_buf_reserve(oi_json_parser *p, size_t additional) {
    size_t needed = p->str_len + additional;
    if (needed <= p->str_cap) {
        return OI_OK;
    }
    size_t new_cap = p->str_cap;
    while (new_cap < needed) {
        if (new_cap > (size_t)-1 / 2) {
            return OI_ERR_NOMEM;
        }
        new_cap *= 2;
    }
    char *nb = realloc(p->str_buf, new_cap);
    if (nb == NULL) {
        return OI_ERR_NOMEM;
    }
    p->str_buf = nb;
    p->str_cap = new_cap;
    return OI_OK;
}

static oi_status str_buf_append_bytes(oi_json_parser *p, const char *bytes,
                                       size_t n) {
    oi_status st = str_buf_reserve(p, n);
    if (st != OI_OK) {
        p->state = PS_ERROR;
        return st;
    }
    memcpy(p->str_buf + p->str_len, bytes, n);
    p->str_len += n;
    return OI_OK;
}

static oi_status str_buf_append_byte(oi_json_parser *p, char c) {
    return str_buf_append_bytes(p, &c, 1);
}

static oi_status emit_utf8(oi_json_parser *p, unsigned cp) {
    unsigned char buf[4];
    int n;

    if (cp <= 0x7Fu) {
        buf[0] = (unsigned char)cp;
        n = 1;
    } else if (cp <= 0x7FFu) {
        buf[0] = (unsigned char)(0xC0u | (cp >> 6));
        buf[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp <= 0xFFFFu) {
        buf[0] = (unsigned char)(0xE0u | (cp >> 12));
        buf[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        buf[0] = (unsigned char)(0xF0u | (cp >> 18));
        buf[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        buf[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    return str_buf_append_bytes(p, (const char *)buf, (size_t)n);
}

/* --- frame stack --- */

static oi_status push_frame(oi_json_parser *p, enum frame_kind kind) {
    if (p->depth >= OI_JSON_MAX_DEPTH) {
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }
    if (p->depth == p->frame_cap) {
        size_t new_cap = p->frame_cap * 2;
        if (new_cap > OI_JSON_MAX_DEPTH) {
            new_cap = OI_JSON_MAX_DEPTH;
        }
        struct frame *nf = realloc(p->frames, new_cap * sizeof *nf);
        if (nf == NULL) {
            p->state = PS_ERROR;
            return OI_ERR_NOMEM;
        }
        p->frames = nf;
        p->frame_cap = new_cap;
    }

    oi_json_value *container = (kind == FRAME_ARRAY)
                                    ? oi_json_new_array(p->arena)
                                    : oi_json_new_object(p->arena);
    if (container == NULL) {
        p->state = PS_ERROR;
        return OI_ERR_NOMEM;
    }

    struct frame *f = &p->frames[p->depth++];
    f->kind = kind;
    f->container = container;
    f->pending_key = NULL;
    f->pending_key_len = 0;
    f->expect = EXPECT_ELEM_OR_CLOSE;
    return OI_OK;
}

/* A value (scalar, or a container just popped) is complete: attach it to
 * the enclosing array/object, or -- if the stack is empty -- finish the
 * whole document. */
static oi_status attach_completed(oi_json_parser *p, oi_json_value *value) {
    if (p->depth == 0) {
        p->root = value;
        p->state = PS_DONE;
        return OI_OK;
    }

    struct frame *top = &p->frames[p->depth - 1];
    oi_status st;
    if (top->kind == FRAME_ARRAY) {
        st = oi_json_array_push(p->arena, top->container, value);
    } else {
        st = oi_json_object_append(p->arena, top->container, top->pending_key,
                                    top->pending_key_len, value);
        top->pending_key = NULL;
        top->pending_key_len = 0;
    }
    if (st != OI_OK) {
        p->state = PS_ERROR;
        return st;
    }
    top->expect = EXPECT_COMMA_OR_CLOSE;
    p->state = PS_VALUE;
    return OI_OK;
}

static oi_status attach_completed_key(oi_json_parser *p) {
    struct frame *top = &p->frames[p->depth - 1];

    char *key_copy = oi_arena_alloc(p->arena, p->str_len + 1);
    if (key_copy == NULL) {
        p->state = PS_ERROR;
        return OI_ERR_NOMEM;
    }
    if (p->str_len > 0) {
        memcpy(key_copy, p->str_buf, p->str_len);
    }
    key_copy[p->str_len] = '\0';

    top->pending_key = key_copy;
    top->pending_key_len = p->str_len;
    top->expect = EXPECT_COLON;
    p->state = PS_VALUE;
    return OI_OK;
}

static oi_status pop_container(oi_json_parser *p) {
    struct frame *top = &p->frames[p->depth - 1];
    oi_json_value *closed = top->container;
    p->depth--;
    return attach_completed(p, closed);
}

/* --- value dispatch --- */

static oi_status dispatch_value_start(oi_json_parser *p, unsigned char c) {
    p->parsing_key = 0;
    if (p->depth > 0) {
        struct frame *top = &p->frames[p->depth - 1];
        if (top->kind == FRAME_OBJECT &&
            (top->expect == EXPECT_ELEM_OR_CLOSE ||
             top->expect == EXPECT_ELEM)) {
            if (c != '"') { /* object keys must be strings */
                p->state = PS_ERROR;
                return OI_ERR_PARSE;
            }
            p->parsing_key = 1;
        }
    }

    switch (c) {
    case '{':
        return push_frame(p, FRAME_OBJECT);
    case '[':
        return push_frame(p, FRAME_ARRAY);
    case '"':
        p->state = PS_STRING;
        p->str_len = 0;
        p->pending_high_surrogate = 0;
        return OI_OK;
    case 't':
        p->state = PS_LITERAL;
        p->literal = "true";
        p->literal_index = 1;
        return OI_OK;
    case 'f':
        p->state = PS_LITERAL;
        p->literal = "false";
        p->literal_index = 1;
        return OI_OK;
    case 'n':
        p->state = PS_LITERAL;
        p->literal = "null";
        p->literal_index = 1;
        return OI_OK;
    case '-':
        p->state = PS_NUMBER;
        p->num_len = 0;
        p->num_buf[p->num_len++] = '-';
        return OI_OK;
    default:
        if (is_digit(c)) {
            p->state = PS_NUMBER;
            p->num_len = 0;
            p->num_buf[p->num_len++] = (char)c;
            return OI_OK;
        }
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }
}

static oi_status process_between_tokens(oi_json_parser *p, unsigned char c) {
    if (is_ws(c)) {
        return OI_OK;
    }
    if (p->depth == 0) {
        return dispatch_value_start(p, c);
    }

    struct frame *top = &p->frames[p->depth - 1];
    unsigned char close_char = (top->kind == FRAME_ARRAY) ? ']' : '}';

    switch (top->expect) {
    case EXPECT_ELEM_OR_CLOSE:
        if (c == close_char) {
            return pop_container(p);
        }
        return dispatch_value_start(p, c);
    case EXPECT_ELEM:
        return dispatch_value_start(p, c);
    case EXPECT_COMMA_OR_CLOSE:
        if (c == close_char) {
            return pop_container(p);
        }
        if (c == ',') {
            top->expect = EXPECT_ELEM;
            return OI_OK;
        }
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    case EXPECT_COLON:
        if (c == ':') {
            top->expect = EXPECT_VALUE;
            return OI_OK;
        }
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    case EXPECT_VALUE:
        return dispatch_value_start(p, c);
    default:
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }
}

/* --- strings --- */

static oi_status finalize_string_value(oi_json_parser *p) {
    if (p->parsing_key) {
        return attach_completed_key(p);
    }
    oi_json_value *v = oi_json_new_string(p->arena, p->str_buf, p->str_len);
    if (v == NULL) {
        p->state = PS_ERROR;
        return OI_ERR_NOMEM;
    }
    return attach_completed(p, v);
}

static oi_status handle_string_byte(oi_json_parser *p, unsigned char c) {
    if (c == '"') {
        if (p->pending_high_surrogate) {
            p->state = PS_ERROR;
            return OI_ERR_PARSE;
        }
        return finalize_string_value(p);
    }
    if (c == '\\') {
        p->state = PS_STRING_ESCAPE;
        return OI_OK;
    }
    if (p->pending_high_surrogate) {
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }
    if (c < 0x20) {
        p->state = PS_ERROR; /* raw control chars must be escaped */
        return OI_ERR_PARSE;
    }
    return str_buf_append_byte(p, (char)c);
}

static oi_status handle_string_escape_byte(oi_json_parser *p,
                                            unsigned char c) {
    if (p->pending_high_surrogate && c != 'u') {
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }

    char mapped;
    switch (c) {
    case '"':
        mapped = '"';
        break;
    case '\\':
        mapped = '\\';
        break;
    case '/':
        mapped = '/';
        break;
    case 'b':
        mapped = '\b';
        break;
    case 'f':
        mapped = '\f';
        break;
    case 'n':
        mapped = '\n';
        break;
    case 'r':
        mapped = '\r';
        break;
    case 't':
        mapped = '\t';
        break;
    case 'u':
        p->state = PS_STRING_UNICODE;
        p->unicode_value = 0;
        p->unicode_hex_count = 0;
        return OI_OK;
    default:
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }

    p->state = PS_STRING;
    return str_buf_append_byte(p, mapped);
}

static oi_status handle_string_unicode_byte(oi_json_parser *p,
                                             unsigned char c) {
    int digit;
    if (c >= '0' && c <= '9') {
        digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
        digit = 10 + (c - 'a');
    } else if (c >= 'A' && c <= 'F') {
        digit = 10 + (c - 'A');
    } else {
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }

    p->unicode_value = (p->unicode_value << 4) | (unsigned)digit;
    p->unicode_hex_count++;
    if (p->unicode_hex_count < 4) {
        return OI_OK;
    }

    unsigned cp = p->unicode_value;
    p->state = PS_STRING;

    if (p->pending_high_surrogate) {
        if (cp < 0xDC00u || cp > 0xDFFFu) {
            p->state = PS_ERROR; /* high surrogate not followed by a low one */
            return OI_ERR_PARSE;
        }
        unsigned combined =
            0x10000u + ((p->high_surrogate_value - 0xD800u) << 10) +
            (cp - 0xDC00u);
        p->pending_high_surrogate = 0;
        return emit_utf8(p, combined);
    }
    if (cp >= 0xD800u && cp <= 0xDBFFu) {
        p->pending_high_surrogate = 1;
        p->high_surrogate_value = cp;
        return OI_OK;
    }
    if (cp >= 0xDC00u && cp <= 0xDFFFu) {
        p->state = PS_ERROR; /* lone low surrogate */
        return OI_ERR_PARSE;
    }
    return emit_utf8(p, cp);
}

/* --- numbers --- */

static int validate_json_number(const char *s, size_t len) {
    size_t i = 0;
    if (i < len && s[i] == '-') {
        i++;
    }
    if (i >= len) {
        return 0;
    }
    if (s[i] == '0') {
        i++;
    } else if (is_digit((unsigned char)s[i])) {
        while (i < len && is_digit((unsigned char)s[i])) {
            i++;
        }
    } else {
        return 0;
    }
    if (i < len && s[i] == '.') {
        i++;
        if (!(i < len && is_digit((unsigned char)s[i]))) {
            return 0;
        }
        while (i < len && is_digit((unsigned char)s[i])) {
            i++;
        }
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        if (!(i < len && is_digit((unsigned char)s[i]))) {
            return 0;
        }
        while (i < len && is_digit((unsigned char)s[i])) {
            i++;
        }
    }
    return i == len;
}

static oi_status finalize_number(oi_json_parser *p) {
    if (!validate_json_number(p->num_buf, p->num_len)) {
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }
    p->num_buf[p->num_len] = '\0';

    char *end;
    double value = strtod(p->num_buf, &end);
    if (end != p->num_buf + p->num_len) {
        p->state = PS_ERROR; /* defensive: validate_json_number should
                               * already guarantee this can't happen */
        return OI_ERR_PARSE;
    }

    oi_json_value *v = oi_json_new_number(p->arena, value);
    if (v == NULL) {
        p->state = PS_ERROR;
        return OI_ERR_NOMEM;
    }
    return attach_completed(p, v);
}

static oi_status handle_number_byte(oi_json_parser *p, unsigned char c,
                                     int *consumed) {
    if (is_number_continuation(c)) {
        if (p->num_len >= sizeof p->num_buf - 1) {
            p->state = PS_ERROR;
            return OI_ERR_PARSE;
        }
        p->num_buf[p->num_len++] = (char)c;
        return OI_OK;
    }
    *consumed = 0;
    return finalize_number(p);
}

/* --- literals --- */

static oi_status handle_literal_byte(oi_json_parser *p, unsigned char c) {
    if (c != (unsigned char)p->literal[p->literal_index]) {
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }
    p->literal_index++;
    if (p->literal[p->literal_index] != '\0') {
        return OI_OK; /* still matching */
    }

    oi_json_value *v = (p->literal[0] == 'n')
                            ? oi_json_new_null(p->arena)
                            : oi_json_new_bool(p->arena, p->literal[0] == 't');
    if (v == NULL) {
        p->state = PS_ERROR;
        return OI_ERR_NOMEM;
    }
    return attach_completed(p, v);
}

/* --- top-level dispatch --- */

static oi_status step(oi_json_parser *p, unsigned char c, int *consumed) {
    *consumed = 1;
    switch (p->state) {
    case PS_VALUE:
        return process_between_tokens(p, c);
    case PS_STRING:
        return handle_string_byte(p, c);
    case PS_STRING_ESCAPE:
        return handle_string_escape_byte(p, c);
    case PS_STRING_UNICODE:
        return handle_string_unicode_byte(p, c);
    case PS_NUMBER:
        return handle_number_byte(p, c, consumed);
    case PS_LITERAL:
        return handle_literal_byte(p, c);
    case PS_DONE:
        if (is_ws(c)) {
            return OI_OK;
        }
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    case PS_ERROR:
        return OI_ERR_PARSE;
    default:
        p->state = PS_ERROR;
        return OI_ERR_PARSE;
    }
}

oi_status oi_json_parser_feed(oi_json_parser *p, const void *bytes,
                               size_t len) {
    if (p == NULL || (bytes == NULL && len > 0)) {
        return OI_ERR_INVAL;
    }

    const unsigned char *buf = bytes;
    size_t i = 0;
    while (i < len) {
        int consumed;
        oi_status st = step(p, buf[i], &consumed);
        if (st != OI_OK) {
            return st;
        }
        if (consumed) {
            i++;
        }
    }
    return OI_OK;
}

oi_status oi_json_parser_finish(oi_json_parser *p) {
    if (p == NULL) {
        return OI_ERR_INVAL;
    }
    if (p->state == PS_DONE) {
        return OI_OK;
    }
    if (p->state == PS_NUMBER) {
        oi_status st = finalize_number(p);
        if (st != OI_OK) {
            return st;
        }
        if (p->state == PS_DONE) {
            return OI_OK;
        }
        p->state = PS_ERROR; /* number resolved, but a container is still open */
        return OI_ERR_PARSE;
    }
    p->state = PS_ERROR;
    return OI_ERR_PARSE;
}
