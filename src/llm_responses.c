#include "llm_responses.h"

#include <string.h>

#include "json_internal.h"

#define WRITE(expr)                                                           \
    do {                                                                      \
        oi_status write_status = (expr);                                      \
        if (write_status != OI_OK) {                                          \
            return write_status;                                              \
        }                                                                     \
    } while (0)

static int string_equals(const oi_json_value *value, const char *expected) {
    const char *text;
    size_t len;
    return oi_json_get_string(value, &text, &len) == OI_OK &&
           len == strlen(expected) && memcmp(text, expected, len) == 0;
}

static int non_empty_string(const oi_json_value *value) {
    const char *text;
    size_t len;
    return oi_json_get_string(value, &text, &len) == OI_OK && len > 0;
}

/* The parser bounds nesting depth, so this recursion is bounded too. */
static oi_status write_value(oi_json_writer *writer,
                             const oi_json_value *value) {
    switch (value->type) {
    case OI_JSON_NULL:
        return oi_json_write_null(writer);
    case OI_JSON_BOOL:
        return oi_json_write_bool(writer, value->u.boolean);
    case OI_JSON_NUMBER:
        return oi_json_write_number(writer, value->u.number);
    case OI_JSON_STRING:
        return oi_json_write_string(writer, value->u.string.ptr,
                                    value->u.string.len);
    case OI_JSON_ARRAY:
        WRITE(oi_json_write_array_begin(writer));
        for (size_t i = 0; i < value->u.array.count; i++) {
            WRITE(write_value(writer, value->u.array.items[i]));
        }
        return oi_json_write_array_end(writer);
    case OI_JSON_OBJECT:
        WRITE(oi_json_write_object_begin(writer));
        for (const struct oi_json_member *member = value->u.object.head;
             member != NULL; member = member->next) {
            WRITE(oi_json_write_object_key(writer, member->key,
                                           member->key_len));
            WRITE(write_value(writer, member->value));
        }
        return oi_json_write_object_end(writer);
    }
    return OI_ERR_PARSE;
}

static oi_status write_literal_member(oi_json_writer *writer, const char *key,
                                      const char *value) {
    WRITE(oi_json_write_object_key(writer, key, strlen(key)));
    return oi_json_write_string(writer, value, strlen(value));
}

static oi_status write_string_member(oi_json_writer *writer, const char *key,
                                     const oi_json_value *value) {
    if (value == NULL || oi_json_type_of(value) != OI_JSON_STRING) {
        return OI_ERR_PARSE;
    }
    WRITE(oi_json_write_object_key(writer, key, strlen(key)));
    return write_value(writer, value);
}

static oi_status write_message_item(oi_json_writer *writer, const char *role,
                                    const oi_json_value *content) {
    WRITE(oi_json_write_object_begin(writer));
    WRITE(write_literal_member(writer, "type", "message"));
    WRITE(write_literal_member(writer, "role", role));
    WRITE(write_string_member(writer, "content", content));
    return oi_json_write_object_end(writer);
}

static oi_status write_function_calls(oi_json_writer *writer,
                                      const oi_json_value *calls) {
    if (calls == NULL) {
        return OI_OK;
    }
    if (oi_json_type_of(calls) != OI_JSON_ARRAY) {
        return OI_ERR_PARSE;
    }
    for (size_t i = 0; i < oi_json_array_len(calls); i++) {
        const oi_json_value *call = oi_json_array_get(calls, i);
        const oi_json_value *function = oi_json_object_get(call, "function");
        WRITE(oi_json_write_object_begin(writer));
        WRITE(write_literal_member(writer, "type", "function_call"));
        WRITE(write_string_member(writer, "call_id",
                                  oi_json_object_get(call, "id")));
        WRITE(write_string_member(writer, "name",
                                  oi_json_object_get(function, "name")));
        WRITE(write_string_member(writer, "arguments",
                                  oi_json_object_get(function, "arguments")));
        WRITE(oi_json_write_object_end(writer));
    }
    return OI_OK;
}

static oi_status write_input_items(oi_json_writer *writer,
                                   const oi_json_value *message) {
    const oi_json_value *role = oi_json_object_get(message, "role");
    const oi_json_value *content = oi_json_object_get(message, "content");
    if (string_equals(role, "tool")) {
        WRITE(oi_json_write_object_begin(writer));
        WRITE(write_literal_member(writer, "type", "function_call_output"));
        WRITE(write_string_member(writer, "call_id",
                                  oi_json_object_get(message, "tool_call_id")));
        WRITE(write_string_member(writer, "output", content));
        return oi_json_write_object_end(writer);
    }
    if (string_equals(role, "assistant")) {
        /* A tool-calling turn stores empty content; an empty message item
         * would add a turn the model never produced. */
        if (non_empty_string(content)) {
            WRITE(write_message_item(writer, "assistant", content));
        }
        return write_function_calls(writer,
                                    oi_json_object_get(message, "tool_calls"));
    }
    if (string_equals(role, "system") || string_equals(role, "developer")) {
        return write_message_item(writer, "developer", content);
    }
    if (string_equals(role, "user")) {
        return write_message_item(writer, "user", content);
    }
    return OI_ERR_PARSE;
}

static oi_status write_tools(oi_json_writer *writer,
                             const oi_json_value *tools) {
    if (tools == NULL) {
        return OI_OK;
    }
    if (oi_json_type_of(tools) != OI_JSON_ARRAY) {
        return OI_ERR_PARSE;
    }
    WRITE(oi_json_write_object_key(writer, "tools", 5));
    WRITE(oi_json_write_array_begin(writer));
    for (size_t i = 0; i < oi_json_array_len(tools); i++) {
        const oi_json_value *tool = oi_json_array_get(tools, i);
        const oi_json_value *function = oi_json_object_get(tool, "function");
        const oi_json_value *description =
            oi_json_object_get(function, "description");
        const oi_json_value *parameters =
            oi_json_object_get(function, "parameters");
        if (!string_equals(oi_json_object_get(tool, "type"), "function")) {
            return OI_ERR_PARSE;
        }
        WRITE(oi_json_write_object_begin(writer));
        WRITE(write_literal_member(writer, "type", "function"));
        WRITE(write_string_member(writer, "name",
                                  oi_json_object_get(function, "name")));
        if (description != NULL) {
            WRITE(write_string_member(writer, "description", description));
        }
        if (parameters != NULL) {
            WRITE(oi_json_write_object_key(writer, "parameters", 10));
            WRITE(write_value(writer, parameters));
        }
        /* Chat tool schemas make no strict-mode promise. */
        WRITE(oi_json_write_object_key(writer, "strict", 6));
        WRITE(oi_json_write_bool(writer, 0));
        WRITE(oi_json_write_object_end(writer));
    }
    return oi_json_write_array_end(writer);
}

static oi_status write_responses_body(oi_json_writer *writer,
                                      const oi_json_value *root) {
    const oi_json_value *messages = oi_json_object_get(root, "messages");
    if (messages == NULL || oi_json_type_of(messages) != OI_JSON_ARRAY) {
        return OI_ERR_PARSE;
    }
    WRITE(oi_json_write_object_begin(writer));
    WRITE(write_string_member(writer, "model",
                              oi_json_object_get(root, "model")));
    WRITE(oi_json_write_object_key(writer, "store", 5));
    WRITE(oi_json_write_bool(writer, 0));
    WRITE(oi_json_write_object_key(writer, "stream", 6));
    WRITE(oi_json_write_bool(writer, 1));
    WRITE(oi_json_write_object_key(writer, "input", 5));
    WRITE(oi_json_write_array_begin(writer));
    for (size_t i = 0; i < oi_json_array_len(messages); i++) {
        WRITE(write_input_items(writer, oi_json_array_get(messages, i)));
    }
    WRITE(oi_json_write_array_end(writer));
    WRITE(write_tools(writer, oi_json_object_get(root, "tools")));
    return oi_json_write_object_end(writer);
}

oi_status oi_llm_responses_translate_body(oi_arena *arena, const char *body,
                                          size_t body_len,
                                          oi_json_writer **out_writer) {
    oi_json_parser *parser = oi_json_parser_create(arena);
    if (parser == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_status st = oi_json_parser_feed(parser, body, body_len);
    if (st == OI_OK) {
        st = oi_json_parser_finish(parser);
    }
    oi_json_value *root = oi_json_parser_root(parser);
    oi_json_parser_destroy(parser);
    if (st != OI_OK) {
        return st;
    }
    if (root == NULL) {
        return OI_ERR_PARSE;
    }

    oi_json_writer *writer = oi_json_writer_create();
    if (writer == NULL) {
        return OI_ERR_NOMEM;
    }
    st = write_responses_body(writer, root);
    if (st != OI_OK) {
        oi_json_writer_destroy(writer);
        return st;
    }
    *out_writer = writer;
    return OI_OK;
}
