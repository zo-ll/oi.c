#ifndef OI_CLI_RENDER_H
#define OI_CLI_RENDER_H

#include <stddef.h>

#include "cli_editor.h"
#include "cli_commands.h"
#include "oi/status.h"

/*
 * ANSI prompt renderer. It borrows the output descriptor and remembers only
 * the number of rows occupied by its previous frame.
 */
struct oi_cli_render {
    int output_fd;
    size_t columns;
    size_t previous_rows;
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
/*
 * Clears the previously drawn frame (if any) without drawing a new one,
 * leaving the physical cursor at the frame's top row and previous_rows at
 * 0 -- the erase half of oi_cli_render_draw, split out so a caller that
 * needs to write other content to the same fd between "clear the old
 * frame" and "draw a new one" (e.g. a turn's streamed output landing
 * between keystrokes) can do so without the frame and that other content
 * corrupting each other's row-count assumptions. A no-op if no frame is
 * currently drawn (previous_rows == 0).
 */
oi_status oi_cli_render_erase(struct oi_cli_render *render);
oi_status oi_cli_render_finish(struct oi_cli_render *render);

#endif
