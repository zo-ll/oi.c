#include "oi/llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "compat.h"
#include "llm_conn.h"
#include "llm_http.h"
#include "llm_sse.h"
#include "oi/json.h"

#define OI_LLM_MAX_ERROR_BODY (64 * 1024)

struct oi_llm_client {
    char *host;
    unsigned short port;
    int use_tls;
    char *ca_file;
    char *api_key;
    char *path;
    int timeout_ms;
};

struct oi_llm_tool_state {
    size_t index;
    char *id;
    size_t id_len;
    char *name;
    size_t name_len;
    char *arguments;
    size_t arguments_len;
};

struct oi_llm_request {
    oi_llm_client *client; /* borrowed */
    oi_arena *arena;       /* borrowed */

    oi_llm_delta_cb on_delta;
    oi_llm_event_cb on_event;
    oi_llm_done_cb on_done;
    void *user_data;

    oi_llm_conn *conn;
    oi_reactor_timer *timer;
    oi_llm_http_parser *http;
    oi_llm_sse_parser *sse; /* created lazily once we know status is 2xx */
    oi_json_parser *json;   /* reused (reset) across SSE events */

    char *body;
    size_t body_len;

    int http_status;
    int is_success_status;

    char *error_buf;
    size_t error_len;
    size_t error_cap;

    /* Set by req_on_body/handle_sse_event when a sub-step fails; since
     * oi_llm_http_body_cb is void-returning, this is how that failure
     * gets back to req_on_data, which is the only place actually
     * driving completion. */
    int body_cb_failed;
    oi_status body_cb_status;
    int saw_done;

    struct oi_llm_tool_state *tools;
    size_t tools_len;

    int finished;

    /* Set by req_on_data before invoking oi_llm_http_parser_feed (which
     * may synchronously call all the way down through the SSE/JSON
     * layers into the caller's on_delta), so a reentrant
     * oi_llm_request_cancel() from within on_delta can signal back that
     * `req` itself was freed. Mirrors the same pattern already used in
     * oi_llm_conn, oi_llm_http_parser, and oi_llm_sse_parser. */
    int *destroyed_flag;
};

/* ================= client ================= */

static int valid_http_host(const char *host) {
    if (host[0] == '\0') {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)host; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static int valid_http_path(const char *path) {
    if (path[0] != '/') {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p <= 0x20 || *p == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static int valid_http_field_value(const char *value) {
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return 0;
        }
    }
    return 1;
}

oi_llm_client *oi_llm_client_create(const struct oi_llm_config *cfg) {
    if (cfg == NULL || cfg->host == NULL || cfg->path == NULL ||
        cfg->port == 0 || cfg->timeout_ms < 0 ||
        !valid_http_host(cfg->host) || !valid_http_path(cfg->path) ||
        (cfg->api_key != NULL && !valid_http_field_value(cfg->api_key))) {
        return NULL;
    }

    oi_llm_client *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }

    c->host = strdup(cfg->host);
    c->path = strdup(cfg->path);
    c->port = cfg->port;
    c->use_tls = cfg->use_tls;
    c->ca_file = cfg->ca_file ? strdup(cfg->ca_file) : NULL;
    c->api_key = cfg->api_key ? strdup(cfg->api_key) : NULL;
    c->timeout_ms = cfg->timeout_ms;

    if (c->host == NULL || c->path == NULL ||
        (cfg->ca_file != NULL && c->ca_file == NULL) ||
        (cfg->api_key != NULL && c->api_key == NULL)) {
        oi_llm_client_destroy(c);
        return NULL;
    }
    return c;
}

void oi_llm_client_destroy(oi_llm_client *c) {
    if (c == NULL) {
        return;
    }
    free(c->host);
    free(c->ca_file);
    free(c->api_key);
    free(c->path);
    free(c);
}

/* ================= request lifecycle ================= */

static void request_teardown(oi_llm_request *req) {
    oi_reactor_timer_cancel(req->timer);
    if (req->conn) {
        oi_llm_conn_close(req->conn);
    }
    oi_llm_http_parser_destroy(req->http);
    oi_llm_sse_parser_destroy(req->sse);
    oi_json_parser_destroy(req->json);
    free(req->error_buf);
    free(req->body);
    for (size_t i = 0; i < req->tools_len; i++) {
        free(req->tools[i].id);
        free(req->tools[i].name);
        free(req->tools[i].arguments);
    }
    free(req->tools);
    if (req->destroyed_flag) {
        *req->destroyed_flag = 1;
    }
    free(req);
}

static void finish(oi_llm_request *req, oi_status status, int http_status,
                    const char *error_body, size_t error_body_len) {
    if (req->finished) {
        return;
    }
    req->finished = 1;
    if (req->on_done) {
        req->on_done(status, http_status, error_body, error_body_len,
                     req->user_data);
    }
    request_teardown(req);
}

void oi_llm_request_cancel(oi_llm_request *req) {
    if (req == NULL || req->finished) {
        return;
    }
    req->finished = 1;
    request_teardown(req);
}

static void request_timed_out(oi_reactor *reactor, void *ud) {
    (void)reactor;
    oi_llm_request *req = ud;
    finish(req, OI_ERR_TIMEOUT, req->http_status, req->error_buf,
           req->error_len);
}

/* ================= error-body accumulation ================= */

static oi_status error_buf_append(oi_llm_request *req, const void *data,
                                   size_t len) {
    if (req->error_len >= OI_LLM_MAX_ERROR_BODY) {
        return OI_OK; /* silently truncated; we already have plenty */
    }
    size_t take = len;
    if (take > OI_LLM_MAX_ERROR_BODY - req->error_len) {
        take = OI_LLM_MAX_ERROR_BODY - req->error_len;
    }

    size_t needed = req->error_len + take;
    if (needed > req->error_cap) {
        size_t new_cap = req->error_cap == 0 ? 256 : req->error_cap;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char *nb = realloc(req->error_buf, new_cap);
        if (nb == NULL) {
            return OI_ERR_NOMEM;
        }
        req->error_buf = nb;
        req->error_cap = new_cap;
    }

    memcpy(req->error_buf + req->error_len, data, take);
    req->error_len += take;
    return OI_OK;
}

/* ================= SSE event -> delta extraction ================= */

static oi_status append_fragment(char **buf, size_t *used, const char *data,
                                  size_t len) {
    if (len > SIZE_MAX - *used - 1) {
        return OI_ERR_NOMEM;
    }
    char *next = realloc(*buf, *used + len + 1);
    if (next == NULL) {
        return OI_ERR_NOMEM;
    }
    if (len > 0) {
        memcpy(next + *used, data, len);
    }
    *used += len;
    next[*used] = '\0';
    *buf = next;
    return OI_OK;
}

static struct oi_llm_tool_state *tool_state(oi_llm_request *req,
                                             size_t index) {
    for (size_t i = 0; i < req->tools_len; i++) {
        if (req->tools[i].index == index) {
            return &req->tools[i];
        }
    }
    if (req->tools_len == SIZE_MAX / sizeof *req->tools) {
        return NULL;
    }
    size_t count = req->tools_len + 1;
    struct oi_llm_tool_state *next =
        realloc(req->tools, count * sizeof *next);
    if (next == NULL) {
        return NULL;
    }
    req->tools = next;
    struct oi_llm_tool_state *state = &next[req->tools_len];
    memset(state, 0, sizeof *state);
    state->index = index;
    req->tools_len = count;
    return state;
}

static oi_status json_size_index(const oi_json_value *value, size_t *out) {
    double number;
    if (oi_json_get_number(value, &number) != OI_OK || number < 0 ||
        number > (double)SIZE_MAX) {
        return OI_ERR_PARSE;
    }
    size_t index = (size_t)number;
    if ((double)index != number) {
        return OI_ERR_PARSE;
    }
    *out = index;
    return OI_OK;
}

static oi_status optional_string(const oi_json_value *object, const char *key,
                                  const char **out, size_t *out_len) {
    oi_json_value *value = oi_json_object_get(object, key);
    if (value == NULL) {
        *out = NULL;
        *out_len = 0;
        return OI_OK;
    }
    return oi_json_get_string(value, out, out_len) == OI_OK ? OI_OK
                                                            : OI_ERR_PARSE;
}

static int emit_event(oi_llm_request *req, const oi_llm_event *event) {
    if (req->on_event == NULL) {
        return 0;
    }
    int *destroyed = req->destroyed_flag;
    req->on_event(event, req->user_data);
    return destroyed != NULL && *destroyed;
}

static oi_status handle_tool_calls(oi_llm_request *req,
                                    const oi_json_value *tool_calls) {
    if (tool_calls == NULL) {
        return OI_OK;
    }
    if (oi_json_type_of(tool_calls) != OI_JSON_ARRAY) {
        return OI_ERR_PARSE;
    }

    size_t count = oi_json_array_len(tool_calls);
    for (size_t i = 0; i < count; i++) {
        oi_json_value *call = oi_json_array_get(tool_calls, i);
        oi_json_value *function = oi_json_object_get(call, "function");
        size_t index;
        if (call == NULL || oi_json_type_of(call) != OI_JSON_OBJECT ||
            json_size_index(oi_json_object_get(call, "index"), &index) !=
                OI_OK ||
            (function != NULL &&
             oi_json_type_of(function) != OI_JSON_OBJECT)) {
            return OI_ERR_PARSE;
        }

        const char *id = NULL;
        const char *name = NULL;
        const char *arguments = NULL;
        const char *type = NULL;
        size_t id_len = 0;
        size_t name_len = 0;
        size_t arguments_len = 0;
        size_t type_len = 0;
        if (optional_string(call, "id", &id, &id_len) != OI_OK ||
            optional_string(call, "type", &type, &type_len) != OI_OK ||
            (function != NULL &&
             (optional_string(function, "name", &name, &name_len) != OI_OK ||
              optional_string(function, "arguments", &arguments,
                              &arguments_len) != OI_OK)) ||
            (type != NULL &&
             (type_len != 8 || memcmp(type, "function", 8) != 0))) {
            return OI_ERR_PARSE;
        }

        struct oi_llm_tool_state *state = tool_state(req, index);
        if (state == NULL) {
            return OI_ERR_NOMEM;
        }
        oi_status st = OI_OK;
        if (id != NULL) {
            st = append_fragment(&state->id, &state->id_len, id, id_len);
        }
        if (st == OI_OK && name != NULL) {
            st = append_fragment(&state->name, &state->name_len, name,
                                 name_len);
        }
        if (st == OI_OK && arguments != NULL) {
            st = append_fragment(&state->arguments, &state->arguments_len,
                                 arguments, arguments_len);
        }
        if (st != OI_OK) {
            return st;
        }

        oi_llm_event event = {
            .type = OI_LLM_EVENT_TOOL_CALL,
            .as.tool_call = {index, id, id_len, name, name_len, arguments,
                             arguments_len},
        };
        if (emit_event(req, &event)) {
            return OI_ERR_CLOSED;
        }
    }
    return OI_OK;
}

static oi_status validate_tool_calls(oi_llm_request *req) {
    for (size_t i = 0; i < req->tools_len; i++) {
        struct oi_llm_tool_state *tool = &req->tools[i];
        if (tool->id_len == 0 || tool->name_len == 0 ||
            tool->arguments_len == 0) {
            return OI_ERR_PARSE;
        }
        oi_json_parser_reset(req->json);
        oi_status st =
            oi_json_parser_feed(req->json, tool->arguments, tool->arguments_len);
        if (st == OI_OK) {
            st = oi_json_parser_finish(req->json);
        }
        oi_json_value *root = oi_json_parser_root(req->json);
        if (st != OI_OK || !oi_json_parser_done(req->json) || root == NULL ||
            oi_json_type_of(root) != OI_JSON_OBJECT) {
            return OI_ERR_PARSE;
        }
    }
    return OI_OK;
}

static void handle_sse_event(const char *data, size_t len, void *ud) {
    oi_llm_request *req = ud;
    if (req->body_cb_failed) {
        return;
    }
    if (len == 6 && memcmp(data, "[DONE]", 6) == 0) {
        if (req->saw_done) {
            req->body_cb_failed = 1;
            req->body_cb_status = OI_ERR_PARSE;
            return;
        }
        oi_status st = validate_tool_calls(req);
        if (st != OI_OK) {
            req->body_cb_failed = 1;
            req->body_cb_status = st;
            return;
        }
        req->saw_done = 1;
        return; /* sentinel, not JSON; body_done drives actual completion */
    }
    if (req->saw_done) {
        req->body_cb_failed = 1;
        req->body_cb_status = OI_ERR_PARSE;
        return;
    }

    oi_json_parser_reset(req->json);
    oi_status st = oi_json_parser_feed(req->json, data, len);
    if (st == OI_OK) {
        st = oi_json_parser_finish(req->json);
    }
    if (st != OI_OK || !oi_json_parser_done(req->json)) {
        req->body_cb_failed = 1;
        req->body_cb_status = OI_ERR_PARSE;
        return;
    }

    oi_json_value *root = oi_json_parser_root(req->json);
    oi_json_value *choices = oi_json_object_get(root, "choices");
    oi_json_value *choice0 = oi_json_array_get(choices, 0);
    oi_json_value *delta = oi_json_object_get(choice0, "delta");
    oi_json_value *content = oi_json_object_get(delta, "content");
    oi_json_value *tool_calls = oi_json_object_get(delta, "tool_calls");

    const char *text;
    size_t text_len;
    if (oi_json_get_string(content, &text, &text_len) == OI_OK) {
        oi_llm_event event = {
            .type = OI_LLM_EVENT_TEXT,
            .as.text = {text, text_len},
        };
        if (emit_event(req, &event)) {
            return;
        }
        if (req->on_delta) {
            req->on_delta(text, text_len, req->user_data);
        }
    }
    oi_status tool_st = handle_tool_calls(req, tool_calls);
    if (tool_st == OI_ERR_CLOSED) {
        return; /* request was cancelled reentrantly by on_event */
    }
    if (tool_st != OI_OK) {
        req->body_cb_failed = 1;
        req->body_cb_status = tool_st;
    }
}

/* ================= HTTP parser callbacks ================= */

static void req_on_headers_done(int status_code, void *ud) {
    oi_llm_request *req = ud;
    req->http_status = status_code;
    req->is_success_status = status_code >= 200 && status_code < 300;

    if (req->is_success_status) {
        req->sse = oi_llm_sse_parser_create(handle_sse_event, req);
        if (req->sse == NULL) {
            req->body_cb_failed = 1;
            req->body_cb_status = OI_ERR_NOMEM;
        }
    }
}

static void req_on_body(const void *data, size_t len, void *ud) {
    oi_llm_request *req = ud;
    if (req->body_cb_failed) {
        return;
    }
    if (req->is_success_status) {
        oi_status st = oi_llm_sse_parser_feed(req->sse, data, len);
        if (st != OI_OK) {
            req->body_cb_failed = 1;
            req->body_cb_status = st;
        }
    } else {
        oi_status st = error_buf_append(req, data, len);
        if (st != OI_OK) {
            req->body_cb_failed = 1;
            req->body_cb_status = st;
        }
    }
}

/* ================= connection callbacks ================= */

static void req_on_connected(oi_llm_conn *c, void *ud) {
    oi_llm_request *req = ud;
    char header[1024];
    int n;

    if (req->client->api_key) {
        n = snprintf(header, sizeof header,
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Authorization: Bearer %s\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      req->client->path, req->client->host, req->body_len,
                      req->client->api_key);
    } else {
        n = snprintf(header, sizeof header,
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      req->client->path, req->client->host, req->body_len);
    }
    if (n < 0 || (size_t)n >= sizeof header) {
        finish(req, OI_ERR_INVAL, 0, NULL, 0);
        return;
    }

    oi_status st = oi_llm_conn_write(c, header, (size_t)n);
    if (st == OI_OK) {
        st = oi_llm_conn_write(c, req->body, req->body_len);
    }
    if (st != OI_OK) {
        finish(req, st, 0, NULL, 0);
        return;
    }
}

static void req_on_data(oi_llm_conn *c, const void *data, size_t len,
                         void *ud) {
    (void)c;
    oi_llm_request *req = ud;

    int destroyed = 0;
OI_DIAG_PUSH_IGNORE_DANGLING
    req->destroyed_flag = &destroyed;
OI_DIAG_POP

    oi_status st = oi_llm_http_parser_feed(req->http, data, len);
    if (destroyed) {
        /* `req` (and everything it owns) was freed by a reentrant
         * oi_llm_request_cancel() from within a nested on_delta call. */
        return;
    }
    req->destroyed_flag = NULL;

    if (st != OI_OK || req->body_cb_failed) {
        finish(req, st != OI_OK ? st : req->body_cb_status, req->http_status,
               req->error_buf, req->error_len);
        return;
    }

    if (oi_llm_http_parser_body_done(req->http)) {
        if (req->is_success_status) {
            int finish_destroyed = 0;
OI_DIAG_PUSH_IGNORE_DANGLING
            req->destroyed_flag = &finish_destroyed;
OI_DIAG_POP
            st = oi_llm_sse_parser_finish(req->sse);
            if (finish_destroyed) {
                return;
            }
            req->destroyed_flag = NULL;
            if (st != OI_OK || req->body_cb_failed || !req->saw_done) {
                finish(req,
                       st != OI_OK
                           ? st
                           : (req->body_cb_failed ? req->body_cb_status
                                                  : OI_ERR_PARSE),
                       req->http_status, NULL, 0);
                return;
            }
            finish(req, OI_OK, req->http_status, NULL, 0);
        } else {
            finish(req, OI_ERR_IO, req->http_status, req->error_buf,
                   req->error_len);
        }
    }
}

static void req_on_error(oi_llm_conn *c, oi_status reason, void *ud) {
    (void)c;
    oi_llm_request *req = ud;
    /* If already finished (body_done already drove completion in an
     * earlier on_data call), the connection closing now is just the
     * expected teardown, not a new failure. */
    finish(req, reason, req->http_status, req->error_buf, req->error_len);
}

/* ================= public entry point ================= */

static oi_status request_start(oi_llm_client *client, oi_reactor *reactor,
                               oi_arena *arena, const char *body,
                               size_t body_len, oi_llm_delta_cb on_delta,
                               oi_llm_event_cb on_event,
                               oi_llm_done_cb on_done, void *user_data,
                               oi_llm_request **out_request) {
    if (client == NULL || reactor == NULL || arena == NULL || body == NULL ||
        out_request == NULL) {
        return OI_ERR_INVAL;
    }

    oi_llm_request *req = calloc(1, sizeof *req);
    if (req == NULL) {
        return OI_ERR_NOMEM;
    }
    req->client = client;
    req->arena = arena;
    req->on_delta = on_delta;
    req->on_event = on_event;
    req->on_done = on_done;
    req->user_data = user_data;

    req->body = malloc(body_len > 0 ? body_len : 1);
    if (req->body == NULL) {
        free(req);
        return OI_ERR_NOMEM;
    }
    if (body_len > 0) {
        memcpy(req->body, body, body_len);
    }
    req->body_len = body_len;

    req->http = oi_llm_http_parser_create(req_on_headers_done, req_on_body,
                                           req);
    if (req->http == NULL) {
        free(req->body);
        free(req);
        return OI_ERR_NOMEM;
    }

    req->json = oi_json_parser_create(arena);
    if (req->json == NULL) {
        oi_llm_http_parser_destroy(req->http);
        free(req->body);
        free(req);
        return OI_ERR_NOMEM;
    }

    struct oi_llm_conn_callbacks cbs = {req_on_connected, req_on_data,
                                         req_on_error};
    oi_status st = oi_llm_conn_connect(reactor, client->host, client->port,
                                        client->use_tls, client->ca_file,
                                        &cbs, req, &req->conn);
    if (st != OI_OK) {
        oi_json_parser_destroy(req->json);
        oi_llm_http_parser_destroy(req->http);
        free(req->body);
        free(req);
        return st;
    }

    if (client->timeout_ms > 0) {
        st = oi_reactor_timer_start(reactor, client->timeout_ms,
                                     request_timed_out, req, &req->timer);
        if (st != OI_OK) {
            request_teardown(req);
            return st;
        }
    }

    *out_request = req;
    return OI_OK;
}

oi_status oi_llm_request_start(oi_llm_client *client, oi_reactor *reactor,
                                oi_arena *arena, const char *body,
                                size_t body_len, oi_llm_delta_cb on_delta,
                                oi_llm_done_cb on_done, void *user_data,
                                oi_llm_request **out_request) {
    return request_start(client, reactor, arena, body, body_len, on_delta, NULL,
                         on_done, user_data, out_request);
}

oi_status oi_llm_request_start_events(
    oi_llm_client *client, oi_reactor *reactor, oi_arena *arena,
    const char *body, size_t body_len, oi_llm_event_cb on_event,
    oi_llm_done_cb on_done, void *user_data, oi_llm_request **out_request) {
    return request_start(client, reactor, arena, body, body_len, NULL, on_event,
                         on_done, user_data, out_request);
}
