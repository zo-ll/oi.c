#include "cli_markdown.h"
#include "test.h"

TEST(starts_empty) {
    struct oi_cli_md_span_list spans;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(spans.len, (size_t)0);
    CHECK(oi_cli_md_span_list_covers(&spans, 0));

    oi_cli_md_span_list_free(&spans);
}

TEST(append_grows_and_stores_fields) {
    struct oi_cli_md_span_list spans;
    size_t i;

    oi_cli_md_span_list_init(&spans);
    for (i = 0; i < 20; i++) {
        CHECK_EQ(oi_cli_md_span_list_append(&spans, i, 1,
                                            OI_CLI_MD_STYLE_BOLD),
                 OI_OK);
    }
    CHECK_EQ(spans.len, (size_t)20);
    for (i = 0; i < 20; i++) {
        CHECK_EQ(spans.runs[i].start, i);
        CHECK_EQ(spans.runs[i].len, (size_t)1);
        CHECK_EQ(spans.runs[i].style_bits, (unsigned)OI_CLI_MD_STYLE_BOLD);
    }
    CHECK(oi_cli_md_span_list_covers(&spans, 20));

    oi_cli_md_span_list_free(&spans);
}

TEST(covers_rejects_gaps_overlaps_and_wrong_total) {
    struct oi_cli_md_span_list spans;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 3, 0), OI_OK);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 5, 2, 0), OI_OK);
    CHECK(!oi_cli_md_span_list_covers(&spans, 7)); /* gap between 3 and 5 */
    oi_cli_md_span_list_free(&spans);

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 3, 0), OI_OK);
    CHECK(!oi_cli_md_span_list_covers(&spans, 5)); /* short of text_len */
    oi_cli_md_span_list_free(&spans);

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 3, 0), OI_OK);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 3, 0), OI_OK);
    CHECK(!oi_cli_md_span_list_covers(&spans, 6)); /* overlap: bad start */
    oi_cli_md_span_list_free(&spans);

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 0, 0), OI_OK);
    CHECK(!oi_cli_md_span_list_covers(&spans, 0)); /* zero-length run */
    oi_cli_md_span_list_free(&spans);
}

TEST(free_resets_to_empty_and_is_safe_to_call_twice) {
    struct oi_cli_md_span_list spans;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 1, 0), OI_OK);
    oi_cli_md_span_list_free(&spans);
    CHECK_EQ(spans.len, (size_t)0);
    CHECK_EQ(spans.cap, (size_t)0);
    CHECK(spans.runs == NULL);
    oi_cli_md_span_list_free(&spans);
}

int main(void) {
    RUN(starts_empty);
    RUN(append_grows_and_stores_fields);
    RUN(covers_rejects_gaps_overlaps_and_wrong_total);
    RUN(free_resets_to_empty_and_is_safe_to_call_twice);
    return oi_test_report();
}
