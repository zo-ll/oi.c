#ifndef OI_CLI_OAUTH_H
#define OI_CLI_OAUTH_H

#include <stddef.h>

#include "oi/reactor.h"
#include "oi/status.h"

#define OI_CLI_OAUTH_REDIRECT_PORT 1455
#define OI_CLI_OAUTH_MAX_MODELS 32U

struct oi_cli_oauth_server {
    char *host;
    int port;
    int use_tls;
};

oi_status oi_cli_oauth_servers(struct oi_cli_oauth_server *out_auth,
                               struct oi_cli_oauth_server *out_api,
                               int *out_is_test);
void oi_cli_oauth_server_free(struct oi_cli_oauth_server *server);

oi_status oi_cli_oauth_random_token(char **out_token);
oi_status oi_cli_oauth_challenge(const char *verifier, char **out_challenge);
oi_status oi_cli_oauth_host_id(char **out_host_id);

struct oi_cli_oauth_authorize {
    const char *client_id;
    const char *host_id;
    const char *state;
    const char *nonce;
    const char *challenge;
    const char *id_token_hint;
};

oi_status oi_cli_oauth_authorize_url(const struct oi_cli_oauth_server *auth,
                                     const struct oi_cli_oauth_authorize *req,
                                     char **out_url);

struct oi_cli_oauth_callback {
    int is_callback;
    char *code;
    char *state;
    char *client_id;
    char *error;
};

oi_status oi_cli_oauth_parse_callback(const char *request,
                                      struct oi_cli_oauth_callback *out);
void oi_cli_oauth_callback_free(struct oi_cli_oauth_callback *callback);

struct oi_cli_oauth_tokens {
    char *access_token;
    char *refresh_token;
    char *id_token;
    char *scope;
    char *error;
    double expires_in;
};

oi_status oi_cli_oauth_parse_tokens(const char *body, size_t body_len,
                                    struct oi_cli_oauth_tokens *out);
void oi_cli_oauth_tokens_free(struct oi_cli_oauth_tokens *tokens);

int oi_cli_oauth_error_is_terminal(const char *error);

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

oi_status oi_cli_oauth_parse_models(const char *body, size_t body_len,
                                    struct oi_cli_oauth_models *out);
void oi_cli_oauth_models_free(struct oi_cli_oauth_models *models);

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

struct oi_cli_oauth_refresh_call;
typedef void (*oi_cli_oauth_tokens_cb)(oi_status status,
                                       struct oi_cli_oauth_tokens *tokens,
                                       void *user_data);
oi_status oi_cli_oauth_refresh_start(
    oi_reactor *reactor, const struct oi_cli_oauth_server *auth,
    const char *ca_file, const char *client_id, const char *refresh_token,
    oi_cli_oauth_tokens_cb done, void *user_data,
    struct oi_cli_oauth_refresh_call **out_call);
void oi_cli_oauth_refresh_cancel(struct oi_cli_oauth_refresh_call *call);

int oi_cli_oauth_refresh_sent(const struct oi_cli_oauth_refresh_call *call);

oi_status oi_cli_oauth_fetch_models(const struct oi_cli_oauth_server *api,
                                    const char *ca_file, const char *bearer,
                                    struct oi_cli_oauth_models *out);

#endif
