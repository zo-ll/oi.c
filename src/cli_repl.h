#ifndef OI_CLI_REPL_H
#define OI_CLI_REPL_H

#include <stdint.h>
#include <stdio.h>

#include "cli_command_dispatch.h"
#include "cli_conversation.h"
#include "cli_message.h"
#include "cli_session_switch.h"
#include "cli_status.h"
#include "cli_tools.h"
#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"
#include "oi/tool.h"

typedef oi_status (*oi_cli_repl_prepare_cb)(void *user_data,
                                            oi_arena **out_arena);

/*
 * Opens a different session and hands over everything the REPL needs to
 * adopt it.
 *
 * Hard precondition: the caller must have destroyed the live conversation
 * before invoking this, because a successful switch destroys the old
 * session's arena -- which the conversation was allocating from. Nothing
 * may still reference that arena.
 *
 * Returns non-OK only for a structural failure. Every refusal a user can
 * provoke arrives as `out_result->outcome`, so a switch that cannot happen
 * never ends the REPL. On any non-OK outcome the implementation has already
 * rolled itself back and the previous session remains open and usable.
 */
typedef oi_status (*oi_cli_repl_switch_session_cb)(
    void *user_data, const char *id, size_t id_len,
    struct oi_cli_session_switch_result *out_result);

/* Permanently deletes an already-trashed session. Only ever called after
 * the REPL has obtained an explicit confirmation. */
typedef oi_status (*oi_cli_repl_delete_session_cb)(void *user_data,
                                                   const char *id,
                                                   size_t id_len,
                                                   char **out_error_detail);

/* Imports a legacy log as a new session, reporting its id. Only ever
 * called after the REPL has obtained an explicit confirmation. */
typedef oi_status (*oi_cli_repl_import_session_cb)(void *user_data,
                                                   const char *path,
                                                   size_t path_len,
                                                   char **out_new_id,
                                                   char **out_error_detail);

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

/*
 * Durable persistence for /compact, implemented by the caller (cli.c owns
 * the replay state's context array, the only place a live message index
 * can be translated into real record ids). `prefix_message_count` is a
 * count into the REPL's own live message list (oi_cli_conversation_messages),
 * never a record id itself -- cli.c must read the checkpoint's source
 * range directly off replay_state->context[0..prefix_message_count-1]'s
 * own record_id fields, never infer it arithmetically, since a live
 * message index and a durable record id are not the same number once any
 * prior checkpoint has already collapsed part of the history. Nullable,
 * matching persist_queued_input/persist_queue_resolved's existing
 * ephemeral-session skip -- an ephemeral session has nothing to persist
 * this into, but /compact's live-side effect still applies regardless.
 */
typedef oi_status (*oi_cli_repl_persist_checkpoint_cb)(
    void *user_data, size_t prefix_message_count, const char *summary,
    size_t summary_len, const char *model, size_t model_len);

/*
 * Reports the durable checkpoint state behind /status. cli.c owns the
 * replay state this reads, and re-reads it on every call rather than
 * caching: a live /compact, and a /session switch, both replace it.
 * Nullable -- a session with no durable storage has no durable checkpoint,
 * and the REPL then reports only what it applied itself this run.
 *
 * `*out_checkpoint` arrives zeroed. Fill in only the durable fields; the
 * REPL owns `applied_this_run` and `context_compacted`, since only it knows
 * what it spliced into a live conversation.
 */
typedef void (*oi_cli_repl_status_checkpoint_cb)(
    void *user_data, struct oi_cli_status_checkpoint *out_checkpoint);

struct oi_cli_repl_config {
    const char *model;
    int max_turns;
    int tool_timeout_ms;
    /*
     * The resolved LLM endpoint and request deadline, for /status only --
     * the REPL never opens a connection itself, and the client it is handed
     * deliberately does not publish what it was configured with. `endpoint`
     * is borrowed and must outlive the run; it carries host/port/path and
     * the TLS flag, and structurally cannot carry a credential.
     */
    struct oi_cli_status_endpoint endpoint;
    int request_timeout_ms;
    /* Where `model` came from at startup. The REPL updates its own copy of
     * this as /model and /session switch change the active model. */
    enum oi_cli_session_model_origin model_origin;
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
    oi_cli_repl_persist_checkpoint_cb persist_checkpoint;
    void *persist_checkpoint_user_data;
    oi_cli_repl_status_checkpoint_cb status_checkpoint;
    void *status_checkpoint_user_data;
    /* Crash-recovery text found by the caller's own replay (an interrupted
     * queue consumption) to seed as the composer's initial draft -- never
     * auto-run, just handed back as an editable line. NULL/0 (the default)
     * means no seeding; the caller is responsible for having already closed
     * out the durable crash-recovery window (QUEUE_RESOLVED) before this
     * runs, since oi_cli_repl_run itself has no replay state to do so. */
    const char *initial_draft;
    size_t initial_draft_len;

    /*
     * /session's read-only and low-risk subcommands, passed straight
     * through to cli_command_dispatch. All-NULL leaves /session reporting
     * that it is unavailable rather than misbehaving.
     */
    struct oi_cli_command_session_ops session_ops;

    /*
     * The three subcommands dispatch cannot serve, because they need the
     * live conversation or the composer's confirmation flow. Each is NULL
     * when unavailable.
     */
    oi_cli_repl_switch_session_cb switch_session;
    void *switch_session_user_data;
    oi_cli_repl_delete_session_cb delete_session;
    void *delete_session_user_data;
    oi_cli_repl_import_session_cb import_session;
    void *import_session_user_data;
};

oi_status oi_cli_repl_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_repl_config *config);

#endif
