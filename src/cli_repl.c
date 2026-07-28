#include "cli_repl.h"

#include "cli_input_history.h"
#include "cli_present.h"
#include "cli_prompt.h"

#include <stdlib.h>

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

oi_status oi_cli_repl_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_repl_config *config) {
    struct oi_cli_input_history input_history;
    struct oi_cli_present present;
    struct oi_cli_conversation_config conversation_config;
    oi_cli_conversation *conversation = NULL;
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

    oi_cli_input_history_init(&input_history);
    status = seed_input_history(&input_history, config->initial_context);
    if (status != OI_OK) {
        goto cleanup_history;
    }
    status = oi_cli_present_init(
        &present, config->out, config->err, 0, config->on_event,
        config->event_user_data);
    if (status != OI_OK) {
        goto cleanup_history;
    }

    conversation_config.model = config->model;
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

        status = oi_cli_prompt_read(
            config->input_fd, config->output_fd, &input_history, &prompt,
            &prompt_len, &exit_requested);
        if (status != OI_OK || exit_requested) {
            free(prompt);
            break;
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
        while (status == OI_OK && !present.done) {
            oi_status step_status;
            if (oi_reactor_step(reactor, -1, &step_status) < 0) {
                status = step_status;
            }
        }
        if (status == OI_OK) {
            status = present.status;
        }
        if (status != OI_OK) {
            if (oi_cli_conversation_is_busy(conversation)) {
                oi_cli_conversation_cancel(conversation);
            }
            break;
        }
    }

    oi_cli_conversation_destroy(conversation);
    oi_cli_present_free(&present);
cleanup_history:
    oi_cli_input_history_free(&input_history);
    return status;
}
