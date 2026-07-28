#include "cli_editor.h"

#include "cli_utf8.h"

#include <stdlib.h>
#include <string.h>

static oi_status editor_reserve(struct oi_cli_editor *editor,
                                size_t required_len) {
    size_t required_cap;
    size_t new_cap;
    char *new_data;

    if (required_len > OI_CLI_EDITOR_MAX_BYTES) {
        return OI_ERR_INVAL;
    }
    required_cap = required_len + 1;
    if (editor->cap >= required_cap) {
        return OI_OK;
    }

    new_cap = editor->cap == 0 ? 64 : editor->cap;
    while (new_cap < required_cap) {
        if (new_cap > (OI_CLI_EDITOR_MAX_BYTES + 1U) / 2U) {
            new_cap = OI_CLI_EDITOR_MAX_BYTES + 1U;
            break;
        }
        new_cap *= 2;
    }

    new_data = realloc(editor->data, new_cap);
    if (new_data == NULL) {
        return OI_ERR_NOMEM;
    }
    if (editor->data == NULL) {
        new_data[0] = '\0';
    }
    editor->data = new_data;
    editor->cap = new_cap;
    return OI_OK;
}

static oi_status validate_editor_text(const char *text, size_t text_len) {
    if (text == NULL && text_len != 0) {
        return OI_ERR_INVAL;
    }
    if (text_len > OI_CLI_EDITOR_MAX_BYTES) {
        return OI_ERR_INVAL;
    }
    if (text_len != 0 && memchr(text, '\0', text_len) != NULL) {
        return OI_ERR_PARSE;
    }
    return oi_cli_utf8_validate(text, text_len);
}

void oi_cli_editor_init(struct oi_cli_editor *editor) {
    if (editor == NULL) {
        return;
    }
    editor->data = NULL;
    editor->len = 0;
    editor->cap = 0;
    editor->cursor = 0;
}

void oi_cli_editor_free(struct oi_cli_editor *editor) {
    if (editor == NULL) {
        return;
    }
    free(editor->data);
    oi_cli_editor_init(editor);
}

void oi_cli_editor_clear(struct oi_cli_editor *editor) {
    if (editor == NULL) {
        return;
    }
    editor->len = 0;
    editor->cursor = 0;
    if (editor->data != NULL) {
        editor->data[0] = '\0';
    }
}

oi_status oi_cli_editor_set(struct oi_cli_editor *editor, const char *text,
                            size_t text_len) {
    oi_status status;

    if (editor == NULL) {
        return OI_ERR_INVAL;
    }
    status = validate_editor_text(text, text_len);
    if (status != OI_OK) {
        return status;
    }
    status = editor_reserve(editor, text_len);
    if (status != OI_OK) {
        return status;
    }
    if (text_len != 0) {
        memcpy(editor->data, text, text_len);
    }
    editor->data[text_len] = '\0';
    editor->len = text_len;
    editor->cursor = text_len;
    return OI_OK;
}

oi_status oi_cli_editor_insert(struct oi_cli_editor *editor, const char *text,
                               size_t text_len) {
    oi_status status;

    if (editor == NULL || editor->cursor > editor->len) {
        return OI_ERR_INVAL;
    }
    status = validate_editor_text(text, text_len);
    if (status != OI_OK) {
        return status;
    }
    if (text_len > OI_CLI_EDITOR_MAX_BYTES - editor->len) {
        return OI_ERR_INVAL;
    }
    if (text_len == 0) {
        return OI_OK;
    }

    status = editor_reserve(editor, editor->len + text_len);
    if (status != OI_OK) {
        return status;
    }
    memmove(editor->data + editor->cursor + text_len,
            editor->data + editor->cursor,
            editor->len - editor->cursor + 1);
    memcpy(editor->data + editor->cursor, text, text_len);
    editor->len += text_len;
    editor->cursor += text_len;
    return OI_OK;
}

oi_status oi_cli_editor_move_left(struct oi_cli_editor *editor) {
    size_t previous;
    oi_status status;

    if (editor == NULL || editor->cursor > editor->len) {
        return OI_ERR_INVAL;
    }
    if (editor->cursor == 0) {
        return OI_OK;
    }
    status = oi_cli_utf8_previous_boundary(editor->data, editor->len,
                                           editor->cursor, &previous);
    if (status == OI_OK) {
        editor->cursor = previous;
    }
    return status;
}

oi_status oi_cli_editor_move_right(struct oi_cli_editor *editor) {
    size_t next;
    oi_status status;

    if (editor == NULL || editor->cursor > editor->len) {
        return OI_ERR_INVAL;
    }
    if (editor->cursor == editor->len) {
        return OI_OK;
    }
    status = oi_cli_utf8_next_boundary(editor->data, editor->len,
                                       editor->cursor, &next);
    if (status == OI_OK) {
        editor->cursor = next;
    }
    return status;
}

void oi_cli_editor_move_line_start(struct oi_cli_editor *editor) {
    if (editor == NULL || editor->cursor > editor->len) {
        return;
    }
    while (editor->cursor > 0 && editor->data[editor->cursor - 1] != '\n') {
        editor->cursor--;
    }
}

void oi_cli_editor_move_line_end(struct oi_cli_editor *editor) {
    if (editor == NULL || editor->cursor > editor->len) {
        return;
    }
    while (editor->cursor < editor->len &&
           editor->data[editor->cursor] != '\n') {
        editor->cursor++;
    }
}

oi_status oi_cli_editor_backspace(struct oi_cli_editor *editor) {
    size_t previous;
    oi_status status;

    if (editor == NULL || editor->cursor > editor->len) {
        return OI_ERR_INVAL;
    }
    if (editor->cursor == 0) {
        return OI_OK;
    }
    status = oi_cli_utf8_previous_boundary(editor->data, editor->len,
                                           editor->cursor, &previous);
    if (status != OI_OK) {
        return status;
    }
    memmove(editor->data + previous, editor->data + editor->cursor,
            editor->len - editor->cursor + 1);
    editor->len -= editor->cursor - previous;
    editor->cursor = previous;
    return OI_OK;
}

oi_status oi_cli_editor_delete(struct oi_cli_editor *editor) {
    size_t next;
    oi_status status;

    if (editor == NULL || editor->cursor > editor->len) {
        return OI_ERR_INVAL;
    }
    if (editor->cursor == editor->len) {
        return OI_OK;
    }
    status = oi_cli_utf8_next_boundary(editor->data, editor->len,
                                       editor->cursor, &next);
    if (status != OI_OK) {
        return status;
    }
    memmove(editor->data + editor->cursor, editor->data + next,
            editor->len - next + 1);
    editor->len -= next - editor->cursor;
    return OI_OK;
}

const char *oi_cli_editor_data(const struct oi_cli_editor *editor) {
    if (editor == NULL || editor->data == NULL) {
        return "";
    }
    return editor->data;
}

size_t oi_cli_editor_length(const struct oi_cli_editor *editor) {
    return editor == NULL ? 0 : editor->len;
}

size_t oi_cli_editor_cursor(const struct oi_cli_editor *editor) {
    return editor == NULL ? 0 : editor->cursor;
}
