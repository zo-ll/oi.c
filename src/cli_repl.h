#ifndef OI_CLI_REPL_H
#define OI_CLI_REPL_H

#include <stdint.h>
#include <stdio.h>

#include "cli_conversation.h"
#include "cli_message.h"
#include "cli_tools.h"
#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"
#include "oi/tool.h"

typedef oi_status (*oi_cli_repl_prepare_cb)(void *user_data,
                                            oi_arena **out_arena);

/*
 * Durable persistence for a /model or /cwd change, implemented by the
 * caller (cli.c owns filesystem paths and metadata; the REPL controller
 * does not). NULL means nothing durable exists yet to persist to (no
 * session created); the live update still applies regardless.
 */
typedef oi_status (*oi_cli_repl_persist_model_cb)(void *user_data,
                                                  const char *name,
                                                  size_t name_len);
typedef oi_status (*oi_cli_repl_persist_cwd_cb)(void *user_data,
                                                const char *path,
                                                size_t path_len);

/*
 * True only if durable session storage itself has failed (cli.c owns the
 * one authoritative signal for this, persistence.last_error, already
 * checked once at final process exit) -- distinguishes a genuine
 * structural failure the REPL cannot recover from from an ordinary
 * recoverable turn error (timeout, tool denial, model error), which should
 * instead print a message and return to a working prompt. Always non-NULL:
 * an ephemeral session (no --session, no automatic session) never
 * exercises the persistence path at all, so it naturally, permanently
 * reports "not failed".
 */
typedef int (*oi_cli_repl_is_durably_failed_cb)(void *user_data);

/*
 * Durable persistence for the one-slot queued-input mechanism, implemented
 * by the caller (cli.c owns the record/turn_id bookkeeping; the REPL
 * controller only ever holds the opaque record id handed back to it).
 * Both nullable, matching persist_model/persist_cwd's existing ephemeral-
 * session skip. persist_queued_input must be called (and must succeed)
 * before the REPL treats a message as durably queued, per the issue's own
 * "persist before acknowledging" requirement; *out_record_id is the id to
 * later pass back to persist_queue_resolved. `consumed` is true if the
 * queued message is becoming the next turn's real user message, false if
 * it's being discarded (refused, cancelled, or a queued command, which the
 * durable schema can only ever resolve as discarded -- see
 * cli_history_replay.c's queue_consumed gate).
 */
typedef oi_status (*oi_cli_repl_persist_queued_input_cb)(
    void *user_data, const char *content, size_t content_len,
    uint64_t *out_record_id);
typedef oi_status (*oi_cli_repl_persist_queue_resolved_cb)(
    void *user_data, uint64_t queued_record_id, int consumed);

struct oi_cli_repl_config {
    const char *model;
    int max_turns;
    int tool_timeout_ms;
    struct oi_cli_permission *permission;
    int input_fd;
    int output_fd;
    FILE *out;
    FILE *err;
    const struct oi_cli_message_list *initial_context;
    oi_cli_conversation_event_cb on_event;
    void *event_user_data;
    oi_cli_repl_prepare_cb prepare;
    void *prepare_user_data;
    const char *(*session_id)(void *user_data);
    void *session_id_user_data;
    /* Shared with the caller's lazy automatic-session preparation, so a
     * /model issued before the first message is honored when the session
     * is eventually created. NULL when no automatic-session concept
     * applies (an explicit --session ID is already fully resolved before
     * the REPL starts). */
    struct oi_cli_string *pending_model;
    oi_cli_repl_persist_model_cb persist_model;
    void *persist_model_user_data;
    oi_cli_repl_persist_cwd_cb persist_cwd;
    void *persist_cwd_user_data;
    oi_cli_repl_is_durably_failed_cb is_durably_failed;
    void *is_durably_failed_user_data;
    oi_cli_repl_persist_queued_input_cb persist_queued_input;
    void *persist_queued_input_user_data;
    oi_cli_repl_persist_queue_resolved_cb persist_queue_resolved;
    void *persist_queue_resolved_user_data;
};

oi_status oi_cli_repl_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_repl_config *config);

#endif
