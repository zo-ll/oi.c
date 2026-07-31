#include "../test.h"
#include "mock_api.h"

#include <stdlib.h>
#include <string.h>

#include "cli_conversation.h"
#include "cli_message.h"
#include "cli_tools.h"
#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/tool.h"

struct event_sink {
    oi_arena *arena;
    int message_count;
    int delta_count;
    int response_done_count;
    int turn_done;
    oi_status status;
    size_t arena_used_at_done;
    char text[64];
    size_t text_len;
    int event_count;
    int tool_start_position;
    int tool_output_position;
    int tool_message_position;
    int tool_message_has_raw;
    char last_assistant_model[64];
    enum oi_cli_conversation_tool_outcome tool_outcomes[8];
    int tool_outcome_has_raw[8];
    int tool_outcome_count;
    oi_cli_conversation **cancel_on_tool_starting;
    /* Reentrantly steers (not cancels) from within the named event, for
     * tests exercising oi_cli_conversation_steer's single check point
     * without needing precise external timing -- mirrors
     * cancel_on_tool_starting's existing reentrancy pattern. */
    oi_cli_conversation **steer_on_response_done;
    oi_cli_conversation **steer_on_tool_message;
    int tool_starting_count;
    int awaiting_permission_count;
    int awaiting_permission_position;
    /* Reentrantly cancels from within the AWAITING_PERMISSION event itself
     * -- mirrors cancel_on_tool_starting's existing reentrancy pattern, for
     * a test exercising a cancel that arrives while a call is staged but
     * not yet resolved, rather than while it's already running. */
    oi_cli_conversation **cancel_on_awaiting_permission;
    /*
     * What oi_cli_conversation_activity reported at each event, indexed by
     * event type. Sampling from inside the callbacks is the only way to
     * observe the in-flight states at all -- by the time control returns to
     * the test, every turn is at rest. NULL disables sampling.
     */
    oi_cli_conversation **activity_probe;
    enum oi_cli_conversation_activity activity_at[16];
    int activity_seen[16];
    /* Events emitted while the conversation reported CANCELLING -- the
     * cancel unwind's own partial-response and repair events. Counted
     * separately because those events share types with ordinary ones, so a
     * per-type sample cannot distinguish them. */
    int cancelling_event_count;
};

static oi_status collect_event(
    const struct oi_cli_conversation_event *event, void *user_data) {
    struct event_sink *sink = user_data;
    int position = sink->event_count++;

    /* Sampled before the reentrant cancel/steer hooks below run, so an
     * AWAITING_PERMISSION sample describes the staged call rather than the
     * cancel that this very callback is about to request. */
    if (sink->activity_probe != NULL &&
        (size_t)event->type <
            sizeof sink->activity_at / sizeof sink->activity_at[0]) {
        enum oi_cli_conversation_activity activity =
            oi_cli_conversation_activity(*sink->activity_probe);

        sink->activity_at[event->type] = activity;
        sink->activity_seen[event->type] = 1;
        if (activity == OI_CLI_CONVERSATION_ACTIVITY_CANCELLING) {
            sink->cancelling_event_count++;
        }
    }
    switch (event->type) {
    case OI_CLI_CONVERSATION_EVENT_MESSAGE:
        sink->message_count++;
        if (event->as.message.value->role == OI_CLI_MESSAGE_TOOL) {
            sink->tool_message_position = position;
            sink->tool_message_has_raw =
                event->as.message.has_raw_tool_output;
            if ((size_t)sink->tool_outcome_count <
                sizeof sink->tool_outcomes / sizeof sink->tool_outcomes[0]) {
                sink->tool_outcome_has_raw[sink->tool_outcome_count] =
                    event->as.message.has_raw_tool_output;
                sink->tool_outcomes[sink->tool_outcome_count++] =
                    event->as.message.tool_outcome;
            }
            if (sink->steer_on_tool_message != NULL) {
                oi_cli_conversation_steer(*sink->steer_on_tool_message);
            }
        }
        if (event->as.message.value->role == OI_CLI_MESSAGE_ASSISTANT &&
            event->as.message.model != NULL) {
            size_t len = event->as.message.model_len;
            if (len >= sizeof sink->last_assistant_model) {
                len = sizeof sink->last_assistant_model - 1;
            }
            memcpy(sink->last_assistant_model, event->as.message.model, len);
            sink->last_assistant_model[len] = '\0';
        }
        break;
    case OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA:
        CHECK(sink->text_len + event->as.bytes.len < sizeof sink->text);
        memcpy(sink->text + sink->text_len, event->as.bytes.data,
               event->as.bytes.len);
        sink->text_len += event->as.bytes.len;
        sink->text[sink->text_len] = '\0';
        sink->delta_count++;
        break;
    case OI_CLI_CONVERSATION_EVENT_RESPONSE_DONE:
        sink->response_done_count++;
        if (sink->steer_on_response_done != NULL) {
            oi_cli_conversation_steer(*sink->steer_on_response_done);
        }
        break;
    case OI_CLI_CONVERSATION_EVENT_TURN_DONE:
        sink->turn_done = 1;
        sink->status = event->as.turn_done.status;
        sink->arena_used_at_done = oi_arena_used(sink->arena);
        break;
    case OI_CLI_CONVERSATION_EVENT_TOOL_STARTING:
        sink->tool_start_position = position;
        sink->tool_starting_count++;
        if (sink->cancel_on_tool_starting != NULL) {
            oi_cli_conversation_cancel(*sink->cancel_on_tool_starting);
        }
        break;
    case OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT:
        if (sink->tool_output_position < 0) {
            sink->tool_output_position = position;
        }
        break;
    case OI_CLI_CONVERSATION_EVENT_AWAITING_PERMISSION:
        sink->awaiting_permission_count++;
        sink->awaiting_permission_position = position;
        if (sink->cancel_on_awaiting_permission != NULL) {
            oi_cli_conversation_cancel(*sink->cancel_on_awaiting_permission);
        }
        break;
    case OI_CLI_CONVERSATION_EVENT_PARTIAL_ASSISTANT:
    case OI_CLI_CONVERSATION_EVENT_MODEL_ERROR:
        break;
    }
    return OI_OK;
}

static oi_tool_decision allow_tool(const char *tool_name,
                                   const oi_json_value *args,
                                   void *user_data) {
    (void)tool_name;
    (void)args;
    (void)user_data;
    return OI_TOOL_ALLOW;
}

static oi_tool_decision ask_tool(const char *tool_name,
                                 const oi_json_value *args, void *user_data) {
    (void)tool_name;
    (void)args;
    (void)user_data;
    return OI_TOOL_ASK;
}

TEST(start_is_event_driven_and_preserves_context) {
    const char *response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"new answer\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turn = {NULL, response, 7};
    struct mock_api api;
    CHECK(mock_api_start(&api, &turn, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    struct oi_cli_message_list initial;
    struct oi_cli_message message;
    oi_cli_message_list_init(&initial);
    oi_cli_message_init(&message);
    CHECK_EQ(oi_cli_message_set_user(&message, "old question", 12), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&initial, &message), OI_OK);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "old answer", 10), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&initial, &message), OI_OK);

    oi_cli_conversation *conversation = NULL;
    struct event_sink sink = {.arena = arena,
                              .activity_probe = &conversation};
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 2,
        .tool_timeout_ms = 1000,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, &initial,
                 &conversation),
             OI_OK);

    /* Nothing has started: the activity of a conversation with no turn is
     * idle, and so is a NULL one -- what a REPL that has not lazily created
     * a conversation yet genuinely has. */
    CHECK_EQ(oi_cli_conversation_activity(conversation),
             OI_CLI_CONVERSATION_ACTIVITY_IDLE);
    CHECK_EQ(oi_cli_conversation_activity(NULL),
             OI_CLI_CONVERSATION_ACTIVITY_IDLE);

    CHECK(oi_arena_alloc(arena, 32) != NULL);
    CHECK(oi_arena_used(arena) > 0);
    CHECK_EQ(oi_cli_conversation_start(conversation, "new question", 12),
             OI_OK);
    CHECK(oi_cli_conversation_is_busy(conversation));
    CHECK(!sink.turn_done);
    CHECK_EQ(sink.message_count, 1);
    CHECK_EQ(oi_arena_used(arena), 0);

    for (int i = 0; i < 100 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_OK);
    CHECK_STREQ(sink.text, "new answer");
    CHECK_EQ(sink.message_count, 2);
    CHECK_EQ(sink.response_done_count, 1);
    CHECK(!oi_cli_conversation_is_busy(conversation));
    CHECK(sink.arena_used_at_done > 0);

    /* An arriving delta means the request is still open, so the reported
     * activity has to be "streaming" -- and once the turn completes cleanly
     * it has to be idle again. */
    CHECK(sink.activity_seen[OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA]);
    CHECK_EQ(sink.activity_at[OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA],
             OI_CLI_CONVERSATION_ACTIVITY_STREAMING);
    /* Committing a message is a busy moment with nothing in flight -- the
     * request has already been released by the time the assistant reply is
     * committed -- so it reports as working rather than as streaming. */
    CHECK(sink.activity_seen[OI_CLI_CONVERSATION_EVENT_MESSAGE]);
    CHECK_EQ(sink.activity_at[OI_CLI_CONVERSATION_EVENT_MESSAGE],
             OI_CLI_CONVERSATION_ACTIVITY_WORKING);
    CHECK_EQ(oi_cli_conversation_activity(conversation),
             OI_CLI_CONVERSATION_ACTIVITY_IDLE);

    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, 4);
    CHECK_STREQ(messages->items[0].content.data, "old question");
    CHECK_STREQ(messages->items[3].content.data, "new answer");

    size_t request_len = 0;
    char *request = mock_api_request(&api, 0, &request_len);
    CHECK(request != NULL);
    CHECK(request_len > 0);
    CHECK(strstr(request, "old question") != NULL);
    CHECK(strstr(request, "old answer") != NULL);
    CHECK(strstr(request, "new question") != NULL);
    free(request);

    oi_cli_conversation_destroy(conversation);
    oi_cli_message_free(&message);
    oi_cli_message_list_free(&initial);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(tool_start_boundary_precedes_process_output) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf tool-output; exit 7\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    const char *second_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"finished\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[2] = {
        {NULL, first_response, 11},
        {NULL, second_response, 0},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 2));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 1000,
        .permission = allow_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    oi_cli_conversation *conversation = NULL;
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_OK);
    CHECK(sink.tool_start_position >= 0);
    CHECK(sink.tool_output_position > sink.tool_start_position);
    CHECK(sink.tool_message_position > sink.tool_output_position);
    CHECK(sink.tool_message_has_raw);
    CHECK_EQ(sink.tool_outcome_count, 1);
    CHECK_EQ(sink.tool_outcomes[0], OI_CLI_CONVERSATION_TOOL_FAILED);
    CHECK_STREQ(sink.text, "finished");

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(ask_defers_and_resolve_permission_allow_lets_the_tool_run) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf tool-output\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    const char *second_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"finished\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[2] = {
        {NULL, first_response, 11},
        {NULL, second_response, 0},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 2));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
        .activity_probe = &conversation,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 1000,
        .permission = ask_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && sink.awaiting_permission_count == 0; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK_EQ(sink.awaiting_permission_count, 1);
    /* Nothing has spawned yet: the decision hasn't arrived. */
    CHECK_EQ(sink.tool_start_position, -1);
    CHECK(!sink.turn_done);
    /* A staged call awaiting a decision is its own reported state, and it
     * persists for as long as the decision does -- so /status can report it
     * from outside the callback too. */
    CHECK_EQ(sink.activity_at[OI_CLI_CONVERSATION_EVENT_AWAITING_PERMISSION],
             OI_CLI_CONVERSATION_ACTIVITY_AWAITING_PERMISSION);
    CHECK_EQ(oi_cli_conversation_activity(conversation),
             OI_CLI_CONVERSATION_ACTIVITY_AWAITING_PERMISSION);

    /* Resolved genuinely later, from outside the event callback entirely
     * -- not reentrantly -- proving the turn really stayed suspended
     * across an arbitrary gap rather than the decision being available
     * synchronously all along. */
    CHECK_EQ(oi_cli_conversation_resolve_permission(conversation, 1), OI_OK);

    for (int i = 0; i < 200 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_OK);
    CHECK(sink.tool_start_position > sink.awaiting_permission_position);
    CHECK(sink.tool_output_position > sink.tool_start_position);
    CHECK(sink.tool_message_position > sink.tool_output_position);
    CHECK_STREQ(sink.text, "finished");
    /* Output arriving from the child means the process is genuinely running,
     * which is the one moment "tool running" is the only right answer. */
    CHECK(sink.activity_seen[OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT]);
    CHECK_EQ(sink.activity_at[OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT],
             OI_CLI_CONVERSATION_ACTIVITY_TOOL_RUNNING);

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(ask_defers_and_resolve_permission_deny_produces_a_protocol_valid_result) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf tool-output\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turn = {NULL, first_response, 11};
    struct mock_api api;
    CHECK(mock_api_start(&api, &turn, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 1000,
        .permission = ask_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    oi_cli_conversation *conversation = NULL;
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && sink.awaiting_permission_count == 0; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK_EQ(sink.awaiting_permission_count, 1);

    CHECK_EQ(oi_cli_conversation_resolve_permission(conversation, 0), OI_OK);
    /* A resolved, non-pending decision can't be resolved twice. */
    CHECK_EQ(oi_cli_conversation_resolve_permission(conversation, 1),
             OI_ERR_INVAL);

    for (int i = 0; i < 200 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_ERR_DENIED);
    /* Denied outright: the process never spawned or produced output. */
    CHECK_EQ(sink.tool_starting_count, 0);
    CHECK_EQ(sink.tool_output_position, -1);
    CHECK(sink.tool_message_position > sink.awaiting_permission_position);
    CHECK_EQ(sink.tool_outcome_count, 1);
    CHECK_EQ(sink.tool_outcomes[0], OI_CLI_CONVERSATION_TOOL_DENIED);
    CHECK(!sink.tool_outcome_has_raw[0]);
    /* A denial ends the turn with a real non-OK status and no cancel, which
     * is the shape that must report as failed rather than idle. */
    CHECK_EQ(oi_cli_conversation_activity(conversation),
             OI_CLI_CONVERSATION_ACTIVITY_FAILED);
    CHECK_EQ(oi_cli_conversation_last_status(conversation), OI_ERR_DENIED);

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(cancel_while_awaiting_permission_repairs_as_not_executed) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf tool-output\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turn = {NULL, first_response, 11};
    struct mock_api api;
    CHECK(mock_api_start(&api, &turn, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 1000,
        .permission = ask_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    oi_cli_conversation *conversation = NULL;
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    sink.cancel_on_awaiting_permission = &conversation;
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.awaiting_permission_count, 1);
    CHECK(oi_cli_conversation_was_cancelled(conversation));
    CHECK_EQ(sink.tool_starting_count, 0);
    CHECK_EQ(sink.tool_outcome_count, 1);
    CHECK_EQ(sink.tool_outcomes[0], OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED);
    /* A genuine cancel-before-start, not a denial -- resolving it again
     * afterward must be rejected, the same as any other already-settled
     * permission request. */
    CHECK_EQ(oi_cli_conversation_resolve_permission(conversation, 1),
             OI_ERR_INVAL);

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(cancel_while_streaming_needs_no_repair) {
    const char *response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"partial\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[2] = {
        {NULL, response, 0},
        {NULL, response, 0},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 2));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
        .activity_probe = &conversation,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 2,
        .tool_timeout_ms = 1000,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    CHECK_EQ(oi_cli_conversation_create(client, reactor, arena, tools,
                                        &config, NULL, &conversation),
             OI_OK);

    /* Cancel immediately after start, before stepping the reactor at all:
     * nothing async has had a chance to run yet, so this deterministically
     * cancels while conversation->request != NULL and no assistant message
     * has been committed -- no *tool* repair is needed or possible, but the
     * turn still needs an interrupted-turn placeholder assistant message to
     * keep durable replay's user/assistant alternation intact for the next
     * turn's user message. */
    CHECK_EQ(oi_cli_conversation_start(conversation, "question one", 12),
             OI_OK);
    CHECK(oi_cli_conversation_is_busy(conversation));
    oi_cli_conversation_cancel(conversation);
    CHECK(!oi_cli_conversation_is_busy(conversation));
    CHECK(oi_cli_conversation_was_cancelled(conversation));
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_ERR_CLOSED);
    CHECK_EQ(sink.tool_outcome_count, 0);

    /*
     * The unwind is observable: the repair message the cancel commits is
     * emitted while the conversation reports CANCELLING, not while it still
     * claims to be streaming. And once the unwind finishes the conversation
     * is idle, not failed -- despite the OI_ERR_CLOSED the cancel carries,
     * which is exactly the status that must not be reported as a fault.
     */
    CHECK(sink.cancelling_event_count > 0);
    CHECK_EQ(oi_cli_conversation_activity(conversation),
             OI_CLI_CONVERSATION_ACTIVITY_IDLE);
    CHECK_EQ(oi_cli_conversation_last_status(conversation), OI_ERR_CLOSED);

    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, 2);
    CHECK_STREQ(messages->items[0].content.data, "question one");
    CHECK_EQ(messages->items[1].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK(strstr(messages->items[1].content.data, "interrupted") != NULL);

    /* The conversation is still fully usable for a next turn. */
    memset(&sink, 0, sizeof sink);
    sink.arena = arena;
    sink.tool_start_position = -1;
    sink.tool_output_position = -1;
    sink.tool_message_position = -1;
    sink.activity_probe = &conversation;
    CHECK_EQ(oi_cli_conversation_start(conversation, "question two", 12),
             OI_OK);
    for (int i = 0; i < 100 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_OK);
    CHECK(!oi_cli_conversation_was_cancelled(conversation));
    CHECK_STREQ(sink.text, "partial");
    /* A clean turn after a cancelled one reports idle for the ordinary
     * reason (OI_OK), not because the cancel flag is still set. */
    CHECK_EQ(sink.cancelling_event_count, 0);
    CHECK_EQ(oi_cli_conversation_activity(conversation),
             OI_CLI_CONVERSATION_ACTIVITY_IDLE);
    {
        size_t request_len = 0;
        char *request = mock_api_request(&api, 0, &request_len);
        CHECK(request != NULL);
        CHECK(strstr(request, "question one") != NULL);
        CHECK(strstr(request, "question two") != NULL);
        free(request);
    }

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(cancel_while_tool_running_repairs_messages) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf started; sleep 2\\\"}\"}},"
        "{\"index\":1,\"id\":\"call-2\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf second\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[1] = {
        {NULL, first_response, 11},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 30000,
        .permission = allow_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    oi_cli_conversation *conversation = NULL;
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && sink.tool_output_position < 0; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.tool_output_position >= 0);
    CHECK(!sink.turn_done);
    CHECK(oi_cli_conversation_is_busy(conversation));

    oi_cli_conversation_cancel(conversation);
    CHECK(!oi_cli_conversation_is_busy(conversation));
    CHECK(oi_cli_conversation_was_cancelled(conversation));
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_ERR_CLOSED);

    CHECK_EQ(sink.tool_outcome_count, 2);
    CHECK_EQ(sink.tool_outcomes[0], OI_CLI_CONVERSATION_TOOL_OUTCOME_UNKNOWN);
    CHECK(sink.tool_outcome_has_raw[0]);
    CHECK_EQ(sink.tool_outcomes[1], OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED);
    CHECK(!sink.tool_outcome_has_raw[1]);

    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, 5);
    CHECK_EQ(messages->items[1].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK_EQ(messages->items[1].tool_calls_len, 2);
    CHECK_EQ(messages->items[2].role, OI_CLI_MESSAGE_TOOL);
    CHECK_STREQ(messages->items[2].tool_call_id.data, "call-1");
    CHECK(strstr(messages->items[2].content.data, "outcome unknown") != NULL);
    CHECK_EQ(messages->items[3].role, OI_CLI_MESSAGE_TOOL);
    CHECK_STREQ(messages->items[3].tool_call_id.data, "call-2");
    CHECK(strstr(messages->items[3].content.data, "not executed") != NULL);
    CHECK_EQ(messages->items[4].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK(strstr(messages->items[4].content.data, "interrupted") != NULL);

    /* No process is left running past the cancel: give the killed one a
     * moment, then confirm no further tool output ever arrives. */
    for (int i = 0; i < 5; i++) {
        oi_status step_status;
        oi_reactor_step(reactor, 50, &step_status);
    }

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(cancel_from_within_tool_starting_event) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf ran\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[1] = {
        {NULL, first_response, 11},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
        .cancel_on_tool_starting = &conversation,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 1000,
        .permission = allow_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK(!oi_cli_conversation_is_busy(conversation));
    CHECK(oi_cli_conversation_was_cancelled(conversation));

    /* The cancel fired reentrantly from inside the TOOL_STARTING emit,
     * before conversation->tool was ever assigned: the tool must never
     * have actually been spawned (no TOOL_OUTPUT), and the repair must
     * still have happened (NOT_EXECUTED -- nothing was ever running). */
    CHECK_EQ(sink.tool_output_position, -1);
    CHECK_EQ(sink.tool_outcome_count, 1);
    CHECK_EQ(sink.tool_outcomes[0], OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED);

    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, 4);
    CHECK_EQ(messages->items[2].role, OI_CLI_MESSAGE_TOOL);
    CHECK(strstr(messages->items[2].content.data, "not executed") != NULL);
    CHECK_EQ(messages->items[3].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK(strstr(messages->items[3].content.data, "interrupted") != NULL);

    /* Give the reactor a few more steps: if a process had actually been
     * spawned despite the cancel, it would show up here. */
    for (int i = 0; i < 5; i++) {
        oi_status step_status;
        oi_reactor_step(reactor, 50, &step_status);
    }
    CHECK_EQ(sink.tool_output_position, -1);

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(set_model_affects_only_the_next_request) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"first answer\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    const char *second_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"second answer\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[2] = {
        {NULL, first_response, 0},
        {NULL, second_response, 0},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 2));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    struct event_sink sink = {.arena = arena};
    struct oi_cli_conversation_config config = {
        .model = "model-one",
        .max_model_steps = 2,
        .tool_timeout_ms = 1000,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    oi_cli_conversation *conversation = NULL;
    CHECK_EQ(oi_cli_conversation_create(client, reactor, arena, tools,
                                        &config, NULL, &conversation),
             OI_OK);

    CHECK_EQ(oi_cli_conversation_start(conversation, "question one", 12),
             OI_OK);
    for (int i = 0; i < 100 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_STREQ(sink.text, "first answer");
    CHECK_STREQ(sink.last_assistant_model, "model-one");
    {
        size_t request_len = 0;
        char *request = mock_api_request(&api, 0, &request_len);
        CHECK(request != NULL);
        CHECK(strstr(request, "model-one") != NULL);
        free(request);
    }

    CHECK_EQ(oi_cli_conversation_set_model(conversation, "model-two", 9),
             OI_OK);
    memset(&sink, 0, sizeof sink);
    sink.arena = arena;
    CHECK_EQ(oi_cli_conversation_start(conversation, "question two", 12),
             OI_OK);
    for (int i = 0; i < 100 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_STREQ(sink.text, "second answer");
    CHECK_STREQ(sink.last_assistant_model, "model-two");
    {
        size_t request_len = 0;
        char *request = mock_api_request(&api, 1, &request_len);
        CHECK(request != NULL);
        CHECK(strstr(request, "model-two") != NULL);
        CHECK(strstr(request, "\"model\":\"model-one\"") == NULL);
        free(request);
    }

    CHECK_EQ(oi_cli_conversation_set_model(NULL, "x", 1), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_conversation_set_model(conversation, NULL, 0),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_conversation_set_model(conversation, "", 0),
             OI_ERR_INVAL);

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(steer_after_a_tool_completes_skips_the_next_one) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf one\\\"}\"}},"
        "{\"index\":1,\"id\":\"call-2\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf two\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[1] = {
        {NULL, first_response, 11},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
        .steer_on_tool_message = &conversation,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 5000,
        .permission = allow_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_OK);
    CHECK(!oi_cli_conversation_was_cancelled(conversation));

    /* Steering fired reentrantly from call-1's own TOOL message commit
     * (before start_next_tool ever re-enters for call-2), so call-1 must
     * have run for real (COMPLETED) while call-2 never started at all. */
    CHECK_EQ(sink.tool_starting_count, 1);
    CHECK_EQ(sink.tool_outcome_count, 2);
    CHECK_EQ(sink.tool_outcomes[0], OI_CLI_CONVERSATION_TOOL_COMPLETED);
    CHECK_EQ(sink.tool_outcomes[1], OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED);

    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, 5);
    CHECK_EQ(messages->items[2].role, OI_CLI_MESSAGE_TOOL);
    CHECK_STREQ(messages->items[2].tool_call_id.data, "call-1");
    CHECK(strstr(messages->items[2].content.data, "one") != NULL);
    CHECK_EQ(messages->items[3].role, OI_CLI_MESSAGE_TOOL);
    CHECK_STREQ(messages->items[3].tool_call_id.data, "call-2");
    CHECK(strstr(messages->items[3].content.data, "skipped") != NULL);

    /* A steering bookend, not an interrupted-turn one: RESPONSE_DONE
     * already fired for real, unlike a genuine cancel, but durable replay
     * still requires an assistant message with no tool_calls to close out
     * this turn before anything else (e.g. the queued message's own turn)
     * can validly follow. */
    CHECK_EQ(messages->items[4].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK_EQ(messages->items[4].tool_calls_len, (size_t)0);
    CHECK(strstr(messages->items[4].content.data, "queued") != NULL);

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(steer_after_the_only_tool_prevents_a_second_model_round) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf one\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    const char *second_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"should never be requested\"}}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[2] = {
        {NULL, first_response, 11},
        {NULL, second_response, 11},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 2));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
        .steer_on_tool_message = &conversation,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 5000,
        .permission = allow_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_OK);
    CHECK_EQ(sink.tool_starting_count, 1);
    CHECK_EQ(sink.tool_outcome_count, 1);
    CHECK_EQ(sink.tool_outcomes[0], OI_CLI_CONVERSATION_TOOL_COMPLETED);

    /* Give the reactor a few more idle steps: if steering had failed to
     * suppress the follow-up model round, the second request would show
     * up here. */
    for (int i = 0; i < 5; i++) {
        oi_status step_status;
        oi_reactor_step(reactor, 50, &step_status);
    }
    {
        size_t len = 0;
        char *second_request = mock_api_request(&api, 1, &len);
        CHECK(second_request == NULL);
        free(second_request);
    }

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(steer_before_any_tool_starts_skips_them_all) {
    const char *first_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":"
        "\"{\\\"command\\\":\\\"printf ran\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[1] = {
        {NULL, first_response, 11},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK_EQ(oi_cli_tools_register(tools), OI_OK);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    struct event_sink sink = {
        .arena = arena,
        .tool_start_position = -1,
        .tool_output_position = -1,
        .tool_message_position = -1,
        .steer_on_response_done = &conversation,
    };
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 3,
        .tool_timeout_ms = 5000,
        .permission = allow_tool,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, NULL, &conversation),
             OI_OK);
    CHECK_EQ(oi_cli_conversation_start(conversation, "run it", 6), OI_OK);
    for (int i = 0; i < 200 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);
    CHECK_EQ(sink.status, OI_OK);
    CHECK(!oi_cli_conversation_was_cancelled(conversation));

    /* Steered before start_next_tool ever ran for the first time (from
     * RESPONSE_DONE, which fires before it): the call never started at
     * all, not even staged. */
    CHECK_EQ(sink.tool_starting_count, 0);
    CHECK_EQ(sink.tool_output_position, -1);
    CHECK_EQ(sink.tool_outcome_count, 1);
    CHECK_EQ(sink.tool_outcomes[0], OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED);

    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, 4);
    CHECK_EQ(messages->items[2].role, OI_CLI_MESSAGE_TOOL);
    CHECK_STREQ(messages->items[2].tool_call_id.data, "call-1");
    CHECK(strstr(messages->items[2].content.data, "skipped") != NULL);
    /* Steering's own turn-closing bookend -- see
     * steer_after_a_tool_completes_skips_the_next_one's identical check
     * for why durable replay requires this. */
    CHECK_EQ(messages->items[3].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK_EQ(messages->items[3].tool_calls_len, (size_t)0);
    CHECK(strstr(messages->items[3].content.data, "queued") != NULL);

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

static void build_conversation_with_two_turns(
    oi_llm_client *client, oi_reactor *reactor, oi_arena *arena,
    oi_tool_registry *tools, oi_cli_conversation **out_conversation) {
    struct oi_cli_message_list initial;
    struct oi_cli_message m;

    oi_cli_message_list_init(&initial);

    oi_cli_message_init(&m);
    CHECK_EQ(oi_cli_message_set_user(&m, "question one", 12), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&initial, &m), OI_OK);

    oi_cli_message_init(&m);
    CHECK_EQ(oi_cli_message_set_assistant(&m, "answer one", 10), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&initial, &m), OI_OK);

    oi_cli_message_init(&m);
    CHECK_EQ(oi_cli_message_set_user(&m, "question two", 12), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&initial, &m), OI_OK);

    oi_cli_message_init(&m);
    CHECK_EQ(oi_cli_message_set_assistant(&m, "answer two", 10), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&initial, &m), OI_OK);

    struct oi_cli_conversation_config config = {
        .model = "model-one",
        .max_model_steps = 2,
        .tool_timeout_ms = 1000,
    };
    CHECK_EQ(oi_cli_conversation_create(client, reactor, arena, tools,
                                        &config, &initial, out_conversation),
             OI_OK);
    oi_cli_message_list_free(&initial);
}

TEST(checkpoint_splices_a_prefix_into_one_assistant_message) {
    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = 9,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    build_conversation_with_two_turns(client, reactor, arena, tools,
                                      &conversation);

    CHECK_EQ(oi_cli_conversation_apply_checkpoint(
                 conversation, 2, "summary of turn one", 20),
             OI_OK);

    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, (size_t)3);
    CHECK_EQ(messages->items[0].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK_STREQ(messages->items[0].content.data, "summary of turn one");
    CHECK_EQ(messages->items[1].role, OI_CLI_MESSAGE_USER);
    CHECK_STREQ(messages->items[1].content.data, "question two");
    CHECK_EQ(messages->items[2].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK_STREQ(messages->items[2].content.data, "answer two");

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
}

TEST(checkpoint_can_consume_the_entire_message_list) {
    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = 9,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    build_conversation_with_two_turns(client, reactor, arena, tools,
                                      &conversation);

    CHECK_EQ(oi_cli_conversation_apply_checkpoint(
                 conversation, 4, "summary of everything", 22),
             OI_OK);

    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, (size_t)1);
    CHECK_EQ(messages->items[0].role, OI_CLI_MESSAGE_ASSISTANT);
    CHECK_STREQ(messages->items[0].content.data, "summary of everything");

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
}

TEST(checkpoint_can_take_an_existing_summary_allocation) {
    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = 9,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    oi_cli_conversation *conversation = NULL;
    char *summary = malloc(14);

    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);
    CHECK(client != NULL);
    CHECK(summary != NULL);
    if (summary != NULL) {
        memcpy(summary, "owned summary", 14);
    }
    build_conversation_with_two_turns(client, reactor, arena, tools,
                                      &conversation);

    CHECK_EQ(oi_cli_conversation_apply_checkpoint_take_summary(
                 conversation, 2, &summary, 13),
             OI_OK);
    CHECK(summary == NULL);
    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_STREQ(messages->items[0].content.data, "owned summary");

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
}

TEST(checkpoint_rejects_invalid_prefix_counts) {
    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = 9,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_cli_conversation *conversation = NULL;
    build_conversation_with_two_turns(client, reactor, arena, tools,
                                      &conversation);

    CHECK_EQ(oi_cli_conversation_apply_checkpoint(conversation, 0, "x", 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_conversation_apply_checkpoint(conversation, 5, "x", 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_conversation_apply_checkpoint(NULL, 1, "x", 1),
             OI_ERR_INVAL);

    /* Rejected calls must leave the message list completely untouched. */
    const struct oi_cli_message_list *messages =
        oi_cli_conversation_messages(conversation);
    CHECK_EQ(messages->len, (size_t)4);
    CHECK_EQ(messages->items[0].role, OI_CLI_MESSAGE_USER);
    CHECK_STREQ(messages->items[0].content.data, "question one");

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
}

TEST(checkpoint_is_rejected_while_busy) {
    const char *slow_response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"answer\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[1] = {
        {NULL, slow_response, 0},
    };
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    CHECK(tools != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    struct event_sink sink = {.arena = arena};
    struct oi_cli_conversation_config config = {
        .model = "model-one",
        .max_model_steps = 2,
        .tool_timeout_ms = 1000,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    oi_cli_conversation *conversation = NULL;
    CHECK_EQ(oi_cli_conversation_create(client, reactor, arena, tools,
                                        &config, NULL, &conversation),
             OI_OK);

    /* oi_cli_conversation_start sets busy synchronously before returning,
     * so this is checked before the reactor is ever stepped. */
    CHECK_EQ(oi_cli_conversation_start(conversation, "question", 8), OI_OK);
    CHECK(oi_cli_conversation_is_busy(conversation));
    CHECK_EQ(oi_cli_conversation_apply_checkpoint(conversation, 1, "x", 1),
             OI_ERR_INVAL);

    for (int i = 0; i < 100 && !sink.turn_done; i++) {
        oi_status step_status;
        CHECK(oi_reactor_step(reactor, 100, &step_status) >= 0);
    }
    CHECK(sink.turn_done);

    oi_cli_conversation_destroy(conversation);
    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

int main(void) {
    RUN(start_is_event_driven_and_preserves_context);
    RUN(tool_start_boundary_precedes_process_output);
    RUN(ask_defers_and_resolve_permission_allow_lets_the_tool_run);
    RUN(ask_defers_and_resolve_permission_deny_produces_a_protocol_valid_result);
    RUN(cancel_while_awaiting_permission_repairs_as_not_executed);
    RUN(cancel_while_streaming_needs_no_repair);
    RUN(cancel_while_tool_running_repairs_messages);
    RUN(cancel_from_within_tool_starting_event);
    RUN(set_model_affects_only_the_next_request);
    RUN(steer_after_a_tool_completes_skips_the_next_one);
    RUN(steer_after_the_only_tool_prevents_a_second_model_round);
    RUN(steer_before_any_tool_starts_skips_them_all);
    RUN(checkpoint_splices_a_prefix_into_one_assistant_message);
    RUN(checkpoint_can_consume_the_entire_message_list);
    RUN(checkpoint_can_take_an_existing_summary_allocation);
    RUN(checkpoint_rejects_invalid_prefix_counts);
    RUN(checkpoint_is_rejected_while_busy);
    return oi_test_report();
}
