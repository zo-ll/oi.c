#include "cli_history_repair.h"

#include <stdint.h>
#include <string.h>

static const char unknown_tool_text[] =
    "[tool outcome unknown: the session ended after execution may have "
    "started]";
static const char unstarted_tool_text[] =
    "[tool not executed: the session ended before execution started]";
static const char interrupted_turn_text[] =
    "[assistant turn interrupted before completion]";

static oi_status append_repair_tool(
    struct oi_cli_history *repairs, uint64_t record_id, uint64_t turn_id,
    const struct oi_cli_history_unresolved_tool *tool) {
    const char *content =
        tool->may_have_started ? unknown_tool_text : unstarted_tool_text;
    enum oi_cli_history_tool_outcome outcome =
        tool->may_have_started ? OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN
                               : OI_CLI_HISTORY_TOOL_NOT_EXECUTED;

    struct oi_cli_message message;
    struct oi_cli_history_record record;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&record);
    oi_status st = oi_cli_message_set_tool(
        &message, tool->tool_call_id.data, tool->tool_call_id.len, content,
        strlen(content));
    if (st == OI_OK) {
        st = oi_cli_history_record_set_message(
            &record, record_id, turn_id, &message,
            OI_CLI_HISTORY_MESSAGE_REPAIR, NULL, 0, outcome, NULL, 0, 0);
    }
    if (st == OI_OK) {
        st = oi_cli_history_append_take(repairs, &record);
    }
    oi_cli_history_record_free(&record);
    oi_cli_message_free(&message);
    return st;
}

static oi_status append_interruption_marker(struct oi_cli_history *repairs,
                                            uint64_t record_id,
                                            uint64_t turn_id) {
    struct oi_cli_message message;
    struct oi_cli_history_record record;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&record);
    oi_status st = oi_cli_message_set_assistant(
        &message, interrupted_turn_text, strlen(interrupted_turn_text));
    if (st == OI_OK) {
        st = oi_cli_history_record_set_message(
            &record, record_id, turn_id, &message,
            OI_CLI_HISTORY_MESSAGE_REPAIR, NULL, 0,
            OI_CLI_HISTORY_TOOL_OUTCOME_NONE, NULL, 0, 0);
    }
    if (st == OI_OK) {
        st = oi_cli_history_append_take(repairs, &record);
    }
    oi_cli_history_record_free(&record);
    oi_cli_message_free(&message);
    return st;
}

oi_status oi_cli_history_build_repairs(
    const struct oi_cli_history_replay_state *state,
    struct oi_cli_history *out_repairs) {
    if (state == NULL || out_repairs == NULL || !state->needs_repair ||
        state->repair_turn_id == 0 || state->next_record_id == 0 ||
        state->unresolved_tools_len == SIZE_MAX) {
        return OI_ERR_INVAL;
    }
    if (state->unresolved_tools_len > UINT64_MAX - state->next_record_id) {
        return OI_ERR_NOMEM;
    }

    struct oi_cli_history repairs;
    oi_cli_history_init(&repairs);
    uint64_t record_id = state->next_record_id;
    oi_status st = OI_OK;
    for (size_t i = 0; st == OI_OK && i < state->unresolved_tools_len; i++) {
        st = append_repair_tool(&repairs, record_id++, state->repair_turn_id,
                                &state->unresolved_tools[i]);
    }
    if (st == OI_OK) {
        st = append_interruption_marker(&repairs, record_id,
                                        state->repair_turn_id);
    }
    if (st == OI_OK) {
        oi_cli_history_free(out_repairs);
        *out_repairs = repairs;
        oi_cli_history_init(&repairs);
    }
    oi_cli_history_free(&repairs);
    return st;
}
