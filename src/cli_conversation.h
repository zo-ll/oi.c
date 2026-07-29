#ifndef OI_CLI_CONVERSATION_H
#define OI_CLI_CONVERSATION_H

#include <stddef.h>

#include "cli_message.h"
#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"
#include "oi/tool.h"

typedef struct oi_cli_conversation oi_cli_conversation;

enum oi_cli_conversation_tool_outcome {
    OI_CLI_CONVERSATION_TOOL_NONE = 0,
    OI_CLI_CONVERSATION_TOOL_COMPLETED,
    OI_CLI_CONVERSATION_TOOL_OUTCOME_UNKNOWN,
    OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED
};

enum oi_cli_conversation_event_type {
    OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA,
    OI_CLI_CONVERSATION_EVENT_MESSAGE,
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
            /*
             * True for a placeholder committed by a live cancel repair
             * (cancelled_tool_unknown_text/cancelled_tool_not_executed_text/
             * interrupted_turn_text), not a real model or tool result --
             * mirrors OI_CLI_HISTORY_MESSAGE_REPAIR, which durable replay
             * requires on the assistant message that closes out a turn
             * after a PARTIAL_ASSISTANT record was already written for it.
             */
            int is_repair;
        } message;
        struct {
            const struct oi_cli_string *id;
            const struct oi_cli_string *name;
            const struct oi_cli_string *arguments;
        } tool_starting;
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

/*
 * Called synchronously at conversation boundaries. Returning non-OK aborts
 * the active turn. TOOL_STARTING is emitted before process spawn, MESSAGE
 * before the message enters model-visible context.
 */
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
};

oi_status oi_cli_conversation_create(
    oi_llm_client *client, oi_reactor *reactor, oi_arena *arena,
    oi_tool_registry *tools,
    const struct oi_cli_conversation_config *config,
    const struct oi_cli_message_list *initial_context,
    oi_cli_conversation **out_conversation);
void oi_cli_conversation_destroy(oi_cli_conversation *conversation);

/*
 * Replaces the conversation's own model copy (validated like
 * oi_cli_conversation_create: non-NULL, non-empty). Affects only the
 * next request this conversation starts; does not touch in-flight state.
 */
oi_status oi_cli_conversation_set_model(oi_cli_conversation *conversation,
                                        const char *model, size_t model_len);

/* Starts one user turn and returns without stepping the reactor. */
oi_status oi_cli_conversation_start(oi_cli_conversation *conversation,
                                    const char *content,
                                    size_t content_len);
void oi_cli_conversation_cancel(oi_cli_conversation *conversation);

int oi_cli_conversation_is_busy(const oi_cli_conversation *conversation);
/*
 * True only when the most recent finished turn ended via
 * oi_cli_conversation_cancel with its repair (if any) fully succeeding --
 * i.e. an ordinary, always-recoverable user cancel, distinct from any other
 * cause that might produce the same oi_status (e.g. OI_ERR_CLOSED can also
 * mean a genuinely-closed LLM connection). Reset at the start of the next
 * oi_cli_conversation_start.
 */
int oi_cli_conversation_was_cancelled(const oi_cli_conversation *conversation);
oi_status oi_cli_conversation_last_status(
    const oi_cli_conversation *conversation);
const struct oi_cli_message_list *oi_cli_conversation_messages(
    const oi_cli_conversation *conversation);

#endif
