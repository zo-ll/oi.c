#include "cli_history_store.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli_history_codec.h"

struct load_context {
    struct oi_cli_history_store *store;
    oi_status status;
    int saw_transition;
};

static int has_typed_prefix(const void *data, size_t len) {
    static const char prefix[] = "{\"version\"";
    return len >= sizeof prefix - 1 &&
           memcmp(data, prefix, sizeof prefix - 1) == 0;
}

static oi_status append_legacy(struct oi_cli_history_store *store,
                               const void *data, size_t len) {
    if (len > OI_CLI_HISTORY_MAX_CONTENT) {
        return OI_ERR_PARSE;
    }
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    oi_status st;
    if (store->legacy_messages.len % 2 == 0) {
        st = oi_cli_message_set_user(&message, data, len);
    } else {
        st = oi_cli_message_set_assistant(&message, data, len);
    }
    if (st == OI_OK) {
        st = oi_cli_message_list_append_take(&store->legacy_messages,
                                              &message);
    }
    oi_cli_message_free(&message);
    return st;
}

static void load_record(const void *data, size_t len, void *user_data) {
    struct load_context *context = user_data;
    if (context->status != OI_OK) {
        return;
    }

    struct oi_cli_history_record record;
    oi_cli_history_record_init(&record);
    oi_status st = oi_cli_history_record_decode(data, len, &record);
    if (!context->saw_transition) {
        if (st == OI_OK) {
            if (record.kind != OI_CLI_HISTORY_RECORD_TRANSITION ||
                record.as.transition.legacy_record_count !=
                    context->store->legacy_messages.len) {
                st = OI_ERR_PARSE;
            } else {
                st = oi_cli_history_append_take(
                    &context->store->typed_history, &record);
                if (st == OI_OK) {
                    context->saw_transition = 1;
                }
            }
        } else if (st == OI_ERR_PARSE || st == OI_ERR_INVAL) {
            st = has_typed_prefix(data, len)
                     ? OI_ERR_PARSE
                     : append_legacy(context->store, data, len);
        }
    } else if (st == OI_OK) {
        if (record.kind == OI_CLI_HISTORY_RECORD_TRANSITION) {
            st = OI_ERR_PARSE;
        } else {
            st = oi_cli_history_append_take(
                &context->store->typed_history, &record);
        }
    }
    oi_cli_history_record_free(&record);
    context->status = st;
}

void oi_cli_history_store_init(struct oi_cli_history_store *store) {
    if (store == NULL) {
        return;
    }
    memset(store, 0, sizeof *store);
    oi_cli_message_list_init(&store->legacy_messages);
    oi_cli_history_init(&store->typed_history);
}

void oi_cli_history_store_free(struct oi_cli_history_store *store) {
    if (store == NULL) {
        return;
    }
    oi_cli_message_list_free(&store->legacy_messages);
    oi_cli_history_free(&store->typed_history);
    memset(store, 0, sizeof *store);
}

oi_status oi_cli_history_store_load(
    oi_sesslog *log, struct oi_cli_history_store *out_store,
    struct oi_cli_history_replay_state *out_state) {
    if (log == NULL || out_store == NULL || out_state == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    store.log = log;
    struct load_context context = {
        .store = &store,
        .status = OI_OK,
    };
    oi_status st = oi_sesslog_replay(log, load_record, &context);
    if (st == OI_OK) {
        st = context.status;
    }
    if (st == OI_OK) {
        st = oi_cli_history_replay(&store.legacy_messages,
                                   &store.typed_history, &state);
    }
    if (st == OI_OK) {
        oi_cli_history_store_free(out_store);
        *out_store = store;
        oi_cli_history_store_init(&store);
        oi_cli_history_replay_state_free(out_state);
        *out_state = state;
        oi_cli_history_replay_state_init(&state);
    }
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_store_free(&store);
    return st;
}

static void typed_history_pop(struct oi_cli_history *history) {
    if (history->len == 0) {
        return;
    }
    history->len--;
    oi_cli_history_record_free(&history->records[history->len]);
}

oi_status oi_cli_history_store_append(
    struct oi_cli_history_store *store,
    const struct oi_cli_history_record *record,
    struct oi_cli_history_replay_state *out_state) {
    if (store == NULL || store->log == NULL || out_state == NULL ||
        !oi_cli_history_record_is_valid(record)) {
        return OI_ERR_INVAL;
    }

    struct oi_cli_history_record clone;
    struct oi_cli_history_replay_state state;
    oi_cli_history_record_init(&clone);
    oi_cli_history_replay_state_init(&state);
    char *json = NULL;
    size_t json_len = 0;
    oi_status st = oi_cli_history_record_clone(record, &clone);
    if (st == OI_OK) {
        st = oi_cli_history_reserve(&store->typed_history,
                                    store->typed_history.len + 1);
    }
    if (st == OI_OK) {
        st = oi_cli_history_append_take(&store->typed_history, &clone);
    }
    if (st == OI_OK) {
        st = oi_cli_history_replay(&store->legacy_messages,
                                   &store->typed_history, &state);
        if (st != OI_OK) {
            typed_history_pop(&store->typed_history);
        }
    }
    if (st == OI_OK) {
        st = oi_cli_history_record_encode(record, &json, &json_len);
        if (st != OI_OK) {
            typed_history_pop(&store->typed_history);
        }
    }
    if (st == OI_OK) {
        st = oi_sesslog_append(store->log, json, json_len);
        if (st != OI_OK) {
            typed_history_pop(&store->typed_history);
        }
    }
    if (st == OI_OK) {
        oi_cli_history_replay_state_free(out_state);
        *out_state = state;
        oi_cli_history_replay_state_init(&state);
    }
    free(json);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&clone);
    return st;
}
