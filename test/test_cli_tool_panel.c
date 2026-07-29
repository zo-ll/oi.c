#include "cli_tool_panel.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

TEST(start_and_feed_produce_a_header_and_content_lines) {
    struct oi_cli_tool_panel panel;
    struct oi_cli_render_line lines[4] = {0};
    size_t count;

    oi_cli_tool_panel_init(&panel);
    CHECK_EQ(oi_cli_tool_panel_start(&panel, "call-1", 6, "shell", 5),
             OI_OK);
    CHECK_EQ(oi_cli_tool_panel_feed(&panel, "hello\nworld", 11), OI_OK);

    count = oi_cli_tool_panel_lines(&panel, lines, 4);
    CHECK_EQ(count, 3);
    CHECK_EQ(lines[0].len, strlen("shell: running"));
    CHECK(memcmp(lines[0].text, "shell: running", lines[0].len) == 0);
    CHECK_EQ(lines[1].len, 5);
    CHECK(memcmp(lines[1].text, "hello", 5) == 0);
    CHECK_EQ(lines[2].len, 5);
    CHECK(memcmp(lines[2].text, "world", 5) == 0);

    oi_cli_tool_panel_free(&panel);
}

TEST(finish_updates_the_header_status) {
    struct oi_cli_tool_panel panel;
    struct oi_cli_render_line lines[2];

    oi_cli_tool_panel_init(&panel);
    CHECK_EQ(oi_cli_tool_panel_start(&panel, "call-1", 6, "shell", 5),
             OI_OK);
    oi_cli_tool_panel_finish(&panel, OI_CLI_TOOL_PANEL_COMPLETED);

    CHECK_EQ(oi_cli_tool_panel_lines(&panel, lines, 2), 1);
    CHECK_EQ(lines[0].len, strlen("shell: completed"));
    CHECK(memcmp(lines[0].text, "shell: completed", lines[0].len) == 0);

    oi_cli_tool_panel_finish(&panel, OI_CLI_TOOL_PANEL_DENIED);
    CHECK_EQ(oi_cli_tool_panel_lines(&panel, lines, 2), 1);
    CHECK(memcmp(lines[0].text, "shell: denied", strlen("shell: denied")) ==
          0);

    oi_cli_tool_panel_free(&panel);
}

TEST(malicious_escape_sequences_are_stripped_from_displayed_output) {
    struct oi_cli_tool_panel panel;
    struct oi_cli_render_line lines[3];
    static const char raw[] = "\x1b[31mred\x1b[0m\n";
    size_t count;

    oi_cli_tool_panel_init(&panel);
    CHECK_EQ(oi_cli_tool_panel_start(&panel, "call-1", 6, "shell", 5),
             OI_OK);
    CHECK_EQ(oi_cli_tool_panel_feed(&panel, raw, sizeof raw - 1), OI_OK);

    count = oi_cli_tool_panel_lines(&panel, lines, 3);
    CHECK_EQ(count, 2);
    /* The CSI sequences are stripped entirely; only the plain text
     * survives, with no raw ESC byte anywhere in the output. */
    CHECK_EQ(lines[1].len, 3);
    CHECK(memcmp(lines[1].text, "red", 3) == 0);
    CHECK(memchr(lines[1].text, '\x1b', lines[1].len) == NULL);

    oi_cli_tool_panel_free(&panel);
}

TEST(output_is_bounded_and_trims_whole_lines_from_the_front) {
    struct oi_cli_tool_panel panel;
    struct oi_cli_render_line lines[2] = {0};
    char line[16];
    int i;

    oi_cli_tool_panel_init(&panel);
    CHECK_EQ(oi_cli_tool_panel_start(&panel, "call-1", 6, "shell", 5),
             OI_OK);
    /* Each fed line is ~11 bytes; feed enough of them to comfortably
     * exceed OI_CLI_TOOL_PANEL_MAX_BYTES (4096). */
    for (i = 0; i < 600; i++) {
        int len = snprintf(line, sizeof line, "line%04d\n", i);
        CHECK_EQ(oi_cli_tool_panel_feed(&panel, line, (size_t)len), OI_OK);
    }
    CHECK(panel.tail.len <= OI_CLI_TOOL_PANEL_MAX_BYTES);
    /* Only whole lines were ever dropped: the buffer's first byte must be
     * the start of a clean "lineNNNN" entry, never a fragment. */
    CHECK(panel.tail.len >= 4);
    CHECK(memcmp(panel.tail.data, "line", 4) == 0);

    /* The most recent line fed is still present. */
    CHECK_EQ(oi_cli_tool_panel_lines(&panel, lines, 2), 2);
    CHECK_EQ(lines[1].len, 8);
    CHECK(memcmp(lines[1].text, "line0599", 8) == 0);

    oi_cli_tool_panel_free(&panel);
}

TEST(lines_keeps_only_the_most_recent_within_max_lines) {
    struct oi_cli_tool_panel panel;
    struct oi_cli_render_line lines[3] = {0};

    oi_cli_tool_panel_init(&panel);
    CHECK_EQ(oi_cli_tool_panel_start(&panel, "call-1", 6, "shell", 5),
             OI_OK);
    CHECK_EQ(oi_cli_tool_panel_feed(&panel, "one\ntwo\nthree\n", 14), OI_OK);

    /* max_lines=3 -> header + last 2 content lines ("two", "three"), not
     * "one". */
    CHECK_EQ(oi_cli_tool_panel_lines(&panel, lines, 3), 3);
    CHECK_EQ(lines[1].len, 3);
    CHECK(memcmp(lines[1].text, "two", 3) == 0);
    CHECK_EQ(lines[2].len, 5);
    CHECK(memcmp(lines[2].text, "three", 5) == 0);

    oi_cli_tool_panel_free(&panel);
}

TEST(clear_resets_to_inactive) {
    struct oi_cli_tool_panel panel;

    oi_cli_tool_panel_init(&panel);
    CHECK_EQ(oi_cli_tool_panel_start(&panel, "call-1", 6, "shell", 5),
             OI_OK);
    CHECK(panel.active);
    oi_cli_tool_panel_finish(&panel, OI_CLI_TOOL_PANEL_COMPLETED);
    oi_cli_tool_panel_clear(&panel);
    CHECK(!panel.active);
    CHECK_EQ(panel.tool_call_id.len, (size_t)0);
    CHECK_EQ(panel.name_len, (size_t)0);
    CHECK_EQ(panel.tail.len, (size_t)0);

    oi_cli_tool_panel_free(&panel);
}

TEST(result_identity_matches_only_the_active_tool_call) {
    struct oi_cli_tool_panel panel;

    oi_cli_tool_panel_init(&panel);
    CHECK_EQ(oi_cli_tool_panel_start(&panel, "call-1", 6, "shell", 5),
             OI_OK);
    CHECK(oi_cli_tool_panel_matches(&panel, "call-1", 6));
    CHECK(!oi_cli_tool_panel_matches(&panel, "call-2", 6));
    CHECK(!oi_cli_tool_panel_matches(&panel, "call-1-extra", 12));

    oi_cli_tool_panel_clear(&panel);
    CHECK(!oi_cli_tool_panel_matches(&panel, "call-1", 6));
    oi_cli_tool_panel_free(&panel);
}

int main(void) {
    RUN(start_and_feed_produce_a_header_and_content_lines);
    RUN(finish_updates_the_header_status);
    RUN(malicious_escape_sequences_are_stripped_from_displayed_output);
    RUN(output_is_bounded_and_trims_whole_lines_from_the_front);
    RUN(lines_keeps_only_the_most_recent_within_max_lines);
    RUN(clear_resets_to_inactive);
    RUN(result_identity_matches_only_the_active_tool_call);
    return oi_test_report();
}
