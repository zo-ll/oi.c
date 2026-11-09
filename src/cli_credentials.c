#include "cli_credentials.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "oi/arena.h"
#include "oi/json.h"

#define OI_CLI_CREDENTIALS_VERSION 1

static const char chat_route[] = "/chat/completions";

static void wipe(char *secret) {
    if (secret == NULL) {
        return;
    }
    /* Volatile so the compiler cannot drop a store to soon-freed memory. */
    for (volatile char *p = secret; *p != '\0'; p++) {
        *p = '\0';
    }
}

static void free_secret(char **slot) {
    wipe(*slot);
    free(*slot);
    *slot = NULL;
}

void oi_cli_credentials_init(struct oi_cli_credentials *credentials) {
    memset(credentials, 0, sizeof *credentials);
}

void oi_cli_credentials_free(struct oi_cli_credentials *credentials) {
    free_secret(&credentials->openai_api_key);
    free_secret(&credentials->openai_client_id);
    free_secret(&credentials->openai_host_id);
    free_secret(&credentials->openai_access_token);
    free_secret(&credentials->openai_refresh_token);
    free_secret(&credentials->openai_id_token);
    free_secret(&credentials->openai_email);
    free_secret(&credentials->openai_model);
    free_secret(&credentials->custom_api_key);
    free(credentials->custom_host);
    free(credentials->custom_path);
    oi_cli_credentials_init(credentials);
}

oi_status oi_cli_credentials_set(char **slot, const char *value) {
    char *copy = NULL;
    if (value != NULL) {
        copy = strdup(value);
        if (copy == NULL) {
            return OI_ERR_NOMEM;
        }
    }
    free_secret(slot);
    *slot = copy;
    return OI_OK;
}

static oi_status join(const char *base, const char *suffix, char **out) {
    size_t base_len = strlen(base);
    size_t suffix_len = strlen(suffix);
    char *joined = malloc(base_len + suffix_len + 1);
    if (joined == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(joined, base, base_len);
    memcpy(joined + base_len, suffix, suffix_len + 1);
    *out = joined;
    return OI_OK;
}

oi_status oi_cli_credentials_default_path(char **out_path) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] == '/') {
        return join(xdg, "/oi/credentials.json", out_path);
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] != '/') {
        return OI_ERR_NOTFOUND;
    }
    return join(home, "/.config/oi/credentials.json", out_path);
}

/* --- reading --- */

static oi_status read_private_file(const char *path, char **out_data,
                                   size_t *out_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return errno == ENOENT ? OI_ERR_NOTFOUND : OI_ERR_IO;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        return OI_ERR_IO;
    }
    if ((info.st_mode & 077) != 0 || info.st_uid != geteuid()) {
        close(fd);
        return OI_ERR_DENIED;
    }
    if ((size_t)info.st_size > OI_CLI_CREDENTIALS_MAX_FILE) {
        close(fd);
        return OI_ERR_PARSE;
    }
    size_t size = (size_t)info.st_size;
    char *data = malloc(size + 1);
    if (data == NULL) {
        close(fd);
        return OI_ERR_NOMEM;
    }
    size_t used = 0;
    while (used < size) {
        ssize_t got = read(fd, data + used, size - used);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            close(fd);
            free(data);
            return OI_ERR_IO;
        }
        used += (size_t)got;
    }
    close(fd);
    data[size] = '\0';
    *out_data = data;
    *out_len = size;
    return OI_OK;
}

static oi_status copy_optional_string(const oi_json_value *object,
                                      const char *key, char **slot) {
    const oi_json_value *value = oi_json_object_get(object, key);
    if (value == NULL) {
        return OI_OK;
    }
    const char *text;
    size_t len;
    if (oi_json_get_string(value, &text, &len) != OI_OK ||
        strlen(text) != len) {
        return OI_ERR_PARSE;
    }
    return oi_cli_credentials_set(slot, text);
}

static oi_status decode_custom(const oi_json_value *custom,
                               struct oi_cli_credentials *out) {
    double port = 0;
    int use_tls = 0;
    oi_status st = copy_optional_string(custom, "host", &out->custom_host);
    if (st == OI_OK) {
        st = copy_optional_string(custom, "path", &out->custom_path);
    }
    if (st == OI_OK) {
        st = copy_optional_string(custom, "api_key", &out->custom_api_key);
    }
    if (st != OI_OK) {
        return st;
    }
    if (oi_json_get_number(oi_json_object_get(custom, "port"), &port) !=
            OI_OK ||
        oi_json_get_bool(oi_json_object_get(custom, "use_tls"), &use_tls) !=
            OI_OK ||
        port < 1 || port > 65535 || port != (double)(int)port ||
        out->custom_host == NULL || out->custom_path == NULL) {
        return OI_ERR_PARSE;
    }
    out->custom_port = (int)port;
    out->custom_use_tls = use_tls;
    return OI_OK;
}

static oi_status decode_openai_oauth(const oi_json_value *openai,
                                     struct oi_cli_credentials *out) {
    const struct {
        const char *key;
        char **slot;
    } fields[] = {
        {"client_id", &out->openai_client_id},
        {"host_id", &out->openai_host_id},
        {"access_token", &out->openai_access_token},
        {"refresh_token", &out->openai_refresh_token},
        {"id_token", &out->openai_id_token},
        {"email", &out->openai_email},
        {"model", &out->openai_model},
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        oi_status st =
            copy_optional_string(openai, fields[i].key, fields[i].slot);
        if (st != OI_OK) {
            return st;
        }
    }
    const char *auth;
    size_t auth_len;
    out->openai_oauth =
        oi_json_get_string(oi_json_object_get(openai, "auth"), &auth,
                           &auth_len) == OI_OK &&
        strcmp(auth, "oauth") == 0;
    (void)oi_json_get_number(oi_json_object_get(openai, "expires_at"),
                             &out->openai_expires_at);
    (void)oi_json_get_bool(oi_json_object_get(openai, "welcome_shown"),
                           &out->openai_welcome_shown);
    return OI_OK;
}

static oi_status decode_credentials(const oi_json_value *root,
                                    struct oi_cli_credentials *out) {
    double version = 0;
    if (oi_json_get_number(oi_json_object_get(root, "version"), &version) !=
            OI_OK ||
        version != OI_CLI_CREDENTIALS_VERSION) {
        return OI_ERR_PARSE;
    }
    const char *active;
    size_t active_len;
    if (oi_json_get_string(oi_json_object_get(root, "active"), &active,
                           &active_len) != OI_OK) {
        return OI_ERR_PARSE;
    }
    if (strcmp(active, "openai") == 0) {
        out->active = OI_CLI_PROVIDER_OPENAI;
    } else if (strcmp(active, "custom") == 0) {
        out->active = OI_CLI_PROVIDER_CUSTOM;
    } else {
        return OI_ERR_PARSE;
    }
    const oi_json_value *openai = oi_json_object_get(root, "openai");
    oi_status st = copy_optional_string(openai, "api_key",
                                        &out->openai_api_key);
    if (st == OI_OK) {
        st = decode_openai_oauth(openai, out);
    }
    if (st != OI_OK) {
        return st;
    }
    const oi_json_value *custom = oi_json_object_get(root, "custom");
    if (custom != NULL) {
        st = decode_custom(custom, out);
    }
    if (st == OI_OK && out->active == OI_CLI_PROVIDER_CUSTOM &&
        custom == NULL) {
        st = OI_ERR_PARSE;
    }
    return st;
}

oi_status oi_cli_credentials_read(const char *path,
                                  struct oi_cli_credentials *out_credentials) {
    char *data = NULL;
    size_t len = 0;
    oi_status st = read_private_file(path, &data, &len);
    if (st != OI_OK) {
        return st;
    }
    oi_arena *arena = oi_arena_create(0);
    oi_json_parser *parser = arena != NULL ? oi_json_parser_create(arena) : NULL;
    if (parser == NULL) {
        st = OI_ERR_NOMEM;
    } else {
        st = oi_json_parser_feed(parser, data, len);
        if (st == OI_OK) {
            st = oi_json_parser_finish(parser);
        }
        if (st == OI_OK) {
            oi_cli_credentials_init(out_credentials);
            st = decode_credentials(oi_json_parser_root(parser),
                                    out_credentials);
            if (st != OI_OK) {
                oi_cli_credentials_free(out_credentials);
            }
        }
    }
    oi_json_parser_destroy(parser);
    oi_arena_destroy(arena);
    memset(data, 0, len);
    free(data);
    return st;
}

/* --- writing --- */

#define WRITE(expr)                                                           \
    do {                                                                      \
        oi_status write_status = (expr);                                      \
        if (write_status != OI_OK) {                                          \
            return write_status;                                              \
        }                                                                     \
    } while (0)

static oi_status write_member(oi_json_writer *writer, const char *key,
                              const char *value) {
    if (value == NULL) {
        return OI_OK;
    }
    WRITE(oi_json_write_object_key(writer, key, strlen(key)));
    return oi_json_write_string(writer, value, strlen(value));
}

static oi_status encode_credentials(
    oi_json_writer *writer, const struct oi_cli_credentials *credentials) {
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "version", 7));
    WRITE(oi_json_write_number(writer, OI_CLI_CREDENTIALS_VERSION));
    WRITE(write_member(writer, "active",
                       credentials->active == OI_CLI_PROVIDER_CUSTOM
                           ? "custom"
                           : "openai"));
    if (credentials->openai_api_key != NULL ||
        credentials->openai_host_id != NULL) {
        WRITE(oi_json_write_object_key(writer, "openai", 6));
        WRITE(oi_json_write_object_begin(writer));
        WRITE(write_member(writer, "auth",
                           credentials->openai_oauth ? "oauth" : "api_key"));
        WRITE(write_member(writer, "api_key", credentials->openai_api_key));
        WRITE(write_member(writer, "client_id", credentials->openai_client_id));
        WRITE(write_member(writer, "host_id", credentials->openai_host_id));
        WRITE(write_member(writer, "access_token",
                           credentials->openai_access_token));
        WRITE(write_member(writer, "refresh_token",
                           credentials->openai_refresh_token));
        WRITE(write_member(writer, "id_token", credentials->openai_id_token));
        WRITE(write_member(writer, "email", credentials->openai_email));
        WRITE(write_member(writer, "model", credentials->openai_model));
        WRITE(oi_json_write_object_key(writer, "expires_at", 10));
        WRITE(oi_json_write_number(writer, credentials->openai_expires_at));
        WRITE(oi_json_write_object_key(writer, "welcome_shown", 13));
        WRITE(oi_json_write_bool(writer, credentials->openai_welcome_shown));
        WRITE(oi_json_write_object_end(writer));
    }
    if (credentials->custom_host != NULL) {
        WRITE(oi_json_write_object_key(writer, "custom", 6));
        WRITE(oi_json_write_object_begin(writer));
        WRITE(write_member(writer, "host", credentials->custom_host));
        WRITE(oi_json_write_object_key(writer, "port", 4));
        WRITE(oi_json_write_number(writer, credentials->custom_port));
        WRITE(oi_json_write_object_key(writer, "use_tls", 7));
        WRITE(oi_json_write_bool(writer, credentials->custom_use_tls));
        WRITE(write_member(writer, "path", credentials->custom_path));
        WRITE(write_member(writer, "api_key", credentials->custom_api_key));
        WRITE(oi_json_write_object_end(writer));
    }
    return oi_json_write_object_end(writer);
}

static oi_status make_parent_directories(const char *path) {
    char *copy = strdup(path);
    if (copy == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_status st = OI_OK;
    for (char *slash = strchr(copy + 1, '/'); slash != NULL;
         slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
            st = OI_ERR_IO;
            break;
        }
        *slash = '/';
    }
    free(copy);
    return st;
}

static oi_status write_all_and_sync(int fd, const char *data, size_t len) {
    size_t used = 0;
    while (used < len) {
        ssize_t put = write(fd, data + used, len - used);
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            return OI_ERR_IO;
        }
        used += (size_t)put;
    }
    return fsync(fd) == 0 ? OI_OK : OI_ERR_IO;
}

oi_status oi_cli_credentials_write(
    const char *path, const struct oi_cli_credentials *credentials) {
    oi_status st = make_parent_directories(path);
    if (st != OI_OK) {
        return st;
    }
    oi_json_writer *writer = oi_json_writer_create();
    if (writer == NULL) {
        return OI_ERR_NOMEM;
    }
    st = encode_credentials(writer, credentials);
    char *tmp_path = NULL;
    if (st == OI_OK) {
        st = join(path, ".tmp", &tmp_path);
    }
    if (st == OI_OK) {
        (void)unlink(tmp_path);
        int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            st = OI_ERR_IO;
        } else {
            size_t len;
            const char *data = oi_json_writer_data(writer, &len);
            st = write_all_and_sync(fd, data, len);
            if (close(fd) != 0 && st == OI_OK) {
                st = OI_ERR_IO;
            }
            if (st == OI_OK && rename(tmp_path, path) != 0) {
                st = OI_ERR_IO;
            }
            if (st != OI_OK) {
                (void)unlink(tmp_path);
            }
        }
    }
    free(tmp_path);
    oi_json_writer_destroy(writer);
    return st;
}

/* --- URL parsing --- */

static int url_text_is_safe(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)text[i];
        if (byte <= 0x20 || byte == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static oi_status copy_span(const char *text, size_t len, char **out) {
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    *out = copy;
    return OI_OK;
}

static int parse_port(const char *text, size_t len, int *out_port) {
    if (len == 0 || len > 5) {
        return 0;
    }
    int port = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        port = port * 10 + (text[i] - '0');
    }
    if (port < 1 || port > 65535) {
        return 0;
    }
    *out_port = port;
    return 1;
}

static oi_status complete_path(const char *path, size_t path_len,
                               char **out_path) {
    size_t route_len = sizeof chat_route - 1;
    while (path_len > 0 && path[path_len - 1] == '/') {
        path_len--;
    }
    if (path_len == 0) {
        return copy_span("/v1/chat/completions", 20, out_path);
    }
    if (memchr(path, '?', path_len) != NULL ||
        (path_len >= route_len &&
         memcmp(path + path_len - route_len, chat_route, route_len) == 0)) {
        return copy_span(path, path_len, out_path);
    }
    char *joined = malloc(path_len + route_len + 1);
    if (joined == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(joined, path, path_len);
    memcpy(joined + path_len, chat_route, route_len + 1);
    *out_path = joined;
    return OI_OK;
}

oi_status oi_cli_credentials_parse_url(const char *url, size_t url_len,
                                       char **out_host, int *out_port,
                                       int *out_use_tls, char **out_path) {
    static const char https[] = "https://";
    static const char http[] = "http://";
    size_t scheme_len;
    int use_tls;
    if (!url_text_is_safe(url, url_len)) {
        return OI_ERR_PARSE;
    }
    if (url_len >= sizeof https - 1 &&
        memcmp(url, https, sizeof https - 1) == 0) {
        scheme_len = sizeof https - 1;
        use_tls = 1;
    } else if (url_len >= sizeof http - 1 &&
               memcmp(url, http, sizeof http - 1) == 0) {
        scheme_len = sizeof http - 1;
        use_tls = 0;
    } else {
        return OI_ERR_PARSE;
    }

    const char *authority = url + scheme_len;
    size_t rest = url_len - scheme_len;
    const char *slash = memchr(authority, '/', rest);
    size_t authority_len = slash != NULL ? (size_t)(slash - authority) : rest;
    if (authority_len == 0 || memchr(authority, '@', authority_len) != NULL ||
        memchr(authority, '[', authority_len) != NULL) {
        return OI_ERR_PARSE;
    }

    int port = use_tls ? 443 : 80;
    size_t host_len = authority_len;
    const char *colon = memchr(authority, ':', authority_len);
    if (colon != NULL) {
        host_len = (size_t)(colon - authority);
        if (host_len == 0 ||
            !parse_port(colon + 1, authority_len - host_len - 1, &port)) {
            return OI_ERR_PARSE;
        }
    }

    char *host = NULL;
    char *path = NULL;
    oi_status st = copy_span(authority, host_len, &host);
    if (st == OI_OK) {
        st = complete_path(slash != NULL ? slash : "",
                           slash != NULL ? rest - authority_len : 0, &path);
    }
    if (st != OI_OK) {
        free(host);
        return st;
    }
    *out_host = host;
    *out_port = port;
    *out_use_tls = use_tls;
    *out_path = path;
    return OI_OK;
}
