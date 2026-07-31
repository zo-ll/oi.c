#ifndef OI_CLI_HISTORY_REPLAY_H
#define OI_CLI_HISTORY_REPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "cli_history.h"
#include "cli_message.h"
#include "oi/status.h"

struct oi_cli_history_context_item {
    uint64_t record_id;
    struct oi_cli_message message;
};

struct oi_cli_history_unresolved_tool {
    struct oi_cli_string tool_call_id;
    int may_have_started;
};

struct oi_cli_history_replay_state {
    struct oi_cli_history_context_item *context;
    size_t context_len;
    size_t context_cap;

    struct oi_cli_string pending_input;
    uint64_t pending_input_record_id;
    uint64_t pending_input_turn_id;
    int has_pending_input;

    struct oi_cli_history_unresolved_tool *unresolved_tools;
    size_t unresolved_tools_len;

    uint64_t repair_turn_id;
    uint64_t next_record_id;
    uint64_t next_turn_id;
    int needs_transition;
    int needs_repair;
    int has_partial_assistant;

    /* Last-known durable session settings, reconstructed from
     * session_setting records. Empty (data == NULL) if never set. */
    struct oi_cli_string last_model;
    struct oi_cli_string last_cwd;

    /*
     * The range collapsed by the most recent checkpoint applied while
     * rebuilding `context`, so a reporting caller (/status) can name what
     * replaced the active context's prefix without re-reading the log or
     * reaching into the store's record array. Taken from the checkpoint
     * record itself rather than derived from `context` -- once a second
     * checkpoint has subsumed the first, only the record still states what
     * it covered. Both are 0 until a checkpoint is applied, and
     * `has_checkpoint` being set is exactly what makes context[0] a summary
     * rather than the session's oldest real message.
     */
    uint64_t checkpoint_source_first_record_id;
    uint64_t checkpoint_source_last_record_id;
    int has_checkpoint;
};

void oi_cli_history_replay_state_init(
    struct oi_cli_history_replay_state *state);
void oi_cli_history_replay_state_free(
    struct oi_cli_history_replay_state *state);

/*
 * Validates the complete typed history and rebuilds model-visible context.
 * `legacy_messages`, when non-NULL, contains the old alternating records in
 * their original order. The output is replaced only on success.
 */
oi_status oi_cli_history_replay(
    const struct oi_cli_message_list *legacy_messages,
    const struct oi_cli_history *typed_history,
    struct oi_cli_history_replay_state *out_state);

#endif
