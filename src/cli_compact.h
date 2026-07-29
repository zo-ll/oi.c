#ifndef OI_CLI_COMPACT_H
#define OI_CLI_COMPACT_H

#include <stddef.h>

#include "cli_message.h"
#include "oi/status.h"

/*
 * Parses the argument text of "/compact [turns]" -- `arguments` is exactly
 * what oi_cli_command_parse_text produces (leading/trailing whitespace
 * already trimmed). An empty `arguments` (len == 0) means no value was
 * given -- *out_has_value is set to 0 and the caller applies its own
 * default; *out_turns is untouched. Otherwise
 * `arguments` must be one or more decimal digits and nothing else (no
 * sign, no surrounding or embedded whitespace, no other trailing text);
 * anything else, or a value that overflows size_t, returns OI_ERR_PARSE.
 */
oi_status oi_cli_compact_parse_turns(const char *arguments, size_t len,
                                     size_t *out_turns, int *out_has_value);

/*
 * Scans `messages` for turn boundaries -- each OI_CLI_MESSAGE_USER message
 * starts a new turn; assistant/tool messages continue the current one, and
 * any leading messages before the first USER message (e.g. an existing
 * checkpoint's synthesized summary) belong to no turn and are always
 * eligible to be swept into a new checkpoint's prefix -- to determine how
 * large a leading prefix must be consumed to leave exactly the `keep_turns`
 * most recent turns untouched.
 *
 * *out_total_turns is always set to the number of USER-started turns
 * found. If *out_total_turns <= keep_turns there is nothing to compact:
 * *out_prefix_count is set to 0 (callers must treat a zero prefix as
 * "nothing to do" and must never call oi_cli_conversation_apply_checkpoint
 * with it). Otherwise *out_prefix_count is the number of leading messages
 * (never zero) that must be consumed to leave exactly `keep_turns` turns
 * behind; `keep_turns == 0` consumes the entire list.
 *
 * OI_ERR_INVAL if `messages` or either output pointer is NULL.
 */
oi_status oi_cli_compact_select_prefix(
    const struct oi_cli_message_list *messages, size_t keep_turns,
    size_t *out_prefix_count, size_t *out_total_turns);

#endif
