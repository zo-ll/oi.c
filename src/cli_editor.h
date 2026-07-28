#ifndef OI_CLI_EDITOR_H
#define OI_CLI_EDITOR_H

#include <stddef.h>

#include "oi/status.h"

#define OI_CLI_EDITOR_MAX_BYTES (1024U * 1024U)

/*
 * Pure editable-text state. Cursor positions are byte offsets at UTF-8 code
 * point boundaries. Terminal input decoding and rendering live elsewhere.
 */
struct oi_cli_editor {
    char *data;
    size_t len;
    size_t cap;
    size_t cursor;
};

void oi_cli_editor_init(struct oi_cli_editor *editor);
void oi_cli_editor_free(struct oi_cli_editor *editor);
void oi_cli_editor_clear(struct oi_cli_editor *editor);

oi_status oi_cli_editor_set(struct oi_cli_editor *editor, const char *text,
                            size_t text_len);
oi_status oi_cli_editor_insert(struct oi_cli_editor *editor, const char *text,
                               size_t text_len);

oi_status oi_cli_editor_move_left(struct oi_cli_editor *editor);
oi_status oi_cli_editor_move_right(struct oi_cli_editor *editor);
void oi_cli_editor_move_line_start(struct oi_cli_editor *editor);
void oi_cli_editor_move_line_end(struct oi_cli_editor *editor);

oi_status oi_cli_editor_backspace(struct oi_cli_editor *editor);
oi_status oi_cli_editor_delete(struct oi_cli_editor *editor);

const char *oi_cli_editor_data(const struct oi_cli_editor *editor);
size_t oi_cli_editor_length(const struct oi_cli_editor *editor);
size_t oi_cli_editor_cursor(const struct oi_cli_editor *editor);

#endif
