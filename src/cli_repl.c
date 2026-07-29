/* realpath(3) is a BSD/SVID extension, and struct signalfd_siginfo /
 * signalfd(2) are GNU/Linux extensions; _GNU_SOURCE is a superset of
 * _DEFAULT_SOURCE (previously defined here just for realpath) and covers
 * both, matching the same precedent in reactor_epoll.c for other GNU fd
 * primitives (timerfd, pidfd). */
#define _GNU_SOURCE

#include "cli_repl.h"

#include "cli_command_dispatch.h"
#include "cli_commands.h"
#include "cli_input_history.h"
#include "cli_present.h"
#include "cli_prompt.h"

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

struct repl_turn_signal_context {
    oi_cli_conversation *conversation;
    /* 0, or the signal (SIGTERM/SIGHUP) that requested the whole REPL
     * terminate once this turn finishes unwinding. */
    int terminate_signal;
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
            /* No editor frame is on screen during a turn; the width is
             * re-read fresh at the start of the next oi_cli_prompt_read
             * regardless (matches issue #23's own reasoning), so there is
             * nothing to do here. */
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
    struct oi_cli_conversation_config conversation_config;
    oi_cli_conversation *conversation = NULL;
    struct oi_cli_string current_model_storage = {0};
    struct oi_cli_string *current_model;
    struct repl_setting_context setting_context;
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

    {
        sigset_t signal_mask;

        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, SIGWINCH);
        sigaddset(&signal_mask, SIGINT);
        sigaddset(&signal_mask, SIGTERM);
        sigaddset(&signal_mask, SIGHUP);
        /* Graceful degradation, never a hard failure: if blocking the
         * signals or creating the signalfd fails for any reason, signal_fd
         * stays -1, which oi_cli_prompt_read already treats as "signal
         * handling unavailable" -- the REPL still works exactly as it does
         * without this feature (just without live resize, and without
         * Ctrl+C/SIGTERM/SIGHUP being caught cleanly). */
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

        status = oi_cli_prompt_read(
            config->input_fd, config->output_fd, signal_fd, &input_history,
            &prompt, &prompt_len, &exit_requested, &terminate_signal);
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
            int signal_registered =
                signal_fd >= 0 &&
                oi_reactor_add(reactor, signal_fd, OI_EV_READ,
                               handle_turn_signal,
                               &turn_signal_context) == OI_OK;

            while (status == OI_OK && !present.done) {
                oi_status step_status;
                if (oi_reactor_step(reactor, -1, &step_status) < 0) {
                    status = step_status;
                }
            }
            if (signal_registered) {
                oi_reactor_remove(reactor, signal_fd);
            }
            if (status == OI_OK) {
                status = present.status;
            }
            if (turn_signal_context.terminate_signal != 0) {
                /* SIGTERM/SIGHUP: the conversation was already cancelled
                 * above; clean up and exit regardless of anything else. */
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
    oi_cli_present_free(&present);
cleanup_history:
    oi_cli_input_history_free(&input_history);
    oi_cli_string_free(&current_model_storage);
    return status;
}
