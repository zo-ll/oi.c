#ifndef OI_CLI_CREDENTIALS_H
#define OI_CLI_CREDENTIALS_H

#include <stddef.h>

#include "oi/status.h"

#define OI_CLI_CREDENTIALS_MAX_FILE (64U * 1024U)

enum oi_cli_provider {
    OI_CLI_PROVIDER_NONE = 0,
    OI_CLI_PROVIDER_OPENAI,
    OI_CLI_PROVIDER_CUSTOM
};

/* Every string is heap-owned, NUL-terminated, and NULL when absent. */
struct oi_cli_credentials {
    enum oi_cli_provider active;
    char *openai_api_key;
    int openai_oauth; /* nonzero: OpenAI uses the tokens below, not the key */
    char *openai_client_id;
    char *openai_host_id;
    char *openai_access_token;
    char *openai_refresh_token;
    char *openai_id_token;
    char *openai_email;
    char *openai_model;
    double openai_expires_at; /* seconds since the epoch */
    int openai_welcome_shown;
    char *custom_host;
    char *custom_path;
    char *custom_api_key;
    int custom_port;
    int custom_use_tls;
};

void oi_cli_credentials_init(struct oi_cli_credentials *credentials);
/* Overwrites every secret before releasing it. */
void oi_cli_credentials_free(struct oi_cli_credentials *credentials);
/* Replaces `*slot`, overwriting the secret it held. */
oi_status oi_cli_credentials_set(char **slot, const char *value);

/* $XDG_CONFIG_HOME/oi/credentials.json, else ~/.config/oi/credentials.json. */
oi_status oi_cli_credentials_default_path(char **out_path);

/*
 * OI_ERR_NOTFOUND when the file does not exist; OI_ERR_DENIED when anyone
 * but the owner can reach it, because a secret that was already exposed
 * must not be used silently; OI_ERR_PARSE when it is malformed or has an
 * unknown version.
 */
oi_status oi_cli_credentials_read(const char *path,
                                  struct oi_cli_credentials *out_credentials);

/*
 * Atomically replaces `path` with a 0600 file, creating missing parent
 * directories as 0700, and syncs it: losing this file means signing in again.
 */
oi_status oi_cli_credentials_write(
    const char *path, const struct oi_cli_credentials *credentials);

/*
 * Splits an http(s) URL into endpoint parts. A URL that names only a host
 * or an API base gets the Chat Completions route appended, so both
 * "https://h" and "https://h/api/v1" work; a path with a query string is
 * used as given. Rejects a missing scheme, user info, an IPv6 literal, a
 * bad port, and control characters or spaces.
 */
oi_status oi_cli_credentials_parse_url(const char *url, size_t url_len,
                                       char **out_host, int *out_port,
                                       int *out_use_tls, char **out_path);

#endif
