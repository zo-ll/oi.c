/*
 * libFuzzer harness for the HTTP/1.1 response parser (src/llm_http.h).
 *
 * This is message framing fed straight from a socket, so the property
 * that matters is the same one the JSON parser makes: the result must
 * not depend on how the bytes were split across feed() calls. Each
 * input is parsed once whole and once one byte at a time, and the two
 * runs must agree on their terminal state, the reported status code,
 * and the exact body bytes handed to the caller.
 *
 * Chunked transfer-decoding lives behind this interface, so a framing
 * bug that duplicates, drops, or reorders body bytes under one
 * particular split shows up here as a body mismatch rather than as a
 * silent corruption downstream.
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "llm_http.h"

#define FUZZ_MAX_INPUT (64u * 1024)

struct http_result {
    int failed;
    int body_done;
    int headers_done_calls;
    int status_code;
    int nomem; /* a capture-side allocation failed; suppresses comparison */
    char *body;
    size_t body_len;
    size_t body_cap;
};

static void on_headers_done(int status_code, void *user_data) {
    struct http_result *r = user_data;
    r->headers_done_calls++;
    r->status_code = status_code;
}

static void on_body(const void *data, size_t len, void *user_data) {
    struct http_result *r = user_data;
    if (r->nomem || len == 0) {
        return;
    }
    if (r->body_len + len > r->body_cap) {
        size_t cap = r->body_cap ? r->body_cap : 256;
        while (cap < r->body_len + len) {
            cap *= 2;
        }
        /* Keep the live pointer until realloc has succeeded, so a
         * failure leaves the already-captured bytes intact. */
        char *grown = realloc(r->body, cap);
        if (!grown) {
            r->nomem = 1;
            return;
        }
        r->body = grown;
        r->body_cap = cap;
    }
    memcpy(r->body + r->body_len, data, len);
    r->body_len += len;
}

static void run_parse(const uint8_t *data, size_t size, size_t chunk,
                       struct http_result *out) {
    memset(out, 0, sizeof *out);

    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, out);
    if (!p) {
        out->nomem = 1;
        return;
    }

    for (size_t off = 0; off < size; off += chunk) {
        size_t n = size - off < chunk ? size - off : chunk;
        oi_status st = oi_llm_http_parser_feed(p, data + off, n);
        if (st == OI_ERR_NOMEM) {
            out->nomem = 1;
        }
        if (st != OI_OK) {
            break;
        }
    }

    out->failed = oi_llm_http_parser_failed(p);
    out->body_done = oi_llm_http_parser_body_done(p);
    oi_llm_http_parser_destroy(p);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > FUZZ_MAX_INPUT) {
        return 0;
    }

    struct http_result whole;
    struct http_result byte_wise;
    run_parse(data, size, size ? size : 1, &whole);
    run_parse(data, size, 1, &byte_wise);

    if (!whole.nomem && !byte_wise.nomem) {
        assert(whole.failed == byte_wise.failed);
        assert(whole.body_done == byte_wise.body_done);
        assert(whole.headers_done_calls == byte_wise.headers_done_calls);
        assert(whole.status_code == byte_wise.status_code);
        assert(whole.body_len == byte_wise.body_len);
        if (whole.body_len > 0) {
            assert(memcmp(whole.body, byte_wise.body, whole.body_len) == 0);
        }
        /* Headers can only complete once per response. */
        assert(whole.headers_done_calls <= 1);
    }

    free(whole.body);
    free(byte_wise.body);
    return 0;
}
