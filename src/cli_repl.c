/* realpath(3) is a BSD/SVID extension, and struct signalfd_siginfo /
 * signalfd(2) are GNU/Linux extensions; _GNU_SOURCE is a superset of
 * _DEFAULT_SOURCE (previously defined here just for realpath) and covers
 * both, matching the same precedent in reactor_epoll.c for other GNU fd
 * primitives (timerfd, pidfd). */
#define _GNU_SOURCE

#include "cli_repl.h"

#include "cli_command_dispatch.h"
#include "cli_commands.h"
#include "cli_composer.h"
#include "cli_input_history.h"
#include "cli_present.h"

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
};

static void handle_turn_escape_timeout(oi_reactor *reactor, void *user_data) {
    struct repl_turn_input_context *context = user_data;
    oi_reactor_timer *timer = context->escape_timer;
    enum oi_cli_composer_action action;
    oi_status status;

    (void)reactor;
    context->escape_timer = NULL;
    context->dirty = 1;
    oi_reactor_timer_cancel(timer);
    status = oi_cli_composer_resolve_escape(context->composer, &action);
    if (status != OI_OK) {
        if (context->status == OI_OK) {
            context->status = status;
        }
        return;
    }
    if (action == OI_CLI_COMPOSER_ACTION_CTRL_C) {
        oi_cli_conversation_cancel(context->conversation);
    }
}

static const char busy_refused_text[] =
    "oi: a message is already queued; it will run once the current turn "
    "reaches a safe point\n";
static const char busy_queued_text[] =
    "oi: queued -- will run once the current turn reaches a safe point\n";

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
        report_busy_io_error(
            context, fputs(busy_queued_text, context->config->err) != EOF &&
                          fflush(context->config->err) == 0);
    }
}

static void handle_turn_input(oi_reactor *reactor, int fd, int revents,
                              void *user_data) {
    struct repl_turn_input_context *context = user_data;
    enum oi_cli_composer_action action;
    oi_status status;

    (void)fd;
    (void)revents;
    context->dirty = 1;
    if (context->escape_timer != NULL) {
        oi_reactor_timer_cancel(context->escape_timer);
        context->escape_timer = NULL;
    }
    status = oi_cli_composer_feed(context->composer, &action);
    if (status != OI_OK) {
        if (context->status == OI_OK) {
            context->status = status;
        }
        return;
    }
    if (action == OI_CLI_COMPOSER_ACTION_CTRL_C) {
        oi_cli_conversation_cancel(context->conversation);
    } else if (action == OI_CLI_COMPOSER_ACTION_SUBMIT) {
        handle_busy_submit(context);
    }
    /* EXIT (Ctrl+D at an empty draft) while busy is still a documented
     * no-op: not explicitly required by the issue, and there is no
     * pressing need to synthesize an "/exit" queue entry for it ahead of
     * an explicitly typed /exit, which already goes through the one-slot
     * rule above like any other command. */
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
    conversation_config.on_event = oi_cli_present_event;
    conversation_config.event_user_data = &present;

    for (;;) {
        char *prompt = NULL;
        size_t prompt_len = 0;
        int exit_requested = 0;
        int terminate_signal = 0;
        struct oi_cli_command_parse parsed;

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
            int signal_registered =
                signal_fd >= 0 &&
                oi_reactor_add(reactor, signal_fd, OI_EV_READ,
                               handle_turn_signal,
                               &turn_signal_context) == OI_OK;
            int input_registered =
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
                if (status == OI_OK && !present.done &&
                    (turn_input_context.dirty ||
                     turn_signal_context.resize_pending)) {
                    oi_status redraw_status =
                        oi_cli_composer_redraw(&composer);
                    if (redraw_status != OI_OK) {
                        status = redraw_status;
                    }
                    turn_input_context.dirty = 0;
                    turn_signal_context.resize_pending = 0;
                }
            }
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
            if (oi_cli_conversation_was_cancelled(conversation)) {
                /* Ctrl+C during this turn: always recoverable, regardless
                 * of whatever oi_status got attached to it. */
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
