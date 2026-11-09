#ifndef OI_CLI_OAUTH_H
#define OI_CLI_OAUTH_H

#include <stddef.h>

#include "oi/status.h"

/*
 * "Sign in with ChatGPT" for open-source clients: authorization code with
 * PKCE and a loopback redirect. Tokens are bound to the Responses API.
 */

#define OI_CLI_OAUTH_REDIRECT_PORT 1455
#define OI_CLI_OAUTH_MAX_MODELS 32U

struct oi_cli_oauth_server {
    char *host;
    int port;
    int use_tls;
};

/*
 * The production servers, unless OI_CHATGPT_TEST_BASE names one http(s)
 * origin that replaces both; that variable exists only for tests.
 * `out_is_test` reports the override, which also disables opening a browser.
 */
oi_status oi_cli_oauth_servers(struct oi_cli_oauth_server *out_auth,
                               struct oi_cli_oauth_server *out_api,
                               int *out_is_test);
void oi_cli_oauth_server_free(struct oi_cli_oauth_server *server);

/* 32 random bytes in unpadded base64url. */
oi_status oi_cli_oauth_random_token(char **out_token);
/* The S256 code_challenge of `verifier`. */
oi_status oi_cli_oauth_challenge(const char *verifier, char **out_challenge);
/* A fresh urn:uuid: host identifier. */
oi_status oi_cli_oauth_host_id(char **out_host_id);

struct oi_cli_oauth_authorize {
    const char *client_id;     /* NULL for first registration */
    const char *host_id;
    const char *state;
    const char *nonce;
    const char *challenge;
    const char *id_token_hint; /* NULL when none is saved */
};

oi_status oi_cli_oauth_authorize_url(const struct oi_cli_oauth_server *auth,
                                     const struct oi_cli_oauth_authorize *req,
                                     char **out_url);

struct oi_cli_oauth_callback {
    int is_callback; /* the request named the callback path */
    char *code;
    char *state;
    char *client_id;
    char *error;
};

/* Parses the head of one HTTP request received on the loopback listener. */
oi_status oi_cli_oauth_parse_callback(const char *request,
                                      struct oi_cli_oauth_callback *out);
void oi_cli_oauth_callback_free(struct oi_cli_oauth_callback *callback);

struct oi_cli_oauth_tokens {
    char *access_token;
    char *refresh_token;
    char *id_token;
    char *scope;
    char *error; /* a machine-readable code; set when the server refused */
    double expires_in;
};

/*
 * On OI_OK, either `access_token` or `error` is set. OI_ERR_PARSE means
 * the body was neither a token set nor a recognizable refusal.
 */
oi_status oi_cli_oauth_parse_tokens(const char *body, size_t body_len,
                                    struct oi_cli_oauth_tokens *out);
void oi_cli_oauth_tokens_free(struct oi_cli_oauth_tokens *tokens);

/* True for the refusals that mean the saved refresh token can never work. */
int oi_cli_oauth_error_is_terminal(const char *error);

/*
 * Checks the claims of an ID token received directly from the token
 * endpoint over TLS: nonce (when `nonce` is non-NULL), audience, expiry,
 * and subject. Does not verify the JWKS signature. OI_ERR_DENIED means a
 * claim did not match. `*out_email` is NULL when the token has none.
 */
oi_status oi_cli_oauth_check_id_token(const char *id_token, const char *nonce,
                                      const char *client_id, double now,
                                      char **out_email);

struct oi_cli_oauth_model {
    char *slug;
    char *name;
};

struct oi_cli_oauth_models {
    struct oi_cli_oauth_model items[OI_CLI_OAUTH_MAX_MODELS];
    size_t count;
};

/* Keeps only models with visibility "list". */
oi_status oi_cli_oauth_parse_models(const char *body, size_t body_len,
                                    struct oi_cli_oauth_models *out);
void oi_cli_oauth_models_free(struct oi_cli_oauth_models *models);

/*
 * One blocking HTTP exchange on a private reactor, so that it is safe from
 * inside another reactor's callback. `bearer` and `form` are optional; a
 * `form` body makes the Content-Type form-encoded. Non-2xx responses still
 * return OI_OK with *out_http_status set. The caller frees *out_body.
 */
oi_status oi_cli_oauth_http(const struct oi_cli_oauth_server *server,
                            const char *ca_file, const char *method,
                            const char *path, const char *bearer,
                            const char *form, int timeout_ms,
                            int *out_http_status, char **out_body,
                            size_t *out_body_len);

oi_status oi_cli_oauth_exchange_code(const struct oi_cli_oauth_server *auth,
                                     const char *ca_file,
                                     const char *client_id, const char *code,
                                     const char *verifier,
                                     struct oi_cli_oauth_tokens *out);

oi_status oi_cli_oauth_refresh(const struct oi_cli_oauth_server *auth,
                               const char *ca_file, const char *client_id,
                               const char *refresh_token,
                               struct oi_cli_oauth_tokens *out);

oi_status oi_cli_oauth_fetch_models(const struct oi_cli_oauth_server *api,
                                    const char *ca_file, const char *bearer,
                                    struct oi_cli_oauth_models *out);

#endif
