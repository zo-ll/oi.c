#include "oi/config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static oi_status set_string(char **field, const char *value) {
    char *copy = strdup(value);
    if (copy == NULL) {
        return OI_ERR_NOMEM;
    }
    free(*field);
    *field = copy;
    return OI_OK;
}

static oi_status parse_int(const char *value, int *out) {
    if (value == NULL || *value == '\0') {
        return OI_ERR_PARSE;
    }
    char *end;
    errno = 0;
    long v = strtol(value, &end, 10);
    if (*end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return OI_ERR_PARSE;
    }
    *out = (int)v;
    return OI_OK;
}

static oi_status parse_bool(const char *value, int *out) {
    if (value == NULL) {
        return OI_ERR_PARSE;
    }
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "1") == 0 ||
        strcasecmp(value, "yes") == 0) {
        *out = 1;
        return OI_OK;
    }
    if (strcasecmp(value, "false") == 0 || strcasecmp(value, "0") == 0 ||
        strcasecmp(value, "no") == 0) {
        *out = 0;
        return OI_OK;
    }
    return OI_ERR_PARSE;
}

oi_status oi_config_init_defaults(oi_config *cfg) {
    if (cfg == NULL) {
        return OI_ERR_INVAL;
    }
    cfg->api_key = NULL;
    cfg->host = NULL;
    cfg->port = 443;
    cfg->use_tls = 1;
    cfg->ca_file = NULL;
    cfg->path = NULL;
    cfg->model = NULL;
    cfg->timeout_ms = 60000;

    oi_status st = set_string(&cfg->host, "api.openai.com");
    if (st != OI_OK) {
        return st;
    }
    st = set_string(&cfg->path, "/v1/chat/completions");
    if (st != OI_OK) {
        return st;
    }
    return set_string(&cfg->model, "gpt-4o-mini");
}

oi_status oi_config_set(oi_config *cfg, const char *key, const char *value) {
    if (cfg == NULL || key == NULL || value == NULL) {
        return OI_ERR_INVAL;
    }

    if (strcmp(key, "api_key") == 0) {
        return set_string(&cfg->api_key, value);
    }
    if (strcmp(key, "host") == 0) {
        return set_string(&cfg->host, value);
    }
    if (strcmp(key, "port") == 0) {
        return parse_int(value, &cfg->port);
    }
    if (strcmp(key, "use_tls") == 0) {
        return parse_bool(value, &cfg->use_tls);
    }
    if (strcmp(key, "ca_file") == 0) {
        return set_string(&cfg->ca_file, value);
    }
    if (strcmp(key, "path") == 0) {
        return set_string(&cfg->path, value);
    }
    if (strcmp(key, "model") == 0) {
        return set_string(&cfg->model, value);
    }
    if (strcmp(key, "timeout_ms") == 0) {
        return parse_int(value, &cfg->timeout_ms);
    }
    return OI_ERR_NOTFOUND;
}

oi_status oi_config_load_env(oi_config *cfg) {
    if (cfg == NULL) {
        return OI_ERR_INVAL;
    }
    const char *v = getenv("OI_API_KEY");
    if (v == NULL || *v == '\0') {
        return OI_OK;
    }
    return set_string(&cfg->api_key, v);
}

static void trim(char **start, char **end) {
    while (*start < *end && (**start == ' ' || **start == '\t')) {
        (*start)++;
    }
    while (*end > *start &&
           ((*end)[-1] == ' ' || (*end)[-1] == '\t' || (*end)[-1] == '\r' ||
            (*end)[-1] == '\n')) {
        (*end)--;
    }
}

static oi_status parse_line(oi_config *cfg, char *line) {
    char *start = line;
    char *end = line + strlen(line);
    trim(&start, &end);
    if (start == end) {
        return OI_OK; /* blank */
    }
    if (*start == '#') {
        return OI_OK; /* comment */
    }

    char *eq = memchr(start, '=', (size_t)(end - start));
    if (eq == NULL) {
        return OI_ERR_PARSE;
    }

    char *key_start = start;
    char *key_end = eq;
    trim(&key_start, &key_end);
    if (key_start == key_end) {
        return OI_ERR_PARSE;
    }

    char *value_start = eq + 1;
    char *value_end = end;
    trim(&value_start, &value_end);

    *key_end = '\0';
    *value_end = '\0';

    if (strcmp(key_start, "api_key") == 0) {
        return OI_ERR_DENIED; /* secrets don't belong in a config file */
    }
    return oi_config_set(cfg, key_start, value_start);
}

oi_status oi_config_load_file(oi_config *cfg, const char *path) {
    if (cfg == NULL || path == NULL) {
        return OI_ERR_INVAL;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return errno == ENOENT ? OI_ERR_NOTFOUND : OI_ERR_IO;
    }

    char line[1024];
    oi_status st = OI_OK;
    while (st == OI_OK && fgets(line, sizeof line, f) != NULL) {
        st = parse_line(cfg, line);
    }
    fclose(f);
    return st;
}

void oi_config_free(oi_config *cfg) {
    if (cfg == NULL) {
        return;
    }
    free(cfg->api_key);
    free(cfg->host);
    free(cfg->ca_file);
    free(cfg->path);
    free(cfg->model);
    cfg->api_key = NULL;
    cfg->host = NULL;
    cfg->ca_file = NULL;
    cfg->path = NULL;
    cfg->model = NULL;
}
