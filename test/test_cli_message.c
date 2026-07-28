#include "cli_message.h"
#include "test.h"

#include <string.h>

TEST(user_content_preserves_explicit_length) {
    static const char content[] = {'a', '\0', 'b'};
    struct oi_cli_message message;
    oi_cli_message_init(&message);

    CHECK_EQ(oi_cli_message_set_user(&message, content, sizeof content),
             OI_OK);
    CHECK_EQ(message.role, OI_CLI_MESSAGE_USER);
    CHECK_EQ(message.content.len, sizeof content);
    CHECK(memcmp(message.content.data, content, sizeof content) == 0);
    CHECK_EQ(message.content.data[sizeof content], '\0');
    CHECK(oi_cli_message_is_valid(&message));

    oi_cli_message_free(&message);
    CHECK_EQ(message.role, OI_CLI_MESSAGE_NONE);
    oi_cli_message_free(&message);
}

TEST(assistant_clone_is_deep) {
    struct oi_cli_message source;
    struct oi_cli_message clone;
    oi_cli_message_init(&source);
    oi_cli_message_init(&clone);

    CHECK_EQ(oi_cli_message_set_assistant(&source, "thinking", 8), OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&source, "call-1", 6, "shell", 5,
                                          "{\"cmd\":\"true\"}", 14),
             OI_OK);
    CHECK_EQ(oi_cli_message_clone(&source, &clone), OI_OK);
    CHECK(oi_cli_message_is_valid(&clone));
    CHECK(source.content.data != clone.content.data);
    CHECK(source.tool_calls != clone.tool_calls);
    CHECK(source.tool_calls[0].arguments.data !=
          clone.tool_calls[0].arguments.data);

    source.content.data[0] = 'X';
    source.tool_calls[0].name.data[0] = 'X';
    CHECK_STREQ(clone.content.data, "thinking");
    CHECK_STREQ(clone.tool_calls[0].name.data, "shell");

    oi_cli_message_free(&source);
    oi_cli_message_free(&clone);
}

TEST(tool_message_requires_call_id) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);

    CHECK_EQ(oi_cli_message_set_tool(&message, NULL, 0, "result", 6),
             OI_ERR_INVAL);
    CHECK_EQ(message.role, OI_CLI_MESSAGE_NONE);
    CHECK_EQ(oi_cli_message_set_tool(&message, "call-1", 6, NULL, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_message_set_tool(&message, "call-1", 6, "", 0), OI_OK);
    CHECK(oi_cli_message_is_valid(&message));

    oi_cli_message_free(&message);
}

TEST(tool_calls_only_attach_to_assistant) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);

    CHECK_EQ(oi_cli_message_set_user(&message, "hello", 5), OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "id", 2, "name", 4,
                                          "{}", 2),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "", 0), OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "", 0, "name", 4, "{}",
                                          2),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "id", 2, "name", 4, "{}",
                                          2),
             OI_OK);
    CHECK(oi_cli_message_is_valid(&message));

    oi_cli_message_free(&message);
}

TEST(list_clone_and_take_have_clear_ownership) {
    struct oi_cli_message source;
    struct oi_cli_message taken;
    struct oi_cli_message_list list;
    oi_cli_message_init(&source);
    oi_cli_message_init(&taken);
    oi_cli_message_list_init(&list);

    CHECK_EQ(oi_cli_message_set_user(&source, "first", 5), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_clone(&list, &source), OI_OK);
    CHECK_EQ(oi_cli_message_set_tool(&taken, "id", 2, "done", 4), OI_OK);
    char *taken_content = taken.content.data;
    CHECK_EQ(oi_cli_message_list_append_take(&list, &taken), OI_OK);

    CHECK_EQ(list.len, 2);
    CHECK(list.items[0].content.data != source.content.data);
    CHECK_EQ(list.items[1].content.data, taken_content);
    CHECK_EQ(taken.role, OI_CLI_MESSAGE_NONE);
    CHECK(taken.content.data == NULL);

    oi_cli_message_free(&source);
    oi_cli_message_free(&taken);
    oi_cli_message_list_free(&list);
    oi_cli_message_list_free(&list);
}

TEST(invalid_messages_are_rejected) {
    struct oi_cli_message invalid;
    struct oi_cli_message destination;
    struct oi_cli_message_list list;
    oi_cli_message_init(&invalid);
    oi_cli_message_init(&destination);
    oi_cli_message_list_init(&list);

    CHECK(!oi_cli_message_is_valid(&invalid));
    CHECK_EQ(oi_cli_message_clone(&invalid, &destination), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_message_list_append_take(&list, &invalid), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_message_set_user(&invalid, NULL, 1), OI_ERR_INVAL);

    oi_cli_message_free(&invalid);
    oi_cli_message_free(&destination);
    oi_cli_message_list_free(&list);
}

int main(void) {
    RUN(user_content_preserves_explicit_length);
    RUN(assistant_clone_is_deep);
    RUN(tool_message_requires_call_id);
    RUN(tool_calls_only_attach_to_assistant);
    RUN(list_clone_and_take_have_clear_ownership);
    RUN(invalid_messages_are_rejected);
    return oi_test_report();
}
