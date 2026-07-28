#ifndef OI_CLI_SESSIONS_H
#define OI_CLI_SESSIONS_H

#include "oi/status.h"

struct oi_cli_session_location {
    char *id;
    char *directory;
    char *history_path;
};

void oi_cli_session_location_init(
    struct oi_cli_session_location *location);
void oi_cli_session_location_free(
    struct oi_cli_session_location *location);

/*
 * Creates a fresh private session directory. `root_override`, when non-NULL,
 * replaces the platform state directory. All returned strings are owned by
 * `out_location`.
 */
oi_status oi_cli_session_location_create(
    const char *root_override,
    struct oi_cli_session_location *out_location);

/* Returns the caller-owned platform default sessions directory. */
oi_status oi_cli_sessions_default_root(char **out_root);

#endif
