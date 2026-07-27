#ifndef OI_CONFIG_H
#define OI_CONFIG_H

#include "oi/status.h"

/*
 * Config resolution: built-in defaults, an optional key=value file for
 * non-secret settings, the environment for secrets, and a generic
 * setter any override source (a config file line, a CLI flag) can call
 * through. Deliberately doesn't parse argv itself -- flag *syntax* is
 * the CLI binary's concern (issue #9); this module only owns turning a
 * resolved (key, value) pair into the right struct field, so both the
 * file parser here and the CLI's flag loop there share one place that
 * knows the field set and how to validate/convert each one.
 *
 * Precedence (highest to lowest) is achieved by call order, not by this
 * module tracking provenance: apply defaults, then the file and env (a
 * config file never sets secrets and the environment never sets
 * anything else, so those two don't compete), then CLI overrides last.
 */

typedef struct {
    char *api_key;   /* secret: environment or CLI only, never the file */
    char *host;
    int port;
    int use_tls;
    char *ca_file;   /* NULL = system default trust store */
    char *path;
    char *model;
    int timeout_ms;  /* resolved but not yet enforced by oi_llm -- no
                       * timer facility exists yet to act on it */
} oi_config;

/* Fills in built-in defaults, overwriting any existing content -- call
 * on a zeroed or freshly-allocated struct, not one with prior values
 * that need freeing first (use oi_config_free for that). Every string
 * field is heap-allocated (including these defaults), so oi_config_free
 * can unconditionally free all of them regardless of source.
 * OI_ERR_NOMEM leaves `cfg` partially filled in; free it either way. */
oi_status oi_config_init_defaults(oi_config *cfg);

/*
 * Sets one named field from a string value, parsing ints/bools as
 * needed. Recognized keys: "api_key", "host", "port", "use_tls",
 * "ca_file", "path", "model", "timeout_ms". Replaces (freeing the old
 * one) any existing value for string-typed keys.
 *
 * OI_ERR_NOTFOUND for an unrecognized key. OI_ERR_PARSE if `value`
 * doesn't parse for that key's type ("port"/"timeout_ms" need an
 * integer, "use_tls" needs true/false/1/0/yes/no, case-insensitive).
 */
oi_status oi_config_set(oi_config *cfg, const char *key, const char *value);

/* Reads OI_API_KEY from the environment into cfg->api_key. A no-op
 * (OI_OK) if the variable isn't set. */
oi_status oi_config_load_env(oi_config *cfg);

/*
 * Parses a `key = value` config file, one setting per line (`#`
 * comments and blank lines ignored), calling oi_config_set for each.
 * The "api_key" key is forbidden here (OI_ERR_DENIED) -- secrets belong
 * in the environment or a CLI flag, not a file that might get
 * committed to version control.
 *
 * OI_ERR_NOTFOUND if the file doesn't exist -- an optional config
 * file's absence is for the caller to decide whether that's fine (the
 * common case) or, e.g. when the user explicitly pointed at a path, an
 * error. Any other open() failure (permission denied, ...) is
 * OI_ERR_IO. OI_ERR_PARSE for a malformed line (no `=`) or whatever
 * oi_config_set returns for a bad key/value on a well-formed one.
 */
oi_status oi_config_load_file(oi_config *cfg, const char *path);

/* Frees every string field inside `cfg` (not `cfg` itself). Safe to
 * call on an already-defaulted, partially-loaded, or zeroed config. */
void oi_config_free(oi_config *cfg);

#endif /* OI_CONFIG_H */
