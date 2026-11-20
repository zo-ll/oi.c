#ifndef OI_CLI_SESSION_SWITCH_H
#define OI_CLI_SESSION_SWITCH_H

#include <stdio.h>

#include "cli_history_replay.h"
#include "cli_history_store.h"
#include "cli_message.h"
#include "cli_sessions.h"
#include "oi/session.h"
#include "oi/status.h"

enum oi_cli_session_switch_outcome {
    OI_CLI_SESSION_SWITCH_OK = 0,
    OI_CLI_SESSION_SWITCH_SAME,
    OI_CLI_SESSION_SWITCH_NOT_FOUND,
    OI_CLI_SESSION_SWITCH_BUSY,
    OI_CLI_SESSION_SWITCH_INVALID,
    OI_CLI_SESSION_SWITCH_CORRUPT
};

struct oi_cli_session_switch_result {
    enum oi_cli_session_switch_outcome outcome;
    oi_session *session;
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message_list initial_context;
    struct oi_cli_string model;
    struct oi_cli_string cwd;
    enum oi_cli_session_model_origin model_origin;
    char *path;
    char *metadata_path;
};

void oi_cli_session_switch_result_init(
    struct oi_cli_session_switch_result *result);
void oi_cli_session_switch_result_free(
    struct oi_cli_session_switch_result *result);

oi_status oi_cli_session_switch(
    oi_session_registry *registry, const char *root_override,
    const char *current_session_id, const char *target_id,
    size_t target_id_len, const char *default_model, const char *default_cwd,
    FILE *diagnostics, struct oi_cli_session_switch_result *out_result);

#endif
