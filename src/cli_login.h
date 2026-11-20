#ifndef OI_CLI_LOGIN_H
#define OI_CLI_LOGIN_H

#include <stdio.h>

#include "cli_composer.h"
#include "cli_status.h"
#include "oi/config.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"

struct login_renewal;

struct oi_cli_login {
    char *credentials_path;
    oi_llm_client *client;
    struct oi_cli_status_endpoint *endpoint;
    const char *ca_file;
    int timeout_ms;
    int key_from_environment;
    FILE *err;
    char *host;
    char *path;
    int oauth_active;
    double expires_at;
    char *selected_model;
    oi_reactor *reactor;
    struct login_renewal *renewal;
};

void oi_cli_login_free(struct oi_cli_login *login);

oi_status oi_cli_login_apply_saved(oi_config *cfg, const char *path,
                                   FILE *err, int *out_oauth,
                                   double *out_expires_at);

oi_status oi_cli_login_before_request(void *user_data);

oi_status oi_cli_login_run(struct oi_cli_login *login,
                           struct oi_cli_composer *composer, int signal_fd,
                           int *out_terminate_signal);

#endif
