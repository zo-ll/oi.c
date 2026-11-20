#ifndef OI_CONFIG_H
#define OI_CONFIG_H

#include "oi/status.h"

typedef struct {
    char *api_key;
    char *host;
    int port;
    int use_tls;
    char *ca_file;
    char *path;
    char *model;
    int timeout_ms;
} oi_config;

oi_status oi_config_init_defaults(oi_config *cfg);

oi_status oi_config_set(oi_config *cfg, const char *key, const char *value);

oi_status oi_config_load_env(oi_config *cfg);

oi_status oi_config_load_file(oi_config *cfg, const char *path);

void oi_config_free(oi_config *cfg);

#endif
