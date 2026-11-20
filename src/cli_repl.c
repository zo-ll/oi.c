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
    enum oi_cli_session_model_origin model_origin;
};

enum oi_cli_repl_pending_kind {
    OI_CLI_REPL_PENDING_NONE = 0,
    OI_CLI_REPL_PENDING_MESSAGE,
    OI_CLI_REPL_PENDING_COMMAND
};

struct oi_cli_repl_pending {
    enum oi_cli_repl_pending_kind kind;
    char *text;
    size_t text_len;
    uint64_t record_id;
};

static void repl_pending_free(struct oi_cli_repl_pending *pending) {
    free(pending->text);
    pending->text = NULL;
    pending->text_len = 0;
    pending->kind = OI_CLI_REPL_PENDING_NONE;
    pending->record_id = 0;
}

struct repl_status_context {
    const struct oi_cli_repl_config *config;
    struct repl_setting_context *setting;
    oi_cli_conversation **conversation;
    const struct oi_cli_repl_pending *pending;
    unsigned int checkpoints_applied;
    char cwd[PATH_MAX];
};

static enum oi_cli_status_session_state repl_session_state(
    const struct oi_cli_repl_config *config, const char *id) {
    if (id != NULL) {
        return config->is_durably_failed != NULL &&
                       config->is_durably_failed(
                           config->is_durably_failed_user_data)
                   ? OI_CLI_STATUS_SESSION_FAILED
                   : OI_CLI_STATUS_SESSION_ACTIVE;
    }
    return config->prepare == NULL ? OI_CLI_STATUS_SESSION_EPHEMERAL
                                   : OI_CLI_STATUS_SESSION_NOT_CREATED;
}

static enum oi_cli_status_queue_state repl_queue_state(
    const struct oi_cli_repl_pending *pending) {
    switch (pending->kind) {
    case OI_CLI_REPL_PENDING_NONE:
        return OI_CLI_STATUS_QUEUE_EMPTY;
    case OI_CLI_REPL_PENDING_MESSAGE:
        return OI_CLI_STATUS_QUEUE_MESSAGE;
    case OI_CLI_REPL_PENDING_COMMAND:
        return OI_CLI_STATUS_QUEUE_COMMAND;
    }
    return OI_CLI_STATUS_QUEUE_EMPTY;
}

static oi_status repl_status_snapshot(
    void *user_data, struct oi_cli_status_snapshot *out_snapshot) {
    struct repl_status_context *context = user_data;
    const struct oi_cli_repl_config *config = context->config;
    const oi_cli_conversation *conversation = *context->conversation;

    out_snapshot->session_id =
        config->session_id == NULL
            ? NULL
            : config->session_id(config->session_id_user_data);
    out_snapshot->session_state =
        repl_session_state(config, out_snapshot->session_id);
    out_snapshot->model = context->setting->current_model->data;
    out_snapshot->model_origin = context->setting->model_origin;
    out_snapshot->endpoint = *config->endpoint;
    out_snapshot->permission =
        oi_cli_status_permission_from_policy(config->permission->policy);
    out_snapshot->request_timeout_ms = config->request_timeout_ms;
    out_snapshot->tool_timeout_ms = config->tool_timeout_ms;
    out_snapshot->cwd = getcwd(context->cwd, sizeof context->cwd);
    out_snapshot->conversation = oi_cli_conversation_activity(conversation);
    out_snapshot->conversation_status =
        conversation == NULL ? OI_OK
                             : oi_cli_conversation_last_status(conversation);
    out_snapshot->steering = oi_cli_conversation_is_steering(conversation);
    out_snapshot->queue = repl_queue_state(context->pending);
    out_snapshot->queue_bytes = context->pending->text_len;
    if (config->status_checkpoint != NULL) {
        config->status_checkpoint(config->status_checkpoint_user_data,
                                  &out_snapshot->checkpoint);
    }
    out_snapshot->checkpoint.applied_this_run =
        context->checkpoints_applied > 0;
    out_snapshot->checkpoint.context_compacted =
        out_snapshot->checkpoint.has_durable_checkpoint ||
        out_snapshot->checkpoint.applied_this_run;
    out_snapshot->checkpoint.known = 1;
    return OI_OK;
}

struct repl_turn_signal_context {
    oi_cli_conversation *conversation;
    int terminate_signal;
    int resize_pending;
};

static void handle_turn_signal(oi_reactor *reactor, int fd, int revents,
                               void *user_data) {
    struct repl_turn_signal_context *context = user_data;
    struct signalfd_siginfo info;

    (void)reactor;
    (void)revents;
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
    struct repl_status_context *status_context;
    struct oi_cli_repl_pending *pending;
    oi_reactor_timer *escape_timer;
    oi_status status;
    int dirty;
    int awaiting_permission;
    size_t permission_selected;
    struct oi_cli_bytebuf permission_tool_line;
    struct oi_cli_bytebuf permission_args_line;
    struct oi_cli_tool_panel panel;
};

#define PERMISSION_OPTION_DENY 2u

static oi_status build_permission_header(struct oi_cli_bytebuf *tool_line,
                                         struct oi_cli_bytebuf *args_line,
                                         const struct oi_cli_string *name) {
    static const char review[] = "Review the request above before you allow it.";
    struct oi_cli_sanitize_state sanitize;
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
        status = oi_cli_bytebuf_append(args_line, review, sizeof review - 1);
    }
    return status;
}

struct repl_event_context {
    struct oi_cli_present *present;
    struct repl_turn_input_context *turn_input;
};

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
            event->as.awaiting_permission.name);
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
        turn_input->permission_selected = PERMISSION_OPTION_DENY;
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
            context->conversation,
            context->permission_selected != PERMISSION_OPTION_DENY);
        if (status != OI_OK) {
            if (oi_cli_conversation_is_busy(context->conversation) &&
                context->status == OI_OK) {
                context->status = status;
            }
            context->awaiting_permission = 0;
            context->dirty = 1;
            return 1;
        }
        if (context->permission_selected == 1) {
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
                .status = repl_status_snapshot,
                .status_user_data = context->status_context,
            };
            enum oi_cli_command_result command_result;

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
        oi_cli_conversation_steer(context->conversation);
        report_busy_io_error(
            context, fputs(busy_queued_text, context->config->err) != EOF &&
                          fflush(context->config->err) == 0);
    }
}

static void discard_pending_on_cancel(struct repl_turn_input_context *context) {
    oi_status status = OI_OK;

    if (context->pending->kind == OI_CLI_REPL_PENDING_NONE) {
        return;
    }
    if (context->config->persist_queue_resolved != NULL) {
        status = context->config->persist_queue_resolved(
            context->config->persist_queue_resolved_user_data,
            context->pending->record_id, 0);
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
    if (status != OI_OK) {
        return status;
    }
    context->model_origin = OI_CLI_SESSION_MODEL_COMMAND;
    if (*context->conversation != NULL) {
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
            if (chdir(previous) != 0) {
            }
            free(previous);
            return status;
        }
    }
    free(previous);
    return OI_OK;
}

static int repl_is_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static void repl_split_first_token(const char *text, size_t len,
                                   const char **out_token,
                                   size_t *out_token_len, const char **out_rest,
                                   size_t *out_rest_len) {
    size_t token_len = 0;
    size_t rest_start;

    while (token_len < len && !repl_is_space(text[token_len])) {
        token_len++;
    }
    rest_start = token_len;
    while (rest_start < len && repl_is_space(text[rest_start])) {
        rest_start++;
    }
    *out_token = text;
    *out_token_len = token_len;
    *out_rest = text + rest_start;
    *out_rest_len = len - rest_start;
}

static int repl_token_equals(const char *token, size_t token_len,
                             const char *value) {
    size_t value_len = strlen(value);
    return token_len == value_len && memcmp(token, value, value_len) == 0;
}

struct repl_session_switch_outcome {
    int terminate;
    int switched;
    enum oi_cli_session_model_origin model_origin;
};

static oi_status repl_confirm(struct oi_cli_composer *composer, int signal_fd,
                              const char *cancel_text,
                              const char *proceed_text, int *out_confirmed,
                              int *out_terminate) {
    struct oi_cli_render_line options[2];
    size_t selected = 0;
    int cancelled = 0;
    oi_status status;

    options[0].text = cancel_text;
    options[0].len = strlen(cancel_text);
    options[1].text = proceed_text;
    options[1].len = strlen(proceed_text);
    *out_confirmed = 0;
    *out_terminate = 0;

    status = oi_cli_composer_select(composer, signal_fd, NULL, 0, options, 2,
                                    0, &selected, &cancelled, out_terminate);
    if (status != OI_OK || *out_terminate != 0) {
        return status;
    }
    status = oi_cli_composer_redraw(composer);
    if (status != OI_OK) {
        return status;
    }
    *out_confirmed = !cancelled && selected == 1;
    return OI_OK;
}

static oi_status repl_confirmation_text(
    const char *prefix, const char *target, size_t target_len,
    const char *suffix, char **out_text) {
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    char *text;

    if (prefix_len > SIZE_MAX - suffix_len - 1 ||
        target_len > SIZE_MAX - prefix_len - suffix_len - 1) {
        return OI_ERR_NOMEM;
    }
    text = malloc(prefix_len + target_len + suffix_len + 1);
    if (text == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(text, prefix, prefix_len);
    memcpy(text + prefix_len, target, target_len);
    memcpy(text + prefix_len + target_len, suffix, suffix_len);
    text[prefix_len + target_len + suffix_len] = '\0';
    *out_text = text;
    return OI_OK;
}

static oi_status repl_print_replay(FILE *out,
                                   const struct oi_cli_message_list *context) {
    size_t index;

    for (index = 0; index < context->len; index++) {
        const struct oi_cli_message *message = &context->items[index];
        const char *role;

        switch (message->role) {
        case OI_CLI_MESSAGE_USER:
            role = "you";
            break;
        case OI_CLI_MESSAGE_ASSISTANT:
            role = "oi";
            break;
        case OI_CLI_MESSAGE_TOOL:
            role = "tool";
            break;
        default:
            role = "?";
            break;
        }
        if (message->content.len > 0) {
            if (message->role == OI_CLI_MESSAGE_TOOL) {
                if (fprintf(out, "%s [%.*s]: %.*s\n", role,
                            (int)message->tool_call_id.len,
                            message->tool_call_id.data,
                            (int)message->content.len,
                            message->content.data) < 0) {
                    return OI_ERR_IO;
                }
            } else if (fprintf(out, "%s: %.*s\n", role,
                               (int)message->content.len,
                               message->content.data) < 0) {
                return OI_ERR_IO;
            }
        }
        for (size_t call_index = 0;
             call_index < message->tool_calls_len; call_index++) {
            const struct oi_cli_tool_call_value *call =
                &message->tool_calls[call_index];

            if (fprintf(out,
                        "%s tool call:\n"
                        "  id: %.*s\n"
                        "  name: %.*s\n"
                        "  arguments: %.*s\n",
                        role, (int)call->id.len, call->id.data,
                        (int)call->name.len, call->name.data,
                        (int)call->arguments.len,
                        call->arguments.data) < 0) {
                return OI_ERR_IO;
            }
        }
    }
    return fflush(out) != 0 ? OI_ERR_IO : OI_OK;
}

static const char *repl_switch_outcome_text(
    enum oi_cli_session_switch_outcome outcome) {
    switch (outcome) {
    case OI_CLI_SESSION_SWITCH_SAME:
        return "already on that session";
    case OI_CLI_SESSION_SWITCH_NOT_FOUND:
        return "no such session";
    case OI_CLI_SESSION_SWITCH_BUSY:
        return "session is open in another process";
    case OI_CLI_SESSION_SWITCH_INVALID:
        return "invalid session id";
    case OI_CLI_SESSION_SWITCH_CORRUPT:
        return "session could not be loaded";
    case OI_CLI_SESSION_SWITCH_OK:
    default:
        return "switched";
    }
}

static oi_status repl_handle_session_switch(
    const struct oi_cli_repl_config *config,
    struct oi_cli_composer *composer, int signal_fd, oi_llm_client *client,
    oi_reactor *reactor, oi_tool_registry *tools,
    struct oi_cli_conversation_config *conversation_config,
    struct oi_cli_string *current_model,
    struct oi_cli_input_history *input_history, const char *rest,
    size_t rest_len, oi_cli_conversation **conversation,
    oi_arena **active_arena, struct oi_cli_message_list *switched_context,
    int *has_switched_context,
    struct repl_session_switch_outcome *out_outcome) {
    struct oi_cli_session_switch_result result;
    struct oi_cli_message_list rollback_context;
    const char *id;
    const char *extra;
    size_t id_len;
    size_t extra_len;
    int had_conversation;
    oi_status status;

    out_outcome->terminate = 0;
    out_outcome->switched = 0;
    out_outcome->model_origin = OI_CLI_SESSION_MODEL_UNKNOWN;
    repl_split_first_token(rest, rest_len, &id, &id_len, &extra, &extra_len);
    if (id_len == 0 || extra_len != 0) {
        return fputs("oi: usage: /session switch ID\n", config->err) == EOF ||
                       fflush(config->err) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (config->switch_session == NULL) {
        return fputs("oi: /session switch is not available in this context\n",
                     config->err) == EOF ||
                       fflush(config->err) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }

    had_conversation = *conversation != NULL;
    oi_cli_message_list_init(&rollback_context);
    if (had_conversation) {
        const struct oi_cli_message_list *live =
            oi_cli_conversation_messages(*conversation);
        size_t index;

        for (index = 0; live != NULL && index < live->len; index++) {
            status = oi_cli_message_list_append_clone(&rollback_context,
                                                      &live->items[index]);
            if (status != OI_OK) {
                oi_cli_message_list_free(&rollback_context);
                return status;
            }
        }
        oi_cli_conversation_destroy(*conversation);
        *conversation = NULL;
    }

    oi_cli_session_switch_result_init(&result);
    status = config->switch_session(config->switch_session_user_data, id,
                                    id_len, &result);
    if (status != OI_OK) {
        oi_cli_session_switch_result_free(&result);
        oi_cli_message_list_free(&rollback_context);
        return status;
    }

    if (result.outcome != OI_CLI_SESSION_SWITCH_OK) {
        oi_status message_status = OI_OK;
        if (fprintf(config->err, "oi: %.*s: %s\n", (int)id_len, id,
                    repl_switch_outcome_text(result.outcome)) < 0 ||
            fflush(config->err) != 0) {
            message_status = OI_ERR_IO;
        }
        oi_cli_session_switch_result_free(&result);
        status = OI_OK;
        if (had_conversation) {
            conversation_config->model = current_model->data;
            status = oi_cli_conversation_create(
                client, reactor, *active_arena, tools, conversation_config,
                &rollback_context, conversation);
        }
        oi_cli_message_list_free(&rollback_context);
        return status != OI_OK ? status : message_status;
    }

    oi_cli_message_list_free(&rollback_context);
    size_t damaged = oi_sesslog_damaged_bytes(oi_session_log(result.session));
    if (damaged > 0) {
        (void)fprintf(config->err,
                      "oi: %.*s: session log was damaged: removed %zu bytes; "
                      "the original file is kept next to it with a .corrupt "
                      "suffix\n",
                      (int)id_len, id, damaged);
    }
    *active_arena = oi_session_arena(result.session);
    status = oi_cli_string_set(current_model, result.model.data,
                               result.model.len);
    if (status == OI_OK) {
        out_outcome->switched = 1;
        out_outcome->model_origin = result.model_origin;
        oi_cli_message_list_free(switched_context);
        *switched_context = result.initial_context;
        result.initial_context = (struct oi_cli_message_list){0};
        *has_switched_context = 1;
        oi_cli_input_history_free(input_history);
        oi_cli_input_history_init(input_history);
        status = seed_input_history(input_history, switched_context);
    }
    if (status == OI_OK) {
        conversation_config->model = current_model->data;
        status = oi_cli_conversation_create(client, reactor, *active_arena,
                                            tools, conversation_config,
                                            switched_context, conversation);
    }
    if (status != OI_OK) {
        oi_cli_session_switch_result_free(&result);
        return status;
    }
    if (fprintf(config->out, "oi: switched to session %.*s\n", (int)id_len,
                id) < 0 ||
        fflush(config->out) != 0) {
        oi_cli_session_switch_result_free(&result);
        return OI_ERR_IO;
    }

    if (switched_context->len > 0) {
        int confirmed = 0;
        int terminate = 0;

        status = repl_confirm(composer, signal_fd, "Continue without replay",
                              "Show the restored conversation", &confirmed,
                              &terminate);
        if (status != OI_OK || terminate) {
            out_outcome->terminate = terminate;
            oi_cli_session_switch_result_free(&result);
            return status;
        }
        if (confirmed) {
            status = repl_print_replay(config->out, switched_context);
            if (status != OI_OK) {
                oi_cli_session_switch_result_free(&result);
                return status;
            }
        }
    }
    oi_cli_session_switch_result_free(&result);
    return OI_OK;
}

static oi_status repl_handle_session_delete(
    const struct oi_cli_repl_config *config,
    struct oi_cli_composer *composer, int signal_fd, const char *rest,
    size_t rest_len) {
    const char *id;
    const char *extra;
    size_t id_len;
    size_t extra_len;
    char *detail = NULL;
    char *confirmation = NULL;
    int confirmed = 0;
    int terminate = 0;
    oi_status status;

    repl_split_first_token(rest, rest_len, &id, &id_len, &extra, &extra_len);
    if (id_len == 0 || extra_len != 0) {
        return fputs("oi: usage: /session delete ID\n", config->err) == EOF ||
                       fflush(config->err) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (config->delete_session == NULL) {
        return fputs("oi: /session delete is not available in this context\n",
                     config->err) == EOF ||
                       fflush(config->err) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    status = repl_confirmation_text(
        "Permanently delete session ", id, id_len, " (cannot be undone)",
        &confirmation);
    if (status == OI_OK) {
        status = repl_confirm(composer, signal_fd, "Cancel", confirmation,
                              &confirmed, &terminate);
    }
    free(confirmation);
    if (status != OI_OK || terminate) {
        return status;
    }
    if (!confirmed) {
        return fputs("oi: deletion cancelled\n", config->err) == EOF ||
                       fflush(config->err) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    status = config->delete_session(config->delete_session_user_data, id,
                                    id_len, &detail);
    if (status == OI_OK) {
        if (fprintf(config->out, "oi: deleted session %.*s\n", (int)id_len,
                    id) < 0 ||
            fflush(config->out) != 0) {
            free(detail);
            return OI_ERR_IO;
        }
    } else if (fprintf(config->err, "oi: %.*s: %s\n", (int)id_len, id,
                       detail != NULL ? detail : "could not delete") < 0 ||
               fflush(config->err) != 0) {
        free(detail);
        return OI_ERR_IO;
    }
    free(detail);
    return OI_OK;
}

static oi_status repl_handle_session_import(
    const struct oi_cli_repl_config *config,
    struct oi_cli_composer *composer, int signal_fd, const char *rest,
    size_t rest_len) {
    char *new_id = NULL;
    char *detail = NULL;
    char *confirmation = NULL;
    int confirmed = 0;
    int terminate = 0;
    oi_status status;

    if (rest_len == 0) {
        return fputs("oi: usage: /session import PATH\n", config->err) ==
                           EOF ||
                       fflush(config->err) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (config->import_session == NULL) {
        return fputs("oi: /session import is not available in this context\n",
                     config->err) == EOF ||
                       fflush(config->err) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    status = repl_confirmation_text("Copy '", rest, rest_len,
                                    "' into a new session", &confirmation);
    if (status == OI_OK) {
        status = repl_confirm(composer, signal_fd, "Cancel", confirmation,
                              &confirmed, &terminate);
    }
    free(confirmation);
    if (status != OI_OK || terminate) {
        return status;
    }
    if (!confirmed) {
        return fputs("oi: import cancelled\n", config->err) == EOF ||
                       fflush(config->err) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    status = config->import_session(config->import_session_user_data, rest,
                                    rest_len, &new_id, &detail);
    if (status == OI_OK) {
        if (fprintf(config->out,
                    "oi: imported as session %s (use /session switch %s to "
                    "open it)\n",
                    new_id, new_id) < 0 ||
            fflush(config->out) != 0) {
            free(new_id);
            free(detail);
            return OI_ERR_IO;
        }
    } else if (fprintf(config->err, "oi: import failed: %s\n",
                       detail != NULL ? detail : "could not import") < 0 ||
               fflush(config->err) != 0) {
        free(new_id);
        free(detail);
        return OI_ERR_IO;
    }
    free(new_id);
    free(detail);
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
    oi_arena *active_arena = arena;
    const struct oi_cli_message_list *active_context = config->initial_context;
    struct oi_cli_message_list switched_context;
    int has_switched_context = 0;
    struct oi_cli_string current_model_storage = {0};
    struct oi_cli_string *current_model;
    struct repl_setting_context setting_context;
    struct repl_status_context status_context = {0};
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
    oi_cli_message_list_init(&switched_context);

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
        &present, config->out, config->err, 0, 1,
        config->on_event, config->event_user_data);
    if (status != OI_OK) {
        goto cleanup_history;
    }

    status = oi_cli_composer_init(&composer, config->input_fd,
                                  config->output_fd, &input_history);
    if (status != OI_OK) {
        goto cleanup_present;
    }
    composer_initialized = 1;
    composer.reactor = reactor;
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
        if (sigprocmask(SIG_BLOCK, &signal_mask, NULL) == 0) {
            signal_fd =
                signalfd(-1, &signal_mask, SFD_NONBLOCK | SFD_CLOEXEC);
        }
    }

    setting_context.config = config;
    setting_context.current_model = current_model;
    setting_context.conversation = &conversation;
    setting_context.model_origin = config->model_origin;
    status_context.config = config;
    status_context.setting = &setting_context;
    status_context.conversation = &conversation;
    status_context.pending = &pending;

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
            if (parsed.command->id == OI_CLI_COMMAND_LOGIN) {
                int login_terminate_signal = 0;

                status = oi_cli_login_run(config->login, &composer, signal_fd,
                                          &login_terminate_signal);
                free(prompt);
                if (status != OI_OK || login_terminate_signal != 0) {
                    break;
                }
                if (config->login->selected_model != NULL) {
                    status = dispatch_set_model(
                        &setting_context, config->login->selected_model,
                        strlen(config->login->selected_model));
                    free(config->login->selected_model);
                    config->login->selected_model = NULL;
                    if (status != OI_OK) {
                        break;
                    }
                }
                continue;
            }
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
                status_context.checkpoints_applied++;
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
            if (parsed.command->id == OI_CLI_COMMAND_SESSION &&
                parsed.arguments_len > 0) {
                const char *subcommand = NULL;
                const char *rest = NULL;
                size_t subcommand_len = 0;
                size_t rest_len = 0;
                int handled = 0;

                repl_split_first_token(parsed.arguments, parsed.arguments_len,
                                       &subcommand, &subcommand_len, &rest,
                                       &rest_len);

                if (repl_token_equals(subcommand, subcommand_len, "switch")) {
                    struct repl_session_switch_outcome outcome;

                    handled = 1;
                    status = repl_handle_session_switch(
                        config, &composer, signal_fd, client, reactor, tools,
                        &conversation_config, current_model, &input_history,
                        rest, rest_len, &conversation, &active_arena,
                        &switched_context, &has_switched_context, &outcome);
                    if (status == OI_OK && outcome.switched) {
                        status_context.checkpoints_applied = 0;
                        setting_context.model_origin = outcome.model_origin;
                    }
                    if (status == OI_OK && outcome.terminate) {
                        free(prompt);
                        break;
                    }
                    if (status == OI_OK && has_switched_context) {
                        active_context = &switched_context;
                    }
                } else if (repl_token_equals(subcommand, subcommand_len,
                                             "delete")) {
                    handled = 1;
                    status = repl_handle_session_delete(config, &composer,
                                                        signal_fd, rest,
                                                        rest_len);
                } else if (repl_token_equals(subcommand, subcommand_len,
                                             "import")) {
                    handled = 1;
                    status = repl_handle_session_import(config, &composer,
                                                        signal_fd, rest,
                                                        rest_len);
                }
                if (handled) {
                    free(prompt);
                    if (status != OI_OK) {
                        break;
                    }
                    continue;
                }
            }
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
                .session = config->session_ops,
                .status = repl_status_snapshot,
                .status_user_data = &status_context,
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
            oi_arena *conversation_arena = active_arena;

            if (config->prepare != NULL && !has_switched_context) {
                status = config->prepare(config->prepare_user_data,
                                         &conversation_arena);
            }
            if (status == OI_OK && conversation_arena == NULL) {
                status = OI_ERR_INVAL;
            }
            if (status == OI_OK) {
                conversation_config.model = current_model->data;
                status = oi_cli_conversation_create(
                    client, reactor, conversation_arena, tools,
                    &conversation_config, active_context, &conversation);
            }
            if (status != OI_OK) {
                free(prompt);
                break;
            }
            active_arena = conversation_arena;
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
                .status_context = &status_context,
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
                status = OI_OK;
                break;
            }
            if (turn_input_context.status == OI_OK &&
                oi_cli_conversation_was_cancelled(conversation)) {
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
    oi_cli_message_list_free(&switched_context);
    oi_cli_string_free(&current_model_storage);
    return status;
}
