#include "cli_session_metadata_codec.h"

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

static oi_status write_int64_string(oi_json_writer *writer, int64_t value) {
    char number[32];
    int len = snprintf(number, sizeof number, "%" PRId64, value);
    if (len < 0 || (size_t)len >= sizeof number) {
        return OI_ERR_INVAL;
    }
    return oi_json_write_string(writer, number, (size_t)len);
}

oi_status oi_cli_session_metadata_encode(
    const struct oi_cli_session_metadata *metadata, char **out_json,
    size_t *out_json_len) {
    if (!oi_cli_session_metadata_is_valid(metadata) || out_json == NULL ||
        out_json_len == NULL) {
        return OI_ERR_INVAL;
    }
    *out_json = NULL;
    *out_json_len = 0;

    oi_json_writer *writer = oi_json_writer_create();
    if (writer == NULL) {
        return OI_ERR_NOMEM;
    }

    oi_status st = oi_json_write_object_begin(writer);
    if (st == OI_OK) {
        st = write_key(writer, "version", 7);
    }
    if (st == OI_OK) {
        st = oi_json_write_number(writer, metadata->version);
    }
    if (st == OI_OK) {
        st = write_key(writer, "session_id", 10);
    }
    if (st == OI_OK) {
        st = oi_json_write_string(writer, metadata->session_id.data,
                                  metadata->session_id.len);
    }
    if (st == OI_OK) {
        st = write_key(writer, "model", 5);
    }
    if (st == OI_OK) {
        st = oi_json_write_string(writer, metadata->model.data,
                                  metadata->model.len);
    }
    if (st == OI_OK) {
        st = write_key(writer, "cwd", 3);
    }
    if (st == OI_OK) {
        st = oi_json_write_string(writer, metadata->cwd.data,
                                  metadata->cwd.len);
    }
    if (st == OI_OK) {
        st = write_key(writer, "created_at", 10);
    }
    if (st == OI_OK) {
        st = write_int64_string(writer, metadata->created_at);
    }
    if (st == OI_OK) {
        st = write_key(writer, "updated_at", 10);
    }
    if (st == OI_OK) {
        st = write_int64_string(writer, metadata->updated_at);
    }
    if (st == OI_OK) {
        st = oi_json_write_object_end(writer);
    }

    size_t json_len = 0;
    const char *json = oi_json_writer_data(writer, &json_len);
    if (st == OI_OK && json_len > OI_CLI_SESSION_METADATA_MAX_FILE) {
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

static oi_status get_string_field(const oi_json_value *object, const char *key,
                                  const char **out, size_t *out_len) {
    oi_json_value *value = oi_json_object_get(object, key);
    if (value == NULL || oi_json_get_string(value, out, out_len) != OI_OK) {
        return OI_ERR_PARSE;
    }
    return OI_OK;
}

static oi_status parse_int64_field(const oi_json_value *object,
                                   const char *key, int64_t *out) {
    const char *text;
    size_t len;
    size_t start = 0;
    int negative = 0;
    uint64_t magnitude = 0;

    if (get_string_field(object, key, &text, &len) != OI_OK || len == 0) {
        return OI_ERR_PARSE;
    }
    if (text[0] == '-') {
        negative = 1;
        start = 1;
    }
    if (start >= len || len - start > 20 ||
        (len - start > 1 && text[start] == '0')) {
        return OI_ERR_PARSE;
    }
    for (size_t i = start; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return OI_ERR_PARSE;
        }
        unsigned int digit = (unsigned int)(text[i] - '0');
        if (magnitude > (UINT64_MAX - digit) / 10) {
            return OI_ERR_PARSE;
        }
        magnitude = magnitude * 10 + digit;
    }
    if (negative) {
        if (magnitude > (uint64_t)INT64_MAX + 1) {
            return OI_ERR_PARSE;
        }
        *out = magnitude == (uint64_t)INT64_MAX + 1 ? INT64_MIN
                                                     : -(int64_t)magnitude;
    } else {
        if (magnitude > (uint64_t)INT64_MAX) {
            return OI_ERR_PARSE;
        }
        *out = (int64_t)magnitude;
    }
    return OI_OK;
}

oi_status oi_cli_session_metadata_decode(
    const char *json, size_t json_len,
    struct oi_cli_session_metadata *out_metadata) {
    if (json == NULL || json_len == 0 ||
        json_len > OI_CLI_SESSION_METADATA_MAX_FILE || out_metadata == NULL) {
        return OI_ERR_INVAL;
    }
    oi_arena *arena = oi_arena_create(OI_CLI_SESSION_METADATA_MAX_FILE);
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
        (object == NULL || oi_json_type_of(object) != OI_JSON_OBJECT ||
         oi_json_object_len(object) != 6)) {
        st = OI_ERR_PARSE;
    }

    double version = 0;
    const char *session_id = NULL;
    const char *model = NULL;
    const char *cwd = NULL;
    size_t session_id_len = 0;
    size_t model_len = 0;
    size_t cwd_len = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    if (st == OI_OK &&
        (oi_json_get_number(oi_json_object_get(object, "version"),
                            &version) != OI_OK ||
         version != OI_CLI_SESSION_METADATA_SCHEMA_VERSION ||
         get_string_field(object, "session_id", &session_id,
                          &session_id_len) != OI_OK ||
         get_string_field(object, "model", &model, &model_len) != OI_OK ||
         get_string_field(object, "cwd", &cwd, &cwd_len) != OI_OK ||
         parse_int64_field(object, "created_at", &created_at) != OI_OK ||
         parse_int64_field(object, "updated_at", &updated_at) != OI_OK)) {
        st = OI_ERR_PARSE;
    }

    struct oi_cli_session_metadata replacement;
    oi_cli_session_metadata_init(&replacement);
    if (st == OI_OK) {
        st = oi_cli_session_metadata_set(&replacement, session_id,
                                         session_id_len, model, model_len,
                                         cwd, cwd_len, created_at,
                                         updated_at);
        if (st == OI_ERR_INVAL) {
            st = OI_ERR_PARSE;
        }
    }
    if (st == OI_OK) {
        oi_cli_session_metadata_free(out_metadata);
        *out_metadata = replacement;
        oi_cli_session_metadata_init(&replacement);
    }
    oi_cli_session_metadata_free(&replacement);
    oi_json_parser_destroy(parser);
    oi_arena_destroy(arena);
    return st;
}
