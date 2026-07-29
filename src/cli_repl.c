/* realpath(3) is a BSD/SVID extension, and struct signalfd_siginfo /
 * signalfd(2) are GNU/Linux extensions; _GNU_SOURCE is a superset of
 * _DEFAULT_SOURCE (previously defined here just for realpath) and covers
 * both, matching the same precedent in reactor_epoll.c for other GNU fd
 * primitives (timerfd, pidfd). */
#define _GNU_SOURCE

#include "cli_repl.h"

#include "cli_bytebuf.h"
#include "cli_command_dispatch.h"
#include "cli_commands.h"
#include "cli_compact.h"
#include "cli_composer.h"
#include "cli_input_history.h"
#include "cli_present.h"
#include "cli_render_sanitize.h"
#include "cli_tool_panel.h"

#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

static oi_status seed_input_history(
    struct oi_cli_input_history *history,
    const struct oi_cli_message_list *messages) {
    size_t i;

    if (messages == NULL) {
        return OI_OK;
    }
    for (i = 0; i < messages->len; i++) {
        const struct oi_cli_message *message = &messages->items[i];
        oi_status status;

        if (message->role != OI_CLI_MESSAGE_USER ||
            message->content.len == 0) {
            continue;
        }
        status = oi_cli_input_history_append(
            history, message->content.data, message->content.len);
        if (status != OI_OK) {
            return status;
        }
    }
    return OI_OK;
}

struct repl_setting_context {
    const struct oi_cli_repl_config *config;
    struct oi_cli_string *current_model;
    oi_cli_conversation **conversation;
};

/*
 * The one-slot pending item, owned entirely by cli_repl.c per this
 * issue's own module-boundary rule (cli_conversation only ever gets a
 * bare "steer" signal, no content; cli_history only encodes/replays the
 * durable record shape, it never decides what's queued). Reset to NONE at
 * the top of oi_cli_repl_run's turn loop each time it's actually consumed
 * or discarded (added in a later commit) -- this commit only ever
 * populates it, nothing yet drains it.
 */
enum oi_cli_repl_pending_kind {
    OI_CLI_REPL_PENDING_NONE = 0,
    OI_CLI_REPL_PENDING_MESSAGE,
    OI_CLI_REPL_PENDING_COMMAND
};

struct oi_cli_repl_pending {
    enum oi_cli_repl_pending_kind kind;
    char *text;
    size_t text_len;
    /* Opaque durable record id from persist_queued_input, passed back
     * verbatim to persist_queue_resolved; 0 if persistence is disabled
     * (ephemeral session) or failed benignly under a store==NULL config. */
    uint64_t record_id;
};

static void repl_pending_free(struct oi_cli_repl_pending *pending) {
    free(pending->text);
    pending->text = NULL;
    pending->text_len = 0;
    pending->kind = OI_CLI_REPL_PENDING_NONE;
    pending->record_id = 0;
}

struct repl_turn_signal_context {
    oi_cli_conversation *conversation;
    /* 0, or the signal (SIGTERM/SIGHUP) that requested the whole REPL
     * terminate once this turn finishes unwinding. */
    int terminate_signal;
    /* Set on SIGWINCH; the turn loop checks this after each step to decide
     * whether the composer's frame needs redrawing -- see
     * repl_turn_input_context.dirty for why this can't just redraw
     * unconditionally on every step. */
    int resize_pending;
};

static void handle_turn_signal(oi_reactor *reactor, int fd, int revents,
                               void *user_data) {
    struct repl_turn_signal_context *context = user_data;
    struct signalfd_siginfo info;

    (void)reactor;
    (void)revents;
    /* Each of SIGWINCH/SIGINT/SIGTERM/SIGHUP is a standard (non-realtime)
     * signal, so at most one of each is ever actually pending -- looping
     * is defensive, not load-bearing. */
    while (read(fd, &info, sizeof info) == (ssize_t)sizeof info) {
        switch (info.ssi_signo) {
        case SIGWINCH:
            context->resize_pending = 1;
            break;
        case SIGINT:
            oi_cli_conversation_cancel(context->conversation);
            break;
        case SIGTERM:
        case SIGHUP:
            oi_cli_conversation_cancel(context->conversation);
            context->terminate_signal = (int)info.ssi_signo;
            break;
        default:
            break;
        }
    }
}

static oi_status dispatch_set_model(void *user_data, const char *name,
                                    size_t name_len);
static oi_status dispatch_set_cwd(void *user_data, const char *path,
                                  size_t path_len);

struct repl_turn_input_context;
static void report_busy_io_error(struct repl_turn_input_context *context,
                                 int wrote_ok);

struct repl_turn_input_context {
    struct oi_cli_composer *composer;
    oi_cli_conversation *conversation;
    const struct oi_cli_repl_config *config;
    struct oi_cli_string *current_model;
    struct repl_setting_context *setting_context;
    struct oi_cli_repl_pending *pending;
    /* Owned by this context: cancelled unconditionally when the turn ends
     * (whichever way), since a still-pending timer firing after this
     * stack-local context goes out of scope would be a use-after-free. */
    oi_reactor_timer *escape_timer;
    /* First composer error seen (e.g. OI_ERR_CLOSED if input_fd hit EOF);
     * checked by the turn loop after each step alongside the reactor's own
     * step_status, since this callback has no return value of its own. */
    oi_status status;
    /*
     * Set whenever input was actually decoded this step; the turn loop
     * checks this (and resize_pending above) after each step to decide
     * whether to redraw the composer's frame at all. Redrawing
     * unconditionally on every step -- regardless of whether *this*
     * step's ready fds had anything to do with the composer -- would
     * interleave its escape sequences into the turn's own streamed
     * output (config->out and config->output_fd are the same fd)
     * whenever a multi-chunk reply and typing land in different steps,
     * corrupting normal token-by-token streaming, not just risking
     * flicker. Only redrawing when something composer-relevant actually
     * happened this step avoids that entirely.
     */
    int dirty;
    /*
     * True from the moment an AWAITING_PERMISSION event arrives until the
     * decision is resolved one way or another -- while set,
     * handle_turn_input/handle_turn_escape_timeout route decoded input
     * through the permission selector instead of the composer's normal
     * editor-applying path, and the turn loop's redraw draws the selector
     * instead of the ordinary prompt frame.
     */
    int awaiting_permission;
    /* 0 = allow once, 1 = allow for process, 2 = deny -- reset to 0 each
     * time a new AWAITING_PERMISSION event arrives. */
    size_t permission_selected;
    /* Sanitized/bounded "Tool: <name>"/"Args: <summary>" header lines,
     * rebuilt fresh by build_permission_header for each AWAITING_PERMISSION
     * event -- copied immediately rather than keeping the event's own
     * borrowed pointers, which aren't guaranteed to outlive the call. */
    struct oi_cli_bytebuf permission_tool_line;
    struct oi_cli_bytebuf permission_args_line;
    /*
     * Live tool-execution presentation, driven by repl_conversation_event
     * (TOOL_STARTING/_OUTPUT/a tool-role MESSAGE). Fresh (inactive) for
     * each new turn via this struct's own zero-initialization; stays
     * active through the rest of the turn once a tool call starts, so its
     * last state (e.g. "shell: completed") remains visible until either
     * another call starts in the same turn or the turn ends.
     */
    struct oi_cli_tool_panel panel;
};

/*
 * Bounds the raw argument JSON before sanitizing it, so a pathologically
 * large tool call can't blow up the header display or the sanitize pass'
 * own bounded memory use -- the full arguments are still whatever the
 * model actually sent; only this pre-approval summary is truncated.
 */
#define OI_CLI_REPL_PERMISSION_ARGS_SUMMARY_MAX 200u

/*
 * Builds the "Tool: <name>" / "Args: <summary>" header lines shown above
 * the permission selector, sanitizing both through the same
 * control-byte/escape-sequence stripper write_tool_start already uses for
 * "oi: running tool <name>" -- untrusted model output must never be able
 * to inject terminal escapes into this display. Copied into
 * caller-owned buffers immediately: `name`/`arguments` are event-scoped
 * borrowed pointers, not guaranteed to outlive this call.
 */
static oi_status build_permission_header(struct oi_cli_bytebuf *tool_line,
                                         struct oi_cli_bytebuf *args_line,
                                         const struct oi_cli_string *name,
                                         const struct oi_cli_string *arguments) {
    struct oi_cli_sanitize_state sanitize;
    size_t bounded_args_len = arguments->len > OI_CLI_REPL_PERMISSION_ARGS_SUMMARY_MAX
                                  ? OI_CLI_REPL_PERMISSION_ARGS_SUMMARY_MAX
                                  : arguments->len;
    oi_status status;

    oi_cli_bytebuf_reset(tool_line);
    oi_cli_bytebuf_reset(args_line);
    status = oi_cli_bytebuf_append(tool_line, "Tool: ", 6);
    if (status == OI_OK) {
        oi_cli_sanitize_init(&sanitize);
        status = oi_cli_sanitize_feed(
            &sanitize, (const unsigned char *)name->data, name->len,
            tool_line);
        if (status == OI_OK) {
            status = oi_cli_sanitize_finish(&sanitize);
        }
    }
    if (status == OI_OK) {
        status = oi_cli_bytebuf_append(args_line, "Args: ", 6);
    }
    if (status == OI_OK) {
        oi_cli_sanitize_init(&sanitize);
        status = oi_cli_sanitize_feed(
            &sanitize, (const unsigned char *)arguments->data,
            bounded_args_len, args_line);
        if (status == OI_OK) {
            status = oi_cli_sanitize_finish(&sanitize);
        }
    }
    if (status == OI_OK && arguments->len > bounded_args_len) {
        status = oi_cli_bytebuf_append(args_line, "...", 3);
    }
    return status;
}

struct repl_event_context {
    struct oi_cli_present *present;
    /* Set to the current turn's context at the start of each turn, and
     * only ever dereferenced while a turn is genuinely active (conversation
     * events can't fire otherwise) -- see oi_cli_repl_run. */
    struct repl_turn_input_context *turn_input;
};

/* Maps the conversation's durable-plus-presentation outcome to the live
 * panel's final label. FAILED and DENIED deliberately remain distinct here
 * even though persistence collapses them into the existing completed and
 * not-executed history outcomes respectively. */
static enum oi_cli_tool_panel_status tool_panel_status_for_outcome(
    enum oi_cli_conversation_tool_outcome outcome) {
    switch (outcome) {
    case OI_CLI_CONVERSATION_TOOL_COMPLETED:
        return OI_CLI_TOOL_PANEL_COMPLETED;
    case OI_CLI_CONVERSATION_TOOL_FAILED:
        return OI_CLI_TOOL_PANEL_FAILED;
    case OI_CLI_CONVERSATION_TOOL_DENIED:
        return OI_CLI_TOOL_PANEL_DENIED;
    case OI_CLI_CONVERSATION_TOOL_OUTCOME_UNKNOWN:
        return OI_CLI_TOOL_PANEL_CANCELLED;
    case OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED:
        return OI_CLI_TOOL_PANEL_CANCELLED;
    case OI_CLI_CONVERSATION_TOOL_NONE:
        break;
    }
    return OI_CLI_TOOL_PANEL_COMPLETED;
}

static oi_status repl_conversation_event(
    const struct oi_cli_conversation_event *event, void *user_data) {
    struct repl_event_context *context = user_data;
    struct repl_turn_input_context *turn_input = context->turn_input;

    switch (event->type) {
    case OI_CLI_CONVERSATION_EVENT_AWAITING_PERMISSION: {
        oi_status status = build_permission_header(
            &turn_input->permission_tool_line,
            &turn_input->permission_args_line,
            event->as.awaiting_permission.name,
            event->as.awaiting_permission.arguments);
        if (status != OI_OK) {
            return status;
        }
        status = oi_cli_tool_panel_start(
            &turn_input->panel, event->as.awaiting_permission.id->data,
            event->as.awaiting_permission.id->len,
            event->as.awaiting_permission.name->data,
            event->as.awaiting_permission.name->len);
        if (status != OI_OK) {
            return status;
        }
        turn_input->awaiting_permission = 1;
        turn_input->permission_selected = 0;
        turn_input->dirty = 1;
        break;
    }
    case OI_CLI_CONVERSATION_EVENT_TOOL_STARTING: {
        oi_status status = oi_cli_tool_panel_start(
            &turn_input->panel, event->as.tool_starting.id->data,
            event->as.tool_starting.id->len,
            event->as.tool_starting.name->data,
            event->as.tool_starting.name->len);
        if (status != OI_OK) {
            return status;
        }
        turn_input->dirty = 1;
        break;
    }
    case OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT: {
        oi_status status = oi_cli_tool_panel_feed(
            &turn_input->panel, event->as.bytes.data, event->as.bytes.len);
        if (status != OI_OK) {
            return status;
        }
        turn_input->dirty = 1;
        break;
    }
    case OI_CLI_CONVERSATION_EVENT_MESSAGE:
        if (event->as.message.value->role == OI_CLI_MESSAGE_TOOL &&
            oi_cli_tool_panel_matches(
                &turn_input->panel,
                event->as.message.value->tool_call_id.data,
                event->as.message.value->tool_call_id.len)) {
            oi_cli_tool_panel_finish(
                &turn_input->panel,
                tool_panel_status_for_outcome(event->as.message.tool_outcome));
            turn_input->dirty = 1;
        }
        break;
    case OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA:
    case OI_CLI_CONVERSATION_EVENT_PARTIAL_ASSISTANT:
    case OI_CLI_CONVERSATION_EVENT_RESPONSE_DONE:
    case OI_CLI_CONVERSATION_EVENT_MODEL_ERROR:
    case OI_CLI_CONVERSATION_EVENT_TURN_DONE:
        break;
    }
    return oi_cli_present_event(event, context->present);
}

static const char permission_option_allow_once[] = "Allow once";
static const char permission_option_allow_process[] =
    "Allow for process (skip future prompts)";
static const char permission_option_deny[] = "Deny";
static const char permission_allowed_for_process_text[] =
    "oi: tool policy set to allow for the rest of this process\n";

/*
 * Applies one decoded event to the permission selector (via
 * oi_cli_selector_apply) instead of the editor -- called through
 * oi_cli_composer_feed_raw/_resolve_escape_raw, which never touch
 * composer->state, so the in-progress draft can't be corrupted by keys
 * being intercepted here. Returns nonzero once the decision is settled
 * (confirmed or cancelled), matching feed_raw's "stop at the first
 * decisive action" contract.
 */
static int permission_selector_key(const struct oi_cli_input_event *event,
                                   void *user_data) {
    struct repl_turn_input_context *context = user_data;
    enum oi_cli_selector_action action;
    oi_status status =
        oi_cli_selector_apply(event, 3, &context->permission_selected,
                              &action);

    if (status != OI_OK) {
        if (context->status == OI_OK) {
            context->status = status;
        }
        return 1;
    }
    switch (action) {
    case OI_CLI_SELECTOR_ACTION_NONE:
        return 0;
    case OI_CLI_SELECTOR_ACTION_REDRAW:
        context->dirty = 1;
        return 0;
    case OI_CLI_SELECTOR_ACTION_CONFIRM:
        status = oi_cli_conversation_resolve_permission(
            context->conversation, context->permission_selected != 2);
        if (status != OI_OK) {
            /* A signal callback earlier in the same reactor batch may
             * already have cancelled the pending call. Stale selector
             * input must not elevate process policy, but it also must not
             * turn that successful cancellation into a new error. */
            if (oi_cli_conversation_is_busy(context->conversation) &&
                context->status == OI_OK) {
                context->status = status;
            }
            context->awaiting_permission = 0;
            context->dirty = 1;
            return 1;
        }
        if (context->permission_selected == 1) {
            /* "Allow for process" is itself the explicit confirming act --
             * distinctly labeled from "allow once" in the selector, so no
             * further confirmation is layered on top here. Apply the
             * elevation only after the staged decision was accepted. */
            context->config->permission->policy = OI_CLI_TOOLS_ALLOW;
            report_busy_io_error(
                context,
                fputs(permission_allowed_for_process_text,
                     context->config->err) != EOF &&
                    fflush(context->config->err) == 0);
        }
        context->awaiting_permission = 0;
        context->dirty = 1;
        return 1;
    case OI_CLI_SELECTOR_ACTION_CANCEL:
        /* Dismissing the prompt (Escape or Ctrl+C while the selector is
         * up) must never leave the tool call hanging -- resolve as deny,
         * exactly like explicitly selecting "Deny" would. */
        status =
            oi_cli_conversation_resolve_permission(context->conversation, 0);
        if (status != OI_OK &&
            oi_cli_conversation_is_busy(context->conversation) &&
            context->status == OI_OK) {
            context->status = status;
        }
        context->awaiting_permission = 0;
        context->dirty = 1;
        return 1;
    }
    return 1;
}

static void handle_turn_escape_timeout(oi_reactor *reactor, void *user_data) {
    struct repl_turn_input_context *context = user_data;
    oi_reactor_timer *timer = context->escape_timer;
    oi_status status;

    (void)reactor;
    context->escape_timer = NULL;
    context->dirty = 1;
    oi_reactor_timer_cancel(timer);
    if (context->awaiting_permission) {
        status = oi_cli_composer_resolve_escape_raw(
            context->composer, permission_selector_key, context);
    } else {
        enum oi_cli_composer_action action;

        status = oi_cli_composer_resolve_escape(context->composer, &action);
        if (status == OI_OK && action == OI_CLI_COMPOSER_ACTION_CTRL_C) {
            oi_cli_conversation_cancel(context->conversation);
        }
    }
    if (status != OI_OK && context->status == OI_OK) {
        context->status = status;
    }
}

static const char busy_refused_text[] =
    "oi: a message is already queued; it will run once the current turn "
    "reaches a safe point\n";
static const char busy_queued_text[] =
    "oi: queued -- will run once the current turn reaches a safe point\n";
static const char pending_discarded_text[] =
    "oi: queued input discarded and restored to your draft\n";

static void report_busy_io_error(struct repl_turn_input_context *context,
                                 int wrote_ok) {
    if (!wrote_ok && context->status == OI_OK) {
        context->status = OI_ERR_IO;
    }
}

/*
 * Handles Enter pressed while a turn is active. /help and /status (the
 * only OI_CLI_COMMAND_READ_ONLY commands) dispatch immediately regardless
 * of the pending slot, matching the issue's own carve-out; everything
 * else -- a plain message, or any other command (including /exit) --
 * goes through the one-slot rule: refused non-destructively if the slot
 * is already occupied (the draft is only ever committed/cleared once the
 * submission is actually accepted, one way or the other), else committed,
 * durably persisted as QUEUED_INPUT, and held here until a later commit
 * wires up steering/consumption to actually run it at the next safe
 * boundary.
 */
static void handle_busy_submit(struct repl_turn_input_context *context) {
    struct oi_cli_command_parse peek;
    oi_status status;

    status = oi_cli_command_parse_text(
        oi_cli_editor_data(&context->composer->state.editor),
        oi_cli_editor_length(&context->composer->state.editor), &peek);
    if (status != OI_OK) {
        if (context->status == OI_OK) {
            context->status = status;
        }
        return;
    }

    if (peek.kind == OI_CLI_COMMAND_PARSE_UNKNOWN) {
        char *text = NULL;
        size_t text_len = 0;

        status = oi_cli_composer_commit_draft(context->composer, &text,
                                              &text_len);
        if (status != OI_OK) {
            if (context->status == OI_OK) {
                context->status = status;
            }
            return;
        }
        report_busy_io_error(
            context, fprintf(context->config->err,
                             "oi: unknown command: %.*s\n", (int)text_len,
                             text) >= 0 &&
                          fflush(context->config->err) == 0);
        free(text);
        return;
    }

    if (peek.kind == OI_CLI_COMMAND_PARSE_COMMAND &&
        peek.command->scope == OI_CLI_COMMAND_READ_ONLY) {
        char *text = NULL;
        size_t text_len = 0;
        struct oi_cli_command_parse committed;

        status = oi_cli_composer_commit_draft(context->composer, &text,
                                              &text_len);
        if (status != OI_OK) {
            if (context->status == OI_OK) {
                context->status = status;
            }
            return;
        }
        status = oi_cli_command_parse_text(text, text_len, &committed);
        if (status == OI_OK) {
            struct oi_cli_command_context command_context = {
                .out = context->config->out,
                .err = context->config->err,
                .model = context->current_model->data,
                .permission = context->config->permission,
                .session_id =
                    context->config->session_id == NULL
                        ? NULL
                        : context->config->session_id(
                              context->config->session_id_user_data),
                .set_model = dispatch_set_model,
                .set_model_user_data = context->setting_context,
                .set_cwd = dispatch_set_cwd,
                .set_cwd_user_data = context->setting_context,
            };
            enum oi_cli_command_result command_result;

            /* Read-only by construction (checked above): /help and
             * /status never return EXIT_REPL or mutate model/cwd, so
             * command_result needs no further handling here. */
            status = oi_cli_command_dispatch(&committed, &command_context,
                                             &command_result);
        }
        if (status != OI_OK && context->status == OI_OK) {
            context->status = status;
        }
        free(text);
        return;
    }

    if (context->pending->kind != OI_CLI_REPL_PENDING_NONE) {
        report_busy_io_error(
            context, fputs(busy_refused_text, context->config->err) != EOF &&
                          fflush(context->config->err) == 0);
        return;
    }

    {
        char *text = NULL;
        size_t text_len = 0;
        enum oi_cli_repl_pending_kind kind =
            peek.kind == OI_CLI_COMMAND_PARSE_COMMAND
                ? OI_CLI_REPL_PENDING_COMMAND
                : OI_CLI_REPL_PENDING_MESSAGE;

        status = oi_cli_composer_commit_draft(context->composer, &text,
                                              &text_len);
        if (status != OI_OK) {
            if (context->status == OI_OK) {
                context->status = status;
            }
            return;
        }
        if (peek.kind == OI_CLI_COMMAND_PARSE_LITERAL_SLASH) {
            memmove(text, text + 1, text_len - 1);
            text_len--;
        }
        if (context->config->persist_queued_input != NULL) {
            status = context->config->persist_queued_input(
                context->config->persist_queued_input_user_data, text,
                text_len, &context->pending->record_id);
            if (status != OI_OK) {
                if (context->status == OI_OK) {
                    context->status = status;
                }
                free(text);
                return;
            }
        } else {
            context->pending->record_id = 0;
        }
        context->pending->kind = kind;
        context->pending->text = text;
        context->pending->text_len = text_len;
        /* Now that something is queued, no new tool call (or follow-up
         * model round) should start ahead of it -- whatever's already in
         * flight (the model request, or a running tool) still finishes
         * naturally. */
        oi_cli_conversation_steer(context->conversation);
        report_busy_io_error(
            context, fputs(busy_queued_text, context->config->err) != EOF &&
                          fflush(context->config->err) == 0);
    }
}

/*
 * Ctrl+C during a turn always cancels it (handled by the caller before this
 * runs); this handles the one additional thing that's true only when
 * something is also queued: the pending item must not silently survive to
 * run after a turn the user just asked to stop entirely, so it's discarded
 * (persisted QUEUE_RESOLVED(DISCARDED), matching the schema -- there's no
 * "abandoned" resolution) and handed back verbatim as the live draft,
 * rather than dropped, since Ctrl+C is "stop", not "delete what I typed".
 */
static void discard_pending_on_cancel(struct repl_turn_input_context *context) {
    oi_status status = OI_OK;

    if (context->pending->kind == OI_CLI_REPL_PENDING_NONE) {
        return;
    }
    if (context->config->persist_queue_resolved != NULL) {
        status = context->config->persist_queue_resolved(
            context->config->persist_queue_resolved_user_data,
            context->pending->record_id, /*consumed=*/0);
    }
    if (status == OI_OK) {
        status = oi_cli_composer_set_draft(context->composer,
                                           context->pending->text,
                                           context->pending->text_len);
    }
    repl_pending_free(context->pending);
    if (status != OI_OK) {
        if (context->status == OI_OK) {
            context->status = status;
        }
        return;
    }
    context->dirty = 1;
    report_busy_io_error(
        context, fputs(pending_discarded_text, context->config->err) != EOF &&
                      fflush(context->config->err) == 0);
}

static void handle_turn_input(oi_reactor *reactor, int fd, int revents,
                              void *user_data) {
    struct repl_turn_input_context *context = user_data;
    oi_status status;

    (void)fd;
    (void)revents;
    context->dirty = 1;
    if (context->escape_timer != NULL) {
        oi_reactor_timer_cancel(context->escape_timer);
        context->escape_timer = NULL;
    }
    if (context->awaiting_permission) {
        /* Keys are intercepted by the selector entirely -- never reach
         * the editor -- for as long as a decision is outstanding. */
        status = oi_cli_composer_feed_raw(context->composer,
                                          permission_selector_key, context);
    } else {
        enum oi_cli_composer_action action;

        status = oi_cli_composer_feed(context->composer, &action);
        if (status == OI_OK) {
            if (action == OI_CLI_COMPOSER_ACTION_CTRL_C) {
                oi_cli_conversation_cancel(context->conversation);
                discard_pending_on_cancel(context);
            } else if (action == OI_CLI_COMPOSER_ACTION_SUBMIT) {
                handle_busy_submit(context);
            }
            /* EXIT (Ctrl+D at an empty draft) while busy is still a
             * documented no-op: not explicitly required by the issue, and
             * there is no pressing need to synthesize an "/exit" queue
             * entry for it ahead of an explicitly typed /exit, which
             * already goes through the one-slot rule above like any other
             * command. */
        }
    }
    if (status != OI_OK) {
        if (context->status == OI_OK) {
            context->status = status;
        }
        return;
    }
    if (oi_cli_composer_escape_pending(context->composer)) {
        (void)oi_reactor_timer_start(reactor,
                                     OI_CLI_COMPOSER_ESCAPE_TIMEOUT_MS,
                                     handle_turn_escape_timeout, context,
                                     &context->escape_timer);
    }
}

static oi_status dispatch_set_model(void *user_data, const char *name,
                                    size_t name_len) {
    struct repl_setting_context *context = user_data;
    oi_status status = OI_OK;

    if (context->config->persist_model != NULL) {
        status = context->config->persist_model(
            context->config->persist_model_user_data, name, name_len);
        if (status != OI_OK) {
            return status;
        }
    }
    status = oi_cli_string_set(context->current_model, name, name_len);
    if (status == OI_OK && *context->conversation != NULL) {
        status = oi_cli_conversation_set_model(*context->conversation, name,
                                               name_len);
    }
    return status;
}

static oi_status dispatch_set_cwd(void *user_data, const char *path,
                                  size_t path_len) {
    struct repl_setting_context *context = user_data;
    char *path_copy;
    char resolved[PATH_MAX];
    char *previous;
    struct stat info;
    oi_status status;

    path_copy = malloc(path_len + 1);
    if (path_copy == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';

    if (stat(path_copy, &info) != 0 || !S_ISDIR(info.st_mode) ||
        realpath(path_copy, resolved) == NULL) {
        free(path_copy);
        return OI_ERR_INVAL;
    }
    free(path_copy);

    previous = getcwd(NULL, 0);
    if (previous == NULL) {
        return OI_ERR_IO;
    }
    if (chdir(resolved) != 0) {
        free(previous);
        return OI_ERR_INVAL;
    }

    if (context->config->persist_cwd != NULL) {
        status = context->config->persist_cwd(
            context->config->persist_cwd_user_data, resolved,
            strlen(resolved));
        if (status != OI_OK) {
            /* Nothing was durably written yet: roll the live chdir back
             * rather than leaving live and durable state disagreeing. A
             * failed rollback has no further fallback -- `status` (the
             * persist failure) is still the right thing to report. */
            if (chdir(previous) != 0) {
                /* best effort: nothing more to do */
            }
            free(previous);
            return status;
        }
    }
    free(previous);
    return OI_OK;
}

oi_status oi_cli_repl_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_repl_config *config) {
    struct oi_cli_input_history input_history;
    struct oi_cli_present present;
    struct oi_cli_composer composer;
    int composer_initialized = 0;
    struct oi_cli_conversation_config conversation_config;
    oi_cli_conversation *conversation = NULL;
    struct oi_cli_string current_model_storage = {0};
    struct oi_cli_string *current_model;
    struct repl_setting_context setting_context;
    struct repl_event_context repl_event_context;
    struct oi_cli_repl_pending pending = {0};
    int signal_fd = -1;
    oi_status status;

    if (client == NULL || reactor == NULL || tools == NULL ||
        config == NULL || config->model == NULL || config->max_turns <= 0 ||
        config->tool_timeout_ms < 0 || config->permission == NULL ||
        config->input_fd < 0 || config->output_fd < 0 ||
        config->out == NULL || config->err == NULL) {
        return OI_ERR_INVAL;
    }
    if (arena == NULL && config->prepare == NULL) {
        return OI_ERR_INVAL;
    }
    current_model = config->pending_model != NULL ? config->pending_model
                                                  : &current_model_storage;

    oi_cli_input_history_init(&input_history);
    status = seed_input_history(&input_history, config->initial_context);
    if (status != OI_OK) {
        goto cleanup_history;
    }
    if (config->pending_model == NULL) {
        status = oi_cli_string_set(current_model, config->model,
                                   strlen(config->model));
        if (status != OI_OK) {
            goto cleanup_history;
        }
    }
    status = oi_cli_present_init(
        &present, config->out, config->err, 0, /*styling_enabled=*/1,
        config->on_event, config->event_user_data);
    if (status != OI_OK) {
        goto cleanup_history;
    }

    /* Raw mode/bracketed paste now spans the whole REPL run, not each
     * prompt read: reading and redrawing terminal input during a turn
     * needs raw mode active during the turn too. */
    status = oi_cli_composer_init(&composer, config->input_fd,
                                  config->output_fd, &input_history);
    if (status != OI_OK) {
        goto cleanup_present;
    }
    composer_initialized = 1;
    if (config->initial_draft != NULL && config->initial_draft_len > 0) {
        status = oi_cli_composer_set_draft(&composer, config->initial_draft,
                                           config->initial_draft_len);
        if (status != OI_OK) {
            goto cleanup_composer;
        }
    }

    {
        sigset_t signal_mask;

        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, SIGWINCH);
        sigaddset(&signal_mask, SIGINT);
        sigaddset(&signal_mask, SIGTERM);
        sigaddset(&signal_mask, SIGHUP);
        /* Graceful degradation, never a hard failure: if blocking the
         * signals or creating the signalfd fails for any reason, signal_fd
         * stays -1, which oi_cli_composer_wait_submit already treats as
         * "signal handling unavailable" -- the REPL still works exactly as
         * it does without this feature (just without live resize, and
         * without Ctrl+C/SIGTERM/SIGHUP being caught cleanly). */
        if (sigprocmask(SIG_BLOCK, &signal_mask, NULL) == 0) {
            signal_fd =
                signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
        }
    }

    setting_context.config = config;
    setting_context.current_model = current_model;
    setting_context.conversation = &conversation;

    /* conversation_config.model is set fresh just before each
     * oi_cli_conversation_create call below (not here): a /model command
     * before the first message reallocates current_model->data, and
     * capturing the pointer this early would leave it dangling. */
    conversation_config.max_model_steps = config->max_turns;
    conversation_config.tool_timeout_ms = config->tool_timeout_ms;
    conversation_config.permission = oi_cli_tool_permission;
    conversation_config.permission_user_data = config->permission;
    repl_event_context.present = &present;
    repl_event_context.turn_input = NULL;
    conversation_config.on_event = repl_conversation_event;
    conversation_config.event_user_data = &repl_event_context;

    for (;;) {
        char *prompt = NULL;
        size_t prompt_len = 0;
        int exit_requested = 0;
        int terminate_signal = 0;
        struct oi_cli_command_parse parsed;

        if (pending.kind != OI_CLI_REPL_PENDING_NONE) {
            /* The safe boundary the previous turn's steering was waiting
             * for has been reached (present.done, back at the top of this
             * loop): resolve the queue durably before running it, per the
             * issue's own requirement, and run it immediately, before
             * returning to an idle read. A queued command always resolves
             * DISCARDED (the durable schema has no "consumed by running a
             * command" shape -- see cli_history_replay.c's queue_consumed
             * gate); only a queued plain message can ever resolve
             * CONSUMED. */
            int consumed = pending.kind == OI_CLI_REPL_PENDING_MESSAGE;

            if (config->persist_queue_resolved != NULL) {
                status = config->persist_queue_resolved(
                    config->persist_queue_resolved_user_data,
                    pending.record_id, consumed);
                if (status != OI_OK) {
                    repl_pending_free(&pending);
                    if (config->is_durably_failed != NULL &&
                        config->is_durably_failed(
                            config->is_durably_failed_user_data)) {
                        break;
                    }
                    if (fprintf(config->err, "oi: turn failed: %s\n",
                               oi_status_str(status)) < 0 ||
                        fflush(config->err) != 0) {
                        status = OI_ERR_IO;
                        break;
                    }
                    status = OI_OK;
                    continue;
                }
            }
            prompt = pending.text;
            prompt_len = pending.text_len;
            pending.text = NULL;
            pending.text_len = 0;
            pending.kind = OI_CLI_REPL_PENDING_NONE;
            pending.record_id = 0;
            if (consumed) {
                /* Already known to be a plain message as of queueing time
                 * (any leading "//" already stripped then, too) --
                 * skip straight past command parsing/dispatch instead of
                 * re-parsing it, which would wrongly re-trigger command
                 * handling for message content that happens to start with
                 * a bare "/". */
                goto have_message;
            }
            status = oi_cli_command_parse_text(prompt, prompt_len, &parsed);
            if (status != OI_OK) {
                free(prompt);
                break;
            }
            goto have_parsed_command;
        }

        status = oi_cli_composer_wait_submit(
            &composer, signal_fd, &prompt, &prompt_len, &exit_requested,
            &terminate_signal);
        /* No turn is active at this point in the loop (we're between
         * turns, about to read the next prompt), so a terminate signal
         * here needs no conversation cancellation -- just end cleanly,
         * exactly like Ctrl+D. */
        if (status != OI_OK || exit_requested || terminate_signal != 0) {
            free(prompt);
            break;
        }
        status = oi_cli_command_parse_text(prompt, prompt_len, &parsed);
        if (status != OI_OK) {
            free(prompt);
            break;
        }
have_parsed_command:
        if (parsed.kind == OI_CLI_COMMAND_PARSE_UNKNOWN) {
            if (fprintf(config->err, "oi: unknown command: %.*s\n",
                        (int)prompt_len, prompt) < 0) {
                status = OI_ERR_IO;
                free(prompt);
                break;
            }
            free(prompt);
            continue;
        }
        if (parsed.kind == OI_CLI_COMMAND_PARSE_COMMAND) {
            /*
             * /compact never reaches cli_command_dispatch.c: dispatch has
             * no access to the conversation, durable history store, or LLM
             * client by design, and this command genuinely needs all
             * three. Handled entirely here, at the same idle/safe-boundary
             * label a freshly-typed and a dequeued pending command both
             * funnel through -- matching the /permissions allow gate right
             * below.
             */
            if (parsed.command->id == OI_CLI_COMMAND_COMPACT) {
                size_t keep_turns = 8;
                size_t turns_value = 0;
                int has_value = 0;

                status = oi_cli_compact_parse_turns(
                    parsed.arguments, parsed.arguments_len, &turns_value,
                    &has_value);
                if (status != OI_OK) {
                    status = OI_OK;
                    if (fputs("oi: usage: /compact [turns]\n",
                             config->err) == EOF ||
                        fflush(config->err) != 0) {
                        status = OI_ERR_IO;
                        free(prompt);
                        break;
                    }
                    free(prompt);
                    continue;
                }
                if (has_value) {
                    keep_turns = turns_value;
                }
                if (conversation == NULL) {
                    if (fputs("oi: nothing to compact yet\n",
                             config->err) == EOF ||
                        fflush(config->err) != 0) {
                        status = OI_ERR_IO;
                        free(prompt);
                        break;
                    }
                    free(prompt);
                    continue;
                }

                const struct oi_cli_message_list *compact_messages =
                    oi_cli_conversation_messages(conversation);
                size_t prefix_count = 0;
                size_t total_turns = 0;

                status = oi_cli_compact_select_prefix(
                    compact_messages, keep_turns, &prefix_count,
                    &total_turns);
                if (status == OI_OK && prefix_count == 0) {
                    if (fprintf(config->err,
                               "oi: nothing to compact (%zu turn%s, "
                               "keeping %zu)\n",
                               total_turns, total_turns == 1 ? "" : "s",
                               keep_turns) < 0 ||
                        fflush(config->err) != 0) {
                        status = OI_ERR_IO;
                        free(prompt);
                        break;
                    }
                    free(prompt);
                    continue;
                }
                if (status != OI_OK) {
                    free(prompt);
                    break;
                }

                oi_json_writer *compact_writer = NULL;
                status = oi_cli_compact_build_request(
                    compact_messages, prefix_count, current_model->data,
                    current_model->len, &compact_writer);
                if (status != OI_OK) {
                    free(prompt);
                    break;
                }
                size_t compact_body_len = 0;
                const char *compact_body =
                    oi_json_writer_data(compact_writer, &compact_body_len);
                struct oi_cli_compact_result compact_result;
                int compact_result_ready = 0;
                status = oi_cli_terminal_set_isig(&composer.terminal, 1);
                if (status == OI_OK) {
                    status = oi_cli_compact_run(
                        client, reactor,
                        oi_cli_conversation_arena(conversation), signal_fd,
                        compact_body, compact_body_len, &compact_result);
                    compact_result_ready = status == OI_OK;
                    oi_status restore_status =
                        oi_cli_terminal_set_isig(&composer.terminal, 0);
                    if (status == OI_OK && restore_status != OI_OK) {
                        status = restore_status;
                    }
                }
                oi_json_writer_destroy(compact_writer);
                if (status != OI_OK) {
                    if (compact_result_ready) {
                        oi_cli_compact_result_free(&compact_result);
                    }
                    free(prompt);
                    break;
                }

                if (compact_result.outcome == OI_CLI_COMPACT_CANCELLED) {
                    int terminate = compact_result.terminate_signal != 0;
                    oi_cli_compact_result_free(&compact_result);
                    if (terminate) {
                        free(prompt);
                        break;
                    }
                    if (fputs("oi: compaction cancelled\n", config->err) ==
                            EOF ||
                        fflush(config->err) != 0) {
                        status = OI_ERR_IO;
                        free(prompt);
                        break;
                    }
                    free(prompt);
                    continue;
                }
                if (compact_result.outcome == OI_CLI_COMPACT_FAILED) {
                    oi_status failed_status = compact_result.status;
                    oi_cli_compact_result_free(&compact_result);
                    if (fprintf(config->err, "oi: compaction failed: %s\n",
                               oi_status_str(failed_status)) < 0 ||
                        fflush(config->err) != 0) {
                        status = OI_ERR_IO;
                        free(prompt);
                        break;
                    }
                    free(prompt);
                    continue;
                }

                /* OI_CLI_COMPACT_OK: durable append must fully succeed
                 * before anything live changes, so a failure here leaves
                 * both the durable log and the live conversation
                 * completely untouched -- see cli.c's persist_checkpoint. */
                status = OI_OK;
                if (config->persist_checkpoint != NULL) {
                    status = config->persist_checkpoint(
                        config->persist_checkpoint_user_data, prefix_count,
                        compact_result.summary, compact_result.summary_len,
                        current_model->data, current_model->len);
                }
                if (status != OI_OK) {
                    oi_cli_compact_result_free(&compact_result);
                    if (config->is_durably_failed != NULL &&
                        config->is_durably_failed(
                            config->is_durably_failed_user_data)) {
                        break;
                    }
                    if (fprintf(config->err, "oi: turn failed: %s\n",
                               oi_status_str(status)) < 0 ||
                        fflush(config->err) != 0) {
                        status = OI_ERR_IO;
                        free(prompt);
                        break;
                    }
                    status = OI_OK;
                    free(prompt);
                    continue;
                }
                /* Allocation-free ownership transfer: compact_result already
                 * owns the summary buffer, so after the durable append there
                 * is no resource-exhaustion failure that can leave durable
                 * and live state out of lock-step. */
                status =
                    oi_cli_conversation_apply_checkpoint_take_summary(
                    conversation, prefix_count, &compact_result.summary,
                    compact_result.summary_len);
                size_t compacted_turns = total_turns - keep_turns;
                oi_cli_compact_result_free(&compact_result);
                if (status != OI_OK) {
                    free(prompt);
                    break;
                }
                if (fprintf(config->out,
                           "oi: compacted %zu turn%s into a checkpoint "
                           "(kept last %zu)\n",
                           compacted_turns, compacted_turns == 1 ? "" : "s",
                           keep_turns) < 0 ||
                    fflush(config->out) != 0) {
                    status = OI_ERR_IO;
                    free(prompt);
                    break;
                }
                free(prompt);
                continue;
            }
            /*
             * /permissions allow must not silently elevate the process-
             * wide tool policy: gated here (not in cli_command_dispatch.c,
             * which has no composer/terminal access by design and
             * shouldn't gain any) rather than in dispatch_permissions
             * itself, so this one insertion covers both a freshly-typed
             * command and a dequeued pending one (issue #25's one-slot
             * queue also funnels through this same have_parsed_command:
             * label). /permissions ask/deny, and a redundant allow when
             * already allowed, skip this gate and dispatch directly.
             */
            if (parsed.command->id == OI_CLI_COMMAND_PERMISSIONS &&
                parsed.arguments_len == 5 &&
                memcmp(parsed.arguments, "allow", 5) == 0 &&
                config->permission->policy != OI_CLI_TOOLS_ALLOW) {
                static const char elevation_option_cancel_text[] =
                    "Cancel (keep current policy)";
                static const char elevation_option_allow_text[] =
                    "Allow all tools for this process (no further prompts "
                    "until changed)";
                static const struct oi_cli_render_line elevation_options[] = {
                    {elevation_option_cancel_text,
                     sizeof elevation_option_cancel_text - 1},
                    {elevation_option_allow_text,
                     sizeof elevation_option_allow_text - 1},
                };
                size_t selected = 0;
                int cancelled = 0;
                int elevation_terminate_signal = 0;

                status = oi_cli_composer_select(
                    &composer, signal_fd, NULL, 0, elevation_options, 2, 0,
                    &selected, &cancelled, &elevation_terminate_signal);
                if (status != OI_OK || elevation_terminate_signal != 0) {
                    free(prompt);
                    break;
                }
                /* Erases the confirm selector's own frame and redraws the
                 * ordinary idle prompt in its place -- self-contained,
                 * same as any other post-selector cleanup in this file. */
                status = oi_cli_composer_redraw(&composer);
                if (status != OI_OK) {
                    free(prompt);
                    break;
                }
                if (cancelled || selected == 0) {
                    if (fputs("oi: permissions unchanged\n", config->err) ==
                            EOF ||
                        fflush(config->err) != 0) {
                        status = OI_ERR_IO;
                        free(prompt);
                        break;
                    }
                    free(prompt);
                    continue;
                }
                /* selected == 1 ("Allow..."): fall through to the ordinary
                 * dispatch below, which is what actually applies it. */
            }
            struct oi_cli_command_context command_context = {
                .out = config->out,
                .err = config->err,
                .model = current_model->data,
                .permission = config->permission,
                .session_id =
                    config->session_id == NULL
                        ? NULL
                        : config->session_id(
                              config->session_id_user_data),
                .set_model = dispatch_set_model,
                .set_model_user_data = &setting_context,
                .set_cwd = dispatch_set_cwd,
                .set_cwd_user_data = &setting_context,
            };
            enum oi_cli_command_result command_result;

            status = oi_cli_command_dispatch(
                &parsed, &command_context, &command_result);
            free(prompt);
            if (status != OI_OK ||
                command_result == OI_CLI_COMMAND_EXIT_REPL) {
                break;
            }
            continue;
        }
        if (parsed.kind == OI_CLI_COMMAND_PARSE_LITERAL_SLASH) {
            memmove(prompt, prompt + 1, prompt_len);
            prompt_len--;
        }
have_message:
        if (conversation == NULL) {
            oi_arena *conversation_arena = arena;

            if (config->prepare != NULL) {
                status = config->prepare(config->prepare_user_data,
                                         &conversation_arena);
            }
            if (status == OI_OK && conversation_arena == NULL) {
                status = OI_ERR_INVAL;
            }
            if (status == OI_OK) {
                /* Read current_model->data only now: config->prepare (the
                 * lazy automatic-session path) may have just resolved a
                 * restored/rebuilt value into it. */
                conversation_config.model = current_model->data;
                status = oi_cli_conversation_create(
                    client, reactor, conversation_arena, tools,
                    &conversation_config, config->initial_context,
                    &conversation);
            }
            if (status != OI_OK) {
                free(prompt);
                break;
            }
        }

        oi_cli_present_reset_turn(&present);
        status =
            oi_cli_conversation_start(conversation, prompt, prompt_len);
        free(prompt);
        {
            struct repl_turn_signal_context turn_signal_context = {
                .conversation = conversation,
            };
            struct repl_turn_input_context turn_input_context = {
                .composer = &composer,
                .conversation = conversation,
                .config = config,
                .current_model = current_model,
                .setting_context = &setting_context,
                .pending = &pending,
            };
            int signal_registered;
            int input_registered;

            repl_event_context.turn_input = &turn_input_context;
            signal_registered =
                signal_fd >= 0 &&
                oi_reactor_add(reactor, signal_fd, OI_EV_READ,
                               handle_turn_signal,
                               &turn_signal_context) == OI_OK;
            input_registered =
                oi_reactor_add(reactor, config->input_fd, OI_EV_READ,
                               handle_turn_input,
                               &turn_input_context) == OI_OK;

            while (status == OI_OK && !present.done) {
                oi_status step_status;

                /* Clear the composer's frame (if drawn) before the step,
                 * then redraw fresh once it settles: the turn's own
                 * streamed output and any decoded keystrokes both land on
                 * the same fd, and interleaving them without this erase/
                 * redraw bracket would corrupt both writers' row-count
                 * assumptions (see oi_cli_render_erase's own doc comment).
                 * The redraw itself only happens when this step actually
                 * touched the composer (input decoded, or a resize) --
                 * redrawing unconditionally on every step, including ones
                 * that were purely model/tool activity, would inject the
                 * composer's escape sequences into the *middle* of a
                 * multi-chunk streamed reply whenever that reply and some
                 * unrelated composer-relevant step happen to land in
                 * separate oi_reactor_step calls, corrupting normal
                 * token-by-token streaming outright, not just risking
                 * flicker. The erase, in contrast, is always safe to run
                 * unconditionally: it's a no-op whenever nothing is
                 * currently drawn. */
                oi_cli_render_erase(&composer.render);
                if (oi_reactor_step(reactor, -1, &step_status) < 0) {
                    status = step_status;
                }
                if (fflush(config->out) != 0 && status == OI_OK) {
                    status = OI_ERR_IO;
                }
                if (turn_input_context.status != OI_OK && status == OI_OK) {
                    status = turn_input_context.status;
                }
                if (status == OI_OK &&
                    (turn_input_context.dirty ||
                     turn_signal_context.resize_pending) &&
                    (!present.done || turn_input_context.panel.active)) {
                    oi_status redraw_status;

                    if (turn_input_context.awaiting_permission) {
                        struct oi_cli_render_line header[2];
                        struct oi_cli_render_line options[3] = {
                            {permission_option_allow_once,
                             sizeof permission_option_allow_once - 1},
                            {permission_option_allow_process,
                             sizeof permission_option_allow_process - 1},
                            {permission_option_deny,
                             sizeof permission_option_deny - 1},
                        };
                        size_t header_count = 0;

                        if (turn_input_context.permission_tool_line.len > 0) {
                            header[header_count].text =
                                turn_input_context.permission_tool_line.data;
                            header[header_count].len =
                                turn_input_context.permission_tool_line.len;
                            header_count++;
                        }
                        if (turn_input_context.permission_args_line.len > 0) {
                            header[header_count].text =
                                turn_input_context.permission_args_line.data;
                            header[header_count].len =
                                turn_input_context.permission_args_line.len;
                            header_count++;
                        }
                        redraw_status = oi_cli_composer_draw_selector(
                            &composer, header, header_count, options, 3,
                            turn_input_context.permission_selected);
                    } else if (turn_input_context.panel.active) {
                        struct oi_cli_render_line
                            panel_lines[OI_CLI_TOOL_PANEL_MAX_LINES];
                        size_t panel_line_count = oi_cli_tool_panel_lines(
                            &turn_input_context.panel, panel_lines,
                            OI_CLI_TOOL_PANEL_MAX_LINES);

                        redraw_status = oi_cli_composer_redraw_panel(
                            &composer, panel_lines, panel_line_count);
                    } else {
                        redraw_status = oi_cli_composer_redraw(&composer);
                    }
                    if (redraw_status != OI_OK) {
                        status = redraw_status;
                    }
                    turn_input_context.dirty = 0;
                    turn_signal_context.resize_pending = 0;
                }
            }
            oi_cli_bytebuf_free(&turn_input_context.permission_tool_line);
            oi_cli_bytebuf_free(&turn_input_context.permission_args_line);
            oi_cli_tool_panel_free(&turn_input_context.panel);
            repl_event_context.turn_input = NULL;
            if (signal_registered) {
                oi_reactor_remove(reactor, signal_fd);
            }
            if (input_registered) {
                oi_reactor_remove(reactor, config->input_fd);
            }
            oi_reactor_timer_cancel(turn_input_context.escape_timer);
            if (status == OI_OK) {
                status = present.status;
            }
            if (turn_signal_context.terminate_signal != 0) {
                /* SIGTERM/SIGHUP: the conversation was already cancelled
                 * above; clean up and exit regardless of anything else.
                 * Report a clean exit (OI_OK), matching the idle-prompt
                 * SIGTERM/SIGHUP path (oi_cli_composer_wait_submit's own
                 * terminate_signal out-param leaves its return status
                 * OI_OK) rather than leaking the cancellation's own
                 * OI_ERR_CLOSED as if the turn itself had failed. */
                status = OI_OK;
                break;
            }
            if (turn_input_context.status == OI_OK &&
                oi_cli_conversation_was_cancelled(conversation)) {
                /* Ctrl+C during this turn: always recoverable, regardless
                 * of whatever oi_status got attached to it -- but only when
                 * nothing else went wrong. A genuine REPL-level failure
                 * (e.g. discard_pending_on_cancel's own persist call
                 * failing) must not be masked as an ordinary cancel; the
                 * turn_input_context.status != OI_OK case falls through to
                 * the generic status != OI_OK handling below instead, which
                 * reports it properly. */
                if (fputs("oi: cancelled\n", config->err) == EOF ||
                    fflush(config->err) != 0) {
                    status = OI_ERR_IO;
                    break;
                }
                continue;
            }
            if (status != OI_OK) {
                if (oi_cli_conversation_is_busy(conversation)) {
                    oi_cli_conversation_cancel(conversation);
                }
                if (config->is_durably_failed != NULL &&
                    config->is_durably_failed(
                        config->is_durably_failed_user_data)) {
                    break;
                }
                if (fprintf(config->err, "oi: turn failed: %s\n",
                           oi_status_str(status)) < 0 ||
                    fflush(config->err) != 0) {
                    status = OI_ERR_IO;
                    break;
                }
                status = OI_OK;
                continue;
            }
        }
    }

    if (signal_fd >= 0) {
        close(signal_fd);
    }
    oi_cli_conversation_destroy(conversation);
cleanup_composer:
    if (composer_initialized) {
        oi_cli_composer_free(&composer);
    }
    repl_pending_free(&pending);
cleanup_present:
    oi_cli_present_free(&present);
cleanup_history:
    oi_cli_input_history_free(&input_history);
    oi_cli_string_free(&current_model_storage);
    return status;
}
