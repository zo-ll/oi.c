#include "cli_message.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int bytes_are_valid(const char *data, size_t len) {
    return data != NULL || len == 0;
}

static oi_status string_copy(struct oi_cli_string *destination,
                             const char *data, size_t len) {
    if (!bytes_are_valid(data, len)) {
        return OI_ERR_INVAL;
    }
    if (len == SIZE_MAX) {
        return OI_ERR_NOMEM;
    }

    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return OI_ERR_NOMEM;
    }
    if (len > 0) {
        memcpy(copy, data, len);
    }
    copy[len] = '\0';
    destination->data = copy;
    destination->len = len;
    return OI_OK;
}

static void string_free(struct oi_cli_string *string) {
    free(string->data);
    memset(string, 0, sizeof *string);
}

static void tool_call_free(struct oi_cli_tool_call_value *call) {
    string_free(&call->id);
    string_free(&call->name);
    string_free(&call->arguments);
}

static oi_status message_set_content(struct oi_cli_message *message,
                                     enum oi_cli_message_role role,
                                     const char *content,
                                     size_t content_len) {
    if (message == NULL || !bytes_are_valid(content, content_len)) {
        return OI_ERR_INVAL;
    }

    struct oi_cli_message replacement;
    oi_cli_message_init(&replacement);
    replacement.role = role;
    oi_status st =
        string_copy(&replacement.content, content, content_len);
    if (st != OI_OK) {
        return st;
    }

    oi_cli_message_free(message);
    *message = replacement;
    return OI_OK;
}

void oi_cli_message_init(struct oi_cli_message *message) {
    if (message != NULL) {
        memset(message, 0, sizeof *message);
    }
}

void oi_cli_message_free(struct oi_cli_message *message) {
    if (message == NULL) {
        return;
    }
    string_free(&message->content);
    string_free(&message->tool_call_id);
    for (size_t i = 0; i < message->tool_calls_len; i++) {
        tool_call_free(&message->tool_calls[i]);
    }
    free(message->tool_calls);
    memset(message, 0, sizeof *message);
}

oi_status oi_cli_message_set_user(struct oi_cli_message *message,
                                  const char *content, size_t content_len) {
    return message_set_content(message, OI_CLI_MESSAGE_USER, content,
                               content_len);
}

oi_status oi_cli_message_set_assistant(struct oi_cli_message *message,
                                       const char *content,
                                       size_t content_len) {
    return message_set_content(message, OI_CLI_MESSAGE_ASSISTANT, content,
                               content_len);
}

oi_status oi_cli_message_set_tool(struct oi_cli_message *message,
                                  const char *tool_call_id,
                                  size_t tool_call_id_len,
                                  const char *content, size_t content_len) {
    if (message == NULL || tool_call_id_len == 0 ||
        !bytes_are_valid(tool_call_id, tool_call_id_len) ||
        !bytes_are_valid(content, content_len)) {
        return OI_ERR_INVAL;
    }

    struct oi_cli_message replacement;
    oi_cli_message_init(&replacement);
    replacement.role = OI_CLI_MESSAGE_TOOL;
    oi_status st =
        string_copy(&replacement.tool_call_id, tool_call_id, tool_call_id_len);
    if (st == OI_OK) {
        st = string_copy(&replacement.content, content, content_len);
    }
    if (st != OI_OK) {
        oi_cli_message_free(&replacement);
        return st;
    }

    oi_cli_message_free(message);
    *message = replacement;
    return OI_OK;
}

oi_status oi_cli_message_add_tool_call(struct oi_cli_message *message,
                                       const char *id, size_t id_len,
                                       const char *name, size_t name_len,
                                       const char *arguments,
                                       size_t arguments_len) {
    if (message == NULL || message->role != OI_CLI_MESSAGE_ASSISTANT ||
        id_len == 0 || name_len == 0 || arguments_len == 0 ||
        !bytes_are_valid(id, id_len) || !bytes_are_valid(name, name_len) ||
        !bytes_are_valid(arguments, arguments_len)) {
        return OI_ERR_INVAL;
    }
    if (message->tool_calls_len == SIZE_MAX / sizeof *message->tool_calls) {
        return OI_ERR_NOMEM;
    }

    struct oi_cli_tool_call_value call;
    memset(&call, 0, sizeof call);
    oi_status st = string_copy(&call.id, id, id_len);
    if (st == OI_OK) {
        st = string_copy(&call.name, name, name_len);
    }
    if (st == OI_OK) {
        st = string_copy(&call.arguments, arguments, arguments_len);
    }
    if (st != OI_OK) {
        tool_call_free(&call);
        return st;
    }

    size_t count = message->tool_calls_len + 1;
    struct oi_cli_tool_call_value *calls =
        realloc(message->tool_calls, count * sizeof *calls);
    if (calls == NULL) {
        tool_call_free(&call);
        return OI_ERR_NOMEM;
    }
    message->tool_calls = calls;
    calls[message->tool_calls_len] = call;
    message->tool_calls_len = count;
    return OI_OK;
}

int oi_cli_message_is_valid(const struct oi_cli_message *message) {
    if (message == NULL || message->content.data == NULL) {
        return 0;
    }
    switch (message->role) {
    case OI_CLI_MESSAGE_USER:
        return message->tool_call_id.data == NULL &&
               message->tool_calls_len == 0 && message->tool_calls == NULL;
    case OI_CLI_MESSAGE_ASSISTANT:
        if (message->tool_call_id.data != NULL ||
            (message->tool_calls_len == 0) != (message->tool_calls == NULL)) {
            return 0;
        }
        for (size_t i = 0; i < message->tool_calls_len; i++) {
            const struct oi_cli_tool_call_value *call =
                &message->tool_calls[i];
            if (call->id.data == NULL || call->id.len == 0 ||
                call->name.data == NULL || call->name.len == 0 ||
                call->arguments.data == NULL || call->arguments.len == 0) {
                return 0;
            }
        }
        return 1;
    case OI_CLI_MESSAGE_TOOL:
        return message->tool_call_id.data != NULL &&
               message->tool_call_id.len > 0 &&
               message->tool_calls_len == 0 && message->tool_calls == NULL;
    case OI_CLI_MESSAGE_NONE:
        return 0;
    }
    return 0;
}

oi_status oi_cli_message_clone(const struct oi_cli_message *source,
                               struct oi_cli_message *destination) {
    if (!oi_cli_message_is_valid(source) || destination == NULL ||
        source == destination) {
        return OI_ERR_INVAL;
    }

    struct oi_cli_message clone;
    oi_cli_message_init(&clone);
    oi_status st;
    if (source->role == OI_CLI_MESSAGE_USER) {
        st = oi_cli_message_set_user(&clone, source->content.data,
                                     source->content.len);
    } else if (source->role == OI_CLI_MESSAGE_ASSISTANT) {
        st = oi_cli_message_set_assistant(&clone, source->content.data,
                                          source->content.len);
        for (size_t i = 0; st == OI_OK && i < source->tool_calls_len; i++) {
            const struct oi_cli_tool_call_value *call =
                &source->tool_calls[i];
            st = oi_cli_message_add_tool_call(
                &clone, call->id.data, call->id.len, call->name.data,
                call->name.len, call->arguments.data, call->arguments.len);
        }
    } else {
        st = oi_cli_message_set_tool(
            &clone, source->tool_call_id.data, source->tool_call_id.len,
            source->content.data, source->content.len);
    }
    if (st != OI_OK) {
        oi_cli_message_free(&clone);
        return st;
    }

    oi_cli_message_free(destination);
    *destination = clone;
    return OI_OK;
}

void oi_cli_message_list_init(struct oi_cli_message_list *list) {
    if (list != NULL) {
        memset(list, 0, sizeof *list);
    }
}

void oi_cli_message_list_free(struct oi_cli_message_list *list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->len; i++) {
        oi_cli_message_free(&list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof *list);
}

oi_status oi_cli_message_list_append_take(struct oi_cli_message_list *list,
                                           struct oi_cli_message *message) {
    if (list == NULL || !oi_cli_message_is_valid(message)) {
        return OI_ERR_INVAL;
    }
    if (list->len == SIZE_MAX / sizeof *list->items) {
        return OI_ERR_NOMEM;
    }

    if (list->len == list->cap) {
        size_t cap = list->cap == 0 ? 8 : list->cap;
        if (list->cap != 0) {
            if (cap > SIZE_MAX / 2) {
                return OI_ERR_NOMEM;
            }
            cap *= 2;
        }
        if (cap > SIZE_MAX / sizeof *list->items) {
            cap = SIZE_MAX / sizeof *list->items;
        }
        struct oi_cli_message *items =
            realloc(list->items, cap * sizeof *items);
        if (items == NULL) {
            return OI_ERR_NOMEM;
        }
        list->items = items;
        list->cap = cap;
    }

    list->items[list->len++] = *message;
    oi_cli_message_init(message);
    return OI_OK;
}

oi_status oi_cli_message_list_append_clone(
    struct oi_cli_message_list *list, const struct oi_cli_message *message) {
    if (list == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_message clone;
    oi_cli_message_init(&clone);
    oi_status st = oi_cli_message_clone(message, &clone);
    if (st == OI_OK) {
        st = oi_cli_message_list_append_take(list, &clone);
    }
    oi_cli_message_free(&clone);
    return st;
}
