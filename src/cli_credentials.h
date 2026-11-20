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

struct oi_cli_credentials {
    enum oi_cli_provider active;
    char *openai_api_key;
    int openai_oauth;
    char *openai_client_id;
    char *openai_host_id;
    char *openai_access_token;
    char *openai_refresh_token;
    char *openai_id_token;
    char *openai_email;
    char *openai_model;
    double openai_expires_at;
    int openai_welcome_shown;
    char *custom_host;
    char *custom_path;
    char *custom_api_key;
    int custom_port;
    int custom_use_tls;
};

void oi_cli_credentials_init(struct oi_cli_credentials *credentials);
void oi_cli_credentials_free(struct oi_cli_credentials *credentials);
oi_status oi_cli_credentials_set(char **slot, const char *value);

oi_status oi_cli_credentials_default_path(char **out_path);

oi_status oi_cli_credentials_read(const char *path,
                                  struct oi_cli_credentials *out_credentials);

oi_status oi_cli_credentials_write(
    const char *path, const struct oi_cli_credentials *credentials);

oi_status oi_cli_credentials_parse_url(const char *url, size_t url_len,
                                       char **out_host, int *out_port,
                                       int *out_use_tls, char **out_path);

#endif
