#ifndef OI_CLI_LOGIN_H
#define OI_CLI_LOGIN_H

#include <stdio.h>

#include "cli_composer.h"
#include "cli_status.h"
#include "oi/config.h"
#include "oi/llm.h"
#include "oi/status.h"

struct oi_cli_login {
    char *credentials_path;                 /* owned; NULL when no home */
    oi_llm_client *client;                  /* borrowed */
    struct oi_cli_status_endpoint *endpoint; /* borrowed; updated on login */
    const char *ca_file;                    /* borrowed */
    int timeout_ms;
    int key_from_environment;               /* OI_API_KEY or --api-key won */
    FILE *err;
    char *host; /* owned; what *endpoint points at after a login */
    char *path; /* owned; what *endpoint points at after a login */
    int oauth_active;       /* the client holds a short-lived access token */
    double expires_at;      /* when that token expires, epoch seconds */
    char *selected_model;   /* owned; set when a login chose a model */
};

void oi_cli_login_free(struct oi_cli_login *login);

/*
 * Fills `cfg` from the saved credentials. A key already present in `cfg` wins.
 * Saved credentials apply only to the default endpoint: a --host or config
 * file pointing elsewhere must never receive a key saved for another server.
 * A missing file is normal; an unusable one is reported on `err` and ignored.
 */
oi_status oi_cli_login_apply_saved(oi_config *cfg, const char *path,
                                   FILE *err, int *out_oauth,
                                   double *out_expires_at);

/*
 * An oi_llm_before_request_cb. While an OAuth login is active, renews an
 * access token that is close to expiry. Holds a file lock across the
 * renewal, because the server rotates the refresh token and a second
 * process must not reuse the old one. Blocks the caller for the exchange.
 */
oi_status oi_cli_login_before_request(void *user_data);

/*
 * Runs the /login dialog on an idle REPL: provider, then method, then the
 * details that method needs. A success updates the live client at once and
 * saves the credentials. Returns non-OK only for a structural failure; every
 * refusal a user can provoke is printed and leaves the REPL running.
 */
oi_status oi_cli_login_run(struct oi_cli_login *login,
                           struct oi_cli_composer *composer, int signal_fd,
                           int *out_terminate_signal);

#endif
