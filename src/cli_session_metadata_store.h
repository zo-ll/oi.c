#ifndef OI_CLI_SESSION_METADATA_STORE_H
#define OI_CLI_SESSION_METADATA_STORE_H

#include "cli_session_metadata.h"
#include "oi/status.h"

oi_status oi_cli_session_metadata_store_read(
    const char *metadata_path, struct oi_cli_session_metadata *out_metadata);

oi_status oi_cli_session_metadata_store_write(
    const char *metadata_path, const struct oi_cli_session_metadata *metadata);

#endif
