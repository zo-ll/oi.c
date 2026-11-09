#include "cli_oauth.h"

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cli_credentials.h"
#include "llm_conn.h"
#include "llm_http.h"
#include "oi/arena.h"
#include "oi/json.h"
#include "oi/reactor.h"

#define OI_CLI_OAUTH_MAX_RESPONSE (1024U * 1024U)
#define OI_CLI_OAUTH_HTTP_TIMEOUT_MS 20000

static const char redirect_uri[] = "http://127.0.0.1:1455/auth/callback";
static const char resource[] = "https://api.openai.com/v1";
static const char scopes[] =
    "openid profile email offline_access resource.invoke "
    "chatgpt.tokens.use.direct";
static const char token_path[] = "/api/accounts/oauth/token";

/* --- text building --- */

struct text {
    char *data;
    size_t len;
    int failed;
};

static void text_append(struct text *text, const char *bytes, size_t len) {
    if (text->failed) {
        return;
    }
    char *grown = realloc(text->data, text->len + len + 1);
    if (grown == NULL) {
        text->failed = 1;
        return;
    }
    memcpy(grown + text->len, bytes, len);
    text->data = grown;
    text->len += len;
    text->data[text->len] = '\0';
}

static void text_puts(struct text *text, const char *value) {
    text_append(text, value, strlen(value));
}

static void text_encoded(struct text *text, const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' ||
            *p == '~') {
            text_append(text, (const char *)p, 1);
        } else {
            char escape[3] = {'%', hex[*p >> 4], hex[*p & 0x0F]};
            text_append(text, escape, 3);
        }
    }
}

static void text_param(struct text *text, const char *key, const char *value) {
    if (text->len > 0 && text->data[text->len - 1] != '?') {
        text_puts(text, "&");
    }
    text_puts(text, key);
    text_puts(text, "=");
    text_encoded(text, value);
}

static oi_status text_finish(struct text *text, char **out) {
    if (text->failed || text->data == NULL) {
        free(text->data);
        return OI_ERR_NOMEM;
    }
    *out = text->data;
    return OI_OK;
}

/* --- servers --- */

void oi_cli_oauth_server_free(struct oi_cli_oauth_server *server) {
    free(server->host);
    server->host = NULL;
}

static oi_status server_named(const char *host,
                              struct oi_cli_oauth_server *out) {
    out->host = strdup(host);
    out->port = 443;
    out->use_tls = 1;
    return out->host != NULL ? OI_OK : OI_ERR_NOMEM;
}

static oi_status server_from_url(const char *url,
                                 struct oi_cli_oauth_server *out) {
    char *path = NULL;
    oi_status st = oi_cli_credentials_parse_url(
        url, strlen(url), &out->host, &out->port, &out->use_tls, &path);
    free(path);
    return st;
}

oi_status oi_cli_oauth_servers(struct oi_cli_oauth_server *out_auth,
                               struct oi_cli_oauth_server *out_api,
                               int *out_is_test) {
    const char *test_base = getenv("OI_CHATGPT_TEST_BASE");
    oi_status st;
    out_auth->host = NULL;
    out_api->host = NULL;
    *out_is_test = test_base != NULL && test_base[0] != '\0';
    if (*out_is_test) {
        st = server_from_url(test_base, out_auth);
        if (st == OI_OK) {
            st = server_from_url(test_base, out_api);
        }
    } else {
        st = server_named("auth.openai.com", out_auth);
        if (st == OI_OK) {
            st = server_named("api.openai.com", out_api);
        }
    }
    if (st != OI_OK) {
        oi_cli_oauth_server_free(out_auth);
        oi_cli_oauth_server_free(out_api);
    }
    return st;
}

/* --- PKCE and identifiers --- */

static oi_status base64url(const unsigned char *bytes, size_t len, char **out) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char *encoded = malloc((len / 3 + 1) * 4 + 1);
    if (encoded == NULL) {
        return OI_ERR_NOMEM;
    }
    size_t used = 0;
    for (size_t i = 0; i < len; i += 3) {
        size_t left = len - i;
        uint32_t chunk = (uint32_t)bytes[i] << 16;
        if (left > 1) {
            chunk |= (uint32_t)bytes[i + 1] << 8;
        }
        if (left > 2) {
            chunk |= bytes[i + 2];
        }
        encoded[used++] = alphabet[(chunk >> 18) & 63];
        encoded[used++] = alphabet[(chunk >> 12) & 63];
        if (left > 1) {
            encoded[used++] = alphabet[(chunk >> 6) & 63];
        }
        if (left > 2) {
            encoded[used++] = alphabet[chunk & 63];
        }
    }
    encoded[used] = '\0';
    *out = encoded;
    return OI_OK;
}

static int base64url_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    return c == '_' ? 63 : -1;
}

static oi_status base64url_decode(const char *text, size_t len, char **out,
                                  size_t *out_len) {
    char *decoded = malloc(len / 4 * 3 + 4);
    if (decoded == NULL) {
        return OI_ERR_NOMEM;
    }
    uint32_t bits = 0;
    int held = 0;
    size_t used = 0;
    for (size_t i = 0; i < len; i++) {
        int value = base64url_value((unsigned char)text[i]);
        if (value < 0) {
            free(decoded);
            return OI_ERR_PARSE;
        }
        bits = (bits << 6) | (uint32_t)value;
        held += 6;
        if (held >= 8) {
            held -= 8;
            decoded[used++] = (char)((bits >> held) & 0xFF);
        }
    }
    decoded[used] = '\0';
    *out = decoded;
    *out_len = used;
    return OI_OK;
}

oi_status oi_cli_oauth_random_token(char **out_token) {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof bytes) != 1) {
        return OI_ERR_IO;
    }
    return base64url(bytes, sizeof bytes, out_token);
}

oi_status oi_cli_oauth_challenge(const char *verifier, char **out_challenge) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)verifier, strlen(verifier), digest);
    return base64url(digest, sizeof digest, out_challenge);
}

oi_status oi_cli_oauth_host_id(char **out_host_id) {
    unsigned char b[16];
    if (RAND_bytes(b, sizeof b) != 1) {
        return OI_ERR_IO;
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    char *id = malloc(46);
    if (id == NULL) {
        return OI_ERR_NOMEM;
    }
    snprintf(id, 46,
             "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
             b[11], b[12], b[13], b[14], b[15]);
    *out_host_id = id;
    return OI_OK;
}

/* --- authorize URL --- */

static void text_origin(struct text *text,
                        const struct oi_cli_oauth_server *server) {
    char port[16] = "";
    if (server->port != (server->use_tls ? 443 : 80)) {
        snprintf(port, sizeof port, ":%d", server->port);
    }
    text_puts(text, server->use_tls ? "https://" : "http://");
    text_puts(text, server->host);
    text_puts(text, port);
}

oi_status oi_cli_oauth_authorize_url(const struct oi_cli_oauth_server *auth,
                                     const struct oi_cli_oauth_authorize *req,
                                     char **out_url) {
    struct text url = {0};
    text_origin(&url, auth);
    text_puts(&url, "/api/accounts/authorize?");
    text_param(&url, "client_id",
               req->client_id != NULL ? req->client_id
                                      : "dynamic_agent_client");
    if (req->client_id == NULL) {
        text_param(&url, "agent_name_hint", "oi");
    }
    text_param(&url, "ext_agent_host_id", req->host_id);
    text_param(&url, "response_type", "code");
    text_param(&url, "redirect_uri", redirect_uri);
    text_param(&url, "scope", scopes);
    text_param(&url, "resource", resource);
    text_param(&url, "state", req->state);
    text_param(&url, "nonce", req->nonce);
    text_param(&url, "code_challenge", req->challenge);
    text_param(&url, "code_challenge_method", "S256");
    if (req->id_token_hint != NULL) {
        text_param(&url, "id_token_hint", req->id_token_hint);
    }
    return text_finish(&url, out_url);
}

/* --- loopback callback --- */

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static oi_status percent_decode(const char *text, size_t len, char **out) {
    char *decoded = malloc(len + 1);
    if (decoded == NULL) {
        return OI_ERR_NOMEM;
    }
    size_t used = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '%') {
            int high = i + 1 < len ? hex_value(text[i + 1]) : -1;
            int low = i + 2 < len ? hex_value(text[i + 2]) : -1;
            if (high < 0 || low < 0 || (high == 0 && low == 0)) {
                free(decoded);
                return OI_ERR_PARSE;
            }
            decoded[used++] = (char)(high * 16 + low);
            i += 2;
        } else {
            decoded[used++] = text[i] == '+' ? ' ' : text[i];
        }
    }
    decoded[used] = '\0';
    *out = decoded;
    return OI_OK;
}

void oi_cli_oauth_callback_free(struct oi_cli_oauth_callback *callback) {
    free(callback->code);
    free(callback->state);
    free(callback->client_id);
    free(callback->error);
    memset(callback, 0, sizeof *callback);
}

static oi_status store_param(struct oi_cli_oauth_callback *out,
                             const char *pair, size_t pair_len) {
    const char *equals = memchr(pair, '=', pair_len);
    if (equals == NULL) {
        return OI_OK;
    }
    size_t key_len = (size_t)(equals - pair);
    char **slot = NULL;
    if (key_len == 4 && memcmp(pair, "code", 4) == 0) {
        slot = &out->code;
    } else if (key_len == 5 && memcmp(pair, "state", 5) == 0) {
        slot = &out->state;
    } else if (key_len == 9 && memcmp(pair, "client_id", 9) == 0) {
        slot = &out->client_id;
    } else if (key_len == 5 && memcmp(pair, "error", 5) == 0) {
        slot = &out->error;
    }
    if (slot == NULL) {
        return OI_OK;
    }
    free(*slot);
    *slot = NULL;
    return percent_decode(equals + 1, pair_len - key_len - 1, slot);
}

oi_status oi_cli_oauth_parse_callback(const char *request,
                                      struct oi_cli_oauth_callback *out) {
    static const char prefix[] = "GET /auth/callback";
    memset(out, 0, sizeof *out);
    if (strncmp(request, prefix, sizeof prefix - 1) != 0) {
        return OI_OK;
    }
    const char *cursor = request + sizeof prefix - 1;
    if (*cursor != '?' && *cursor != ' ') {
        return OI_OK;
    }
    out->is_callback = 1;
    if (*cursor == ' ') {
        return OI_OK;
    }
    cursor++;
    const char *end = strchr(cursor, ' ');
    if (end == NULL) {
        oi_cli_oauth_callback_free(out);
        return OI_ERR_PARSE;
    }
    while (cursor < end) {
        const char *amp = memchr(cursor, '&', (size_t)(end - cursor));
        const char *stop = amp != NULL ? amp : end;
        oi_status st = store_param(out, cursor, (size_t)(stop - cursor));
        if (st != OI_OK) {
            oi_cli_oauth_callback_free(out);
            return st;
        }
        cursor = stop + (amp != NULL ? 1 : 0);
    }
    return OI_OK;
}

/* --- JSON documents --- */

static oi_status parse_json(oi_arena *arena, const char *body, size_t len,
                            oi_json_value **out_root) {
    oi_json_parser *parser = oi_json_parser_create(arena);
    if (parser == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_status st = oi_json_parser_feed(parser, body, len);
    if (st == OI_OK) {
        st = oi_json_parser_finish(parser);
    }
    *out_root = st == OI_OK ? oi_json_parser_root(parser) : NULL;
    oi_json_parser_destroy(parser);
    if (st != OI_OK) {
        return st;
    }
    return *out_root != NULL ? OI_OK : OI_ERR_PARSE;
}

static oi_status dup_member(const oi_json_value *object, const char *key,
                            char **slot) {
    const oi_json_value *value = oi_json_object_get(object, key);
    const char *text;
    size_t len;
    if (oi_json_get_string(value, &text, &len) != OI_OK) {
        return OI_OK;
    }
    if (strlen(text) != len) {
        return OI_ERR_PARSE;
    }
    return oi_cli_credentials_set(slot, text);
}

void oi_cli_oauth_tokens_free(struct oi_cli_oauth_tokens *tokens) {
    (void)oi_cli_credentials_set(&tokens->access_token, NULL);
    (void)oi_cli_credentials_set(&tokens->refresh_token, NULL);
    (void)oi_cli_credentials_set(&tokens->id_token, NULL);
    free(tokens->scope);
    free(tokens->error);
    memset(tokens, 0, sizeof *tokens);
}

static oi_status decode_tokens(const oi_json_value *root,
                               struct oi_cli_oauth_tokens *out) {
    oi_status st = dup_member(root, "access_token", &out->access_token);
    if (st == OI_OK) {
        st = dup_member(root, "refresh_token", &out->refresh_token);
    }
    if (st == OI_OK) {
        st = dup_member(root, "id_token", &out->id_token);
    }
    if (st == OI_OK) {
        st = dup_member(root, "scope", &out->scope);
    }
    /* The token endpoint's refusal shape is undocumented: accept a string
     * "error", a nested "error.code", and a top-level "code". */
    if (st == OI_OK) {
        st = dup_member(root, "error", &out->error);
    }
    if (st == OI_OK && out->error == NULL) {
        st = dup_member(oi_json_object_get(root, "error"), "code", &out->error);
    }
    if (st == OI_OK && out->error == NULL) {
        st = dup_member(root, "code", &out->error);
    }
    if (st != OI_OK) {
        return st;
    }
    if (out->access_token == NULL && out->error == NULL) {
        return OI_ERR_PARSE;
    }
    if (oi_json_get_number(oi_json_object_get(root, "expires_in"),
                           &out->expires_in) != OI_OK) {
        out->expires_in = 3600;
    }
    return OI_OK;
}

oi_status oi_cli_oauth_parse_tokens(const char *body, size_t body_len,
                                    struct oi_cli_oauth_tokens *out) {
    memset(out, 0, sizeof *out);
    oi_arena *arena = oi_arena_create(0);
    if (arena == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_json_value *root = NULL;
    oi_status st = parse_json(arena, body, body_len, &root);
    if (st == OI_OK) {
        st = decode_tokens(root, out);
    }
    oi_arena_destroy(arena);
    if (st != OI_OK) {
        oi_cli_oauth_tokens_free(out);
    }
    return st;
}

int oi_cli_oauth_error_is_terminal(const char *error) {
    static const char *const terminal[] = {
        "invalid_grant",         "invalid_refresh_token",
        "token_expired",         "refresh_token_expired",
        "refresh_token_invalidated", "refresh_token_reused",
    };
    if (error == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof terminal / sizeof terminal[0]; i++) {
        if (strcmp(error, terminal[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int string_equals(const oi_json_value *value, const char *expected) {
    const char *text;
    size_t len;
    return oi_json_get_string(value, &text, &len) == OI_OK &&
           len == strlen(expected) && memcmp(text, expected, len) == 0;
}

static int audience_matches(const oi_json_value *aud, const char *client_id) {
    if (string_equals(aud, client_id)) {
        return 1;
    }
    for (size_t i = 0; aud != NULL && i < oi_json_array_len(aud); i++) {
        if (string_equals(oi_json_array_get(aud, i), client_id)) {
            return 1;
        }
    }
    return 0;
}

static oi_status check_claims(const oi_json_value *claims, const char *nonce,
                              const char *client_id, double now,
                              char **out_email) {
    double expires = 0;
    const char *sub;
    size_t sub_len;
    if ((nonce != NULL &&
         !string_equals(oi_json_object_get(claims, "nonce"), nonce)) ||
        !audience_matches(oi_json_object_get(claims, "aud"), client_id) ||
        oi_json_get_number(oi_json_object_get(claims, "exp"), &expires) !=
            OI_OK ||
        expires <= now ||
        oi_json_get_string(oi_json_object_get(claims, "sub"), &sub,
                           &sub_len) != OI_OK ||
        sub_len == 0) {
        return OI_ERR_DENIED;
    }
    return dup_member(claims, "email", out_email);
}

oi_status oi_cli_oauth_check_id_token(const char *id_token, const char *nonce,
                                      const char *client_id, double now,
                                      char **out_email) {
    *out_email = NULL;
    const char *first = strchr(id_token, '.');
    const char *second = first != NULL ? strchr(first + 1, '.') : NULL;
    if (second == NULL) {
        return OI_ERR_PARSE;
    }
    char *payload = NULL;
    size_t payload_len = 0;
    oi_status st = base64url_decode(first + 1, (size_t)(second - first - 1),
                                    &payload, &payload_len);
    if (st != OI_OK) {
        return st;
    }
    oi_arena *arena = oi_arena_create(0);
    oi_json_value *root = NULL;
    st = arena != NULL ? parse_json(arena, payload, payload_len, &root)
                       : OI_ERR_NOMEM;
    if (st == OI_OK) {
        st = check_claims(root, nonce, client_id, now, out_email);
    }
    oi_arena_destroy(arena);
    free(payload);
    return st;
}

void oi_cli_oauth_models_free(struct oi_cli_oauth_models *models) {
    for (size_t i = 0; i < models->count; i++) {
        free(models->items[i].slug);
        free(models->items[i].name);
    }
    models->count = 0;
}

static oi_status decode_models(const oi_json_value *root,
                               struct oi_cli_oauth_models *out) {
    const oi_json_value *models = oi_json_object_get(root, "models");
    if (models == NULL || oi_json_type_of(models) != OI_JSON_ARRAY) {
        return OI_ERR_PARSE;
    }
    for (size_t i = 0; i < oi_json_array_len(models) &&
                       out->count < OI_CLI_OAUTH_MAX_MODELS;
         i++) {
        const oi_json_value *model = oi_json_array_get(models, i);
        char *slug = NULL;
        char *name = NULL;
        if (!string_equals(oi_json_object_get(model, "visibility"), "list")) {
            continue;
        }
        oi_status st = dup_member(model, "slug", &slug);
        if (st == OI_OK) {
            st = dup_member(model, "display_name", &name);
        }
        if (st != OI_OK || slug == NULL) {
            free(slug);
            free(name);
            return st != OI_OK ? st : OI_ERR_PARSE;
        }
        out->items[out->count].slug = slug;
        out->items[out->count].name = name;
        out->count++;
    }
    return OI_OK;
}

oi_status oi_cli_oauth_parse_models(const char *body, size_t body_len,
                                    struct oi_cli_oauth_models *out) {
    out->count = 0;
    oi_arena *arena = oi_arena_create(0);
    if (arena == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_json_value *root = NULL;
    oi_status st = parse_json(arena, body, body_len, &root);
    if (st == OI_OK) {
        st = decode_models(root, out);
    }
    oi_arena_destroy(arena);
    if (st != OI_OK) {
        oi_cli_oauth_models_free(out);
    }
    return st;
}

/* --- blocking HTTP --- */

struct http_call {
    oi_llm_http_parser *parser;
    oi_llm_conn *conn;
    const char *request;
    size_t request_len;
    int http_status;
    int done;
    oi_status status;
    struct text body;
};

static void http_finish(struct http_call *call, oi_status status) {
    if (!call->done) {
        call->done = 1;
        call->status = status;
    }
}

static void http_on_headers(int status_code, void *user_data) {
    struct http_call *call = user_data;
    call->http_status = status_code;
}

static void http_on_body(const void *data, size_t len, void *user_data) {
    struct http_call *call = user_data;
    if (call->body.len + len > OI_CLI_OAUTH_MAX_RESPONSE) {
        http_finish(call, OI_ERR_PARSE);
        return;
    }
    text_append(&call->body, data, len);
}

static void http_on_connected(oi_llm_conn *conn, void *user_data) {
    struct http_call *call = user_data;
    oi_status st = oi_llm_conn_write(conn, call->request, call->request_len);
    if (st != OI_OK) {
        http_finish(call, st);
    }
}

static void http_on_data(oi_llm_conn *conn, const void *data, size_t len,
                         void *user_data) {
    (void)conn;
    struct http_call *call = user_data;
    oi_status st = oi_llm_http_parser_feed(call->parser, data, len);
    if (st != OI_OK) {
        http_finish(call, st);
    } else if (oi_llm_http_parser_body_done(call->parser)) {
        http_finish(call, OI_OK);
    }
}

static void http_on_error(oi_llm_conn *conn, oi_status reason,
                          void *user_data) {
    (void)conn;
    struct http_call *call = user_data;
    call->conn = NULL; /* closes itself once this callback returns */
    http_finish(call, reason);
}

static oi_status build_request(const struct oi_cli_oauth_server *server,
                               const char *method, const char *path,
                               const char *bearer, const char *form,
                               char **out, size_t *out_len) {
    struct text request = {0};
    char length[32];
    text_puts(&request, method);
    text_puts(&request, " ");
    text_puts(&request, path);
    text_puts(&request, " HTTP/1.1\r\nHost: ");
    text_puts(&request, server->host);
    text_puts(&request, "\r\nAccept: application/json\r\nConnection: close\r\n");
    if (bearer != NULL) {
        text_puts(&request, "Authorization: Bearer ");
        text_puts(&request, bearer);
        text_puts(&request, "\r\n");
    }
    if (form != NULL) {
        snprintf(length, sizeof length, "%zu", strlen(form));
        text_puts(&request,
                  "Content-Type: application/x-www-form-urlencoded\r\n"
                  "Content-Length: ");
        text_puts(&request, length);
        text_puts(&request, "\r\n");
    }
    text_puts(&request, "\r\n");
    if (form != NULL) {
        text_puts(&request, form);
    }
    *out_len = request.len;
    return text_finish(&request, out);
}

static double monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

oi_status oi_cli_oauth_http(const struct oi_cli_oauth_server *server,
                            const char *ca_file, const char *method,
                            const char *path, const char *bearer,
                            const char *form, int timeout_ms,
                            int *out_http_status, char **out_body,
                            size_t *out_body_len) {
    struct http_call call;
    memset(&call, 0, sizeof call);
    char *request = NULL;
    oi_status st = build_request(server, method, path, bearer, form, &request,
                                 &call.request_len);
    if (st != OI_OK) {
        return st;
    }
    call.request = request;

    oi_reactor *reactor = oi_reactor_create();
    call.parser =
        oi_llm_http_parser_create(http_on_headers, http_on_body, &call);
    if (reactor == NULL || call.parser == NULL) {
        st = OI_ERR_NOMEM;
    } else {
        struct oi_llm_conn_callbacks callbacks = {
            http_on_connected, http_on_data, http_on_error};
        st = oi_llm_conn_connect(reactor, server->host,
                                 (unsigned short)server->port, server->use_tls,
                                 ca_file, &callbacks, &call, &call.conn);
    }
    if (st == OI_OK) {
        double deadline = monotonic_ms() + timeout_ms;
        while (!call.done) {
            double left = deadline - monotonic_ms();
            if (left <= 0) {
                http_finish(&call, OI_ERR_TIMEOUT);
                break;
            }
            oi_status step = OI_OK;
            oi_reactor_step(reactor, (int)left, &step);
            if (step != OI_OK) {
                http_finish(&call, step);
            }
        }
        st = call.status;
    }
    if (call.conn != NULL) {
        oi_llm_conn_close(call.conn);
    }
    oi_llm_http_parser_destroy(call.parser);
    oi_reactor_destroy(reactor);
    memset(request, 0, call.request_len);
    free(request);

    if (st != OI_OK || call.body.failed) {
        free(call.body.data);
        return st != OI_OK ? st : OI_ERR_NOMEM;
    }
    *out_http_status = call.http_status;
    *out_body = call.body.data != NULL ? call.body.data : strdup("");
    *out_body_len = call.body.len;
    return *out_body != NULL ? OI_OK : OI_ERR_NOMEM;
}

static oi_status post_token_form(const struct oi_cli_oauth_server *auth,
                                 const char *ca_file, struct text *form,
                                 struct oi_cli_oauth_tokens *out) {
    char *form_text = NULL;
    oi_status st = text_finish(form, &form_text);
    if (st != OI_OK) {
        return st;
    }
    int http_status = 0;
    char *body = NULL;
    size_t body_len = 0;
    st = oi_cli_oauth_http(auth, ca_file, "POST", token_path, NULL, form_text,
                           OI_CLI_OAUTH_HTTP_TIMEOUT_MS, &http_status, &body,
                           &body_len);
    memset(form_text, 0, strlen(form_text));
    free(form_text);
    if (st != OI_OK) {
        return st;
    }
    st = oi_cli_oauth_parse_tokens(body, body_len, out);
    memset(body, 0, body_len);
    free(body);
    if (st == OI_OK && http_status >= 200 && http_status < 300 &&
        out->access_token == NULL) {
        oi_cli_oauth_tokens_free(out);
        return OI_ERR_PARSE;
    }
    return st;
}

oi_status oi_cli_oauth_exchange_code(const struct oi_cli_oauth_server *auth,
                                     const char *ca_file,
                                     const char *client_id, const char *code,
                                     const char *verifier,
                                     struct oi_cli_oauth_tokens *out) {
    struct text form = {0};
    text_param(&form, "grant_type", "authorization_code");
    text_param(&form, "client_id", client_id);
    text_param(&form, "code", code);
    text_param(&form, "code_verifier", verifier);
    text_param(&form, "redirect_uri", redirect_uri);
    text_param(&form, "resource", resource);
    return post_token_form(auth, ca_file, &form, out);
}

oi_status oi_cli_oauth_refresh(const struct oi_cli_oauth_server *auth,
                               const char *ca_file, const char *client_id,
                               const char *refresh_token,
                               struct oi_cli_oauth_tokens *out) {
    struct text form = {0};
    text_param(&form, "grant_type", "refresh_token");
    text_param(&form, "client_id", client_id);
    text_param(&form, "refresh_token", refresh_token);
    text_param(&form, "resource", resource);
    return post_token_form(auth, ca_file, &form, out);
}

oi_status oi_cli_oauth_fetch_models(const struct oi_cli_oauth_server *api,
                                    const char *ca_file, const char *bearer,
                                    struct oi_cli_oauth_models *out) {
    int http_status = 0;
    char *body = NULL;
    size_t body_len = 0;
    out->count = 0;
    oi_status st = oi_cli_oauth_http(api, ca_file, "GET", "/v1/models", bearer,
                                     NULL, OI_CLI_OAUTH_HTTP_TIMEOUT_MS,
                                     &http_status, &body, &body_len);
    if (st != OI_OK) {
        return st;
    }
    st = http_status >= 200 && http_status < 300
             ? oi_cli_oauth_parse_models(body, body_len, out)
             : OI_ERR_IO;
    free(body);
    return st;
}
