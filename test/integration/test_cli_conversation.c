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
};

static oi_status collect_event(
    const struct oi_cli_conversation_event *event, void *user_data) {
    struct event_sink *sink = user_data;
    int position = sink->event_count++;
    switch (event->type) {
    case OI_CLI_CONVERSATION_EVENT_MESSAGE:
        sink->message_count++;
        if (event->as.message.value->role == OI_CLI_MESSAGE_TOOL) {
            sink->tool_message_position = position;
            sink->tool_message_has_raw =
                event->as.message.has_raw_tool_output;
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
        break;
    case OI_CLI_CONVERSATION_EVENT_TURN_DONE:
        sink->turn_done = 1;
        sink->status = event->as.turn_done.status;
        sink->arena_used_at_done = oi_arena_used(sink->arena);
        break;
    case OI_CLI_CONVERSATION_EVENT_TOOL_STARTING:
        sink->tool_start_position = position;
        break;
    case OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT:
        if (sink->tool_output_position < 0) {
            sink->tool_output_position = position;
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

    struct event_sink sink = {.arena = arena};
    struct oi_cli_conversation_config config = {
        .model = "mock-model",
        .max_model_steps = 2,
        .tool_timeout_ms = 1000,
        .on_event = collect_event,
        .event_user_data = &sink,
    };
    oi_cli_conversation *conversation = NULL;
    CHECK_EQ(oi_cli_conversation_create(
                 client, reactor, arena, tools, &config, &initial,
                 &conversation),
             OI_OK);

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
    CHECK_STREQ(sink.text, "finished");

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
    return oi_test_report();
}
