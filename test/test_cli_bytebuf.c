#include "cli_bytebuf.h"
#include "test.h"

#include <string.h>

TEST(append_starts_empty_and_grows) {
    struct oi_cli_bytebuf buf;

    oi_cli_bytebuf_init(&buf);
    CHECK_EQ(buf.len, (size_t)0);
    CHECK_EQ(oi_cli_bytebuf_append(&buf, "abc", 3), OI_OK);
    CHECK_EQ(buf.len, (size_t)3);
    CHECK(memcmp(buf.data, "abc", 3) == 0);

    oi_cli_bytebuf_free(&buf);
}

TEST(append_crosses_the_initial_capacity_boundary) {
    struct oi_cli_bytebuf buf;
    char chunk[100];
    size_t i;

    memset(chunk, 'x', sizeof chunk);
    oi_cli_bytebuf_init(&buf);
    for (i = 0; i < 3; i++) {
        CHECK_EQ(oi_cli_bytebuf_append(&buf, chunk, sizeof chunk), OI_OK);
    }
    CHECK_EQ(buf.len, (size_t)300);
    CHECK(buf.cap >= 300);
    for (i = 0; i < 3; i++) {
        CHECK(memcmp(buf.data + i * sizeof chunk, chunk, sizeof chunk) == 0);
    }

    oi_cli_bytebuf_free(&buf);
}

TEST(zero_length_append_is_a_no_op) {
    struct oi_cli_bytebuf buf;

    oi_cli_bytebuf_init(&buf);
    CHECK_EQ(oi_cli_bytebuf_append(&buf, "abc", 3), OI_OK);
    CHECK_EQ(oi_cli_bytebuf_append(&buf, NULL, 0), OI_OK);
    CHECK_EQ(buf.len, (size_t)3);

    oi_cli_bytebuf_free(&buf);
}

TEST(reset_keeps_capacity_and_clears_length) {
    struct oi_cli_bytebuf buf;
    size_t cap_after_first_append;

    oi_cli_bytebuf_init(&buf);
    CHECK_EQ(oi_cli_bytebuf_append(&buf, "hello", 5), OI_OK);
    cap_after_first_append = buf.cap;
    oi_cli_bytebuf_reset(&buf);
    CHECK_EQ(buf.len, (size_t)0);
    CHECK_EQ(buf.cap, cap_after_first_append);
    CHECK_EQ(oi_cli_bytebuf_append(&buf, "hi", 2), OI_OK);
    CHECK_EQ(buf.len, (size_t)2);
    CHECK(memcmp(buf.data, "hi", 2) == 0);

    oi_cli_bytebuf_free(&buf);
}

TEST(overflow_guard_rejects_without_corrupting_state) {
    struct oi_cli_bytebuf buf;

    oi_cli_bytebuf_init(&buf);
    CHECK_EQ(oi_cli_bytebuf_append(&buf, "abc", 3), OI_OK);
    CHECK_EQ(oi_cli_bytebuf_append(&buf, "x", (size_t)-1), OI_ERR_NOMEM);
    CHECK_EQ(buf.len, (size_t)3);
    CHECK(memcmp(buf.data, "abc", 3) == 0);

    oi_cli_bytebuf_free(&buf);
}

TEST(free_resets_to_empty_and_is_safe_to_call_twice) {
    struct oi_cli_bytebuf buf;

    oi_cli_bytebuf_init(&buf);
    CHECK_EQ(oi_cli_bytebuf_append(&buf, "abc", 3), OI_OK);
    oi_cli_bytebuf_free(&buf);
    CHECK_EQ(buf.len, (size_t)0);
    CHECK_EQ(buf.cap, (size_t)0);
    CHECK(buf.data == NULL);
    oi_cli_bytebuf_free(&buf);
}

int main(void) {
    RUN(append_starts_empty_and_grows);
    RUN(append_crosses_the_initial_capacity_boundary);
    RUN(zero_length_append_is_a_no_op);
    RUN(reset_keeps_capacity_and_clears_length);
    RUN(overflow_guard_rejects_without_corrupting_state);
    RUN(free_resets_to_empty_and_is_safe_to_call_twice);
    return oi_test_report();
}
