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
    CHECK_EQ(render.previous_rows, 3);

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
    oi_cli_editor_free(&editor);
}

int main(void) {
    RUN(draws_and_repositions_a_single_line);
    RUN(multiline_redraw_clears_the_previous_frame);
    RUN(finish_moves_to_a_fresh_line);
    RUN(command_menu_is_rendered_below_the_prompt);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
