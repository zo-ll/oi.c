#include "cli_loop.h"

#include <stdlib.h>
#include <string.h>

#include "cli_conversation.h"
#include "cli_present.h"

oi_status oi_cli_loop_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_loop_config *config,
                          const char *prompt,
                          struct oi_cli_loop_result *out_result) {
    if (client == NULL || reactor == NULL || arena == NULL || tools == NULL ||
        config == NULL || config->model == NULL || config->max_turns <= 0 ||
        config->tool_timeout_ms < 0 || config->permission == NULL ||
        config->out == NULL || config->err == NULL || prompt == NULL ||
        out_result == NULL) {
        return OI_ERR_INVAL;
    }
    memset(out_result, 0, sizeof *out_result);
    struct oi_cli_present present;
    oi_status st = oi_cli_present_init(
        &present, config->out, config->err, 1, config->on_event,
        config->event_user_data);
    if (st != OI_OK) {
        return st;
    }
    struct oi_cli_conversation_config conversation_config = {
        .model = config->model,
        .max_model_steps = config->max_turns,
        .tool_timeout_ms = config->tool_timeout_ms,
        .permission = oi_cli_tool_permission,
        .permission_user_data = config->permission,
        .on_event = oi_cli_present_event,
        .event_user_data = &present,
    };
    oi_cli_conversation *conversation = NULL;
    st = oi_cli_conversation_create(
        client, reactor, arena, tools, &conversation_config,
        config->initial_context,
        &conversation);
    if (st == OI_OK) {
        st = oi_cli_conversation_start(conversation, prompt, strlen(prompt));
    }
    while (st == OI_OK && !present.done) {
        oi_status step_status;
        if (oi_reactor_step(reactor, -1, &step_status) < 0) {
            st = step_status;
        }
    }
    if (st != OI_OK && conversation != NULL &&
        oi_cli_conversation_is_busy(conversation)) {
        oi_cli_conversation_cancel(conversation);
    }
    if (st == OI_OK) {
        st = present.status;
    }
    if (st != OI_OK) {
        if (st == OI_ERR_DENIED) {
            fprintf(config->err, "oi: tool permission denied\n");
        }
        if (st == OI_ERR_INVAL) {
            fprintf(config->err, "oi: model tool loop exceeded %d turns\n",
                    config->max_turns);
        }
        fprintf(config->err, "oi: model/tool loop failed (status=%d)\n",
                (int)st);
    }
    if (st == OI_OK) {
        st = oi_cli_present_take_assistant(
            &present, &out_result->assistant_text,
            &out_result->assistant_text_len);
    }
    oi_cli_present_free(&present);
    oi_cli_conversation_destroy(conversation);
    return st;
}
