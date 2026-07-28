#ifndef OI_CLI_RENDER_H
#define OI_CLI_RENDER_H

#include <stddef.h>

#include "cli_editor.h"
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
oi_status oi_cli_render_finish(struct oi_cli_render *render);

#endif
