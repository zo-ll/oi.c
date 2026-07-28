#ifndef OI_CLI_MESSAGE_H
#define OI_CLI_MESSAGE_H

#include <stddef.h>

#include "oi/status.h"

enum oi_cli_message_role {
    OI_CLI_MESSAGE_NONE = 0,
    OI_CLI_MESSAGE_USER,
    OI_CLI_MESSAGE_ASSISTANT,
    OI_CLI_MESSAGE_TOOL
};

struct oi_cli_string {
    char *data;
    size_t len;
};

struct oi_cli_tool_call_value {
    struct oi_cli_string id;
    struct oi_cli_string name;
    struct oi_cli_string arguments;
};

/*
 * Heap-owned model-visible message. Strings carry explicit lengths and also
 * have a trailing NUL for APIs that accept ordinary C strings.
 */
struct oi_cli_message {
    enum oi_cli_message_role role;
    struct oi_cli_string content;
    struct oi_cli_string tool_call_id;
    struct oi_cli_tool_call_value *tool_calls;
    size_t tool_calls_len;
};

struct oi_cli_message_list {
    struct oi_cli_message *items;
    size_t len;
    size_t cap;
};

void oi_cli_message_init(struct oi_cli_message *message);
void oi_cli_message_free(struct oi_cli_message *message);

oi_status oi_cli_message_set_user(struct oi_cli_message *message,
                                  const char *content, size_t content_len);
oi_status oi_cli_message_set_assistant(struct oi_cli_message *message,
                                       const char *content,
                                       size_t content_len);
oi_status oi_cli_message_set_tool(struct oi_cli_message *message,
                                  const char *tool_call_id,
                                  size_t tool_call_id_len,
                                  const char *content, size_t content_len);
oi_status oi_cli_message_add_tool_call(struct oi_cli_message *message,
                                       const char *id, size_t id_len,
                                       const char *name, size_t name_len,
                                       const char *arguments,
                                       size_t arguments_len);
oi_status oi_cli_message_clone(const struct oi_cli_message *source,
                               struct oi_cli_message *destination);
int oi_cli_message_is_valid(const struct oi_cli_message *message);

void oi_cli_message_list_init(struct oi_cli_message_list *list);
void oi_cli_message_list_free(struct oi_cli_message_list *list);
oi_status oi_cli_message_list_append_clone(
    struct oi_cli_message_list *list, const struct oi_cli_message *message);
/*
 * On success ownership moves into the list and `message` is reset. On
 * failure the caller retains ownership.
 */
oi_status oi_cli_message_list_append_take(struct oi_cli_message_list *list,
                                           struct oi_cli_message *message);

#endif
