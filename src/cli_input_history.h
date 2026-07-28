#ifndef OI_CLI_INPUT_HISTORY_H
#define OI_CLI_INPUT_HISTORY_H

#include <stddef.h>

#include "oi/status.h"

#define OI_CLI_INPUT_HISTORY_MAX_ENTRIES 256U
#define OI_CLI_INPUT_HISTORY_MAX_BYTES (4U * 1024U * 1024U)

struct oi_cli_input_history_entry {
    char *data;
    size_t len;
};

/*
 * Process-scoped submitted-input history. Navigation returns borrowed text
 * valid until the history is mutated or freed.
 */
struct oi_cli_input_history {
    struct oi_cli_input_history_entry *entries;
    size_t len;
    size_t cap;
    size_t bytes;
    char *draft;
    size_t draft_len;
    size_t navigation_index;
    int navigating;
};

void oi_cli_input_history_init(struct oi_cli_input_history *history);
void oi_cli_input_history_free(struct oi_cli_input_history *history);

oi_status oi_cli_input_history_append(struct oi_cli_input_history *history,
                                      const char *text, size_t text_len);

oi_status oi_cli_input_history_previous(struct oi_cli_input_history *history,
                                        const char *draft, size_t draft_len,
                                        const char **out_text,
                                        size_t *out_len);
oi_status oi_cli_input_history_next(struct oi_cli_input_history *history,
                                    const char **out_text, size_t *out_len);

#endif
