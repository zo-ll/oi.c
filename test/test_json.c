#include "oi/arena.h"
#include "oi/json.h"
#include "json_internal.h"
#include "test.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Feeds `doc` to a fresh parser, one byte at a time -- the harshest
 * possible chunking, since a parser correct here is correct for any
 * coarser chunking too. Returns the completed root, or NULL. */
static oi_json_value *parse_byte_by_byte(oi_arena *arena, const char *doc,
                                          size_t len, oi_status *out_st) {
    oi_json_parser *p = oi_json_parser_create(arena);
    oi_status st = OI_OK;
    for (size_t i = 0; i < len && st == OI_OK; i++) {
        st = oi_json_parser_feed(p, doc + i, 1);
    }
    if (out_st) {
        *out_st = st;
    }
    oi_json_value *root = (st == OI_OK) ? oi_json_parser_root(p) : NULL;
    /* Root must survive after the parser is destroyed: it lives on the
     * arena, not inside the parser. */
    oi_json_parser_destroy(p);
    return root;
}

/* Feeds `doc` split at every possible single cut point (i.e. two feed()
 * calls: [0,k) and [k,len)), for every k, plus the whole-buffer and
 * fully-byte-by-byte cases. All must agree the value is done and equal
 * in shape to what byte-by-byte parsing produced (checked by caller via
 * CHECK_EQ on scalar fields, since a general deep-equal isn't needed for
 * these tests). This exercises "split mid-escape / mid-number / etc."
 * without hand-writing every split point per test. */
static void check_all_split_points_succeed(const char *doc, size_t len) {
    for (size_t k = 0; k <= len; k++) {
        oi_arena *arena = oi_arena_create(0);
        oi_json_parser *p = oi_json_parser_create(arena);
        oi_status st = oi_json_parser_feed(p, doc, k);
        CHECK_EQ(st, OI_OK);
        if (st == OI_OK) {
            st = oi_json_parser_feed(p, doc + k, len - k);
            CHECK_EQ(st, OI_OK);
        }
        CHECK(oi_json_parser_done(p));
        oi_json_parser_destroy(p);
        oi_arena_destroy(arena);
    }
}

/* --- basic scalars --- */

TEST(parse_null) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, "null", 4, &st);
    CHECK_EQ(st, OI_OK);
    CHECK(v != NULL);
    CHECK_EQ(oi_json_type_of(v), OI_JSON_NULL);
    oi_arena_destroy(a);
}

TEST(parse_true_false) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    int b;

    oi_json_value *vt = parse_byte_by_byte(a, "true", 4, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_json_get_bool(vt, &b), OI_OK);
    CHECK_EQ(b, 1);

    oi_json_value *vf = parse_byte_by_byte(a, "false", 5, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_json_get_bool(vf, &b), OI_OK);
    CHECK_EQ(b, 0);

    oi_arena_destroy(a);
}

TEST(literal_typo_is_error) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    parse_byte_by_byte(a, "nul", 3, &st);
    CHECK_EQ(st, OI_OK); /* incomplete, not yet wrong */
    parse_byte_by_byte(a, "nulx", 4, &st);
    CHECK_EQ(st, OI_ERR_PARSE);
    parse_byte_by_byte(a, "True", 4, &st); /* JSON is case-sensitive */
    CHECK_EQ(st, OI_ERR_PARSE);
    oi_arena_destroy(a);
}

/* --- numbers --- */

struct number_case {
    const char *text;
    double expected;
    int valid;
};

TEST(numbers) {
    static const struct number_case cases[] = {
        {"0", 0.0, 1},          {"-0", -0.0, 1},
        {"42", 42.0, 1},        {"-42", -42.0, 1},
        {"3.14", 3.14, 1},      {"1e10", 1e10, 1},
        {"1E10", 1e10, 1},      {"1e+10", 1e10, 1},
        {"1e-10", 1e-10, 1},    {"1.5e3", 1500.0, 1},
        {"0.5", 0.5, 1},        {"-0.5", -0.5, 1},
        {"01", 0, 0},           /* leading zero */
        {"1.", 0, 0},           /* trailing dot */
        {".5", 0, 0},           /* leading dot */
        {"1e", 0, 0},           /* dangling exponent */
        {"1.2.3", 0, 0},        {"--5", 0, 0},
        {"-", 0, 0},            {"+5", 0, 0},
        {"5-", 0, 0},           {"1e+", 0, 0},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        oi_arena *a = oi_arena_create(0);
        size_t len = strlen(cases[i].text);
        oi_status st;
        /* A bare number needs finish() to disambiguate "more digits
         * could still come"; feed then finish explicitly here. */
        oi_json_parser *p = oi_json_parser_create(a);
        st = oi_json_parser_feed(p, cases[i].text, len);
        if (st == OI_OK) {
            st = oi_json_parser_finish(p);
        }
        if (cases[i].valid) {
            CHECK_EQ(st, OI_OK);
            double got;
            CHECK_EQ(oi_json_get_number(oi_json_parser_root(p), &got), OI_OK);
            CHECK(got == cases[i].expected);
        } else {
            CHECK_EQ(st, OI_ERR_PARSE);
        }
        oi_json_parser_destroy(p);
        oi_arena_destroy(a);
    }
}

TEST(number_terminated_by_following_byte_needs_no_finish) {
    /* Inside an array, the ',' unambiguously terminates the number. */
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, "[1,2,3]", 7, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_json_array_len(v), 3u);
    oi_arena_destroy(a);
}

TEST(bare_number_not_done_until_finish) {
    oi_arena *a = oi_arena_create(0);
    oi_json_parser *p = oi_json_parser_create(a);
    CHECK_EQ(oi_json_parser_feed(p, "42", 2), OI_OK);
    CHECK(!oi_json_parser_done(p));
    CHECK_EQ(oi_json_parser_finish(p), OI_OK);
    CHECK(oi_json_parser_done(p));
    double v;
    CHECK_EQ(oi_json_get_number(oi_json_parser_root(p), &v), OI_OK);
    CHECK(v == 42.0);
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

TEST(finish_rejects_unclosed_container) {
    oi_arena *a = oi_arena_create(0);
    oi_json_parser *p = oi_json_parser_create(a);
    CHECK_EQ(oi_json_parser_feed(p, "[1,2", 4), OI_OK);
    CHECK_EQ(oi_json_parser_finish(p), OI_ERR_PARSE);
    CHECK(oi_json_parser_failed(p));
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

TEST(finish_on_empty_input_is_error) {
    oi_arena *a = oi_arena_create(0);
    oi_json_parser *p = oi_json_parser_create(a);
    CHECK_EQ(oi_json_parser_finish(p), OI_ERR_PARSE);
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

/* --- strings --- */

TEST(simple_string) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, "\"hello\"", 7, &st);
    CHECK_EQ(st, OI_OK);
    const char *ptr;
    size_t len;
    CHECK_EQ(oi_json_get_string(v, &ptr, &len), OI_OK);
    CHECK_EQ(len, 5u);
    CHECK(memcmp(ptr, "hello", 5) == 0);
    oi_arena_destroy(a);
}

TEST(empty_string) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, "\"\"", 2, &st);
    CHECK_EQ(st, OI_OK);
    const char *ptr;
    size_t len;
    CHECK_EQ(oi_json_get_string(v, &ptr, &len), OI_OK);
    CHECK_EQ(len, 0u);
    oi_arena_destroy(a);
}

TEST(string_simple_escapes) {
    oi_arena *a = oi_arena_create(0);
    const char *doc = "\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"";
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, doc, strlen(doc), &st);
    CHECK_EQ(st, OI_OK);
    const char *ptr;
    size_t len;
    CHECK_EQ(oi_json_get_string(v, &ptr, &len), OI_OK);
    const char *expected = "a\"b\\c/d\be\ff\ng\rh\ti";
    CHECK_EQ(len, strlen(expected));
    CHECK(memcmp(ptr, expected, len) == 0);
    oi_arena_destroy(a);
}

TEST(string_unicode_escape_bmp) {
    oi_arena *a = oi_arena_create(0);
    /* \u00e9 = e-acute, UTF-8: 0xC3 0xA9 */
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, "\"caf\\u00e9\"", 11, &st);
    CHECK_EQ(st, OI_OK);
    const char *ptr;
    size_t len;
    CHECK_EQ(oi_json_get_string(v, &ptr, &len), OI_OK);
    CHECK_EQ(len, 5u); /* c,a,f, 0xC3,0xA9 */
    CHECK(memcmp(ptr, "caf", 3) == 0);
    CHECK_EQ((unsigned char)ptr[3], 0xC3u);
    CHECK_EQ((unsigned char)ptr[4], 0xA9u);
    oi_arena_destroy(a);
}

TEST(string_unicode_surrogate_pair) {
    /* U+1F600 (grinning face) = surrogate pair \ud83d\ude00,
     * UTF-8: F0 9F 98 80 */
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v =
        parse_byte_by_byte(a, "\"\\ud83d\\ude00\"", 14, &st);
    CHECK_EQ(st, OI_OK);
    const char *ptr;
    size_t len;
    CHECK_EQ(oi_json_get_string(v, &ptr, &len), OI_OK);
    CHECK_EQ(len, 4u);
    CHECK_EQ((unsigned char)ptr[0], 0xF0u);
    CHECK_EQ((unsigned char)ptr[1], 0x9Fu);
    CHECK_EQ((unsigned char)ptr[2], 0x98u);
    CHECK_EQ((unsigned char)ptr[3], 0x80u);
    oi_arena_destroy(a);
}

TEST(string_lone_high_surrogate_errors) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    parse_byte_by_byte(a, "\"\\ud83d\"", 8, &st); /* closes without pair */
    CHECK_EQ(st, OI_ERR_PARSE);
    parse_byte_by_byte(a, "\"\\ud83dx\"", 9, &st); /* not followed by \u */
    CHECK_EQ(st, OI_ERR_PARSE);
    parse_byte_by_byte(a, "\"\\ud83d\\n\"", 10, &st); /* wrong escape after */
    CHECK_EQ(st, OI_ERR_PARSE);
    oi_arena_destroy(a);
}

TEST(string_lone_low_surrogate_errors) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    parse_byte_by_byte(a, "\"\\ude00\"", 8, &st);
    CHECK_EQ(st, OI_ERR_PARSE);
    oi_arena_destroy(a);
}

TEST(string_unescaped_control_char_errors) {
    oi_arena *a = oi_arena_create(0);
    char doc[3] = {'"', '\n', '"'};
    oi_status st;
    parse_byte_by_byte(a, doc, 3, &st);
    CHECK_EQ(st, OI_ERR_PARSE);
    oi_arena_destroy(a);
}

TEST(string_unterminated_is_incomplete_not_error) {
    oi_arena *a = oi_arena_create(0);
    oi_json_parser *p = oi_json_parser_create(a);
    CHECK_EQ(oi_json_parser_feed(p, "\"abc", 4), OI_OK);
    CHECK(!oi_json_parser_done(p));
    CHECK(!oi_json_parser_failed(p));
    CHECK_EQ(oi_json_parser_finish(p), OI_ERR_PARSE);
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

TEST(string_embedded_nul_via_escape) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, "\"a\\u0000b\"", 10, &st);
    CHECK_EQ(st, OI_OK);
    const char *ptr;
    size_t len;
    CHECK_EQ(oi_json_get_string(v, &ptr, &len), OI_OK);
    CHECK_EQ(len, 3u);
    CHECK_EQ(ptr[0], 'a');
    CHECK_EQ(ptr[1], '\0');
    CHECK_EQ(ptr[2], 'b');
    oi_arena_destroy(a);
}

/* --- containers --- */

TEST(empty_array_and_object) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *arr = parse_byte_by_byte(a, "[]", 2, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_json_type_of(arr), OI_JSON_ARRAY);
    CHECK_EQ(oi_json_array_len(arr), 0u);

    oi_json_value *obj = parse_byte_by_byte(a, "{}", 2, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_json_type_of(obj), OI_JSON_OBJECT);
    CHECK_EQ(oi_json_object_len(obj), 0u);
    oi_arena_destroy(a);
}

TEST(array_of_mixed_values) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    const char *doc = "[1, \"two\", true, null, [3], {\"k\":4}]";
    oi_json_value *v = parse_byte_by_byte(a, doc, strlen(doc), &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_json_array_len(v), 6u);

    double n;
    CHECK_EQ(oi_json_get_number(oi_json_array_get(v, 0), &n), OI_OK);
    CHECK(n == 1.0);

    const char *ptr;
    size_t len;
    CHECK_EQ(oi_json_get_string(oi_json_array_get(v, 1), &ptr, &len), OI_OK);
    CHECK_EQ(len, 3u);

    int b;
    CHECK_EQ(oi_json_get_bool(oi_json_array_get(v, 2), &b), OI_OK);
    CHECK_EQ(b, 1);
    CHECK_EQ(oi_json_type_of(oi_json_array_get(v, 3)), OI_JSON_NULL);
    CHECK_EQ(oi_json_type_of(oi_json_array_get(v, 4)), OI_JSON_ARRAY);
    CHECK_EQ(oi_json_type_of(oi_json_array_get(v, 5)), OI_JSON_OBJECT);

    CHECK(oi_json_array_get(v, 6) == NULL); /* out of range */
    oi_arena_destroy(a);
}

TEST(nested_object) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    const char *doc = "{\"a\":{\"b\":{\"c\":1}}}";
    oi_json_value *v = parse_byte_by_byte(a, doc, strlen(doc), &st);
    CHECK_EQ(st, OI_OK);
    oi_json_value *b = oi_json_object_get(v, "a");
    CHECK(b != NULL);
    b = oi_json_object_get(b, "b");
    CHECK(b != NULL);
    b = oi_json_object_get(b, "c");
    CHECK(b != NULL);
    double n;
    CHECK_EQ(oi_json_get_number(b, &n), OI_OK);
    CHECK(n == 1.0);
    CHECK(oi_json_object_get(v, "missing") == NULL);
    oi_arena_destroy(a);
}

TEST(deeply_nested_array_within_limit) {
    oi_arena *a = oi_arena_create(0);
    enum { N = 100 };
    char doc[2 * N + 1];
    for (int i = 0; i < N; i++) {
        doc[i] = '[';
    }
    for (int i = 0; i < N; i++) {
        doc[N + i] = ']';
    }
    doc[2 * N] = '\0';
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, doc, 2 * N, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_json_type_of(v), OI_JSON_ARRAY);
    oi_arena_destroy(a);
}

TEST(excessive_nesting_is_rejected) {
    oi_arena *a = oi_arena_create(0);
    enum { N = 2000 };
    char *doc = malloc(N + 1);
    for (int i = 0; i < N; i++) {
        doc[i] = '[';
    }
    doc[N] = '\0';
    oi_json_parser *p = oi_json_parser_create(a);
    oi_status st = OI_OK;
    for (int i = 0; i < N && st == OI_OK; i++) {
        st = oi_json_parser_feed(p, doc + i, 1);
    }
    CHECK_EQ(st, OI_ERR_PARSE);
    free(doc);
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

/* --- structural errors --- */

TEST(structural_errors) {
    static const char *bad[] = {
        "",       "[",      "]",       "{",      "}",
        ",",      ":",      "[,]",     "[1,]",   "{,}",
        "{\"a\":1,}", "{1:1}", "{\"a\"}", "{\"a\":}", "[1 2]",
        "{\"a\":1 \"b\":2}", "nul", "tru", "fals", "01",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        oi_arena *a = oi_arena_create(0);
        oi_json_parser *p = oi_json_parser_create(a);
        size_t len = strlen(bad[i]);
        oi_status st = oi_json_parser_feed(p, bad[i], len);
        if (st == OI_OK) {
            st = oi_json_parser_finish(p);
        }
        CHECK(st != OI_OK);
        oi_json_parser_destroy(p);
        oi_arena_destroy(a);
    }
}

TEST(trailing_garbage_after_value_is_error) {
    oi_arena *a = oi_arena_create(0);
    oi_json_parser *p = oi_json_parser_create(a);
    CHECK_EQ(oi_json_parser_feed(p, "{}", 2), OI_OK);
    CHECK(oi_json_parser_done(p));
    CHECK_EQ(oi_json_parser_feed(p, "x", 1), OI_ERR_PARSE);
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

TEST(trailing_whitespace_after_value_is_fine) {
    oi_arena *a = oi_arena_create(0);
    oi_json_parser *p = oi_json_parser_create(a);
    CHECK_EQ(oi_json_parser_feed(p, "{}  \n\t", 6), OI_OK);
    CHECK(oi_json_parser_done(p));
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

TEST(sticky_error_state) {
    oi_arena *a = oi_arena_create(0);
    oi_json_parser *p = oi_json_parser_create(a);
    CHECK_EQ(oi_json_parser_feed(p, "[", 1), OI_OK);
    CHECK_EQ(oi_json_parser_feed(p, ",", 1), OI_ERR_PARSE);
    CHECK(oi_json_parser_failed(p));
    /* Further feeds keep failing without re-examining input. */
    CHECK_EQ(oi_json_parser_feed(p, "1", 1), OI_ERR_PARSE);
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

/* --- reset / reuse across multiple values on one parser --- */

TEST(reset_allows_parsing_next_value) {
    oi_arena *a = oi_arena_create(0);
    oi_json_parser *p = oi_json_parser_create(a);

    CHECK_EQ(oi_json_parser_feed(p, "{\"n\":1}", 7), OI_OK);
    CHECK(oi_json_parser_done(p));
    double n;
    CHECK_EQ(
        oi_json_get_number(oi_json_object_get(oi_json_parser_root(p), "n"),
                            &n),
        OI_OK);
    CHECK(n == 1.0);

    oi_json_parser_reset(p);
    CHECK(!oi_json_parser_done(p));

    CHECK_EQ(oi_json_parser_feed(p, "{\"n\":2}", 7), OI_OK);
    CHECK(oi_json_parser_done(p));
    CHECK_EQ(
        oi_json_get_number(oi_json_object_get(oi_json_parser_root(p), "n"),
                            &n),
        OI_OK);
    CHECK(n == 2.0);

    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

/* --- byte-boundary splitting: every cut point of representative docs --- */

TEST(every_split_point_of_representative_docs) {
    static const char *docs[] = {
        /* Parenthesized so the wrap reads as one document rather than
         * two array elements with a missing comma (-Wstring-concatenation). */
        ("{\"model\":\"gpt\",\"messages\":[{\"role\":\"user\",\"content\":"
         "\"hi\\nthere\"}],\"n\":1,\"temperature\":0.5,\"stream\":true}"),
        "\"escaped \\\"quote\\\" and \\u00e9 and \\ud83d\\ude00 end\"",
        "[1,2.5,-3,4e2,null,true,false,\"s\",[1,2],{\"a\":1}]",
        "{}",
        "[]",
    };
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        check_all_split_points_succeed(docs[i], strlen(docs[i]));
    }
}

/* --- rejects NULL / zero-length misuse --- */

TEST(create_rejects_null_arena) { CHECK(oi_json_parser_create(NULL) == NULL); }

TEST(destroy_null_safe) { oi_json_parser_destroy(NULL); }

TEST(accessors_reject_wrong_type_and_null) {
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, "1", 1, &st);
    CHECK_EQ(st, OI_OK);
    /* "1" alone needs finish() in real use; here we just want a NUMBER
     * value to probe wrong-type accessors against, so reparse using
     * finish() directly. */
    oi_json_parser *p = oi_json_parser_create(a);
    oi_json_parser_feed(p, "1", 1);
    oi_json_parser_finish(p);
    v = oi_json_parser_root(p);

    int b;
    const char *ptr;
    size_t len;
    CHECK_EQ(oi_json_get_bool(v, &b), OI_ERR_INVAL);
    CHECK_EQ(oi_json_get_string(v, &ptr, &len), OI_ERR_INVAL);
    CHECK_EQ(oi_json_get_bool(NULL, &b), OI_ERR_INVAL);
    CHECK_EQ(oi_json_array_len(v), 0u);
    CHECK(oi_json_array_get(v, 0) == NULL);
    CHECK_EQ(oi_json_object_len(NULL), 0u);
    CHECK(oi_json_object_get(NULL, "x") == NULL);

    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

/* --- arena interaction: a value too big for a small block_size --- */

TEST(string_larger_than_arena_block_size_is_nomem) {
    oi_arena *a = oi_arena_create(64); /* deliberately tiny */
    oi_json_parser *p = oi_json_parser_create(a);
    char doc[200];
    doc[0] = '"';
    memset(doc + 1, 'x', 190);
    doc[191] = '"';
    oi_status st = oi_json_parser_feed(p, doc, 192);
    CHECK_EQ(st, OI_ERR_NOMEM);
    oi_json_parser_destroy(p);
    oi_arena_destroy(a);
}

/* ================= writer ================= */

TEST(write_scalars) {
    oi_json_writer *w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_null(w), OI_OK);
    size_t len;
    CHECK_STREQ(oi_json_writer_data(w, &len), "null");
    CHECK_EQ(len, 4u);
    oi_json_writer_destroy(w);

    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_bool(w, 1), OI_OK);
    CHECK_STREQ(oi_json_writer_data(w, NULL), "true");
    oi_json_writer_destroy(w);

    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_number(w, 42), OI_OK);
    CHECK_STREQ(oi_json_writer_data(w, NULL), "42");
    oi_json_writer_destroy(w);

    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_string(w, "hi\n\"x\"", 6), OI_OK);
    CHECK_STREQ(oi_json_writer_data(w, NULL), "\"hi\\n\\\"x\\\"\"");
    oi_json_writer_destroy(w);
}

TEST(write_rejects_nonfinite_number) {
    oi_json_writer *w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_number(w, NAN), OI_ERR_INVAL);
    CHECK_EQ(oi_json_write_number(w, INFINITY), OI_ERR_INVAL);
    CHECK_EQ(oi_json_write_number(w, -INFINITY), OI_ERR_INVAL);
    oi_json_writer_destroy(w);
}

TEST(write_array) {
    oi_json_writer *w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_array_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_number(w, 1), OI_OK);
    CHECK_EQ(oi_json_write_number(w, 2), OI_OK);
    CHECK_EQ(oi_json_write_string(w, "x", 1), OI_OK);
    CHECK_EQ(oi_json_write_array_end(w), OI_OK);
    CHECK_STREQ(oi_json_writer_data(w, NULL), "[1,2,\"x\"]");
    oi_json_writer_destroy(w);
}

TEST(write_empty_array_and_object) {
    oi_json_writer *w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_array_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_array_end(w), OI_OK);
    CHECK_STREQ(oi_json_writer_data(w, NULL), "[]");
    oi_json_writer_destroy(w);

    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_object_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_object_end(w), OI_OK);
    CHECK_STREQ(oi_json_writer_data(w, NULL), "{}");
    oi_json_writer_destroy(w);
}

TEST(write_object) {
    oi_json_writer *w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_object_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_object_key(w, "a", 1), OI_OK);
    CHECK_EQ(oi_json_write_number(w, 1), OI_OK);
    CHECK_EQ(oi_json_write_object_key(w, "b", 1), OI_OK);
    CHECK_EQ(oi_json_write_bool(w, 0), OI_OK);
    CHECK_EQ(oi_json_write_object_end(w), OI_OK);
    CHECK_STREQ(oi_json_writer_data(w, NULL), "{\"a\":1,\"b\":false}");
    oi_json_writer_destroy(w);
}

TEST(write_nested) {
    oi_json_writer *w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_object_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_object_key(w, "arr", 3), OI_OK);
    CHECK_EQ(oi_json_write_array_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_object_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_object_key(w, "x", 1), OI_OK);
    CHECK_EQ(oi_json_write_number(w, 1), OI_OK);
    CHECK_EQ(oi_json_write_object_end(w), OI_OK);
    CHECK_EQ(oi_json_write_array_end(w), OI_OK);
    CHECK_EQ(oi_json_write_object_end(w), OI_OK);
    CHECK_STREQ(oi_json_writer_data(w, NULL), "{\"arr\":[{\"x\":1}]}");
    oi_json_writer_destroy(w);
}

TEST(write_output_round_trips_through_parser) {
    oi_json_writer *w = oi_json_writer_create();
    oi_json_write_object_begin(w);
    oi_json_write_object_key(w, "msg", 3);
    oi_json_write_string(w, "hello \"world\"\n", 15);
    oi_json_write_object_key(w, "n", 1);
    oi_json_write_number(w, 3.5);
    oi_json_write_object_end(w);

    size_t len;
    const char *text = oi_json_writer_data(w, &len);

    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, text, len, &st);
    CHECK_EQ(st, OI_OK);
    const char *ptr;
    size_t slen;
    CHECK_EQ(
        oi_json_get_string(oi_json_object_get(v, "msg"), &ptr, &slen),
        OI_OK);
    CHECK_EQ(slen, 15u);
    CHECK(memcmp(ptr, "hello \"world\"\n", 15) == 0);
    double n;
    CHECK_EQ(oi_json_get_number(oi_json_object_get(v, "n"), &n), OI_OK);
    CHECK(n == 3.5);

    oi_arena_destroy(a);
    oi_json_writer_destroy(w);
}

TEST(write_structural_misuse_is_rejected) {
    oi_json_writer *w = oi_json_writer_create();
    /* second top-level value */
    CHECK_EQ(oi_json_write_number(w, 1), OI_OK);
    CHECK_EQ(oi_json_write_number(w, 2), OI_ERR_INVAL);
    oi_json_writer_destroy(w);

    /* key outside any object */
    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_object_key(w, "a", 1), OI_ERR_INVAL);
    oi_json_writer_destroy(w);

    /* value without a preceding key, inside an object */
    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_object_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_number(w, 1), OI_ERR_INVAL);
    oi_json_writer_destroy(w);

    /* mismatched close */
    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_array_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_object_end(w), OI_ERR_INVAL);
    oi_json_writer_destroy(w);

    /* closing an object right after a key, with no value */
    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_object_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_object_key(w, "a", 1), OI_OK);
    CHECK_EQ(oi_json_write_object_end(w), OI_ERR_INVAL);
    oi_json_writer_destroy(w);

    /* close with nothing open */
    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_array_end(w), OI_ERR_INVAL);
    oi_json_writer_destroy(w);
}

TEST(writer_create_destroy_null_safe) {
    oi_json_writer *w = oi_json_writer_create();
    CHECK(w != NULL);
    oi_json_writer_destroy(w);
    oi_json_writer_destroy(NULL);
}

TEST(writer_rejects_invalid_and_overflowing_inputs) {
    size_t len = 123;
    CHECK_EQ(oi_json_write_null(NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_json_write_array_end(NULL), OI_ERR_INVAL);
    CHECK(oi_json_writer_data(NULL, &len) == NULL);
    CHECK_EQ(len, 0u);

    oi_json_writer *w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_string(w, NULL, 1), OI_ERR_INVAL);
    CHECK_EQ(oi_json_write_string(w, "", (size_t)-1), OI_ERR_NOMEM);
    oi_json_writer_destroy(w);

    w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_object_begin(w), OI_OK);
    CHECK_EQ(oi_json_write_object_key(w, NULL, 1), OI_ERR_INVAL);
    CHECK_EQ(oi_json_write_object_key(w, "", (size_t)-1), OI_ERR_NOMEM);
    oi_json_writer_destroy(w);
}

TEST(value_builders_reject_overflowing_lengths) {
    oi_arena *a = oi_arena_create(0);
    CHECK(oi_json_new_string(a, "x", (size_t)-1) == NULL);
    CHECK(oi_json_new_string(a, NULL, 1) == NULL);

    oi_json_value *object = oi_json_new_object(a);
    CHECK_EQ(oi_json_object_append(a, object, "x", (size_t)-1, NULL),
              OI_ERR_NOMEM);

    oi_json_value *array = oi_json_new_array(a);
    array->u.array.capacity = (size_t)-1 / 2 + 1;
    array->u.array.count = array->u.array.capacity;
    CHECK_EQ(oi_json_array_push(a, array, NULL), OI_ERR_NOMEM);
    oi_arena_destroy(a);
}

TEST(write_many_elements_grows_buffer_and_frames) {
    oi_json_writer *w = oi_json_writer_create();
    CHECK_EQ(oi_json_write_array_begin(w), OI_OK);
    for (int i = 0; i < 500; i++) {
        CHECK_EQ(oi_json_write_number(w, i), OI_OK);
    }
    CHECK_EQ(oi_json_write_array_end(w), OI_OK);

    size_t len;
    const char *text = oi_json_writer_data(w, &len);
    oi_arena *a = oi_arena_create(0);
    oi_status st;
    oi_json_value *v = parse_byte_by_byte(a, text, len, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_json_array_len(v), 500u);
    oi_arena_destroy(a);
    oi_json_writer_destroy(w);
}

int main(void) {
    RUN(parse_null);
    RUN(parse_true_false);
    RUN(literal_typo_is_error);
    RUN(numbers);
    RUN(number_terminated_by_following_byte_needs_no_finish);
    RUN(bare_number_not_done_until_finish);
    RUN(finish_rejects_unclosed_container);
    RUN(finish_on_empty_input_is_error);
    RUN(simple_string);
    RUN(empty_string);
    RUN(string_simple_escapes);
    RUN(string_unicode_escape_bmp);
    RUN(string_unicode_surrogate_pair);
    RUN(string_lone_high_surrogate_errors);
    RUN(string_lone_low_surrogate_errors);
    RUN(string_unescaped_control_char_errors);
    RUN(string_unterminated_is_incomplete_not_error);
    RUN(string_embedded_nul_via_escape);
    RUN(empty_array_and_object);
    RUN(array_of_mixed_values);
    RUN(nested_object);
    RUN(deeply_nested_array_within_limit);
    RUN(excessive_nesting_is_rejected);
    RUN(structural_errors);
    RUN(trailing_garbage_after_value_is_error);
    RUN(trailing_whitespace_after_value_is_fine);
    RUN(sticky_error_state);
    RUN(reset_allows_parsing_next_value);
    RUN(every_split_point_of_representative_docs);
    RUN(create_rejects_null_arena);
    RUN(destroy_null_safe);
    RUN(accessors_reject_wrong_type_and_null);
    RUN(string_larger_than_arena_block_size_is_nomem);

    RUN(write_scalars);
    RUN(write_rejects_nonfinite_number);
    RUN(write_array);
    RUN(write_empty_array_and_object);
    RUN(write_object);
    RUN(write_nested);
    RUN(write_output_round_trips_through_parser);
    RUN(write_structural_misuse_is_rejected);
    RUN(writer_create_destroy_null_safe);
    RUN(writer_rejects_invalid_and_overflowing_inputs);
    RUN(value_builders_reject_overflowing_lengths);
    RUN(write_many_elements_grows_buffer_and_frames);

    return oi_test_report();
}
