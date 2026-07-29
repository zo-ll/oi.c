#include "cli_compact.h"
#include "cli_message.h"
#include "test.h"

#include <string.h>

static void append_user(struct oi_cli_message_list *list, const char *text) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    CHECK_EQ(oi_cli_message_set_user(&message, text, strlen(text)), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(list, &message), OI_OK);
}

static void append_assistant(struct oi_cli_message_list *list,
                              const char *text) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    CHECK_EQ(oi_cli_message_set_assistant(&message, text, strlen(text)),
             OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(list, &message), OI_OK);
}

static void append_tool(struct oi_cli_message_list *list, const char *id,
                         const char *text) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    CHECK_EQ(oi_cli_message_set_tool(&message, id, strlen(id), text,
                                     strlen(text)),
             OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(list, &message), OI_OK);
}

TEST(parse_turns_reports_no_value_for_empty_arguments) {
    size_t turns = 99;
    int has_value = 1;
    CHECK_EQ(oi_cli_compact_parse_turns("", 0, &turns, &has_value), OI_OK);
    CHECK_EQ(has_value, 0);
}

TEST(parse_turns_accepts_a_plain_decimal) {
    size_t turns = 0;
    int has_value = 0;
    CHECK_EQ(oi_cli_compact_parse_turns("12", 2, &turns, &has_value), OI_OK);
    CHECK_EQ(has_value, 1);
    CHECK_EQ(turns, (size_t)12);
}

TEST(parse_turns_accepts_a_leading_zero) {
    size_t turns = 0;
    int has_value = 0;
    CHECK_EQ(oi_cli_compact_parse_turns("007", 3, &turns, &has_value), OI_OK);
    CHECK_EQ(has_value, 1);
    CHECK_EQ(turns, (size_t)7);
}

TEST(parse_turns_accepts_zero) {
    size_t turns = 99;
    int has_value = 0;
    CHECK_EQ(oi_cli_compact_parse_turns("0", 1, &turns, &has_value), OI_OK);
    CHECK_EQ(has_value, 1);
    CHECK_EQ(turns, (size_t)0);
}

TEST(parse_turns_rejects_overflow) {
    const char *huge = "999999999999999999999999999999";
    size_t turns = 0;
    int has_value = 0;
    CHECK_EQ(oi_cli_compact_parse_turns(huge, strlen(huge), &turns,
                                        &has_value),
             OI_ERR_PARSE);
}

TEST(parse_turns_rejects_embedded_space) {
    size_t turns = 0;
    int has_value = 0;
    CHECK_EQ(oi_cli_compact_parse_turns("5 6", 3, &turns, &has_value),
             OI_ERR_PARSE);
}

TEST(parse_turns_rejects_a_sign) {
    size_t turns = 0;
    int has_value = 0;
    CHECK_EQ(oi_cli_compact_parse_turns("-1", 2, &turns, &has_value),
             OI_ERR_PARSE);
    CHECK_EQ(oi_cli_compact_parse_turns("+1", 2, &turns, &has_value),
             OI_ERR_PARSE);
}

TEST(parse_turns_rejects_trailing_garbage) {
    size_t turns = 0;
    int has_value = 0;
    CHECK_EQ(oi_cli_compact_parse_turns("5x", 2, &turns, &has_value),
             OI_ERR_PARSE);
}

TEST(parse_turns_rejects_null_outputs) {
    size_t turns = 0;
    int has_value = 0;
    CHECK_EQ(oi_cli_compact_parse_turns("5", 1, NULL, &has_value),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_compact_parse_turns("5", 1, &turns, NULL), OI_ERR_INVAL);
}

TEST(select_prefix_on_an_empty_conversation_finds_nothing) {
    struct oi_cli_message_list messages;
    oi_cli_message_list_init(&messages);

    size_t prefix = 99;
    size_t total = 99;
    CHECK_EQ(oi_cli_compact_select_prefix(&messages, 8, &prefix, &total),
             OI_OK);
    CHECK_EQ(total, (size_t)0);
    CHECK_EQ(prefix, (size_t)0);

    oi_cli_message_list_free(&messages);
}

TEST(select_prefix_is_a_noop_when_within_keep_turns) {
    struct oi_cli_message_list messages;
    oi_cli_message_list_init(&messages);
    append_user(&messages, "q1");
    append_assistant(&messages, "a1");
    append_user(&messages, "q2");
    append_assistant(&messages, "a2");

    size_t prefix = 99;
    size_t total = 0;
    CHECK_EQ(oi_cli_compact_select_prefix(&messages, 2, &prefix, &total),
             OI_OK);
    CHECK_EQ(total, (size_t)2);
    CHECK_EQ(prefix, (size_t)0);

    CHECK_EQ(oi_cli_compact_select_prefix(&messages, 5, &prefix, &total),
             OI_OK);
    CHECK_EQ(total, (size_t)2);
    CHECK_EQ(prefix, (size_t)0);

    oi_cli_message_list_free(&messages);
}

TEST(select_prefix_consumes_exactly_one_more_turn_than_kept) {
    struct oi_cli_message_list messages;
    oi_cli_message_list_init(&messages);
    append_user(&messages, "q1");
    append_assistant(&messages, "a1");
    append_user(&messages, "q2");
    append_assistant(&messages, "a2");
    append_user(&messages, "q3");
    append_assistant(&messages, "a3");

    size_t prefix = 0;
    size_t total = 0;
    CHECK_EQ(oi_cli_compact_select_prefix(&messages, 2, &prefix, &total),
             OI_OK);
    CHECK_EQ(total, (size_t)3);
    /* One turn to compact (q1/a1); the kept suffix starts at index 2. */
    CHECK_EQ(prefix, (size_t)2);

    oi_cli_message_list_free(&messages);
}

TEST(select_prefix_tool_messages_do_not_start_new_turns) {
    struct oi_cli_message_list messages;
    oi_cli_message_list_init(&messages);
    append_user(&messages, "q1");
    append_assistant(&messages, "a1 calls a tool");
    append_tool(&messages, "call-1", "tool result");
    append_tool(&messages, "call-2", "tool result 2");
    append_assistant(&messages, "a1 final");
    append_user(&messages, "q2");
    append_assistant(&messages, "a2");

    size_t prefix = 0;
    size_t total = 0;
    CHECK_EQ(oi_cli_compact_select_prefix(&messages, 1, &prefix, &total),
             OI_OK);
    CHECK_EQ(total, (size_t)2);
    CHECK_EQ(prefix, (size_t)5);

    oi_cli_message_list_free(&messages);
}

TEST(select_prefix_with_keep_turns_zero_consumes_everything) {
    struct oi_cli_message_list messages;
    oi_cli_message_list_init(&messages);
    append_user(&messages, "q1");
    append_assistant(&messages, "a1");
    append_user(&messages, "q2");
    append_assistant(&messages, "a2");

    size_t prefix = 0;
    size_t total = 0;
    CHECK_EQ(oi_cli_compact_select_prefix(&messages, 0, &prefix, &total),
             OI_OK);
    CHECK_EQ(total, (size_t)2);
    CHECK_EQ(prefix, messages.len);

    oi_cli_message_list_free(&messages);
}

TEST(select_prefix_rejects_null_arguments) {
    struct oi_cli_message_list messages;
    oi_cli_message_list_init(&messages);
    size_t prefix = 0;
    size_t total = 0;

    CHECK_EQ(oi_cli_compact_select_prefix(NULL, 1, &prefix, &total),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_compact_select_prefix(&messages, 1, NULL, &total),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_compact_select_prefix(&messages, 1, &prefix, NULL),
             OI_ERR_INVAL);

    oi_cli_message_list_free(&messages);
}

int main(void) {
    RUN(parse_turns_reports_no_value_for_empty_arguments);
    RUN(parse_turns_accepts_a_plain_decimal);
    RUN(parse_turns_accepts_a_leading_zero);
    RUN(parse_turns_accepts_zero);
    RUN(parse_turns_rejects_overflow);
    RUN(parse_turns_rejects_embedded_space);
    RUN(parse_turns_rejects_a_sign);
    RUN(parse_turns_rejects_trailing_garbage);
    RUN(parse_turns_rejects_null_outputs);
    RUN(select_prefix_on_an_empty_conversation_finds_nothing);
    RUN(select_prefix_is_a_noop_when_within_keep_turns);
    RUN(select_prefix_consumes_exactly_one_more_turn_than_kept);
    RUN(select_prefix_tool_messages_do_not_start_new_turns);
    RUN(select_prefix_with_keep_turns_zero_consumes_everything);
    RUN(select_prefix_rejects_null_arguments);
    return oi_test_report();
}
