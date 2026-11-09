#include "cli_login.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cli_credentials.h"
#include "cli_oauth.h"

#define LINE(text) {text, sizeof text - 1}
#define COUNT(lines) (sizeof lines / sizeof lines[0])

enum { PROVIDER_OPENAI = 0, PROVIDER_CUSTOM };
enum { METHOD_API_KEY = 0, METHOD_OAUTH };

static void free_text_secret(char **text) {
    (void)oi_cli_credentials_set(text, NULL);
}

static const char openai_host[] = "api.openai.com";
static const char openai_path[] = "/v1/chat/completions";
static const char responses_path[] = "/v1/responses";
static const char default_model[] = "gpt-4o-mini";
static const char usage_url[] = "https://chatgpt.com/settings/usage";

/* Renew this long before expiry, so that a long turn keeps a valid token. */
#define REFRESH_MARGIN_SECONDS 300.0
#define CALLBACK_TIMEOUT_MS (5 * 60 * 1000)

static const struct oi_cli_render_line provider_header[] = {
    LINE("Log in: choose a provider"),
};
static const struct oi_cli_render_line provider_options[] = {
    LINE("OpenAI"),
    LINE("Custom OpenAI-compatible endpoint"),
};
static const struct oi_cli_render_line method_header[] = {
    LINE("Choose a login method"),
};
static const struct oi_cli_render_line method_options[] = {
    LINE("API key"),
    LINE("OAuth"),
};
static const struct oi_cli_render_line url_header[] = {
    LINE("Enter the endpoint URL and press Enter (Esc cancels)"),
};
static const struct oi_cli_render_line key_header[] = {
    LINE("Paste the API key and press Enter (Esc cancels)"),
};

void oi_cli_login_free(struct oi_cli_login *login) {
    free(login->credentials_path);
    free(login->host);
    free(login->path);
    free(login->selected_model);
    login->credentials_path = NULL;
    login->host = NULL;
    login->path = NULL;
    login->selected_model = NULL;
}

static int is_default_endpoint(const oi_config *cfg) {
    return strcmp(cfg->host, openai_host) == 0 && cfg->port == 443 &&
           cfg->use_tls && strcmp(cfg->path, openai_path) == 0;
}

static oi_status apply_custom(oi_config *cfg,
                              const struct oi_cli_credentials *saved) {
    char port[16];
    snprintf(port, sizeof port, "%d", saved->custom_port);
    oi_status st = oi_config_set(cfg, "host", saved->custom_host);
    if (st == OI_OK) {
        st = oi_config_set(cfg, "port", port);
    }
    if (st == OI_OK) {
        st = oi_config_set(cfg, "use_tls",
                           saved->custom_use_tls ? "true" : "false");
    }
    if (st == OI_OK) {
        st = oi_config_set(cfg, "path", saved->custom_path);
    }
    if (st == OI_OK && saved->custom_api_key != NULL) {
        st = oi_config_set(cfg, "api_key", saved->custom_api_key);
    }
    return st;
}

static oi_status apply_chatgpt(oi_config *cfg,
                               const struct oi_cli_credentials *saved) {
    struct oi_cli_oauth_server auth;
    struct oi_cli_oauth_server api;
    int is_test = 0;
    char port[16];
    oi_status st = oi_cli_oauth_servers(&auth, &api, &is_test);
    if (st != OI_OK) {
        return st;
    }
    snprintf(port, sizeof port, "%d", api.port);
    st = oi_config_set(cfg, "host", api.host);
    if (st == OI_OK) {
        st = oi_config_set(cfg, "port", port);
    }
    if (st == OI_OK) {
        st = oi_config_set(cfg, "use_tls", api.use_tls ? "true" : "false");
    }
    if (st == OI_OK) {
        st = oi_config_set(cfg, "path", responses_path);
    }
    if (st == OI_OK) {
        st = oi_config_set(cfg, "api_key", saved->openai_access_token);
    }
    if (st == OI_OK && saved->openai_model != NULL &&
        strcmp(cfg->model, default_model) == 0) {
        st = oi_config_set(cfg, "model", saved->openai_model);
    }
    oi_cli_oauth_server_free(&auth);
    oi_cli_oauth_server_free(&api);
    return st;
}

oi_status oi_cli_login_apply_saved(oi_config *cfg, const char *path,
                                   FILE *err, int *out_oauth,
                                   double *out_expires_at) {
    *out_oauth = 0;
    *out_expires_at = 0;
    if (path == NULL || (cfg->api_key != NULL && cfg->api_key[0] != '\0') ||
        !is_default_endpoint(cfg)) {
        return OI_OK;
    }
    struct oi_cli_credentials saved;
    oi_status st = oi_cli_credentials_read(path, &saved);
    if (st == OI_ERR_NOTFOUND) {
        return OI_OK;
    }
    if (st != OI_OK) {
        fprintf(err, "oi: ignoring saved credentials in %s: %s\n", path,
                oi_status_str(st));
        return OI_OK;
    }
    if (saved.active == OI_CLI_PROVIDER_CUSTOM) {
        st = apply_custom(cfg, &saved);
    } else if (saved.openai_oauth && saved.openai_access_token != NULL) {
        st = apply_chatgpt(cfg, &saved);
        *out_oauth = st == OI_OK;
        *out_expires_at = saved.openai_expires_at;
    } else if (saved.openai_api_key != NULL) {
        st = oi_config_set(cfg, "api_key", saved.openai_api_key);
    }
    oi_cli_credentials_free(&saved);
    return st;
}

struct login_target {
    int provider;
    const char *host;
    const char *path;
    int port;
    int use_tls;
};

static oi_status save_credentials(const struct oi_cli_login *login,
                                  const struct login_target *target,
                                  const char *key) {
    struct oi_cli_credentials saved;
    oi_status st = oi_cli_credentials_read(login->credentials_path, &saved);
    if (st == OI_ERR_NOTFOUND) {
        oi_cli_credentials_init(&saved);
    } else if (st != OI_OK) {
        return st;
    }
    if (target->provider == PROVIDER_OPENAI) {
        saved.active = OI_CLI_PROVIDER_OPENAI;
        saved.openai_oauth = 0;
        st = oi_cli_credentials_set(&saved.openai_api_key, key);
    } else {
        saved.active = OI_CLI_PROVIDER_CUSTOM;
        saved.custom_port = target->port;
        saved.custom_use_tls = target->use_tls;
        st = oi_cli_credentials_set(&saved.custom_host, target->host);
        if (st == OI_OK) {
            st = oi_cli_credentials_set(&saved.custom_path, target->path);
        }
        if (st == OI_OK) {
            st = oi_cli_credentials_set(&saved.custom_api_key, key);
        }
    }
    if (st == OI_OK) {
        st = oi_cli_credentials_write(login->credentials_path, &saved);
    }
    oi_cli_credentials_free(&saved);
    return st;
}

static oi_status adopt_target(struct oi_cli_login *login,
                              const struct login_target *target) {
    char *host = strdup(target->host);
    char *path = strdup(target->path);
    if (host == NULL || path == NULL) {
        free(host);
        free(path);
        return OI_ERR_NOMEM;
    }
    free(login->host);
    free(login->path);
    login->host = host;
    login->path = path;
    login->endpoint->host = host;
    login->endpoint->path = path;
    login->endpoint->port = (unsigned short)target->port;
    login->endpoint->use_tls = target->use_tls;
    return OI_OK;
}

/* Reports through `message`; a non-OK return is a structural failure. */
static oi_status log_in_with_key(struct oi_cli_login *login,
                                 const struct login_target *target,
                                 const char *key, char *message,
                                 size_t message_size) {
    struct oi_llm_config llm = {
        target->host, (unsigned short)target->port, target->use_tls,
        login->ca_file, key, target->path, login->timeout_ms,
    };
    if (oi_llm_client_reconfigure(login->client, &llm, 0) != OI_OK) {
        snprintf(message, message_size,
                 "oi: login failed: that key or endpoint is not valid\n");
        return OI_OK;
    }
    login->oauth_active = 0;
    oi_status st = adopt_target(login, target);
    if (st != OI_OK) {
        return st;
    }

    oi_status saved = login->credentials_path == NULL
                          ? OI_ERR_NOTFOUND
                          : save_credentials(login, target, key);
    const char *name =
        target->provider == PROVIDER_OPENAI ? "OpenAI" : target->host;
    int used;
    if (saved != OI_OK) {
        used = snprintf(message, message_size,
                        "oi: logged in to %s for this run only; the key was "
                        "not saved: %s\n",
                        name, oi_status_str(saved));
    } else {
        used = snprintf(message, message_size,
                        "oi: logged in to %s with an API key\n", name);
    }
    if (!target->use_tls && used > 0 && (size_t)used < message_size) {
        used += snprintf(message + used, message_size - (size_t)used,
                         "oi: warning: this endpoint has no TLS, so the key "
                         "is sent unencrypted\n");
    }
    if (target->provider == PROVIDER_CUSTOM && used > 0 &&
        (size_t)used < message_size) {
        used += snprintf(message + used, message_size - (size_t)used,
                         "oi: use /model to choose a model on this endpoint\n");
    }
    if (saved == OI_OK && login->key_from_environment && used > 0 &&
        (size_t)used < message_size) {
        snprintf(message + used, message_size - (size_t)used,
                 "oi: at the next start, OI_API_KEY or --api-key takes "
                 "priority over the saved key\n");
    }
    return OI_OK;
}

/* --- token renewal --- */

static oi_status adopt_access_token(struct oi_cli_login *login,
                                    const char *access_token,
                                    double expires_at) {
    struct oi_llm_config llm = {
        login->endpoint->host, login->endpoint->port, login->endpoint->use_tls,
        login->ca_file, access_token, responses_path, login->timeout_ms,
    };
    oi_status st = oi_llm_client_reconfigure(login->client, &llm, 1);
    if (st == OI_OK) {
        login->expires_at = expires_at;
    }
    return st;
}

static int lock_credentials(const char *path) {
    char lock_path[4096];
    if (snprintf(lock_path, sizeof lock_path, "%s.lock", path) >=
        (int)sizeof lock_path) {
        return -1;
    }
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }
    struct flock lock;
    memset(&lock, 0, sizeof lock);
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    int rc;
    do {
        rc = fcntl(fd, F_SETLKW, &lock);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* A transient failure is harmless while the current token still works. */
static oi_status refresh_unavailable(const struct oi_cli_login *login,
                                     double now, const char *reason) {
    if (login->expires_at > now) {
        return OI_OK;
    }
    fprintf(login->err, "oi: could not renew the ChatGPT login: %s\n", reason);
    return OI_ERR_IO;
}

static oi_status session_ended(const struct oi_cli_login *login) {
    fputs("oi: the ChatGPT login has ended: use /login\n", login->err);
    return OI_ERR_DENIED;
}

static oi_status refresh_with_saved(struct oi_cli_login *login,
                                    struct oi_cli_credentials *saved,
                                    double now) {
    struct oi_cli_oauth_server auth;
    struct oi_cli_oauth_server api;
    struct oi_cli_oauth_tokens tokens;
    int is_test = 0;
    oi_status st = oi_cli_oauth_servers(&auth, &api, &is_test);
    if (st != OI_OK) {
        return st;
    }
    st = oi_cli_oauth_refresh(&auth, login->ca_file, saved->openai_client_id,
                              saved->openai_refresh_token, &tokens);
    oi_cli_oauth_server_free(&auth);
    oi_cli_oauth_server_free(&api);
    if (st == OI_ERR_NOMEM) {
        return st;
    }
    if (st != OI_OK) {
        return refresh_unavailable(login, now, oi_status_str(st));
    }
    if (tokens.access_token == NULL) {
        if (oi_cli_oauth_error_is_terminal(tokens.error)) {
            (void)oi_cli_credentials_set(&saved->openai_access_token, NULL);
            (void)oi_cli_credentials_set(&saved->openai_refresh_token, NULL);
            (void)oi_cli_credentials_write(login->credentials_path, saved);
            oi_cli_oauth_tokens_free(&tokens);
            return session_ended(login);
        }
        st = refresh_unavailable(login, now, tokens.error);
        oi_cli_oauth_tokens_free(&tokens);
        return st;
    }

    double expires_at = now + tokens.expires_in;
    st = oi_cli_credentials_set(&saved->openai_access_token,
                                tokens.access_token);
    if (st == OI_OK && tokens.refresh_token != NULL) {
        st = oi_cli_credentials_set(&saved->openai_refresh_token,
                                    tokens.refresh_token);
    }
    if (st == OI_OK && tokens.id_token != NULL) {
        st = oi_cli_credentials_set(&saved->openai_id_token, tokens.id_token);
    }
    if (st == OI_OK) {
        saved->openai_expires_at = expires_at;
        if (oi_cli_credentials_write(login->credentials_path, saved) != OI_OK) {
            fputs("oi: warning: could not save the renewed ChatGPT login\n",
                  login->err);
        }
        st = adopt_access_token(login, tokens.access_token, expires_at);
    }
    oi_cli_oauth_tokens_free(&tokens);
    return st;
}

oi_status oi_cli_login_before_request(void *user_data) {
    struct oi_cli_login *login = user_data;
    double now = (double)time(NULL);
    if (!login->oauth_active ||
        login->expires_at - now > REFRESH_MARGIN_SECONDS) {
        return OI_OK;
    }
    if (login->credentials_path == NULL) {
        return refresh_unavailable(login, now, "no credentials file");
    }

    int lock_fd = lock_credentials(login->credentials_path);
    if (lock_fd < 0) {
        return refresh_unavailable(login, now, "could not lock credentials");
    }
    struct oi_cli_credentials saved;
    oi_status st = oi_cli_credentials_read(login->credentials_path, &saved);
    if (st != OI_OK) {
        close(lock_fd);
        return st == OI_ERR_NOMEM
                   ? st
                   : refresh_unavailable(login, now, oi_status_str(st));
    }
    if (!saved.openai_oauth || saved.openai_client_id == NULL ||
        saved.openai_refresh_token == NULL ||
        saved.openai_access_token == NULL) {
        st = session_ended(login);
    } else if (saved.openai_expires_at - now > REFRESH_MARGIN_SECONDS) {
        /* Another process renewed the token while this one waited. */
        st = adopt_access_token(login, saved.openai_access_token,
                                saved.openai_expires_at);
    } else {
        st = refresh_with_saved(login, &saved, now);
    }
    oi_cli_credentials_free(&saved);
    close(lock_fd);
    return st;
}

/* --- ChatGPT sign-in --- */

static int open_callback_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(OI_CLI_OAUTH_REDIRECT_PORT);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Tries the usual openers; the URL is on screen if none of them works. */
static void open_browser(const char *url) {
    static const char script[] =
        "wslview \"$1\" || xdg-open \"$1\" || open \"$1\" || "
        "powershell.exe -NoProfile -Command \"Start-Process '$1'\"";
    pid_t child = fork();
    if (child < 0) {
        return;
    }
    if (child == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setsid();
        if (fork() != 0) {
            _exit(0);
        }
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
        }
        execl("/bin/sh", "sh", "-c", script, "sh", url, (char *)NULL);
        _exit(127);
    }
    (void)waitpid(child, NULL, 0);
}

static void read_request_head(int fd, char *head, size_t size) {
    size_t used = 0;
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    while (used < size - 1 && poll(&descriptor, 1, 3000) > 0) {
        ssize_t got = read(fd, head + used, size - 1 - used);
        if (got <= 0) {
            break;
        }
        used += (size_t)got;
        head[used] = '\0';
        if (strstr(head, "\r\n\r\n") != NULL) {
            break;
        }
    }
    head[used] = '\0';
}

static void reply_to_browser(int fd, const char *status, const char *text) {
    char response[512];
    int len = snprintf(response, sizeof response,
                       "HTTP/1.1 %s\r\nContent-Type: text/plain; "
                       "charset=utf-8\r\nContent-Length: %zu\r\n"
                       "Connection: close\r\n\r\n%s",
                       status, strlen(text), text);
    if (len > 0 && (size_t)len < sizeof response) {
        (void)!write(fd, response, (size_t)len);
    }
}

struct chatgpt_wait {
    int cancelled;
    int timed_out;
    struct oi_cli_oauth_callback callback;
};

static oi_status wait_for_callback(struct oi_cli_composer *composer,
                                   int signal_fd, int listen_fd,
                                   struct chatgpt_wait *wait,
                                   int *out_terminate_signal) {
    static const struct oi_cli_render_line status_line =
        LINE("Waiting for the browser sign-in (Esc cancels)");
    int remaining = CALLBACK_TIMEOUT_MS;
    for (;;) {
        struct timespec before;
        struct timespec after;
        int ready = 0;
        clock_gettime(CLOCK_MONOTONIC, &before);
        oi_status st = oi_cli_composer_wait_fd(
            composer, signal_fd, &status_line, listen_fd, remaining, &ready,
            &wait->cancelled, out_terminate_signal);
        if (st != OI_OK || wait->cancelled || *out_terminate_signal != 0) {
            return st;
        }
        if (!ready) {
            wait->timed_out = 1;
            return OI_OK;
        }
        clock_gettime(CLOCK_MONOTONIC, &after);
        remaining -= (int)((after.tv_sec - before.tv_sec) * 1000);

        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            continue;
        }
        char head[8192];
        read_request_head(client, head, sizeof head);
        st = oi_cli_oauth_parse_callback(head, &wait->callback);
        if (st == OI_ERR_NOMEM) {
            close(client);
            return st;
        }
        if (st != OI_OK || !wait->callback.is_callback) {
            reply_to_browser(client, "404 Not Found", "Not found.\n");
            close(client);
            oi_cli_oauth_callback_free(&wait->callback);
            continue;
        }
        reply_to_browser(client, "200 OK",
                         "oi received the sign-in. You can close this "
                         "window.\n");
        close(client);
        return OI_OK;
    }
}

static oi_status choose_chatgpt_model(struct oi_cli_login *login,
                                      struct oi_cli_composer *composer,
                                      int signal_fd,
                                      const struct oi_cli_oauth_server *api,
                                      const char *access_token,
                                      struct oi_cli_credentials *saved,
                                      int *out_terminate_signal) {
    static const struct oi_cli_render_line header[] = {LINE("Choose a model")};
    struct oi_cli_oauth_models models;
    oi_status st = oi_cli_oauth_fetch_models(api, login->ca_file, access_token,
                                             &models);
    if (st == OI_ERR_NOMEM) {
        return st;
    }
    if (st != OI_OK || models.count == 0) {
        return OI_OK;
    }

    struct oi_cli_render_line options[OI_CLI_OAUTH_MAX_MODELS];
    size_t selected = 0;
    size_t initial = 0;
    int cancelled = 0;
    for (size_t i = 0; i < models.count; i++) {
        const char *label = models.items[i].name != NULL
                                ? models.items[i].name
                                : models.items[i].slug;
        options[i].text = label;
        options[i].len = strlen(label);
        if (saved->openai_model != NULL &&
            strcmp(saved->openai_model, models.items[i].slug) == 0) {
            initial = i;
        }
    }
    st = oi_cli_composer_select(composer, signal_fd, header, COUNT(header),
                                options, models.count, initial, &selected,
                                &cancelled, out_terminate_signal);
    if (st == OI_OK && *out_terminate_signal == 0 && !cancelled) {
        const char *slug = models.items[selected].slug;
        st = oi_cli_credentials_set(&saved->openai_model, slug);
        if (st == OI_OK) {
            st = oi_cli_credentials_set(&login->selected_model, slug);
        }
    }
    oi_cli_oauth_models_free(&models);
    return st;
}

static oi_status show_plan_notice(struct oi_cli_composer *composer,
                                  int signal_fd,
                                  struct oi_cli_credentials *saved,
                                  int *out_terminate_signal) {
    static const struct oi_cli_render_line header[] = {
        LINE("You're using your ChatGPT plan"),
        LINE("Eligible usage in this app uses your ChatGPT plan. Manage "
             "usage in your ChatGPT settings."),
    };
    static const struct oi_cli_render_line options[] = {LINE("Got it")};
    size_t selected = 0;
    int cancelled = 0;
    if (saved->openai_welcome_shown) {
        return OI_OK;
    }
    oi_status st = oi_cli_composer_select(
        composer, signal_fd, header, COUNT(header), options, COUNT(options), 0,
        &selected, &cancelled, out_terminate_signal);
    if (st == OI_OK && *out_terminate_signal == 0) {
        saved->openai_welcome_shown = 1;
    }
    return st;
}

struct chatgpt_flow {
    struct oi_cli_oauth_server auth;
    struct oi_cli_oauth_server api;
    int is_test;
    struct oi_cli_credentials saved;
    char *verifier;
    char *state;
    char *nonce;
    char *challenge;
    char *url;
    char *email;
    struct chatgpt_wait wait;
    struct oi_cli_oauth_tokens tokens;
};

static void chatgpt_flow_free(struct chatgpt_flow *flow) {
    oi_cli_oauth_server_free(&flow->auth);
    oi_cli_oauth_server_free(&flow->api);
    oi_cli_credentials_free(&flow->saved);
    free_text_secret(&flow->verifier);
    free_text_secret(&flow->state);
    free_text_secret(&flow->nonce);
    free_text_secret(&flow->challenge);
    free_text_secret(&flow->url);
    free(flow->email);
    oi_cli_oauth_callback_free(&flow->wait.callback);
    oi_cli_oauth_tokens_free(&flow->tokens);
}

static oi_status prepare_chatgpt_flow(const struct oi_cli_login *login,
                                      struct chatgpt_flow *flow) {
    oi_status st = oi_cli_credentials_read(login->credentials_path,
                                           &flow->saved);
    if (st == OI_ERR_NOTFOUND) {
        oi_cli_credentials_init(&flow->saved);
        st = OI_OK;
    }
    if (st == OI_OK) {
        st = oi_cli_oauth_servers(&flow->auth, &flow->api, &flow->is_test);
    }
    if (st == OI_OK && flow->saved.openai_host_id == NULL) {
        st = oi_cli_oauth_host_id(&flow->saved.openai_host_id);
        if (st == OI_OK) {
            /* The host identity must exist before the first registration. */
            st = oi_cli_credentials_write(login->credentials_path,
                                          &flow->saved);
        }
    }
    if (st == OI_OK) {
        st = oi_cli_oauth_random_token(&flow->verifier);
    }
    if (st == OI_OK) {
        st = oi_cli_oauth_random_token(&flow->state);
    }
    if (st == OI_OK) {
        st = oi_cli_oauth_random_token(&flow->nonce);
    }
    if (st == OI_OK) {
        st = oi_cli_oauth_challenge(flow->verifier, &flow->challenge);
    }
    if (st == OI_OK) {
        struct oi_cli_oauth_authorize request = {
            flow->saved.openai_client_id, flow->saved.openai_host_id,
            flow->state, flow->nonce, flow->challenge,
            flow->saved.openai_client_id != NULL ? flow->saved.openai_id_token
                                                 : NULL,
        };
        st = oi_cli_oauth_authorize_url(&flow->auth, &request, &flow->url);
    }
    return st;
}

/* Validates the callback; on a refusal, writes `message` and returns 0. */
static int callback_is_acceptable(const struct chatgpt_flow *flow,
                                  char *message, size_t message_size) {
    const struct oi_cli_oauth_callback *callback = &flow->wait.callback;
    const char *known = flow->saved.openai_client_id;
    const char *reason = NULL;
    if (callback->state == NULL || strcmp(callback->state, flow->state) != 0) {
        reason = "the browser response did not match this attempt";
    } else if (callback->error != NULL) {
        reason = strcmp(callback->error, "access_denied") == 0
                     ? "access was denied in the browser"
                     : "the sign-in server returned an error";
    } else if (callback->code == NULL) {
        reason = "the browser response had no code";
    } else if (known == NULL && callback->client_id == NULL) {
        reason = "OpenAI did not complete the client registration";
    } else if (known != NULL && callback->client_id != NULL &&
               strcmp(known, callback->client_id) != 0) {
        reason = "OpenAI returned a different client";
    }
    if (reason != NULL) {
        snprintf(message, message_size, "oi: login failed: %s\n", reason);
        return 0;
    }
    return 1;
}

static oi_status store_chatgpt_tokens(struct chatgpt_flow *flow,
                                      const char *client_id, double now) {
    struct oi_cli_credentials *saved = &flow->saved;
    saved->active = OI_CLI_PROVIDER_OPENAI;
    saved->openai_oauth = 1;
    saved->openai_expires_at = now + flow->tokens.expires_in;
    oi_status st = oi_cli_credentials_set(&saved->openai_client_id, client_id);
    if (st == OI_OK) {
        st = oi_cli_credentials_set(&saved->openai_access_token,
                                    flow->tokens.access_token);
    }
    if (st == OI_OK) {
        st = oi_cli_credentials_set(&saved->openai_refresh_token,
                                    flow->tokens.refresh_token);
    }
    if (st == OI_OK) {
        st = oi_cli_credentials_set(&saved->openai_id_token,
                                    flow->tokens.id_token);
    }
    if (st == OI_OK) {
        st = oi_cli_credentials_set(&saved->openai_email, flow->email);
    }
    return st;
}

/* Exchanges the code and installs the login; refusals go to `message`. */
static oi_status finish_chatgpt_login(struct oi_cli_login *login,
                                      struct oi_cli_composer *composer,
                                      int signal_fd, struct chatgpt_flow *flow,
                                      int *out_terminate_signal, char *message,
                                      size_t message_size) {
    const char *client_id = flow->saved.openai_client_id != NULL
                                ? flow->saved.openai_client_id
                                : flow->wait.callback.client_id;
    char *client_copy = strdup(client_id);
    if (client_copy == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_status st = oi_cli_oauth_exchange_code(
        &flow->auth, login->ca_file, client_copy, flow->wait.callback.code,
        flow->verifier, &flow->tokens);
    if (st != OI_OK || flow->tokens.access_token == NULL) {
        if (st != OI_ERR_NOMEM) {
            snprintf(message, message_size, "oi: login failed: %s\n",
                     st != OI_OK ? oi_status_str(st) : flow->tokens.error);
            st = OI_OK;
        }
        free(client_copy);
        return st;
    }
    if (flow->tokens.refresh_token == NULL || flow->tokens.id_token == NULL) {
        snprintf(message, message_size,
                 "oi: login failed: the token response was incomplete\n");
        free(client_copy);
        return OI_OK;
    }
    if (flow->tokens.scope != NULL &&
        strstr(flow->tokens.scope, "chatgpt.tokens.use.direct") == NULL) {
        snprintf(message, message_size,
                 "oi: login failed: this account did not grant ChatGPT plan "
                 "usage\n");
        free(client_copy);
        return OI_OK;
    }
    double now = (double)time(NULL);
    st = oi_cli_oauth_check_id_token(flow->tokens.id_token, flow->nonce,
                                     client_copy, now, &flow->email);
    if (st != OI_OK) {
        if (st != OI_ERR_NOMEM) {
            snprintf(message, message_size,
                     "oi: login failed: the ID token did not validate\n");
            st = OI_OK;
        }
        free(client_copy);
        return st;
    }

    st = store_chatgpt_tokens(flow, client_copy, now);
    free(client_copy);
    if (st != OI_OK) {
        return st;
    }
    oi_status save_st =
        oi_cli_credentials_write(login->credentials_path, &flow->saved);

    struct login_target target = {
        PROVIDER_OPENAI, flow->api.host, responses_path, flow->api.port,
        flow->api.use_tls,
    };
    st = adopt_target(login, &target);
    if (st != OI_OK) {
        return st;
    }
    st = adopt_access_token(login, flow->tokens.access_token,
                            flow->saved.openai_expires_at);
    if (st != OI_OK) {
        snprintf(message, message_size,
                 "oi: login failed: the access token was not usable\n");
        return OI_OK;
    }
    login->oauth_active = 1;

    st = choose_chatgpt_model(login, composer, signal_fd, &flow->api,
                              flow->tokens.access_token, &flow->saved,
                              out_terminate_signal);
    if (st == OI_OK && *out_terminate_signal == 0) {
        st = show_plan_notice(composer, signal_fd, &flow->saved,
                              out_terminate_signal);
    }
    if (st != OI_OK || *out_terminate_signal != 0) {
        return st;
    }
    if (save_st == OI_OK) {
        save_st =
            oi_cli_credentials_write(login->credentials_path, &flow->saved);
    }

    int used = snprintf(message, message_size,
                        "oi: logged in to OpenAI%s%s\n"
                        "oi: Using ChatGPT plan. Manage usage: %s\n",
                        flow->email != NULL ? " as " : "",
                        flow->email != NULL ? flow->email : "", usage_url);
    if (save_st != OI_OK && used > 0 && (size_t)used < message_size) {
        used += snprintf(message + used, message_size - (size_t)used,
                         "oi: warning: the login was not saved: %s\n",
                         oi_status_str(save_st));
    }
    if (login->selected_model == NULL && used > 0 &&
        (size_t)used < message_size) {
        used += snprintf(message + used, message_size - (size_t)used,
                         "oi: use /model to choose a model\n");
    }
    if (login->key_from_environment && used > 0 &&
        (size_t)used < message_size) {
        snprintf(message + used, message_size - (size_t)used,
                 "oi: at the next start, OI_API_KEY or --api-key takes "
                 "priority over this login\n");
    }
    return OI_OK;
}

static oi_status log_in_with_chatgpt(struct oi_cli_login *login,
                                     struct oi_cli_composer *composer,
                                     int signal_fd, int *out_terminate_signal,
                                     char *message, size_t message_size) {
    if (login->credentials_path == NULL) {
        snprintf(message, message_size,
                 "oi: login failed: no place to save credentials\n");
        return OI_OK;
    }
    struct chatgpt_flow flow;
    memset(&flow, 0, sizeof flow);
    int listen_fd = -1;
    oi_status st = prepare_chatgpt_flow(login, &flow);
    if (st != OI_OK) {
        if (st != OI_ERR_NOMEM) {
            snprintf(message, message_size,
                     "oi: login failed: could not read or save credentials: "
                     "%s\n",
                     oi_status_str(st));
            st = OI_OK;
        }
        goto done;
    }
    listen_fd = open_callback_listener();
    if (listen_fd < 0) {
        snprintf(message, message_size,
                 "oi: login failed: could not listen on 127.0.0.1:%d. Stop "
                 "the program that uses this port and try again\n",
                 OI_CLI_OAUTH_REDIRECT_PORT);
        goto done;
    }

    st = oi_cli_composer_redraw(composer);
    if (st != OI_OK) {
        goto done;
    }
    if (fprintf(login->err, "oi: open this URL to sign in:\n%s\n", flow.url) <
            0 ||
        fflush(login->err) != 0) {
        st = OI_ERR_IO;
        goto done;
    }
    if (!flow.is_test) {
        open_browser(flow.url);
    }

    st = wait_for_callback(composer, signal_fd, listen_fd, &flow.wait,
                           out_terminate_signal);
    close(listen_fd);
    listen_fd = -1;
    if (st != OI_OK || *out_terminate_signal != 0 || flow.wait.cancelled) {
        goto done;
    }
    if (flow.wait.timed_out) {
        snprintf(message, message_size,
                 "oi: login failed: the browser sign-in timed out\n");
        goto done;
    }
    if (callback_is_acceptable(&flow, message, message_size)) {
        st = finish_chatgpt_login(login, composer, signal_fd, &flow,
                                  out_terminate_signal, message, message_size);
    }

done:
    if (listen_fd >= 0) {
        close(listen_fd);
    }
    chatgpt_flow_free(&flow);
    return st;
}

static oi_status run_dialog(struct oi_cli_login *login,
                            struct oi_cli_composer *composer, int signal_fd,
                            int *out_terminate_signal, char *message,
                            size_t message_size) {
    static const char cancelled_text[] = "oi: login cancelled\n";
    size_t provider = 0;
    size_t method = 0;
    int cancelled = 0;

    snprintf(message, message_size, "%s", cancelled_text);
    oi_status st = oi_cli_composer_select(
        composer, signal_fd, provider_header, COUNT(provider_header),
        provider_options, COUNT(provider_options), 0, &provider, &cancelled,
        out_terminate_signal);
    if (st != OI_OK || *out_terminate_signal != 0 || cancelled) {
        return st;
    }
    st = oi_cli_composer_select(composer, signal_fd, method_header,
                                COUNT(method_header), method_options,
                                COUNT(method_options), 0, &method, &cancelled,
                                out_terminate_signal);
    if (st != OI_OK || *out_terminate_signal != 0 || cancelled) {
        return st;
    }
    if (method == METHOD_OAUTH) {
        if (provider == PROVIDER_OPENAI) {
            return log_in_with_chatgpt(login, composer, signal_fd,
                                       out_terminate_signal, message,
                                       message_size);
        }
        snprintf(message, message_size,
                 "oi: OAuth is not available for custom endpoints\n");
        return OI_OK;
    }

    struct login_target target = {
        (int)provider, openai_host, openai_path, 443, 1,
    };
    char *url = NULL;
    char *host = NULL;
    char *path = NULL;
    char *key = NULL;
    size_t len = 0;
    if (provider == PROVIDER_CUSTOM) {
        st = oi_cli_composer_read_line(composer, signal_fd, url_header,
                                       COUNT(url_header), "URL: ", 0, &url,
                                       &len, &cancelled, out_terminate_signal);
        if (st != OI_OK || *out_terminate_signal != 0 || cancelled) {
            goto done;
        }
        if (oi_cli_credentials_parse_url(url, len, &host, &target.port,
                                         &target.use_tls, &path) != OI_OK) {
            snprintf(message, message_size,
                     "oi: login failed: enter a full http or https URL\n");
            goto done;
        }
        target.host = host;
        target.path = path;
    }
    st = oi_cli_composer_read_line(composer, signal_fd, key_header,
                                   COUNT(key_header), "API key: ", 1, &key,
                                   &len, &cancelled, out_terminate_signal);
    if (st != OI_OK || *out_terminate_signal != 0 || cancelled) {
        goto done;
    }
    if (len == 0) {
        snprintf(message, message_size,
                 "oi: login failed: no API key entered\n");
        goto done;
    }
    st = log_in_with_key(login, &target, key, message, message_size);

done:
    free(url);
    free(host);
    free(path);
    free_text_secret(&key);
    return st;
}

oi_status oi_cli_login_run(struct oi_cli_login *login,
                           struct oi_cli_composer *composer, int signal_fd,
                           int *out_terminate_signal) {
    char message[768];
    *out_terminate_signal = 0;
    oi_status st = run_dialog(login, composer, signal_fd, out_terminate_signal,
                              message, sizeof message);
    if (st != OI_OK || *out_terminate_signal != 0) {
        return st;
    }
    st = oi_cli_composer_redraw(composer);
    if (st != OI_OK) {
        return st;
    }
    if (fputs(message, login->err) == EOF || fflush(login->err) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}
