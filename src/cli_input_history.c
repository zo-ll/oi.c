#include "cli_input_history.h"

#include "cli_utf8.h"

#include <stdlib.h>
#include <string.h>

static oi_status validate_text(const char *text, size_t text_len,
                               int allow_empty) {
    if (text == NULL && text_len != 0) {
        return OI_ERR_INVAL;
    }
    if (!allow_empty && text_len == 0) {
        return OI_ERR_INVAL;
    }
    if (text_len > OI_CLI_INPUT_HISTORY_MAX_BYTES) {
        return OI_ERR_INVAL;
    }
    if (text_len != 0 && memchr(text, '\0', text_len) != NULL) {
        return OI_ERR_PARSE;
    }
    return oi_cli_utf8_validate(text, text_len);
}

static char *copy_text(const char *text, size_t text_len) {
    char *copy = malloc(text_len + 1);

    if (copy == NULL) {
        return NULL;
    }
    if (text_len != 0) {
        memcpy(copy, text, text_len);
    }
    copy[text_len] = '\0';
    return copy;
}

static void reset_navigation(struct oi_cli_input_history *history) {
    free(history->draft);
    history->draft = NULL;
    history->draft_len = 0;
    history->navigation_index = history->len;
    history->navigating = 0;
}

static void remove_oldest(struct oi_cli_input_history *history) {
    size_t removed_len = history->entries[0].len;

    free(history->entries[0].data);
    if (history->len > 1) {
        memmove(history->entries, history->entries + 1,
                (history->len - 1) * sizeof history->entries[0]);
    }
    history->len--;
    history->bytes -= removed_len;
}

static oi_status reserve_entries(struct oi_cli_input_history *history) {
    size_t new_cap;
    struct oi_cli_input_history_entry *new_entries;

    if (history->len < history->cap) {
        return OI_OK;
    }
    new_cap = history->cap == 0 ? 16 : history->cap * 2;
    if (new_cap > OI_CLI_INPUT_HISTORY_MAX_ENTRIES) {
        new_cap = OI_CLI_INPUT_HISTORY_MAX_ENTRIES;
    }
    new_entries = realloc(history->entries,
                          new_cap * sizeof history->entries[0]);
    if (new_entries == NULL) {
        return OI_ERR_NOMEM;
    }
    history->entries = new_entries;
    history->cap = new_cap;
    return OI_OK;
}

void oi_cli_input_history_init(struct oi_cli_input_history *history) {
    if (history == NULL) {
        return;
    }
    history->entries = NULL;
    history->len = 0;
    history->cap = 0;
    history->bytes = 0;
    history->draft = NULL;
    history->draft_len = 0;
    history->navigation_index = 0;
    history->navigating = 0;
}

void oi_cli_input_history_free(struct oi_cli_input_history *history) {
    size_t i;

    if (history == NULL) {
        return;
    }
    for (i = 0; i < history->len; i++) {
        free(history->entries[i].data);
    }
    free(history->entries);
    free(history->draft);
    oi_cli_input_history_init(history);
}

oi_status oi_cli_input_history_append(struct oi_cli_input_history *history,
                                      const char *text, size_t text_len) {
    char *copy;
    oi_status status;

    if (history == NULL) {
        return OI_ERR_INVAL;
    }
    status = validate_text(text, text_len, 0);
    if (status != OI_OK) {
        return status;
    }
    if (history->len != 0 &&
        history->entries[history->len - 1].len == text_len &&
        memcmp(history->entries[history->len - 1].data, text, text_len) == 0) {
        reset_navigation(history);
        return OI_OK;
    }

    copy = copy_text(text, text_len);
    if (copy == NULL) {
        return OI_ERR_NOMEM;
    }
    while (history->len != 0 &&
           (history->len >= OI_CLI_INPUT_HISTORY_MAX_ENTRIES ||
            history->bytes > OI_CLI_INPUT_HISTORY_MAX_BYTES - text_len)) {
        remove_oldest(history);
    }
    status = reserve_entries(history);
    if (status != OI_OK) {
        free(copy);
        return status;
    }
    history->entries[history->len].data = copy;
    history->entries[history->len].len = text_len;
    history->len++;
    history->bytes += text_len;
    reset_navigation(history);
    return OI_OK;
}

oi_status oi_cli_input_history_previous(struct oi_cli_input_history *history,
                                        const char *draft, size_t draft_len,
                                        const char **out_text,
                                        size_t *out_len) {
    char *draft_copy;
    oi_status status;

    if (history == NULL || out_text == NULL || out_len == NULL) {
        return OI_ERR_INVAL;
    }
    if (history->len == 0 ||
        (history->navigating && history->navigation_index == 0)) {
        return OI_ERR_NOTFOUND;
    }
    if (!history->navigating) {
        status = validate_text(draft, draft_len, 1);
        if (status != OI_OK) {
            return status;
        }
        draft_copy = copy_text(draft, draft_len);
        if (draft_copy == NULL) {
            return OI_ERR_NOMEM;
        }
        free(history->draft);
        history->draft = draft_copy;
        history->draft_len = draft_len;
        history->navigation_index = history->len;
        history->navigating = 1;
    }

    history->navigation_index--;
    *out_text = history->entries[history->navigation_index].data;
    *out_len = history->entries[history->navigation_index].len;
    return OI_OK;
}

oi_status oi_cli_input_history_next(struct oi_cli_input_history *history,
                                    const char **out_text, size_t *out_len) {
    if (history == NULL || out_text == NULL || out_len == NULL) {
        return OI_ERR_INVAL;
    }
    if (!history->navigating ||
        history->navigation_index == history->len) {
        return OI_ERR_NOTFOUND;
    }

    history->navigation_index++;
    if (history->navigation_index == history->len) {
        *out_text = history->draft == NULL ? "" : history->draft;
        *out_len = history->draft_len;
    } else {
        *out_text = history->entries[history->navigation_index].data;
        *out_len = history->entries[history->navigation_index].len;
    }
    return OI_OK;
}
