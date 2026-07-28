#include "../src/llm_http.h"
#include "test.h"

#include <string.h>

struct capture {
    int headers_done;
    int status_code;
    char body[4096];
    size_t body_len;
};

static void on_headers_done(int status_code, void *ud) {
    struct capture *c = ud;
    c->headers_done = 1;
    c->status_code = status_code;
}

static void on_body(const void *data, size_t len, void *ud) {
    struct capture *c = ud;
    CHECK(c->body_len + len <= sizeof c->body);
    memcpy(c->body + c->body_len, data, len);
    c->body_len += len;
}

static oi_status feed_byte_by_byte(oi_llm_http_parser *p, const char *doc,
                                    size_t len) {
    oi_status st = OI_OK;
    for (size_t i = 0; i < len && st == OI_OK; i++) {
        st = oi_llm_http_parser_feed(p, doc + i, 1);
    }
    return st;
}

TEST(chunked_response) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);

    const char *doc =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/event-stream\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "6\r\n"
        " world\r\n"
        "0\r\n"
        "\r\n";
    oi_status st = feed_byte_by_byte(p, doc, strlen(doc));
    CHECK_EQ(st, OI_OK);
    CHECK(c.headers_done);
    CHECK_EQ(c.status_code, 200);
    CHECK_EQ(c.body_len, 11u);
    CHECK(memcmp(c.body, "hello world", 11) == 0);
    CHECK(oi_llm_http_parser_body_done(p));
    CHECK(!oi_llm_http_parser_failed(p));

    oi_llm_http_parser_destroy(p);
}

TEST(content_length_response) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);

    const char *doc =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"error\":42}\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_OK);
    CHECK_EQ(c.status_code, 400);
    CHECK_EQ(c.body_len, 13u);
    CHECK(memcmp(c.body, "{\"error\":42}\n", 13) == 0);
    CHECK(oi_llm_http_parser_body_done(p));

    oi_llm_http_parser_destroy(p);
}

TEST(zero_content_length_completes_immediately) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    const char *doc = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_OK);
    CHECK(oi_llm_http_parser_body_done(p));
    CHECK_EQ(c.body_len, 0u);
    oi_llm_http_parser_destroy(p);
}

TEST(no_body_indicated_completes_after_headers) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    const char *doc = "HTTP/1.1 304 Not Modified\r\n\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_OK);
    CHECK(oi_llm_http_parser_body_done(p));
    oi_llm_http_parser_destroy(p);
}

TEST(header_name_case_insensitive) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    const char *doc =
        "HTTP/1.1 200 OK\r\ntransfer-ENCODING: chunked\r\n\r\n0\r\n\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_OK);
    CHECK(oi_llm_http_parser_body_done(p));
    CHECK_EQ(c.body_len, 0u);
    oi_llm_http_parser_destroy(p);
}

TEST(bare_lf_line_endings_accepted) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    const char *doc = "HTTP/1.1 200 OK\nContent-Length: 2\n\nhi";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_OK);
    CHECK_EQ(c.body_len, 2u);
    CHECK(memcmp(c.body, "hi", 2) == 0);
    oi_llm_http_parser_destroy(p);
}

TEST(malformed_status_line_errors) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    const char *doc = "NOT AN HTTP LINE\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_ERR_PARSE);
    CHECK(oi_llm_http_parser_failed(p));
    oi_llm_http_parser_destroy(p);
}

TEST(non_http_status_line_errors) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    const char *doc = "FAKE/1.1 200 OK\r\n";
    CHECK_EQ(oi_llm_http_parser_feed(p, doc, strlen(doc)), OI_ERR_PARSE);
    oi_llm_http_parser_destroy(p);
}

TEST(malformed_chunk_size_errors) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    const char *doc =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZZ\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_ERR_PARSE);
    oi_llm_http_parser_destroy(p);
}

TEST(chunk_missing_trailing_crlf_errors) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    /* "hello" chunk of size 5 but followed by garbage instead of CRLF */
    const char *doc =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_ERR_PARSE);
    oi_llm_http_parser_destroy(p);
}

TEST(feed_after_done_is_ignored) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    const char *doc = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_OK);
    CHECK(oi_llm_http_parser_body_done(p));
    CHECK_EQ(oi_llm_http_parser_feed(p, "garbage", 7), OI_OK);
    oi_llm_http_parser_destroy(p);
}

TEST(feed_after_error_stays_failed) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);
    CHECK_EQ(oi_llm_http_parser_feed(p, "bad\r\n", 5), OI_ERR_PARSE);
    CHECK_EQ(oi_llm_http_parser_feed(p, "more", 4), OI_ERR_PARSE);
    oi_llm_http_parser_destroy(p);
}

TEST(every_split_point_of_chunked_doc) {
    const char *doc =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    size_t len = strlen(doc);
    for (size_t k = 0; k <= len; k++) {
        struct capture c = {0};
        oi_llm_http_parser *p =
            oi_llm_http_parser_create(on_headers_done, on_body, &c);
        oi_status st = oi_llm_http_parser_feed(p, doc, k);
        if (st == OI_OK) {
            st = oi_llm_http_parser_feed(p, doc + k, len - k);
        }
        CHECK_EQ(st, OI_OK);
        CHECK(oi_llm_http_parser_body_done(p));
        CHECK_EQ(c.body_len, 5u);
        CHECK(memcmp(c.body, "hello", 5) == 0);
        oi_llm_http_parser_destroy(p);
    }
}

TEST(create_null_callbacks_ok) {
    oi_llm_http_parser *p = oi_llm_http_parser_create(NULL, NULL, NULL);
    CHECK(p != NULL);
    const char *doc = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_OK);
    CHECK(oi_llm_http_parser_body_done(p));
    oi_llm_http_parser_destroy(p);
}

TEST(destroy_null_safe) { oi_llm_http_parser_destroy(NULL); }

struct destroy_headers_ctx {
    oi_llm_http_parser *parser;
    int called;
};

static void destroy_on_headers(int status_code, void *ud) {
    (void)status_code;
    struct destroy_headers_ctx *ctx = ud;
    ctx->called++;
    oi_llm_http_parser_destroy(ctx->parser);
}

TEST(destroy_from_headers_callback_is_safe) {
    struct destroy_headers_ctx ctx = {0};
    ctx.parser = oi_llm_http_parser_create(destroy_on_headers, NULL, &ctx);
    const char *doc =
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nbody";
    CHECK_EQ(oi_llm_http_parser_feed(ctx.parser, doc, strlen(doc)), OI_OK);
    CHECK_EQ(ctx.called, 1);
}

/*
 * Regression tests for three signed-overflow defects found by
 * test/fuzz/fuzz_http.c. Content-Length and chunk-size digits arrive
 * from an untrusted response header and were accumulated into a `long`
 * with no bound, so a long enough digit run was undefined behavior and
 * could leave the body reader with a negative byte count.
 */

TEST(overlong_content_length_errors) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);

    const char *doc =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 99999999999999999999999\r\n"
        "\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_ERR_PARSE);
    CHECK(oi_llm_http_parser_failed(p));

    oi_llm_http_parser_destroy(p);
}

TEST(overlong_chunk_size_errors) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);

    const char *doc =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "ffffffffffffffffffff\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_ERR_PARSE);
    CHECK(oi_llm_http_parser_failed(p));

    oi_llm_http_parser_destroy(p);
}

TEST(empty_content_length_value_errors) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);

    /* Previously parsed as 0, silently framing the response as having
     * no body rather than rejecting the malformed header. */
    const char *doc =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length:\r\n"
        "\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_ERR_PARSE);
    CHECK(oi_llm_http_parser_failed(p));

    oi_llm_http_parser_destroy(p);
}

TEST(ambiguous_response_framing_errors) {
    const char *docs[] = {
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunkedx\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 3\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
        "Content-Length: 4\r\n\r\n",
        "HTTP/1.1 200 OK\r\n: value\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        struct capture c = {0};
        oi_llm_http_parser *p =
            oi_llm_http_parser_create(on_headers_done, on_body, &c);
        CHECK_EQ(feed_byte_by_byte(p, docs[i], strlen(docs[i])),
                  OI_ERR_PARSE);
        CHECK(oi_llm_http_parser_failed(p));
        oi_llm_http_parser_destroy(p);
    }
}

/* The largest accepted values must still parse, so the bound rejects
 * only genuinely out-of-range input. */
TEST(large_but_valid_content_length_accepted) {
    struct capture c = {0};
    oi_llm_http_parser *p =
        oi_llm_http_parser_create(on_headers_done, on_body, &c);

    const char *doc =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 1099511627776\r\n" /* exactly 1 TiB */
        "\r\n";
    CHECK_EQ(feed_byte_by_byte(p, doc, strlen(doc)), OI_OK);
    CHECK(c.headers_done);
    CHECK(!oi_llm_http_parser_failed(p));
    CHECK(!oi_llm_http_parser_body_done(p)); /* still awaiting the body */

    oi_llm_http_parser_destroy(p);
}

int main(void) {
    RUN(chunked_response);
    RUN(content_length_response);
    RUN(zero_content_length_completes_immediately);
    RUN(no_body_indicated_completes_after_headers);
    RUN(header_name_case_insensitive);
    RUN(bare_lf_line_endings_accepted);
    RUN(malformed_status_line_errors);
    RUN(non_http_status_line_errors);
    RUN(malformed_chunk_size_errors);
    RUN(chunk_missing_trailing_crlf_errors);
    RUN(feed_after_done_is_ignored);
    RUN(feed_after_error_stays_failed);
    RUN(every_split_point_of_chunked_doc);
    RUN(create_null_callbacks_ok);
    RUN(destroy_null_safe);
    RUN(destroy_from_headers_callback_is_safe);
    RUN(overlong_content_length_errors);
    RUN(overlong_chunk_size_errors);
    RUN(empty_content_length_value_errors);
    RUN(ambiguous_response_framing_errors);
    RUN(large_but_valid_content_length_accepted);
    return oi_test_report();
}
