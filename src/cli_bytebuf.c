#include "cli_bytebuf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void oi_cli_bytebuf_init(struct oi_cli_bytebuf *buf) {
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

oi_status oi_cli_bytebuf_append(struct oi_cli_bytebuf *buf, const void *data,
                                size_t len) {
    size_t required;
    size_t cap;
    char *next;

    if (len > SIZE_MAX - buf->len) {
        return OI_ERR_NOMEM;
    }
    required = buf->len + len;
    if (required <= buf->cap) {
        if (len != 0) {
            memcpy(buf->data + buf->len, data, len);
        }
        buf->len = required;
        return OI_OK;
    }

    cap = buf->cap == 0 ? 128 : buf->cap;
    while (cap < required) {
        if (cap > SIZE_MAX / 2) {
            return OI_ERR_NOMEM;
        }
        cap *= 2;
    }
    next = realloc(buf->data, cap);
    if (next == NULL) {
        return OI_ERR_NOMEM;
    }
    buf->data = next;
    buf->cap = cap;
    if (len != 0) {
        memcpy(buf->data + buf->len, data, len);
    }
    buf->len = required;
    return OI_OK;
}

void oi_cli_bytebuf_reset(struct oi_cli_bytebuf *buf) { buf->len = 0; }

void oi_cli_bytebuf_free(struct oi_cli_bytebuf *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}
