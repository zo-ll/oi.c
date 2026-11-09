#include "llm_responses.h"
#include "test.h"

#include <string.h>

static oi_status translate(oi_arena *arena, const char *body,
                           oi_json_writer **writer) {
    return oi_llm_responses_translate_body(arena, body, strlen(body), writer);
}

TEST(translates_history_and_tools) {
    const char *chat =
        "{\"model\":\"m\",\"stream\":true,\"temperature\":1,\"messages\":["
        "{\"role\":\"system\",\"content\":\"S\"},"
        "{\"role\":\"user\",\"content\":\"U\"},"
        "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":["
        "{\"id\":\"c1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"shell\",\"arguments\":\"{}\"}}]},"
        "{\"role\":\"tool\",\"content\":\"out\",\"tool_call_id\":\"c1\"},"
        "{\"role\":\"assistant\",\"content\":\"A\"}],"
        "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"shell\","
        "\"description\":\"d\",\"parameters\":{\"type\":\"object\","
        "\"required\":[\"command\"],\"additionalProperties\":false,"
        "\"n\":1.5,\"x\":null}}}]}";
    const char *expected =
        "{\"model\":\"m\",\"store\":false,\"stream\":true,\"input\":["
        "{\"type\":\"message\",\"role\":\"developer\",\"content\":\"S\"},"
        "{\"type\":\"message\",\"role\":\"user\",\"content\":\"U\"},"
        "{\"type\":\"function_call\",\"call_id\":\"c1\",\"name\":\"shell\","
        "\"arguments\":\"{}\"},"
        "{\"type\":\"function_call_output\",\"call_id\":\"c1\","
        "\"output\":\"out\"},"
        "{\"type\":\"message\",\"role\":\"assistant\",\"content\":\"A\"}],"
        "\"tools\":[{\"type\":\"function\",\"name\":\"shell\","
        "\"description\":\"d\",\"parameters\":{\"type\":\"object\","
        "\"required\":[\"command\"],\"additionalProperties\":false,"
        "\"n\":1.5,\"x\":null},\"strict\":false}]}";

    oi_arena *arena = oi_arena_create(0);
    oi_json_writer *writer = NULL;
    CHECK_EQ(translate(arena, chat, &writer), OI_OK);
    CHECK_STREQ(oi_json_writer_data(writer, NULL), expected);
    oi_json_writer_destroy(writer);
    oi_arena_destroy(arena);
}

TEST(omits_tools_when_the_chat_body_has_none) {
    oi_arena *arena = oi_arena_create(0);
    oi_json_writer *writer = NULL;
    CHECK_EQ(translate(arena, "{\"model\":\"m\",\"messages\":[]}", &writer),
             OI_OK);
    CHECK_STREQ(oi_json_writer_data(writer, NULL),
                "{\"model\":\"m\",\"store\":false,\"stream\":true,"
                "\"input\":[]}");
    oi_json_writer_destroy(writer);
    oi_arena_destroy(arena);
}

TEST(rejects_bodies_that_are_not_chat_requests) {
    const char *bad[] = {
        "not json",
        "{",
        "{}",
        "{\"model\":\"m\"}",
        "{\"messages\":[]}",
        "{\"model\":\"m\",\"messages\":{}}",
        "{\"model\":\"m\",\"messages\":[{\"role\":\"robot\","
        "\"content\":\"x\"}]}",
        "{\"model\":\"m\",\"messages\":[{\"role\":\"user\"}]}",
        "{\"model\":\"m\",\"messages\":[{\"role\":\"tool\","
        "\"content\":\"x\"}]}",
        "{\"model\":\"m\",\"messages\":[],\"tools\":[{\"type\":\"custom\"}]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        oi_arena *arena = oi_arena_create(0);
        oi_json_writer *writer = NULL;
        CHECK_EQ(translate(arena, bad[i], &writer), OI_ERR_PARSE);
        CHECK(writer == NULL);
        oi_arena_destroy(arena);
    }
}

int main(void) {
    RUN(translates_history_and_tools);
    RUN(omits_tools_when_the_chat_body_has_none);
    RUN(rejects_bodies_that_are_not_chat_requests);
    return oi_test_report();
}
