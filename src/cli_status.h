#ifndef OI_CLI_STATUS_H
#define OI_CLI_STATUS_H

#include <stdint.h>
#include <stdio.h>

#include "cli_conversation.h"
#include "cli_sessions.h"
#include "cli_tools.h"
#include "oi/status.h"

enum oi_cli_status_session_state {
    OI_CLI_STATUS_SESSION_UNKNOWN = 0,
    OI_CLI_STATUS_SESSION_NOT_CREATED,
    OI_CLI_STATUS_SESSION_EPHEMERAL,
    OI_CLI_STATUS_SESSION_ACTIVE,
    OI_CLI_STATUS_SESSION_FAILED
};

enum oi_cli_status_permission {
    OI_CLI_STATUS_PERMISSION_UNKNOWN = 0,
    OI_CLI_STATUS_PERMISSION_ASK,
    OI_CLI_STATUS_PERMISSION_ALLOW,
    OI_CLI_STATUS_PERMISSION_DENY
};

enum oi_cli_status_permission oi_cli_status_permission_from_policy(
    oi_cli_tool_policy policy);

enum oi_cli_status_queue_state {
    OI_CLI_STATUS_QUEUE_UNKNOWN = 0,
    OI_CLI_STATUS_QUEUE_EMPTY,
    OI_CLI_STATUS_QUEUE_MESSAGE,
    OI_CLI_STATUS_QUEUE_COMMAND
};

struct oi_cli_status_endpoint {
    const char *host;
    const char *path;
    unsigned short port;
    int use_tls;
};

struct oi_cli_status_checkpoint {
    int known;
    int has_durable_checkpoint;
    uint64_t source_first_record_id;
    uint64_t source_last_record_id;
    int applied_this_run;
    int context_compacted;
};

struct oi_cli_status_snapshot {
    enum oi_cli_status_session_state session_state;
    const char *session_id;

    const char *model;
    enum oi_cli_session_model_origin model_origin;

    struct oi_cli_status_endpoint endpoint;
    enum oi_cli_status_permission permission;

    int request_timeout_ms;
    int tool_timeout_ms;

    const char *cwd;

    enum oi_cli_conversation_activity conversation;
    oi_status conversation_status;
    int steering;

    enum oi_cli_status_queue_state queue;
    size_t queue_bytes;

    struct oi_cli_status_checkpoint checkpoint;
};

void oi_cli_status_snapshot_init(struct oi_cli_status_snapshot *snapshot);

oi_status oi_cli_status_write(const struct oi_cli_status_snapshot *snapshot,
                              FILE *out);

#define OI_CLI_STATUS_MAX_FIELD_BYTES 4096u

#endif
