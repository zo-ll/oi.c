#include "cli_history_codec.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oi/json.h"

static oi_status write_key(oi_json_writer *writer, const char *key,
                           size_t key_len) {
    return oi_json_write_object_key(writer, key, key_len);
}

static oi_status write_uint64_string(oi_json_writer *writer, uint64_t value) {
    char number[32];
    int len = snprintf(number, sizeof number, "%" PRIu64, value);
    if (len < 0 || (size_t)len >= sizeof number) {
        return OI_ERR_INVAL;
    }
    return oi_json_write_string(writer, number, (size_t)len);
}

static const char *record_kind_name(enum oi_cli_history_record_kind kind,
                                    size_t *out_len) {
    const char *name = NULL;
    switch (kind) {
    case OI_CLI_HISTORY_RECORD_TRANSITION:
        name = "transition";
        break;
    case OI_CLI_HISTORY_RECORD_MESSAGE:
        name = "message";
        break;
    case OI_CLI_HISTORY_RECORD_TOOL_STARTED:
        name = "tool_started";
        break;
    case OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT:
        name = "partial_assistant";
        break;
    case OI_CLI_HISTORY_RECORD_QUEUED_INPUT:
        name = "queued_input";
        break;
    case OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED:
        name = "queue_resolved";
        break;
    case OI_CLI_HISTORY_RECORD_CHECKPOINT:
        name = "checkpoint";
        break;
    case OI_CLI_HISTORY_RECORD_SESSION_SETTING:
        name = "session_setting";
        break;
    case OI_CLI_HISTORY_RECORD_NONE:
        return NULL;
    }
    if (name == NULL) {
        return NULL;
    }
    *out_len = strlen(name);
    return name;
}

static const char *session_setting_field_name(
    enum oi_cli_history_session_setting_field field, size_t *out_len) {
    const char *name = NULL;
    switch (field) {
    case OI_CLI_HISTORY_SESSION_SETTING_MODEL:
        name = "model";
        break;
    case OI_CLI_HISTORY_SESSION_SETTING_CWD:
        name = "cwd";
        break;
    }
    if (name == NULL) {
        return NULL;
    }
    *out_len = strlen(name);
    return name;
}

static const char *message_role_name(enum oi_cli_message_role role,
                                     size_t *out_len) {
    const char *name = NULL;
    switch (role) {
    case OI_CLI_MESSAGE_USER:
        name = "user";
        break;
    case OI_CLI_MESSAGE_ASSISTANT:
        name = "assistant";
        break;
    case OI_CLI_MESSAGE_TOOL:
        name = "tool";
        break;
    case OI_CLI_MESSAGE_NONE:
        return NULL;
    }
    if (name == NULL) {
        return NULL;
    }
    *out_len = strlen(name);
    return name;
}

static const char *tool_outcome_name(enum oi_cli_history_tool_outcome outcome,
                                     size_t *out_len) {
    const char *name = NULL;
    switch (outcome) {
    case OI_CLI_HISTORY_TOOL_COMPLETED:
        name = "completed";
        break;
    case OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN:
        name = "outcome_unknown";
        break;
    case OI_CLI_HISTORY_TOOL_NOT_EXECUTED:
        name = "not_executed";
        break;
    case OI_CLI_HISTORY_TOOL_OUTCOME_NONE:
        return NULL;
    }
    if (name == NULL) {
        return NULL;
    }
    *out_len = strlen(name);
    return name;
}

static oi_status base64_encode(const unsigned char *data, size_t len,
                               char **out, size_t *out_len) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (len > (SIZE_MAX - 2) / 3) {
        return OI_ERR_NOMEM;
    }
    size_t groups = (len + 2) / 3;
    if (groups > (SIZE_MAX - 1) / 4) {
        return OI_ERR_NOMEM;
    }
    size_t encoded_len = groups * 4;
    char *encoded = malloc(encoded_len + 1);
    if (encoded == NULL) {
        return OI_ERR_NOMEM;
    }

    size_t input = 0;
    size_t output = 0;
    while (input + 3 <= len) {
        uint32_t value = ((uint32_t)data[input] << 16) |
                         ((uint32_t)data[input + 1] << 8) |
                         (uint32_t)data[input + 2];
        encoded[output++] = alphabet[(value >> 18) & 0x3fu];
        encoded[output++] = alphabet[(value >> 12) & 0x3fu];
        encoded[output++] = alphabet[(value >> 6) & 0x3fu];
        encoded[output++] = alphabet[value & 0x3fu];
        input += 3;
    }
    size_t remaining = len - input;
    if (remaining > 0) {
        uint32_t value = (uint32_t)data[input] << 16;
        if (remaining == 2) {
            value |= (uint32_t)data[input + 1] << 8;
        }
        encoded[output++] = alphabet[(value >> 18) & 0x3fu];
        encoded[output++] = alphabet[(value >> 12) & 0x3fu];
        encoded[output++] =
            remaining == 2 ? alphabet[(value >> 6) & 0x3fu] : '=';
        encoded[output++] = '=';
    }
    encoded[output] = '\0';
    *out = encoded;
    *out_len = output;
    return OI_OK;
}

static oi_status write_tool_calls(
    oi_json_writer *writer, const struct oi_cli_message *message) {
    oi_status st = write_key(writer, "tool_calls", 10);
    if (st == OI_OK) {
        st = oi_json_write_array_begin(writer);
    }
    for (size_t i = 0; st == OI_OK && i < message->tool_calls_len; i++) {
        const struct oi_cli_tool_call_value *call = &message->tool_calls[i];
        st = oi_json_write_object_begin(writer);
        if (st == OI_OK) {
            st = write_key(writer, "id", 2);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer, call->id.data, call->id.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "name", 4);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer, call->name.data,
                                      call->name.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "arguments", 9);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer, call->arguments.data,
                                      call->arguments.len);
        }
        if (st == OI_OK) {
            st = oi_json_write_object_end(writer);
        }
    }
    if (st == OI_OK) {
        st = oi_json_write_array_end(writer);
    }
    return st;
}

static oi_status write_message(oi_json_writer *writer,
                               const struct oi_cli_history_message *history) {
    const struct oi_cli_message *message = &history->value;
    size_t name_len;
    const char *name = message_role_name(message->role, &name_len);
    if (name == NULL) {
        return OI_ERR_INVAL;
    }

    oi_status st = write_key(writer, "role", 4);
    if (st == OI_OK) {
        st = oi_json_write_string(writer, name, name_len);
    }
    if (st == OI_OK) {
        st = write_key(writer, "content", 7);
    }
    if (st == OI_OK) {
        st = oi_json_write_string(writer, message->content.data,
                                  message->content.len);
    }
    if (st == OI_OK) {
        st = write_key(writer, "source", 6);
    }
    if (st == OI_OK) {
        name = history->source == OI_CLI_HISTORY_MESSAGE_NORMAL ? "normal"
                                                                : "repair";
        st = oi_json_write_string(writer, name, strlen(name));
    }
    if (st == OI_OK && history->model.data != NULL) {
        st = write_key(writer, "model", 5);
        if (st == OI_OK) {
            st = oi_json_write_string(writer, history->model.data,
                                      history->model.len);
        }
    }
    if (st == OI_OK && message->role == OI_CLI_MESSAGE_ASSISTANT &&
        message->tool_calls_len > 0) {
        st = write_tool_calls(writer, message);
    }
    if (st == OI_OK && message->role == OI_CLI_MESSAGE_TOOL) {
        st = write_key(writer, "tool_call_id", 12);
        if (st == OI_OK) {
            st = oi_json_write_string(writer, message->tool_call_id.data,
                                      message->tool_call_id.len);
        }
        name = tool_outcome_name(history->tool_outcome, &name_len);
        if (st == OI_OK && name == NULL) {
            st = OI_ERR_INVAL;
        }
        if (st == OI_OK) {
            st = write_key(writer, "tool_outcome", 12);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer, name, name_len);
        }
        if (st == OI_OK && history->has_raw_tool_output) {
            char *encoded = NULL;
            size_t encoded_len = 0;
            st = base64_encode(history->raw_tool_output.data,
                               history->raw_tool_output.len, &encoded,
                               &encoded_len);
            if (st == OI_OK) {
                st = write_key(writer, "raw_output_base64", 17);
            }
            if (st == OI_OK) {
                st = oi_json_write_string(writer, encoded, encoded_len);
            }
            free(encoded);
        }
    }
    return st;
}

static oi_status write_payload(oi_json_writer *writer,
                               const struct oi_cli_history_record *record) {
    oi_status st = OI_OK;
    switch (record->kind) {
    case OI_CLI_HISTORY_RECORD_TRANSITION:
        st = write_key(writer, "legacy_record_count", 19);
        if (st == OI_OK) {
            st = write_uint64_string(
                writer, record->as.transition.legacy_record_count);
        }
        break;
    case OI_CLI_HISTORY_RECORD_MESSAGE:
        st = write_message(writer, &record->as.message);
        break;
    case OI_CLI_HISTORY_RECORD_TOOL_STARTED:
        st = write_key(writer, "tool_call_id", 12);
        if (st == OI_OK) {
            st = oi_json_write_string(
                writer, record->as.tool_started.tool_call_id.data,
                record->as.tool_started.tool_call_id.len);
        }
        break;
    case OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT:
        st = write_key(writer, "content", 7);
        if (st == OI_OK) {
            st = oi_json_write_string(
                writer, record->as.partial_assistant.content.data,
                record->as.partial_assistant.content.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "model", 5);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(
                writer, record->as.partial_assistant.model.data,
                record->as.partial_assistant.model.len);
        }
        break;
    case OI_CLI_HISTORY_RECORD_QUEUED_INPUT:
        st = write_key(writer, "content", 7);
        if (st == OI_OK) {
            st = oi_json_write_string(writer,
                                      record->as.queued_input.content.data,
                                      record->as.queued_input.content.len);
        }
        break;
    case OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED: {
        st = write_key(writer, "queued_record_id", 16);
        if (st == OI_OK) {
            st = write_uint64_string(
                writer, record->as.queue_resolved.queued_record_id);
        }
        if (st == OI_OK) {
            st = write_key(writer, "resolution", 10);
        }
        const char *resolution =
            record->as.queue_resolved.resolution ==
                    OI_CLI_HISTORY_QUEUE_CONSUMED
                ? "consumed"
                : "discarded";
        if (st == OI_OK) {
            st = oi_json_write_string(writer, resolution,
                                      strlen(resolution));
        }
        break;
    }
    case OI_CLI_HISTORY_RECORD_CHECKPOINT:
        st = write_key(writer, "summary", 7);
        if (st == OI_OK) {
            st = oi_json_write_string(writer,
                                      record->as.checkpoint.summary.data,
                                      record->as.checkpoint.summary.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "model", 5);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer,
                                      record->as.checkpoint.model.data,
                                      record->as.checkpoint.model.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "source_first_record_id", 22);
        }
        if (st == OI_OK) {
            st = write_uint64_string(
                writer, record->as.checkpoint.source_first_record_id);
        }
        if (st == OI_OK) {
            st = write_key(writer, "source_last_record_id", 21);
        }
        if (st == OI_OK) {
            st = write_uint64_string(
                writer, record->as.checkpoint.source_last_record_id);
        }
        break;
    case OI_CLI_HISTORY_RECORD_SESSION_SETTING: {
        size_t field_len;
        const char *field = session_setting_field_name(
            record->as.session_setting.field, &field_len);
        if (field == NULL) {
            return OI_ERR_INVAL;
        }
        st = write_key(writer, "field", 5);
        if (st == OI_OK) {
            st = oi_json_write_string(writer, field, field_len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "value", 5);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer,
                                      record->as.session_setting.value.data,
                                      record->as.session_setting.value.len);
        }
        break;
    }
    case OI_CLI_HISTORY_RECORD_NONE:
        st = OI_ERR_INVAL;
        break;
    }
    return st;
}

oi_status oi_cli_history_record_encode(
    const struct oi_cli_history_record *record, char **out_json,
    size_t *out_json_len) {
    if (!oi_cli_history_record_is_valid(record) || out_json == NULL ||
        out_json_len == NULL) {
        return OI_ERR_INVAL;
    }
    *out_json = NULL;
    *out_json_len = 0;

    size_t kind_len;
    const char *kind = record_kind_name(record->kind, &kind_len);
    if (kind == NULL) {
        return OI_ERR_INVAL;
    }
    oi_json_writer *writer = oi_json_writer_create();
    if (writer == NULL) {
        return OI_ERR_NOMEM;
    }

    oi_status st = oi_json_write_object_begin(writer);
    if (st == OI_OK) {
        st = write_key(writer, "version", 7);
    }
    if (st == OI_OK) {
        st = oi_json_write_number(writer, record->version);
    }
    if (st == OI_OK) {
        st = write_key(writer, "record_id", 9);
    }
    if (st == OI_OK) {
        st = write_uint64_string(writer, record->record_id);
    }
    if (st == OI_OK) {
        st = write_key(writer, "turn_id", 7);
    }
    if (st == OI_OK) {
        st = write_uint64_string(writer, record->turn_id);
    }
    if (st == OI_OK) {
        st = write_key(writer, "type", 4);
    }
    if (st == OI_OK) {
        st = oi_json_write_string(writer, kind, kind_len);
    }
    if (st == OI_OK) {
        st = write_payload(writer, record);
    }
    if (st == OI_OK) {
        st = oi_json_write_object_end(writer);
    }

    size_t json_len = 0;
    const char *json = oi_json_writer_data(writer, &json_len);
    if (st == OI_OK && json_len > OI_CLI_HISTORY_MAX_RECORD) {
        st = OI_ERR_NOMEM;
    }
    char *copy = NULL;
    if (st == OI_OK) {
        if (json_len == SIZE_MAX) {
            st = OI_ERR_NOMEM;
        } else {
            copy = malloc(json_len + 1);
            if (copy == NULL) {
                st = OI_ERR_NOMEM;
            }
        }
    }
    if (st == OI_OK) {
        memcpy(copy, json, json_len + 1);
        *out_json = copy;
        *out_json_len = json_len;
    }
    oi_json_writer_destroy(writer);
    return st;
}

static int string_equals(const char *value, size_t value_len,
                         const char *expected) {
    size_t expected_len = strlen(expected);
    return value_len == expected_len &&
           memcmp(value, expected, expected_len) == 0;
}

static oi_status get_string_field(const oi_json_value *object,
                                  const char *key, const char **out,
                                  size_t *out_len) {
    oi_json_value *value = oi_json_object_get(object, key);
    if (value == NULL ||
        oi_json_get_string(value, out, out_len) != OI_OK) {
        return OI_ERR_PARSE;
    }
    return OI_OK;
}

static oi_status parse_uint64_field(const oi_json_value *object,
                                    const char *key, uint64_t *out) {
    const char *text;
    size_t len;
    if (get_string_field(object, key, &text, &len) != OI_OK || len == 0 ||
        len > 20 || (len > 1 && text[0] == '0')) {
        return OI_ERR_PARSE;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return OI_ERR_PARSE;
        }
        unsigned int digit = (unsigned int)(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return OI_ERR_PARSE;
        }
        value = value * 10 + digit;
    }
    *out = value;
    return OI_OK;
}

static int base64_value(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z') {
        return byte - 'A';
    }
    if (byte >= 'a' && byte <= 'z') {
        return byte - 'a' + 26;
    }
    if (byte >= '0' && byte <= '9') {
        return byte - '0' + 52;
    }
    if (byte == '+') {
        return 62;
    }
    if (byte == '/') {
        return 63;
    }
    return -1;
}

static oi_status base64_decode(const char *text, size_t len,
                               unsigned char **out, size_t *out_len) {
    if (len % 4 != 0) {
        return OI_ERR_PARSE;
    }
    size_t padding = 0;
    if (len > 0 && text[len - 1] == '=') {
        padding++;
    }
    if (len > 1 && text[len - 2] == '=') {
        padding++;
    }
    if (len / 4 > SIZE_MAX / 3) {
        return OI_ERR_NOMEM;
    }
    size_t decoded_len = (len / 4) * 3 - padding;
    if (decoded_len > OI_CLI_HISTORY_MAX_CONTENT) {
        return OI_ERR_PARSE;
    }
    unsigned char *decoded = malloc(decoded_len > 0 ? decoded_len : 1);
    if (decoded == NULL) {
        return OI_ERR_NOMEM;
    }

    size_t output = 0;
    for (size_t input = 0; input < len; input += 4) {
        int a = base64_value((unsigned char)text[input]);
        int b = base64_value((unsigned char)text[input + 1]);
        int c = text[input + 2] == '='
                    ? 0
                    : base64_value((unsigned char)text[input + 2]);
        int d = text[input + 3] == '='
                    ? 0
                    : base64_value((unsigned char)text[input + 3]);
        int final_group = input + 4 == len;
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (text[input + 2] == '=' &&
             (!final_group || text[input + 3] != '=')) ||
            (text[input + 3] == '=' && !final_group) ||
            (!final_group &&
             (text[input + 2] == '=' || text[input + 3] == '=')) ||
            (text[input + 2] == '=' && (b & 0x0f) != 0) ||
            (text[input + 3] == '=' && text[input + 2] != '=' &&
             (c & 0x03) != 0)) {
            free(decoded);
            return OI_ERR_PARSE;
        }
        uint32_t value = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                         ((uint32_t)c << 6) | (uint32_t)d;
        if (output < decoded_len) {
            decoded[output++] = (unsigned char)(value >> 16);
        }
        if (output < decoded_len) {
            decoded[output++] = (unsigned char)(value >> 8);
        }
        if (output < decoded_len) {
            decoded[output++] = (unsigned char)value;
        }
    }
    *out = decoded;
    *out_len = decoded_len;
    return OI_OK;
}

static oi_status parse_source(const oi_json_value *object,
                              enum oi_cli_history_message_source *out) {
    const char *source;
    size_t source_len;
    if (get_string_field(object, "source", &source, &source_len) != OI_OK) {
        return OI_ERR_PARSE;
    }
    if (string_equals(source, source_len, "normal")) {
        *out = OI_CLI_HISTORY_MESSAGE_NORMAL;
        return OI_OK;
    }
    if (string_equals(source, source_len, "repair")) {
        *out = OI_CLI_HISTORY_MESSAGE_REPAIR;
        return OI_OK;
    }
    return OI_ERR_PARSE;
}

static oi_status parse_tool_calls(const oi_json_value *object,
                                  struct oi_cli_message *message) {
    oi_json_value *calls = oi_json_object_get(object, "tool_calls");
    if (calls == NULL || oi_json_type_of(calls) != OI_JSON_ARRAY ||
        oi_json_array_len(calls) == 0) {
        return OI_ERR_PARSE;
    }
    for (size_t i = 0; i < oi_json_array_len(calls); i++) {
        oi_json_value *call = oi_json_array_get(calls, i);
        if (call == NULL || oi_json_type_of(call) != OI_JSON_OBJECT ||
            oi_json_object_len(call) != 3) {
            return OI_ERR_PARSE;
        }
        const char *id;
        const char *name;
        const char *arguments;
        size_t id_len;
        size_t name_len;
        size_t arguments_len;
        if (get_string_field(call, "id", &id, &id_len) != OI_OK ||
            get_string_field(call, "name", &name, &name_len) != OI_OK ||
            get_string_field(call, "arguments", &arguments,
                             &arguments_len) != OI_OK) {
            return OI_ERR_PARSE;
        }
        oi_status st = oi_cli_message_add_tool_call(
            message, id, id_len, name, name_len, arguments, arguments_len);
        if (st != OI_OK) {
            return st == OI_ERR_NOMEM ? st : OI_ERR_PARSE;
        }
    }
    return OI_OK;
}

static oi_status decode_message(const oi_json_value *object,
                                uint64_t record_id, uint64_t turn_id,
                                struct oi_cli_history_record *out) {
    const char *role;
    const char *content;
    size_t role_len;
    size_t content_len;
    enum oi_cli_history_message_source source;
    if (get_string_field(object, "role", &role, &role_len) != OI_OK ||
        get_string_field(object, "content", &content, &content_len) != OI_OK ||
        parse_source(object, &source) != OI_OK) {
        return OI_ERR_PARSE;
    }

    struct oi_cli_message message;
    oi_cli_message_init(&message);
    oi_status st = OI_ERR_PARSE;
    const char *model = NULL;
    size_t model_len = 0;
    enum oi_cli_history_tool_outcome outcome =
        OI_CLI_HISTORY_TOOL_OUTCOME_NONE;
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    int has_raw = 0;

    if (string_equals(role, role_len, "user")) {
        if (oi_json_object_len(object) != 7 ||
            source != OI_CLI_HISTORY_MESSAGE_NORMAL) {
            goto done;
        }
        st = oi_cli_message_set_user(&message, content, content_len);
    } else if (string_equals(role, role_len, "assistant")) {
        oi_json_value *calls = oi_json_object_get(object, "tool_calls");
        if (source == OI_CLI_HISTORY_MESSAGE_NORMAL) {
            if (oi_json_object_len(object) != (calls == NULL ? 8 : 9) ||
                get_string_field(object, "model", &model, &model_len) !=
                    OI_OK) {
                goto done;
            }
        } else if (oi_json_object_len(object) != 7 || calls != NULL) {
            goto done;
        }
        st = oi_cli_message_set_assistant(&message, content, content_len);
        if (st == OI_OK && calls != NULL) {
            st = parse_tool_calls(object, &message);
        }
    } else if (string_equals(role, role_len, "tool")) {
        oi_json_value *raw_value =
            oi_json_object_get(object, "raw_output_base64");
        size_t expected_fields = raw_value == NULL ? 9 : 10;
        const char *tool_call_id;
        const char *outcome_text;
        size_t tool_call_id_len;
        size_t outcome_len;
        if (oi_json_object_len(object) != expected_fields ||
            get_string_field(object, "tool_call_id", &tool_call_id,
                             &tool_call_id_len) != OI_OK ||
            get_string_field(object, "tool_outcome", &outcome_text,
                             &outcome_len) != OI_OK) {
            goto done;
        }
        if (string_equals(outcome_text, outcome_len, "completed")) {
            outcome = OI_CLI_HISTORY_TOOL_COMPLETED;
        } else if (string_equals(outcome_text, outcome_len,
                                 "outcome_unknown")) {
            outcome = OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN;
        } else if (string_equals(outcome_text, outcome_len,
                                 "not_executed")) {
            outcome = OI_CLI_HISTORY_TOOL_NOT_EXECUTED;
        } else {
            goto done;
        }
        if (source == OI_CLI_HISTORY_MESSAGE_NORMAL) {
            if ((outcome == OI_CLI_HISTORY_TOOL_COMPLETED &&
                 raw_value == NULL) ||
                (outcome == OI_CLI_HISTORY_TOOL_NOT_EXECUTED &&
                 raw_value != NULL)) {
                goto done;
            }
            if (raw_value != NULL) {
                const char *encoded;
                size_t encoded_len;
                if (get_string_field(object, "raw_output_base64", &encoded,
                                     &encoded_len) != OI_OK) {
                    goto done;
                }
                st = base64_decode(encoded, encoded_len, &raw, &raw_len);
                if (st != OI_OK) {
                    goto done;
                }
                has_raw = 1;
            }
        } else if (outcome != OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN &&
                   outcome != OI_CLI_HISTORY_TOOL_NOT_EXECUTED) {
            goto done;
        }
        st = oi_cli_message_set_tool(&message, tool_call_id, tool_call_id_len,
                                     content, content_len);
    } else {
        goto done;
    }

    if (st == OI_OK) {
        st = oi_cli_history_record_set_message(
            out, record_id, turn_id, &message, source, model, model_len,
            outcome, raw, raw_len, has_raw);
        if (st == OI_ERR_INVAL) {
            st = OI_ERR_PARSE;
        }
    }

done:
    free(raw);
    oi_cli_message_free(&message);
    return st;
}

static oi_status decode_payload(const oi_json_value *object, const char *type,
                                size_t type_len, uint64_t record_id,
                                uint64_t turn_id,
                                struct oi_cli_history_record *out) {
    oi_status st = OI_ERR_PARSE;
    if (string_equals(type, type_len, "transition")) {
        uint64_t legacy_count;
        if (oi_json_object_len(object) == 5 &&
            parse_uint64_field(object, "legacy_record_count",
                               &legacy_count) == OI_OK) {
            st = oi_cli_history_record_set_transition(out, record_id,
                                                       legacy_count);
        }
    } else if (string_equals(type, type_len, "message")) {
        st = decode_message(object, record_id, turn_id, out);
    } else if (string_equals(type, type_len, "tool_started")) {
        const char *tool_call_id;
        size_t tool_call_id_len;
        if (oi_json_object_len(object) == 5 &&
            get_string_field(object, "tool_call_id", &tool_call_id,
                             &tool_call_id_len) == OI_OK) {
            st = oi_cli_history_record_set_tool_started(
                out, record_id, turn_id, tool_call_id, tool_call_id_len);
        }
    } else if (string_equals(type, type_len, "partial_assistant")) {
        const char *content;
        const char *model;
        size_t content_len;
        size_t model_len;
        if (oi_json_object_len(object) == 6 &&
            get_string_field(object, "content", &content, &content_len) ==
                OI_OK &&
            get_string_field(object, "model", &model, &model_len) == OI_OK) {
            st = oi_cli_history_record_set_partial_assistant(
                out, record_id, turn_id, content, content_len, model,
                model_len);
        }
    } else if (string_equals(type, type_len, "queued_input")) {
        const char *content;
        size_t content_len;
        if (oi_json_object_len(object) == 5 &&
            get_string_field(object, "content", &content, &content_len) ==
                OI_OK) {
            st = oi_cli_history_record_set_queued_input(
                out, record_id, turn_id, content, content_len);
        }
    } else if (string_equals(type, type_len, "queue_resolved")) {
        uint64_t queued_record_id;
        const char *resolution_text;
        size_t resolution_len;
        enum oi_cli_history_queue_resolution resolution;
        if (oi_json_object_len(object) != 6 ||
            parse_uint64_field(object, "queued_record_id",
                               &queued_record_id) != OI_OK ||
            get_string_field(object, "resolution", &resolution_text,
                             &resolution_len) != OI_OK) {
            return OI_ERR_PARSE;
        }
        if (string_equals(resolution_text, resolution_len, "consumed")) {
            resolution = OI_CLI_HISTORY_QUEUE_CONSUMED;
        } else if (string_equals(resolution_text, resolution_len,
                                 "discarded")) {
            resolution = OI_CLI_HISTORY_QUEUE_DISCARDED;
        } else {
            return OI_ERR_PARSE;
        }
        st = oi_cli_history_record_set_queue_resolved(
            out, record_id, turn_id, queued_record_id, resolution);
    } else if (string_equals(type, type_len, "checkpoint")) {
        const char *summary;
        const char *model;
        size_t summary_len;
        size_t model_len;
        uint64_t first;
        uint64_t last;
        if (oi_json_object_len(object) == 8 &&
            get_string_field(object, "summary", &summary, &summary_len) ==
                OI_OK &&
            get_string_field(object, "model", &model, &model_len) == OI_OK &&
            parse_uint64_field(object, "source_first_record_id", &first) ==
                OI_OK &&
            parse_uint64_field(object, "source_last_record_id", &last) ==
                OI_OK) {
            st = oi_cli_history_record_set_checkpoint(
                out, record_id, summary, summary_len, model, model_len, first,
                last);
        }
    } else if (string_equals(type, type_len, "session_setting")) {
        const char *field_text;
        const char *value;
        size_t field_len;
        size_t value_len;
        enum oi_cli_history_session_setting_field field;
        if (oi_json_object_len(object) != 6 ||
            get_string_field(object, "field", &field_text, &field_len) !=
                OI_OK ||
            get_string_field(object, "value", &value, &value_len) != OI_OK) {
            return OI_ERR_PARSE;
        }
        if (string_equals(field_text, field_len, "model")) {
            field = OI_CLI_HISTORY_SESSION_SETTING_MODEL;
        } else if (string_equals(field_text, field_len, "cwd")) {
            field = OI_CLI_HISTORY_SESSION_SETTING_CWD;
        } else {
            return OI_ERR_PARSE;
        }
        st = oi_cli_history_record_set_session_setting(out, record_id, field,
                                                        value, value_len);
    }
    return st == OI_ERR_INVAL ? OI_ERR_PARSE : st;
}

oi_status oi_cli_history_record_decode(
    const char *json, size_t json_len,
    struct oi_cli_history_record *out_record) {
    if (json == NULL || json_len == 0 ||
        json_len > OI_CLI_HISTORY_MAX_RECORD || out_record == NULL) {
        return OI_ERR_INVAL;
    }
    oi_arena *arena = oi_arena_create(OI_CLI_HISTORY_MAX_RECORD);
    if (arena == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_json_parser *parser = oi_json_parser_create(arena);
    if (parser == NULL) {
        oi_arena_destroy(arena);
        return OI_ERR_NOMEM;
    }
    oi_status st = oi_json_parser_feed(parser, json, json_len);
    if (st == OI_OK) {
        st = oi_json_parser_finish(parser);
    }
    oi_json_value *object = oi_json_parser_root(parser);
    if (st == OI_OK &&
        (object == NULL || oi_json_type_of(object) != OI_JSON_OBJECT)) {
        st = OI_ERR_PARSE;
    }

    double version;
    uint64_t record_id;
    uint64_t turn_id;
    const char *type;
    size_t type_len;
    if (st == OI_OK &&
        (oi_json_get_number(oi_json_object_get(object, "version"),
                            &version) != OI_OK ||
         version != OI_CLI_HISTORY_SCHEMA_VERSION ||
         parse_uint64_field(object, "record_id", &record_id) != OI_OK ||
         parse_uint64_field(object, "turn_id", &turn_id) != OI_OK ||
         get_string_field(object, "type", &type, &type_len) != OI_OK)) {
        st = OI_ERR_PARSE;
    }

    struct oi_cli_history_record replacement;
    oi_cli_history_record_init(&replacement);
    if (st == OI_OK) {
        st = decode_payload(object, type, type_len, record_id, turn_id,
                            &replacement);
    }
    if (st == OI_OK) {
        oi_cli_history_record_free(out_record);
        *out_record = replacement;
        oi_cli_history_record_init(&replacement);
    }
    oi_cli_history_record_free(&replacement);
    oi_json_parser_destroy(parser);
    oi_arena_destroy(arena);
    return st;
}
