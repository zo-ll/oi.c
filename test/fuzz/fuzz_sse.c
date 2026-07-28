/*
 * libFuzzer harness for the SSE event splitter (src/llm_sse.h).
 *
 * The last framing layer between raw socket bytes and the JSON parser,
 * and the one most exposed to arbitrary server output. As with the HTTP
 * and JSON harnesses, the invariant under test is chunk-independence:
 * the same bytes split differently across feed() calls must yield the
 * same sequence of events.
 *
 * Events are captured length-prefixed so the comparison covers event
 * *boundaries*, not just the concatenated payload -- splitting one
 * event into two (or merging two into one) is exactly the kind of
 * framing bug a boundary-blind comparison would let through.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm_sse.h"

#define FUZZ_MAX_INPUT (64u * 1024)

struct sse_result {
    int event_count;
    int nomem; /* a capture-side allocation failed; suppresses comparison */
    char *events;
    size_t len;
    size_t cap;
};

static int reserve(struct sse_result *r, size_t extra) {
    if (r->len + extra <= r->cap) {
        return 1;
    }
    size_t cap = r->cap ? r->cap : 256;
    while (cap < r->len + extra) {
        cap *= 2;
    }
    char *grown = realloc(r->events, cap);
    if (!grown) {
        r->nomem = 1;
        return 0;
    }
    r->events = grown;
    r->cap = cap;
    return 1;
}

static void on_event(const char *data, size_t len, void *user_data) {
    struct sse_result *r = user_data;
    r->event_count++;
    if (r->nomem) {
        return;
    }

    char header[32];
    int hn = snprintf(header, sizeof header, "%zu\n", len);
    if (hn < 0) {
        r->nomem = 1;
        return;
    }
    if (!reserve(r, (size_t)hn + len)) {
        return;
    }
    memcpy(r->events + r->len, header, (size_t)hn);
    r->len += (size_t)hn;
    if (len > 0) {
        memcpy(r->events + r->len, data, len);
        r->len += len;
    }
}

static void run_parse(const uint8_t *data, size_t size, size_t chunk,
                       struct sse_result *out) {
    memset(out, 0, sizeof *out);

    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, out);
    if (!p) {
        out->nomem = 1;
        return;
    }

    for (size_t off = 0; off < size; off += chunk) {
        size_t n = size - off < chunk ? size - off : chunk;
        oi_status st = oi_llm_sse_parser_feed(p, data + off, n);
        if (st == OI_ERR_NOMEM) {
            out->nomem = 1;
        }
        if (st != OI_OK) {
            break;
        }
    }

    oi_llm_sse_parser_destroy(p);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > FUZZ_MAX_INPUT) {
        return 0;
    }

    struct sse_result whole;
    struct sse_result byte_wise;
    run_parse(data, size, size ? size : 1, &whole);
    run_parse(data, size, 1, &byte_wise);

    if (!whole.nomem && !byte_wise.nomem) {
        assert(whole.event_count == byte_wise.event_count);
        assert(whole.len == byte_wise.len);
        if (whole.len > 0) {
            assert(memcmp(whole.events, byte_wise.events, whole.len) == 0);
        }
    }

    free(whole.events);
    free(byte_wise.events);
    return 0;
}
