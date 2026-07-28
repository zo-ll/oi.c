/*
 * libFuzzer harness for the JSON parser (oi/json.h).
 *
 * Checks more than "doesn't crash": oi/json.h promises that feed()
 * behaves identically regardless of how the input is chunked -- the
 * property the whole non-blocking-socket design rests on. Every input is
 * therefore parsed twice, once as a single feed and once one byte at a
 * time, and the two runs must agree on their terminal state and, when a
 * value was produced, on its serialization.
 *
 * Re-serializing the value tree also drags json_write.c into the fuzzed
 * surface, and comparing the two texts catches value-level divergence
 * (a dropped member, a mis-decoded escape) that comparing done/failed
 * flags alone would miss.
 *
 * json_internal.h is included because the public API can look up object
 * members by key but cannot enumerate them in order; walking the member
 * list directly is what makes the object comparison exact.
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "json_internal.h"
#include "oi/arena.h"
#include "oi/json.h"

/* Bounds the byte-at-a-time run, which costs one feed() call per byte.
 * libFuzzer's own default max_len is well under this; the cap only stops
 * a hand-added corpus file from dominating the run. */
#define FUZZ_MAX_INPUT (64u * 1024)

static oi_status write_value(oi_json_writer *w, const oi_json_value *v) {
    switch (oi_json_type_of(v)) {
    case OI_JSON_NULL:
        return oi_json_write_null(w);
    case OI_JSON_BOOL: {
        int b = 0;
        oi_status st = oi_json_get_bool(v, &b);
        return st == OI_OK ? oi_json_write_bool(w, b) : st;
    }
    case OI_JSON_NUMBER: {
        double d = 0;
        oi_status st = oi_json_get_number(v, &d);
        return st == OI_OK ? oi_json_write_number(w, d) : st;
    }
    case OI_JSON_STRING: {
        const char *ptr = NULL;
        size_t len = 0;
        oi_status st = oi_json_get_string(v, &ptr, &len);
        return st == OI_OK ? oi_json_write_string(w, ptr, len) : st;
    }
    case OI_JSON_ARRAY: {
        oi_status st = oi_json_write_array_begin(w);
        if (st != OI_OK) {
            return st;
        }
        size_t n = oi_json_array_len(v);
        for (size_t i = 0; i < n; i++) {
            st = write_value(w, oi_json_array_get(v, i));
            if (st != OI_OK) {
                return st;
            }
        }
        return oi_json_write_array_end(w);
    }
    case OI_JSON_OBJECT: {
        oi_status st = oi_json_write_object_begin(w);
        if (st != OI_OK) {
            return st;
        }
        for (const struct oi_json_member *m = v->u.object.head; m;
             m = m->next) {
            st = oi_json_write_object_key(w, m->key, m->key_len);
            if (st != OI_OK) {
                return st;
            }
            st = write_value(w, m->value);
            if (st != OI_OK) {
                return st;
            }
        }
        return oi_json_write_object_end(w);
    }
    }
    return OI_ERR_INVAL;
}

struct parse_result {
    int failed;
    int done;
    /* Set when a resource limit (arena block size, writer growth) was
     * hit. Those limits are allowed to depend on chunking, so the two
     * runs are only compared when neither tripped one. */
    int nomem;
    char *text; /* re-serialized root, NULL if none was produced */
    size_t text_len;
};

/* Serializes `root` into a freshly malloc'd NUL-terminated buffer.
 * Ownership transfers to `out`; any failure leaves out->text NULL and
 * sets out->nomem, which suppresses the comparison rather than
 * reporting a bug. */
static void capture_text(const oi_json_value *root, struct parse_result *out) {
    oi_json_writer *w = oi_json_writer_create();
    if (!w) {
        out->nomem = 1;
        return;
    }
    if (write_value(w, root) != OI_OK) {
        out->nomem = 1;
        oi_json_writer_destroy(w);
        return;
    }
    size_t len = 0;
    const char *s = oi_json_writer_data(w, &len);
    if (s) {
        char *copy = malloc(len + 1);
        if (copy) {
            memcpy(copy, s, len + 1);
            out->text = copy;
            out->text_len = len;
        } else {
            out->nomem = 1;
        }
    }
    oi_json_writer_destroy(w);
}

static void run_parse(const uint8_t *data, size_t size, size_t chunk,
                       struct parse_result *out) {
    memset(out, 0, sizeof *out);

    oi_arena *arena = oi_arena_create(0);
    if (!arena) {
        out->nomem = 1;
        return;
    }
    oi_json_parser *p = oi_json_parser_create(arena);
    if (!p) {
        out->nomem = 1;
        oi_arena_destroy(arena);
        return;
    }

    for (size_t off = 0; off < size; off += chunk) {
        size_t n = size - off < chunk ? size - off : chunk;
        oi_status st = oi_json_parser_feed(p, data + off, n);
        if (st == OI_ERR_NOMEM) {
            out->nomem = 1;
        }
        /* Parse errors are sticky, so stopping at the first one matches
         * what the single-feed run reports after consuming the rest. */
        if (st != OI_OK) {
            break;
        }
    }

    /* Deliberately called even on an already-failed parser: "finish
     * after an error" is itself a path worth fuzzing, and both runs
     * reach it identically. */
    if (oi_json_parser_finish(p) == OI_ERR_NOMEM) {
        out->nomem = 1;
    }

    out->failed = oi_json_parser_failed(p);
    out->done = oi_json_parser_done(p);

    if (out->done && !out->failed) {
        oi_json_value *root = oi_json_parser_root(p);
        if (root) {
            capture_text(root, out);
        }
    }

    oi_json_parser_destroy(p);
    oi_arena_destroy(arena);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > FUZZ_MAX_INPUT) {
        return 0;
    }

    struct parse_result whole;
    struct parse_result byte_wise;
    run_parse(data, size, size ? size : 1, &whole);
    run_parse(data, size, 1, &byte_wise);

    if (!whole.nomem && !byte_wise.nomem) {
        assert(whole.failed == byte_wise.failed);
        assert(whole.done == byte_wise.done);
        assert((whole.text == NULL) == (byte_wise.text == NULL));
        if (whole.text && byte_wise.text) {
            assert(whole.text_len == byte_wise.text_len);
            assert(memcmp(whole.text, byte_wise.text, whole.text_len) == 0);
        }
    }

    free(whole.text);
    free(byte_wise.text);
    return 0;
}
