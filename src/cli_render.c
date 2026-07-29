#include "cli_render.h"

#include "cli_utf8.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char prompt[] = "> ";

struct render_buffer {
    char *data;
    size_t len;
    size_t cap;
};

struct cursor_position {
    size_t row;
    size_t column;
};

static oi_status buffer_append(struct render_buffer *buffer, const char *data,
                               size_t len) {
    size_t required;
    size_t cap;
    char *new_data;

    if (len > SIZE_MAX - buffer->len) {
        return OI_ERR_NOMEM;
    }
    required = buffer->len + len;
    if (required <= buffer->cap) {
        if (len != 0) {
            memcpy(buffer->data + buffer->len, data, len);
        }
        buffer->len = required;
        return OI_OK;
    }

    cap = buffer->cap == 0 ? 128 : buffer->cap;
    while (cap < required) {
        if (cap > SIZE_MAX / 2) {
            return OI_ERR_NOMEM;
        }
        cap *= 2;
    }
    new_data = realloc(buffer->data, cap);
    if (new_data == NULL) {
        return OI_ERR_NOMEM;
    }
    buffer->data = new_data;
    buffer->cap = cap;
    if (len != 0) {
        memcpy(buffer->data + buffer->len, data, len);
    }
    buffer->len = required;
    return OI_OK;
}

static oi_status buffer_control_number(struct render_buffer *buffer,
                                       size_t number, char suffix) {
    char sequence[64];
    int len = snprintf(sequence, sizeof sequence, "\x1b[%zu%c", number,
                       suffix);

    if (len < 0 || (size_t)len >= sizeof sequence) {
        return OI_ERR_INVAL;
    }
    return buffer_append(buffer, sequence, (size_t)len);
}

static oi_status write_all(int fd, const char *data, size_t len) {
    size_t written = 0;

    while (written < len) {
        ssize_t result = write(fd, data + written, len - written);
        if (result > 0) {
            written += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return OI_ERR_IO;
        }
    }
    return OI_OK;
}

static void advance_columns(struct cursor_position *position, size_t columns,
                            size_t width) {
    while (width != 0) {
        size_t available = columns - position->column;
        if (available == 0) {
            position->row++;
            position->column = 0;
            continue;
        }
        if (width < available) {
            position->column += width;
            return;
        }
        width -= available;
        position->row++;
        position->column = 0;
    }
}

/*
 * Advances by one code point's display width, treating it as atomic: a
 * wide (width 2) glyph is never split across a row boundary -- if it
 * doesn't fully fit in the columns remaining, wrap to a fresh row first,
 * then place it there. Unlike advance_columns (used for the "> " prompt's
 * byte-run, where splitting plain ASCII across a boundary is harmless),
 * this must never leave half a glyph's cells on one row and half on the
 * next.
 */
static void advance_codepoint(struct cursor_position *position,
                              size_t columns, size_t width) {
    if (width == 0) {
        return;
    }
    if (columns - position->column < width) {
        position->row++;
        position->column = 0;
    }
    position->column += width;
}

static oi_status append_prompt(struct render_buffer *buffer,
                               struct cursor_position *position,
                               size_t columns) {
    oi_status status = buffer_append(buffer, prompt, sizeof prompt - 1);
    if (status == OI_OK) {
        advance_columns(position, columns, sizeof prompt - 1);
    }
    return status;
}

static oi_status build_frame(const struct oi_cli_editor *editor,
                             size_t columns, struct render_buffer *buffer,
                             struct cursor_position *out_cursor,
                             struct cursor_position *out_end) {
    const char *data = oi_cli_editor_data(editor);
    size_t len = oi_cli_editor_length(editor);
    size_t cursor = oi_cli_editor_cursor(editor);
    struct cursor_position position = {0};
    size_t offset = 0;
    int cursor_recorded = 0;
    oi_status status;

    status = append_prompt(buffer, &position, columns);
    if (status != OI_OK) {
        return status;
    }
    while (offset < len) {
        size_t sequence_len;

        if (!cursor_recorded && offset == cursor) {
            *out_cursor = position;
            cursor_recorded = 1;
        }
        status = oi_cli_utf8_sequence_length(
            (const unsigned char *)data + offset, len - offset,
            &sequence_len);
        if (status != OI_OK) {
            return status;
        }
        if (data[offset] == '\n') {
            status = buffer_append(buffer, "\r\n", 2);
            if (status != OI_OK) {
                return status;
            }
            position.row++;
            position.column = 0;
            status = append_prompt(buffer, &position, columns);
        } else {
            uint32_t codepoint = 0;

            status = oi_cli_utf8_decode(data + offset, sequence_len,
                                        &codepoint);
            if (status == OI_OK) {
                status = buffer_append(buffer, data + offset, sequence_len);
            }
            if (status == OI_OK) {
                advance_codepoint(&position, columns,
                                  oi_cli_utf8_codepoint_width(codepoint));
            }
        }
        if (status != OI_OK) {
            return status;
        }
        offset += sequence_len;
    }
    if (!cursor_recorded) {
        *out_cursor = position;
    }
    *out_end = position;
    return OI_OK;
}

oi_status oi_cli_render_init(struct oi_cli_render *render, int output_fd,
                             size_t columns) {
    if (render == NULL || output_fd < 0 || columns < sizeof prompt) {
        return OI_ERR_INVAL;
    }
    render->output_fd = output_fd;
    render->columns = columns;
    render->previous_rows = 0;
    return OI_OK;
}

void oi_cli_render_set_columns(struct oi_cli_render *render, size_t columns) {
    if (render != NULL && columns >= sizeof prompt) {
        render->columns = columns;
    }
}

static oi_status append_header_lines(struct render_buffer *buffer,
                                     const struct oi_cli_render_line *lines,
                                     size_t line_count) {
    size_t i;
    oi_status status = OI_OK;

    for (i = 0; i < line_count && status == OI_OK; i++) {
        status = buffer_append(buffer, lines[i].text, lines[i].len);
        if (status == OI_OK) {
            status = buffer_append(buffer, "\r\n", 2);
        }
    }
    return status;
}

static oi_status render_draw(
    struct oi_cli_render *render, const struct oi_cli_editor *editor,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const size_t *command_indices, size_t command_count,
    size_t selected_command) {
    struct render_buffer buffer = {0};
    struct cursor_position cursor = {0};
    struct cursor_position end = {0};
    oi_status status;

    if (render == NULL || editor == NULL || render->output_fd < 0 ||
        render->columns < sizeof prompt ||
        (header_lines == NULL && header_count != 0) ||
        (command_indices == NULL && command_count != 0) ||
        (command_count != 0 && selected_command >= command_count)) {
        return OI_ERR_INVAL;
    }

    status = buffer_append(&buffer, "\r", 1);
    if (status == OI_OK && render->previous_rows > 1) {
        status =
            buffer_control_number(&buffer, render->previous_rows - 1, 'A');
    }
    if (status == OI_OK) {
        status = buffer_append(&buffer, "\x1b[J", 3);
    }
    if (status == OI_OK) {
        status = append_header_lines(&buffer, header_lines, header_count);
    }
    if (status == OI_OK) {
        status = build_frame(editor, render->columns, &buffer, &cursor, &end);
    }
    if (status == OI_OK) {
        size_t i;
        for (i = 0; i < command_count; i++) {
            const struct oi_cli_command_definition *command =
                oi_cli_command_at(command_indices[i]);
            const char *marker = i == selected_command ? "> " : "  ";

            if (command == NULL) {
                status = OI_ERR_INVAL;
                break;
            }
            status = buffer_append(&buffer, "\r\n", 2);
            if (status == OI_OK) {
                status = buffer_append(&buffer, marker, 2);
            }
            if (status == OI_OK) {
                status = buffer_append(&buffer, command->name,
                                       strlen(command->name));
            }
            if (status == OI_OK) {
                status = buffer_append(&buffer, "  ", 2);
            }
            if (status == OI_OK) {
                status = buffer_append(&buffer, command->description,
                                       strlen(command->description));
            }
            if (status != OI_OK) {
                break;
            }
            end.row++;
            end.column = 0;
        }
    }
    if (status == OI_OK && end.row > cursor.row) {
        status = buffer_append(&buffer, "\r", 1);
        if (status == OI_OK) {
            status = buffer_control_number(&buffer, end.row - cursor.row, 'A');
        }
    } else if (status == OI_OK && end.column != cursor.column) {
        status = buffer_append(&buffer, "\r", 1);
    }
    if (status == OI_OK && cursor.column != 0 &&
        (end.row != cursor.row || end.column != cursor.column)) {
        status = buffer_control_number(&buffer, cursor.column, 'C');
    }
    if (status == OI_OK) {
        status = write_all(render->output_fd, buffer.data, buffer.len);
    }
    if (status == OI_OK) {
        /* The repositioning above always leaves the physical cursor at
         * `cursor.row`, not `end.row` (which can be further down whenever
         * a command menu is showing, or the edit cursor sits before
         * wrapped trailing content) -- previous_rows must track where the
         * cursor actually ends up, or the next draw's clear step
         * overshoots past this frame's top row and erases real prior
         * terminal content. header_count is a uniform offset added to
         * every row position build_frame computed relative to the first
         * editor row, so it's added in only here, once. */
        render->previous_rows = header_count + cursor.row + 1;
    }
    free(buffer.data);
    return status;
}

oi_status oi_cli_render_draw(struct oi_cli_render *render,
                             const struct oi_cli_editor *editor) {
    return render_draw(render, editor, NULL, 0, NULL, 0, 0);
}

oi_status oi_cli_render_draw_commands(
    struct oi_cli_render *render, const struct oi_cli_editor *editor,
    const size_t *command_indices, size_t command_count,
    size_t selected_command) {
    return render_draw(render, editor, NULL, 0, command_indices,
                       command_count, selected_command);
}

oi_status oi_cli_render_draw_panel(
    struct oi_cli_render *render, const struct oi_cli_editor *editor,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const size_t *command_indices, size_t command_count,
    size_t selected_command) {
    return render_draw(render, editor, header_lines, header_count,
                       command_indices, command_count, selected_command);
}

oi_status oi_cli_render_draw_selector(
    struct oi_cli_render *render,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t selected_option) {
    struct render_buffer buffer = {0};
    oi_status status;
    size_t row_count;
    size_t i;

    if (render == NULL || render->output_fd < 0 ||
        (header_lines == NULL && header_count != 0) ||
        (option_lines == NULL && option_count != 0) || option_count == 0 ||
        selected_option >= option_count) {
        return OI_ERR_INVAL;
    }

    status = buffer_append(&buffer, "\r", 1);
    if (status == OI_OK && render->previous_rows > 1) {
        status =
            buffer_control_number(&buffer, render->previous_rows - 1, 'A');
    }
    if (status == OI_OK) {
        status = buffer_append(&buffer, "\x1b[J", 3);
    }
    if (status == OI_OK) {
        status = append_header_lines(&buffer, header_lines, header_count);
    }
    row_count = header_count;
    for (i = 0; status == OI_OK && i < option_count; i++) {
        const char *marker = i == selected_option ? "> " : "  ";

        status = buffer_append(&buffer, marker, 2);
        if (status == OI_OK) {
            status = buffer_append(&buffer, option_lines[i].text,
                                   option_lines[i].len);
        }
        /* No trailing "\r\n" after the last option: the physical cursor
         * must end up on the frame's last row (row_count - 1 once this
         * loop finishes), matching what previous_rows below claims, so
         * the next erase's cursor-up count is correct. */
        if (status == OI_OK && i + 1 < option_count) {
            status = buffer_append(&buffer, "\r\n", 2);
        }
        row_count++;
    }
    if (status == OI_OK) {
        status = write_all(render->output_fd, buffer.data, buffer.len);
    }
    if (status == OI_OK) {
        render->previous_rows = row_count;
    }
    free(buffer.data);
    return status;
}

oi_status oi_cli_render_erase(struct oi_cli_render *render) {
    struct render_buffer buffer = {0};
    oi_status status;

    if (render == NULL || render->output_fd < 0) {
        return OI_ERR_INVAL;
    }
    if (render->previous_rows == 0) {
        return OI_OK;
    }

    status = buffer_append(&buffer, "\r", 1);
    if (status == OI_OK && render->previous_rows > 1) {
        status =
            buffer_control_number(&buffer, render->previous_rows - 1, 'A');
    }
    if (status == OI_OK) {
        status = buffer_append(&buffer, "\x1b[J", 3);
    }
    if (status == OI_OK) {
        status = write_all(render->output_fd, buffer.data, buffer.len);
    }
    if (status == OI_OK) {
        render->previous_rows = 0;
    }
    free(buffer.data);
    return status;
}

oi_status oi_cli_render_finish(struct oi_cli_render *render) {
    oi_status status;

    if (render == NULL || render->output_fd < 0) {
        return OI_ERR_INVAL;
    }
    status = write_all(render->output_fd, "\r\n", 2);
    if (status == OI_OK) {
        render->previous_rows = 0;
    }
    return status;
}
