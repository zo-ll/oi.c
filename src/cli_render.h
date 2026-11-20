#ifndef OI_CLI_RENDER_H
#define OI_CLI_RENDER_H

#include <stddef.h>

#include "cli_editor.h"
#include "cli_commands.h"
#include "oi/status.h"

struct oi_cli_render {
    int output_fd;
    size_t columns;
    size_t previous_rows;
};

struct oi_cli_render_line {
    const char *text;
    size_t len;
};

oi_status oi_cli_render_init(struct oi_cli_render *render, int output_fd,
                             size_t columns);
void oi_cli_render_set_columns(struct oi_cli_render *render, size_t columns);
oi_status oi_cli_render_draw(struct oi_cli_render *render,
                             const struct oi_cli_editor *editor);
oi_status oi_cli_render_draw_commands(
    struct oi_cli_render *render, const struct oi_cli_editor *editor,
    const size_t *command_indices, size_t command_count,
    size_t selected_command);
oi_status oi_cli_render_draw_panel(
    struct oi_cli_render *render, const struct oi_cli_editor *editor,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const size_t *command_indices, size_t command_count,
    size_t selected_command);
oi_status oi_cli_render_draw_selector(
    struct oi_cli_render *render,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t selected_option);
oi_status oi_cli_render_erase(struct oi_cli_render *render);
oi_status oi_cli_render_finish(struct oi_cli_render *render);

#endif
