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

/* One pre-formatted, single-row line of text -- the caller is responsible
 * for bounding/wrapping/sanitizing it before handing it here (the same
 * simplifying assumption oi_cli_render_draw_commands already makes for its
 * own menu entries). */
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
/*
 * Like oi_cli_render_draw_commands, but with `header_lines` drawn above the
 * prompt/editor content -- e.g. a live tool-execution panel. Row-count
 * bookkeeping: header rows are written before the normal frame (which is
 * unaware of them and computes cursor positions exactly as it does today)
 * and simply added into the final previous_rows: the cursor-repositioning
 * math only ever depends on the *delta* between the edit cursor's row and
 * the frame's last row, which a uniform header offset added to both
 * doesn't change.
 */
oi_status oi_cli_render_draw_panel(
    struct oi_cli_render *render, const struct oi_cli_editor *editor,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const size_t *command_indices, size_t command_count,
    size_t selected_command);
/*
 * A self-contained modal frame -- header_lines followed by selectable
 * option_lines (selected_option marked "> ", others "  ", mirroring
 * oi_cli_render_draw_commands' marker convention) -- with no
 * oi_cli_editor involved at all: nothing being edited is drawn or
 * implied while this is showing. Composes with oi_cli_render_erase
 * exactly like the other draw functions (same previous_rows bookkeeping);
 * a later oi_cli_render_draw/_draw_panel call restores the caller's
 * in-progress draft untouched, since this never reads or writes any
 * editor state.
 */
oi_status oi_cli_render_draw_selector(
    struct oi_cli_render *render,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t selected_option);
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
