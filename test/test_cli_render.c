#include "cli_render.h"
#include "test.h"

#include <string.h>
#include <unistd.h>

static size_t read_available(int fd, char *data, size_t cap) {
    ssize_t len = read(fd, data, cap);
    CHECK(len >= 0);
    return len < 0 ? 0 : (size_t)len;
}

TEST(draws_and_repositions_a_single_line) {
    static const char expected[] = "\r\x1b[J> abc\r\x1b[4C";
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "abc", 3), OI_OK);
    CHECK_EQ(oi_cli_editor_move_left(&editor), OI_OK);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw(&render, &editor), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    CHECK_EQ(render.previous_rows, 1);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(multiline_redraw_clears_the_previous_frame) {
    static const char first[] = "\r\x1b[J> a\r\n> b";
    static const char second[] = "\r\x1b[1A\x1b[J> x";
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "a\nb", 3), OI_OK);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw(&render, &editor), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof first - 1);
    CHECK(memcmp(output, first, sizeof first - 1) == 0);
    CHECK_EQ(render.previous_rows, 2);

    CHECK_EQ(oi_cli_editor_set(&editor, "x", 1), OI_OK);
    CHECK_EQ(oi_cli_render_draw(&render, &editor), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof second - 1);
    CHECK(memcmp(output, second, sizeof second - 1) == 0);
    CHECK_EQ(render.previous_rows, 1);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(ascii_wraps_at_the_exact_column_boundary) {
    /* columns=4: "> " occupies col 0-1, leaving 2 columns of content per
     * row. "abc" fills "a","b" on row 0 (cols 2,3) and wraps "c" to row 1
     * (col 0) purely via the terminal's own line-wrap -- no explicit
     * escape is emitted for a natural wrap, only the internal cursor
     * bookkeeping changes. */
    static const char expected[] = "\r\x1b[J> abc";
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "abc", 3), OI_OK);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 4), OI_OK);
    CHECK_EQ(oi_cli_render_draw(&render, &editor), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    CHECK_EQ(render.previous_rows, 2);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(a_wide_code_point_wraps_whole_rather_than_splitting) {
    /* columns=5: "> " leaves 3 content columns on row 0. "a","b" use 2 of
     * them; the wide (2-column) CJK glyph U+4E2D only has 1 column left,
     * so it must wrap to row 1 whole rather than splitting its cells
     * across the boundary. */
    static const char expected[] = "\r\x1b[J> ab\xe4\xb8\xad";
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "ab\xe4\xb8\xad", 5), OI_OK);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 5), OI_OK);
    CHECK_EQ(oi_cli_render_draw(&render, &editor), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    /* Cursor ends on row 1 (after the wide glyph wrapped there whole). */
    CHECK_EQ(render.previous_rows, 2);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(a_combining_mark_does_not_advance_the_column) {
    /* U+0301 COMBINING ACUTE ACCENT stacks onto the preceding "a" and must
     * not itself occupy a column. */
    static const char expected[] = "\r\x1b[J> a\xcc\x81";
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "a\xcc\x81", 3), OI_OK);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw(&render, &editor), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    CHECK_EQ(render.previous_rows, 1);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(finish_moves_to_a_fresh_line) {
    struct oi_cli_render render;
    char output[8];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_finish(&render), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, 2);
    CHECK(memcmp(output, "\r\n", 2) == 0);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

TEST(command_menu_is_rendered_below_the_prompt) {
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    size_t matches[8];
    size_t match_count;
    char output[512];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "/s", 2), OI_OK);
    match_count = oi_cli_command_filter("/s", 2, matches, 8);
    CHECK_EQ(match_count, 2);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw_commands(&render, &editor, matches,
                                         match_count, 1),
             OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output - 1);
    output[len] = '\0';
    CHECK(strstr(output, "\r\n  /session") != NULL);
    CHECK(strstr(output, "\r\n> /status") != NULL);
    /* The physical cursor is repositioned back up to the prompt row (row
     * 0 here) before this call returns, not left on the menu's last row --
     * previous_rows must track where the cursor actually ends up, not the
     * frame's total height, or the next draw's clear step overshoots. */
    CHECK_EQ(render.previous_rows, 1);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(second_draw_after_menu_does_not_overshoot_the_clear) {
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    size_t matches[8];
    size_t match_count;
    char output[512];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "/s", 2), OI_OK);
    match_count = oi_cli_command_filter("/s", 2, matches, 8);
    CHECK_EQ(match_count, 2);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw_commands(&render, &editor, matches,
                                         match_count, 1),
             OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK(len > 0);
    CHECK_EQ(render.previous_rows, 1);

    CHECK_EQ(oi_cli_editor_set(&editor, "/status", 7), OI_OK);
    match_count = oi_cli_command_filter("/status", 7, matches, 8);
    CHECK_EQ(match_count, 1);
    CHECK_EQ(oi_cli_render_draw_commands(&render, &editor, matches,
                                         match_count, 0),
             OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    /* previous_rows was 1 (not > 1) after the first draw, so this clear
     * prefix must be exactly "\r" + "\x1b[J" -- no cursor-up escape at
     * all. Before the previous_rows fix, the first draw would have
     * (wrongly) stored 3, and this clear would have emitted
     * "\r\x1b[2A\x1b[J", moving two rows above this frame's actual top
     * and erasing whatever preceded it on a real terminal. */
    CHECK(len >= 4);
    CHECK(memcmp(output, "\r\x1b[J", 4) == 0);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_render render;
    struct oi_cli_editor editor;

    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_render_init(NULL, 1, 80), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_init(&render, -1, 80), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_init(&render, 1, 2), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_init(&render, 1, 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw(NULL, &editor), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_draw(&render, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_finish(NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_erase(NULL), OI_ERR_INVAL);
    oi_cli_editor_free(&editor);
}

TEST(erase_is_a_no_op_before_anything_is_drawn) {
    struct oi_cli_render render;
    char output[8];
    int pipe_fds[2];

    CHECK_EQ(pipe(pipe_fds), 0);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_erase(&render), OI_OK);
    CHECK_EQ(render.previous_rows, 0);
    /* Confirm nothing was written: closing the write end and reading
     * should immediately see EOF (0), not any bytes. */
    close(pipe_fds[1]);
    CHECK_EQ(read_available(pipe_fds[0], output, sizeof output), 0);

    close(pipe_fds[0]);
}

TEST(erase_clears_a_multiline_frame_and_resets_previous_rows) {
    static const char expected[] = "\r\x1b[1A\x1b[J";
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "a\nb", 3), OI_OK);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw(&render, &editor), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK(len > 0);
    CHECK_EQ(render.previous_rows, 2);

    CHECK_EQ(oi_cli_render_erase(&render), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    CHECK_EQ(render.previous_rows, 0);

    /* Erasing again immediately (nothing redrawn in between) is a no-op --
     * previous_rows is already 0, matching the "already erased" case a
     * caller hits if a turn produces no output between two reactor steps. */
    CHECK_EQ(oi_cli_render_erase(&render), OI_OK);
    close(pipe_fds[1]);
    CHECK_EQ(read_available(pipe_fds[0], output, sizeof output), 0);

    close(pipe_fds[0]);
    oi_cli_editor_free(&editor);
}

TEST(panel_header_lines_are_drawn_above_the_prompt_and_counted) {
    static const char expected[] = "\r\x1b[JHDR\r\n> abc\r\x1b[4C";
    struct oi_cli_render_line header[] = {{"HDR", 3}};
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "abc", 3), OI_OK);
    CHECK_EQ(oi_cli_editor_move_left(&editor), OI_OK);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw_panel(&render, &editor, header, 1, NULL, 0, 0),
             OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    /* One header row plus one prompt row. */
    CHECK_EQ(render.previous_rows, 2);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(panel_erase_clears_the_header_plus_prompt_row_count) {
    static const char expected[] = "\r\x1b[1A\x1b[J";
    struct oi_cli_render_line header[] = {{"HDR", 3}};
    struct oi_cli_editor editor;
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "abc", 3), OI_OK);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw_panel(&render, &editor, header, 1, NULL, 0, 0),
             OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK(len > 0);
    CHECK_EQ(render.previous_rows, 2);

    CHECK_EQ(oi_cli_render_erase(&render), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    CHECK_EQ(render.previous_rows, 0);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
    oi_cli_editor_free(&editor);
}

TEST(selector_draws_options_with_markers_and_no_editor_content) {
    static const char expected[] = "\r\x1b[J> Allow once\r\n  Deny";
    struct oi_cli_render_line options[] = {{"Allow once", 10}, {"Deny", 4}};
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(
        oi_cli_render_draw_selector(&render, NULL, 0, options, 2, 0), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    CHECK_EQ(render.previous_rows, 2);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

TEST(selector_with_header_marks_the_selected_option) {
    static const char expected[] =
        "\r\x1b[JTool: shell\r\n  Allow once\r\n> Deny";
    struct oi_cli_render_line header[] = {{"Tool: shell", 11}};
    struct oi_cli_render_line options[] = {{"Allow once", 10}, {"Deny", 4}};
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(oi_cli_render_draw_selector(&render, header, 1, options, 2, 1),
             OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK_EQ(len, sizeof expected - 1);
    CHECK(memcmp(output, expected, sizeof expected - 1) == 0);
    /* One header row plus two option rows. */
    CHECK_EQ(render.previous_rows, 3);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

TEST(selector_second_draw_does_not_overshoot_the_clear) {
    static const char second_prefix[] = "\r\x1b[1A\x1b[J";
    struct oi_cli_render_line options[] = {{"Allow once", 10}, {"Deny", 4}};
    struct oi_cli_render_line fewer_options[] = {{"Deny", 4}};
    struct oi_cli_render render;
    char output[128];
    int pipe_fds[2];
    size_t len;

    CHECK_EQ(pipe(pipe_fds), 0);
    CHECK_EQ(oi_cli_render_init(&render, pipe_fds[1], 80), OI_OK);
    CHECK_EQ(
        oi_cli_render_draw_selector(&render, NULL, 0, options, 2, 0), OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK(len > 0);
    CHECK_EQ(render.previous_rows, 2);

    CHECK_EQ(oi_cli_render_draw_selector(&render, NULL, 0, fewer_options, 1, 0),
             OI_OK);
    len = read_available(pipe_fds[0], output, sizeof output);
    CHECK(len >= sizeof second_prefix - 1);
    CHECK(memcmp(output, second_prefix, sizeof second_prefix - 1) == 0);
    CHECK_EQ(render.previous_rows, 1);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

TEST(panel_and_selector_bad_arguments_are_rejected) {
    struct oi_cli_render_line header[] = {{"HDR", 3}};
    struct oi_cli_render_line options[] = {{"Deny", 4}};
    struct oi_cli_editor editor;
    struct oi_cli_render render;

    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_render_init(&render, 1, 80), OI_OK);
    CHECK_EQ(
        oi_cli_render_draw_panel(NULL, &editor, header, 1, NULL, 0, 0),
        OI_ERR_INVAL);
    CHECK_EQ(
        oi_cli_render_draw_panel(&render, NULL, header, 1, NULL, 0, 0),
        OI_ERR_INVAL);
    CHECK_EQ(
        oi_cli_render_draw_panel(&render, &editor, NULL, 1, NULL, 0, 0),
        OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_draw_selector(NULL, NULL, 0, options, 1, 0),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_draw_selector(&render, NULL, 1, options, 1, 0),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_draw_selector(&render, NULL, 0, NULL, 1, 0),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_draw_selector(&render, NULL, 0, options, 0, 0),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_draw_selector(&render, NULL, 0, options, 1, 1),
             OI_ERR_INVAL);
    oi_cli_editor_free(&editor);
}

int main(void) {
    RUN(draws_and_repositions_a_single_line);
    RUN(multiline_redraw_clears_the_previous_frame);
    RUN(ascii_wraps_at_the_exact_column_boundary);
    RUN(a_wide_code_point_wraps_whole_rather_than_splitting);
    RUN(a_combining_mark_does_not_advance_the_column);
    RUN(finish_moves_to_a_fresh_line);
    RUN(command_menu_is_rendered_below_the_prompt);
    RUN(second_draw_after_menu_does_not_overshoot_the_clear);
    RUN(bad_arguments_are_rejected);
    RUN(erase_is_a_no_op_before_anything_is_drawn);
    RUN(erase_clears_a_multiline_frame_and_resets_previous_rows);
    RUN(panel_header_lines_are_drawn_above_the_prompt_and_counted);
    RUN(panel_erase_clears_the_header_plus_prompt_row_count);
    RUN(selector_draws_options_with_markers_and_no_editor_content);
    RUN(selector_with_header_marks_the_selected_option);
    RUN(selector_second_draw_does_not_overshoot_the_clear);
    RUN(panel_and_selector_bad_arguments_are_rejected);
    return oi_test_report();
}
