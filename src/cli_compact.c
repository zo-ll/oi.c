#include "cli_compact.h"

#include <stddef.h>
#include <stdint.h>

static int is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

oi_status oi_cli_compact_parse_turns(const char *arguments, size_t len,
                                     size_t *out_turns, int *out_has_value) {
    if (out_turns == NULL || out_has_value == NULL) {
        return OI_ERR_INVAL;
    }
    if (len == 0) {
        *out_has_value = 0;
        return OI_OK;
    }
    if (arguments == NULL) {
        return OI_ERR_INVAL;
    }

    size_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (!is_ascii_digit(arguments[i])) {
            return OI_ERR_PARSE;
        }
        size_t digit = (size_t)(arguments[i] - '0');
        if (value > (SIZE_MAX - digit) / 10) {
            return OI_ERR_PARSE;
        }
        value = value * 10 + digit;
    }

    *out_turns = value;
    *out_has_value = 1;
    return OI_OK;
}

oi_status oi_cli_compact_select_prefix(
    const struct oi_cli_message_list *messages, size_t keep_turns,
    size_t *out_prefix_count, size_t *out_total_turns) {
    if (messages == NULL || out_prefix_count == NULL ||
        out_total_turns == NULL) {
        return OI_ERR_INVAL;
    }

    size_t total_turns = 0;
    for (size_t i = 0; i < messages->len; i++) {
        if (messages->items[i].role == OI_CLI_MESSAGE_USER) {
            total_turns++;
        }
    }
    *out_total_turns = total_turns;

    if (total_turns <= keep_turns) {
        *out_prefix_count = 0;
        return OI_OK;
    }

    size_t compact_turns = total_turns - keep_turns;
    if (keep_turns == 0) {
        *out_prefix_count = messages->len;
        return OI_OK;
    }

    size_t seen = 0;
    for (size_t i = 0; i < messages->len; i++) {
        if (messages->items[i].role == OI_CLI_MESSAGE_USER) {
            if (seen == compact_turns) {
                *out_prefix_count = i;
                return OI_OK;
            }
            seen++;
        }
    }
    /* Unreachable: compact_turns < total_turns guarantees a USER message at
     * this position exists. */
    *out_prefix_count = messages->len;
    return OI_OK;
}
