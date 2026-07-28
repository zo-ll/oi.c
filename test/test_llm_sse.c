#include "../src/llm_sse.h"
#include "test.h"

#include <string.h>

struct capture {
    char events[16][256];
    size_t lens[16];
    int count;
};

static void on_event(const char *data, size_t len, void *ud) {
    struct capture *c = ud;
    CHECK(c->count < 16);
    CHECK(len < sizeof c->events[0]);
    memcpy(c->events[c->count], data, len);
    c->lens[c->count] = len;
    c->count++;
}

TEST(single_event) {
    struct capture c = {0};
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, &c);
    const char *doc = "data: {\"a\":1}\n\n";
    CHECK_EQ(oi_llm_sse_parser_feed(p, doc, strlen(doc)), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK_EQ(c.lens[0], 7u);
    CHECK(memcmp(c.events[0], "{\"a\":1}", 7) == 0);
    oi_llm_sse_parser_destroy(p);
}

TEST(multiple_events) {
    struct capture c = {0};
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, &c);
    const char *doc = "data: one\n\ndata: two\n\ndata: [DONE]\n\n";
    CHECK_EQ(oi_llm_sse_parser_feed(p, doc, strlen(doc)), OI_OK);
    CHECK_EQ(c.count, 3);
    CHECK(memcmp(c.events[0], "one", 3) == 0);
    CHECK(memcmp(c.events[1], "two", 3) == 0);
    CHECK(memcmp(c.events[2], "[DONE]", 6) == 0);
    oi_llm_sse_parser_destroy(p);
}

TEST(data_without_space_still_strips_colon) {
    struct capture c = {0};
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, &c);
    const char *doc = "data:noSpace\n\n";
    CHECK_EQ(oi_llm_sse_parser_feed(p, doc, strlen(doc)), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK_EQ(c.lens[0], 7u);
    CHECK(memcmp(c.events[0], "noSpace", 7) == 0);
    oi_llm_sse_parser_destroy(p);
}

TEST(non_data_lines_ignored) {
    struct capture c = {0};
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, &c);
    const char *doc = ": this is a comment\nevent: message\nid: 5\n\ndata: only-this\n\n";
    CHECK_EQ(oi_llm_sse_parser_feed(p, doc, strlen(doc)), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK(memcmp(c.events[0], "only-this", 9) == 0);
    oi_llm_sse_parser_destroy(p);
}

TEST(empty_data_line) {
    struct capture c = {0};
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, &c);
    const char *doc = "data:\n\n";
    CHECK_EQ(oi_llm_sse_parser_feed(p, doc, strlen(doc)), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK_EQ(c.lens[0], 0u);
    oi_llm_sse_parser_destroy(p);
}

TEST(crlf_line_endings) {
    struct capture c = {0};
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, &c);
    const char *doc = "data: x\r\n\r\n";
    CHECK_EQ(oi_llm_sse_parser_feed(p, doc, strlen(doc)), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK_EQ(c.lens[0], 1u);
    CHECK_EQ(c.events[0][0], 'x');
    oi_llm_sse_parser_destroy(p);
}

TEST(split_across_many_small_feeds) {
    struct capture c = {0};
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, &c);
    const char *doc = "data: hello world\n\ndata: second\n\n";
    size_t len = strlen(doc);
    for (size_t i = 0; i < len; i++) {
        CHECK_EQ(oi_llm_sse_parser_feed(p, doc + i, 1), OI_OK);
    }
    CHECK_EQ(c.count, 2);
    CHECK(memcmp(c.events[0], "hello world", 11) == 0);
    CHECK(memcmp(c.events[1], "second", 6) == 0);
    oi_llm_sse_parser_destroy(p);
}

TEST(create_null_callback_ok) {
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(NULL, NULL);
    CHECK(p != NULL);
    CHECK_EQ(oi_llm_sse_parser_feed(p, "data: x\n\n", 9), OI_OK);
    oi_llm_sse_parser_destroy(p);
}

TEST(destroy_null_safe) { oi_llm_sse_parser_destroy(NULL); }

TEST(finish_dispatches_unterminated_final_line) {
    struct capture c = {0};
    oi_llm_sse_parser *p = oi_llm_sse_parser_create(on_event, &c);
    CHECK_EQ(oi_llm_sse_parser_feed(p, "data: final", 11), OI_OK);
    CHECK_EQ(c.count, 0);
    CHECK_EQ(oi_llm_sse_parser_finish(p), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK_EQ(c.lens[0], 5u);
    CHECK(memcmp(c.events[0], "final", 5) == 0);
    CHECK_EQ(oi_llm_sse_parser_finish(p), OI_OK);
    CHECK_EQ(c.count, 1);
    oi_llm_sse_parser_destroy(p);
    CHECK_EQ(oi_llm_sse_parser_finish(NULL), OI_ERR_INVAL);
}

int main(void) {
    RUN(single_event);
    RUN(multiple_events);
    RUN(data_without_space_still_strips_colon);
    RUN(non_data_lines_ignored);
    RUN(empty_data_line);
    RUN(crlf_line_endings);
    RUN(split_across_many_small_feeds);
    RUN(create_null_callback_ok);
    RUN(destroy_null_safe);
    RUN(finish_dispatches_unterminated_final_line);
    return oi_test_report();
}
