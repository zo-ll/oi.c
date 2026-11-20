#ifndef OI_CLI_CONVERSATION_H
#define OI_CLI_CONVERSATION_H

#include <stddef.h>

#include "cli_message.h"
#include "oi/arena.h"
#include "oi/json.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"
#include "oi/tool.h"

typedef struct oi_cli_conversation oi_cli_conversation;

enum oi_cli_conversation_tool_outcome {
    OI_CLI_CONVERSATION_TOOL_NONE = 0,
    OI_CLI_CONVERSATION_TOOL_COMPLETED,
    OI_CLI_CONVERSATION_TOOL_FAILED,
    OI_CLI_CONVERSATION_TOOL_DENIED,
    OI_CLI_CONVERSATION_TOOL_OUTCOME_UNKNOWN,
    OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED
};

enum oi_cli_conversation_activity {
    OI_CLI_CONVERSATION_ACTIVITY_UNKNOWN = 0,
    OI_CLI_CONVERSATION_ACTIVITY_IDLE,
    OI_CLI_CONVERSATION_ACTIVITY_STREAMING,
    OI_CLI_CONVERSATION_ACTIVITY_AWAITING_PERMISSION,
    OI_CLI_CONVERSATION_ACTIVITY_TOOL_RUNNING,
    OI_CLI_CONVERSATION_ACTIVITY_CANCELLING,
    OI_CLI_CONVERSATION_ACTIVITY_WORKING,
    OI_CLI_CONVERSATION_ACTIVITY_FAILED
};

enum oi_cli_conversation_event_type {
    OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA,
    OI_CLI_CONVERSATION_EVENT_MESSAGE,
    OI_CLI_CONVERSATION_EVENT_AWAITING_PERMISSION,
    OI_CLI_CONVERSATION_EVENT_TOOL_STARTING,
    OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT,
    OI_CLI_CONVERSATION_EVENT_PARTIAL_ASSISTANT,
    OI_CLI_CONVERSATION_EVENT_RESPONSE_DONE,
    OI_CLI_CONVERSATION_EVENT_MODEL_ERROR,
    OI_CLI_CONVERSATION_EVENT_TURN_DONE
};

struct oi_cli_conversation_event {
    enum oi_cli_conversation_event_type type;
    union {
        struct {
            const char *data;
            size_t len;
        } bytes;
        struct {
            const struct oi_cli_message *value;
            const char *model;
            size_t model_len;
            enum oi_cli_conversation_tool_outcome tool_outcome;
            const unsigned char *raw_tool_output;
            size_t raw_tool_output_len;
            int has_raw_tool_output;
            int is_repair;
        } message;
        struct {
            const struct oi_cli_string *id;
            const struct oi_cli_string *name;
            const struct oi_cli_string *arguments;
        } tool_starting;
        struct {
            const struct oi_cli_string *id;
            const struct oi_cli_string *name;
            const struct oi_cli_string *arguments;
            const oi_json_value *parsed_arguments;
        } awaiting_permission;
        struct {
            int http_status;
            const char *body;
            size_t body_len;
        } model_error;
        struct {
            oi_status status;
            int http_status;
        } turn_done;
    } as;
};

typedef oi_status (*oi_cli_conversation_event_cb)(
    const struct oi_cli_conversation_event *event, void *user_data);

struct oi_cli_conversation_config {
    const char *model;
    int max_model_steps;
    int tool_timeout_ms;
    oi_tool_permission_cb permission;
    void *permission_user_data;
    oi_cli_conversation_event_cb on_event;
    void *event_user_data;
    size_t tool_output_limit;
    const char *tool_output_dir;
};

oi_status oi_cli_conversation_create(
    oi_llm_client *client, oi_reactor *reactor, oi_arena *arena,
    oi_tool_registry *tools,
    const struct oi_cli_conversation_config *config,
    const struct oi_cli_message_list *initial_context,
    oi_cli_conversation **out_conversation);
void oi_cli_conversation_destroy(oi_cli_conversation *conversation);

oi_status oi_cli_conversation_set_model(oi_cli_conversation *conversation,
                                        const char *model, size_t model_len);

oi_status oi_cli_conversation_start(oi_cli_conversation *conversation,
                                    const char *content,
                                    size_t content_len);
void oi_cli_conversation_cancel(oi_cli_conversation *conversation);

void oi_cli_conversation_steer(oi_cli_conversation *conversation);
int oi_cli_conversation_is_steering(const oi_cli_conversation *conversation);

oi_status oi_cli_conversation_resolve_permission(
    oi_cli_conversation *conversation, int allow);

int oi_cli_conversation_is_busy(const oi_cli_conversation *conversation);

enum oi_cli_conversation_activity oi_cli_conversation_activity(
    const oi_cli_conversation *conversation);

struct oi_cli_usage {
    size_t requests;
    size_t reported_requests;
    size_t cached_reported_requests;
    size_t input_tokens;
    size_t cached_input_tokens;
    size_t output_tokens;
};

const struct oi_cli_usage *oi_cli_conversation_usage(
    const oi_cli_conversation *conversation);
int oi_cli_conversation_was_cancelled(const oi_cli_conversation *conversation);
oi_status oi_cli_conversation_last_status(
    const oi_cli_conversation *conversation);
const struct oi_cli_message_list *oi_cli_conversation_messages(
    const oi_cli_conversation *conversation);

oi_arena *oi_cli_conversation_arena(const oi_cli_conversation *conversation);

oi_status oi_cli_conversation_apply_checkpoint(
    oi_cli_conversation *conversation, size_t prefix_count,
    const char *summary, size_t summary_len);

oi_status oi_cli_conversation_apply_checkpoint_take_summary(
    oi_cli_conversation *conversation, size_t prefix_count, char **summary,
    size_t summary_len);

#endif
