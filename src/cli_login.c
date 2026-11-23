#include "cli_login.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cli_credentials.h"
#include "cli_oauth.h"

#define LINE(text) {text, sizeof text - 1}
#define COUNT(lines) (sizeof lines / sizeof lines[0])

enum { PROVIDER_OPENAI = 0, PROVIDER_OPENCODE_GO };

static void free_text_secret(char **text) {
    (void)oi_cli_credentials_set(text, NULL);
}

static const char openai_host[] = "api.openai.com";
static const char openai_path[] = "/v1/chat/completions";
static const char responses_path[] = "/v1/responses";
static const char default_model[] = "gpt-4o-mini";
static const char opencode_go_host[] = "opencode.ai";
static const char opencode_go_path[] = "/inference/go/openai/v1/chat/completions";
static const char opencode_go_model[] = "deepseek-v4.1-flash";
static const char usage_url[] = "https://chatgpt.com/settings/usage";

#define REFRESH_MARGIN_SECONDS 300.0
#define USABLE_SECONDS 30.0
#define CALLBACK_TIMEOUT_MS (5 * 60 * 1000)
#define HEAD_TIMEOUT_MS 3000

static const struct oi_cli_render_line provider_header[] = {
    LINE("Log in: choose a provider"),
};
static const struct oi_cli_render_line provider_options[] = {
    [PROVIDER_OPENAI] = LINE("OpenAI (Sign in with ChatGPT)"),
    [PROVIDER_OPENCODE_GO] = LINE("OpenCode Go (API key)"),
};
static const struct oi_cli_render_line key_header[] = {
    LINE("Paste the API key and press Enter (Esc cancels)"),
};

static void renewal_free(struct login_renewal *renewal);
static void finish_sent_renewal(struct oi_cli_login *login);

void oi_cli_login_free(struct oi_cli_login *login) {
    finish_sent_renewal(login);
    renewal_free(login->renewal);
    login->renewal = NULL;
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

static oi_status apply_opencode_go(oi_config *cfg, const char *key) {
    oi_status st = oi_config_set(cfg, "host", opencode_go_host);
    if (st == OI_OK) {
        st = oi_config_set(cfg, "path", opencode_go_path);
    }
    if (st == OI_OK) {
        st = oi_config_set(cfg, "api_key", key);
    }
    if (st == OI_OK && strcmp(cfg->model, default_model) == 0) {
        st = oi_config_set(cfg, "model", opencode_go_model);
    }
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
    } else if (saved.active == OI_CLI_PROVIDER_OPENCODE_GO) {
        st = apply_opencode_go(cfg, saved.opencode_go_api_key);
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
    const char *host;
    const char *path;
    int port;
    int use_tls;
};

static oi_status save_opencode_go_key(const struct oi_cli_login *login,
                                      const char *key) {
    struct oi_cli_credentials saved;
    oi_status st = oi_cli_credentials_read(login->credentials_path, &saved);
    if (st == OI_ERR_NOTFOUND) {
        oi_cli_credentials_init(&saved);
    } else if (st != OI_OK) {
        return st;
    }
    saved.active = OI_CLI_PROVIDER_OPENCODE_GO;
    st = oi_cli_credentials_set(&saved.opencode_go_api_key, key);
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

static oi_status log_in_to_opencode_go(struct oi_cli_login *login,
                                      const char *key, char *message,
                                      size_t message_size) {
    const struct login_target target = {
        opencode_go_host, opencode_go_path, 443, 1,
    };
    struct oi_llm_config llm = {
        target.host, (unsigned short)target.port, target.use_tls,
        login->ca_file, key, target.path, login->timeout_ms,
    };
    if (oi_llm_client_reconfigure(login->client, &llm, 0) != OI_OK) {
        snprintf(message, message_size,
                 "oi: login failed: that key or endpoint is not valid\n");
        return OI_OK;
    }
    login->oauth_active = 0;
    oi_status st = adopt_target(login, &target);
    if (st == OI_OK) {
        st = oi_cli_credentials_set(&login->selected_model, opencode_go_model);
    }
    if (st != OI_OK) {
        return st;
    }

    oi_status saved = login->credentials_path == NULL
                          ? OI_ERR_NOTFOUND
                          : save_opencode_go_key(login, key);
    int used;
    if (saved != OI_OK) {
        used = snprintf(message, message_size,
                        "oi: OpenCode Go key set for this run only; the key "
                        "was not saved: %s\n", oi_status_str(saved));
    } else {
        used = snprintf(message, message_size,
                        "oi: OpenCode Go key saved\n");
    }
    if (saved == OI_OK && login->key_from_environment && used > 0 &&
        (size_t)used < message_size) {
        snprintf(message + used, message_size - (size_t)used,
                 "oi: at the next start, OI_API_KEY or --api-key takes "
                 "priority over the saved key\n");
    }
    return OI_OK;
}

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
        errno = ENAMETOOLONG;
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
        rc = fcntl(fd, F_SETLK, &lock);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

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

struct login_renewal {
    struct oi_cli_login *login;
    struct oi_cli_oauth_refresh_call *call;
    int lock_fd;
    struct oi_cli_credentials saved;
    int finished;
    oi_status status;
    int ended;
    char *access_token;
    double expires_at;
    char reason[128];
};

static void renewal_free(struct login_renewal *renewal) {
    if (renewal == NULL) {
        return;
    }
    oi_cli_oauth_refresh_cancel(renewal->call);
    if (renewal->lock_fd >= 0) {
        close(renewal->lock_fd);
    }
    oi_cli_credentials_free(&renewal->saved);
    free_text_secret(&renewal->access_token);
    free(renewal);
}

static int token_usable(const struct oi_cli_login *login, double now) {
    return login->expires_at - now > USABLE_SECONDS;
}

static void renewal_done(oi_status st, struct oi_cli_oauth_tokens *tokens,
                         void *user_data) {
    struct login_renewal *renewal = user_data;
    struct oi_cli_credentials *saved = &renewal->saved;
    const char *path = renewal->login->credentials_path;
    renewal->call = NULL;
    renewal->finished = 1;
    renewal->status = st == OI_ERR_NOMEM ? OI_ERR_NOMEM : OI_OK;
    if (st != OI_OK) {
        snprintf(renewal->reason, sizeof renewal->reason, "%s",
                 oi_status_str(st));
    } else if (tokens->access_token == NULL) {
        if (oi_cli_oauth_error_is_terminal(tokens->error)) {
            (void)oi_cli_credentials_set(&saved->openai_access_token, NULL);
            (void)oi_cli_credentials_set(&saved->openai_refresh_token, NULL);
            (void)oi_cli_credentials_write(path, saved);
            renewal->ended = 1;
        } else {
            snprintf(renewal->reason, sizeof renewal->reason, "%s",
                     tokens->error);
        }
    } else {
        double expires_at = (double)time(NULL) + tokens->expires_in;
        oi_status save = oi_cli_credentials_set(&saved->openai_access_token,
                                                tokens->access_token);
        if (save == OI_OK && tokens->refresh_token != NULL) {
            save = oi_cli_credentials_set(&saved->openai_refresh_token,
                                          tokens->refresh_token);
        }
        if (save == OI_OK && tokens->id_token != NULL) {
            save = oi_cli_credentials_set(&saved->openai_id_token,
                                          tokens->id_token);
        }
        if (save == OI_OK) {
            saved->openai_expires_at = expires_at;
            if (oi_cli_credentials_write(path, saved) != OI_OK) {
                fputs("oi: warning: could not save the renewed ChatGPT "
                      "login\n",
                      renewal->login->err);
            }
            save = oi_cli_credentials_set(&renewal->access_token,
                                          tokens->access_token);
        }
        renewal->expires_at = expires_at;
        if (save != OI_OK) {
            renewal->status = save;
        }
    }
    oi_cli_oauth_tokens_free(tokens);
    close(renewal->lock_fd);
    renewal->lock_fd = -1;
}

static oi_status settle_renewal(struct oi_cli_login *login, double now) {
    struct login_renewal *renewal = login->renewal;
    login->renewal = NULL;
    oi_status st;
    if (renewal->status != OI_OK) {
        st = renewal->status;
    } else if (renewal->access_token != NULL) {
        st = adopt_access_token(login, renewal->access_token,
                                renewal->expires_at);
    } else if (renewal->ended) {
        st = session_ended(login);
    } else {
        st = refresh_unavailable(login, now, renewal->reason);
    }
    renewal_free(renewal);
    return st;
}

static oi_status start_renewal(struct oi_cli_login *login, double now) {
    if (login->credentials_path == NULL) {
        return refresh_unavailable(login, now, "no credentials file");
    }
    struct login_renewal *renewal = calloc(1, sizeof *renewal);
    if (renewal == NULL) {
        return OI_ERR_NOMEM;
    }
    renewal->login = login;
    renewal->lock_fd = lock_credentials(login->credentials_path);
    if (renewal->lock_fd < 0) {
        int busy = errno == EAGAIN || errno == EACCES;
        free(renewal);
        return busy ? OI_OK
                    : refresh_unavailable(login, now,
                                          "could not lock credentials");
    }
    oi_status st = oi_cli_credentials_read(login->credentials_path,
                                           &renewal->saved);
    if (st != OI_OK) {
        close(renewal->lock_fd);
        free(renewal);
        return st == OI_ERR_NOMEM
                   ? st
                   : refresh_unavailable(login, now, oi_status_str(st));
    }
    struct oi_cli_credentials *saved = &renewal->saved;
    if (!saved->openai_oauth || saved->openai_client_id == NULL ||
        saved->openai_refresh_token == NULL ||
        saved->openai_access_token == NULL) {
        st = session_ended(login);
    } else if (saved->openai_expires_at - now > REFRESH_MARGIN_SECONDS) {
        st = adopt_access_token(login, saved->openai_access_token,
                                saved->openai_expires_at);
    } else {
        struct oi_cli_oauth_server auth;
        struct oi_cli_oauth_server api;
        int is_test = 0;
        st = oi_cli_oauth_servers(&auth, &api, &is_test);
        if (st == OI_OK) {
            st = oi_cli_oauth_refresh_start(
                login->reactor, &auth, login->ca_file,
                saved->openai_client_id, saved->openai_refresh_token,
                renewal_done, renewal, &renewal->call);
            oi_cli_oauth_server_free(&auth);
            oi_cli_oauth_server_free(&api);
        }
        if (st == OI_OK) {
            login->renewal = renewal;
            return OI_OK;
        }
        st = st == OI_ERR_NOMEM ? st
                                : refresh_unavailable(login, now,
                                                      oi_status_str(st));
    }
    renewal_free(renewal);
    return st;
}

static int renewal_running_and_sent(const struct oi_cli_login *login) {
    return login->renewal != NULL && !login->renewal->finished &&
           oi_cli_oauth_refresh_sent(login->renewal->call);
}

static void interrupt_wait(oi_reactor *reactor, int fd, int revents,
                           void *user_data) {
    (void)reactor;
    (void)revents;
    struct signalfd_siginfo info;
    while (read(fd, &info, sizeof info) == (ssize_t)sizeof info) {
    }
    *(int *)user_data = 1;
}

static void finish_sent_renewal(struct oi_cli_login *login) {
    if (!renewal_running_and_sent(login)) {
        return;
    }
    fputs("oi: finishing the ChatGPT token renewal (Ctrl+C skips it)\n",
          login->err);
    fflush(login->err);
    int interrupted = 0;
    int sigint_fd = -1;
    sigset_t current;
    if (pthread_sigmask(SIG_BLOCK, NULL, &current) == 0 &&
        sigismember(&current, SIGINT)) {
        sigset_t only;
        sigemptyset(&only);
        sigaddset(&only, SIGINT);
        sigint_fd = signalfd(-1, &only, SFD_NONBLOCK | SFD_CLOEXEC);
        if (sigint_fd >= 0 &&
            oi_reactor_add(login->reactor, sigint_fd, OI_EV_READ,
                           interrupt_wait, &interrupted) != OI_OK) {
            close(sigint_fd);
            sigint_fd = -1;
        }
    }
    while (!interrupted && renewal_running_and_sent(login)) {
        oi_status st = OI_OK;
        if (oi_reactor_step(login->reactor, 1000, &st) < 0) {
            break;
        }
    }
    if (sigint_fd >= 0) {
        oi_reactor_remove(login->reactor, sigint_fd);
        close(sigint_fd);
    }
}

oi_status oi_cli_login_before_request(void *user_data) {
    struct oi_cli_login *login = user_data;
    double now = (double)time(NULL);
    if (!login->oauth_active) {
        return OI_OK;
    }
    if (login->renewal != NULL && login->renewal->finished) {
        oi_status st = settle_renewal(login, now);
        if (st != OI_OK) {
            return st;
        }
    }
    if (login->expires_at - now > REFRESH_MARGIN_SECONDS) {
        return OI_OK;
    }
    if (login->renewal == NULL) {
        oi_status st = start_renewal(login, now);
        if (st != OI_OK || login->expires_at - now > REFRESH_MARGIN_SECONDS) {
            return st;
        }
    }
    return token_usable(login, now) ? OI_OK : OI_ERR_AGAIN;
}

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

static int64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static const struct oi_cli_render_line callback_status_line =
    LINE("Waiting for the browser sign-in (Esc cancels)");

static oi_status read_request_head(struct oi_cli_composer *composer,
                                   int signal_fd, int fd, char *head,
                                   size_t size, int *out_cancelled,
                                   int *out_terminate_signal) {
    size_t used = 0;
    int64_t deadline = monotonic_ms() + HEAD_TIMEOUT_MS;
    int64_t left;
    head[0] = '\0';
    while (used < size - 1 && (left = deadline - monotonic_ms()) > 0) {
        int ready = 0;
        oi_status st = oi_cli_composer_wait_fd(
            composer, signal_fd, &callback_status_line, fd, (int)left, &ready,
            out_cancelled, out_terminate_signal);
        if (st != OI_OK || *out_cancelled || *out_terminate_signal != 0) {
            return st;
        }
        if (!ready) {
            break;
        }
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
    return OI_OK;
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
                                   const char *state,
                                   struct chatgpt_wait *wait,
                                   int *out_terminate_signal) {
    int64_t deadline = monotonic_ms() + CALLBACK_TIMEOUT_MS;
    for (;;) {
        int ready = 0;
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) {
            wait->timed_out = 1;
            return OI_OK;
        }
        oi_status st = oi_cli_composer_wait_fd(
            composer, signal_fd, &callback_status_line, listen_fd,
            (int)remaining, &ready, &wait->cancelled, out_terminate_signal);
        if (st != OI_OK || wait->cancelled || *out_terminate_signal != 0) {
            return st;
        }
        if (!ready) {
            wait->timed_out = 1;
            return OI_OK;
        }

        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            continue;
        }
        char head[8192];
        st = read_request_head(composer, signal_fd, client, head, sizeof head,
                               &wait->cancelled, out_terminate_signal);
        if (st != OI_OK || wait->cancelled || *out_terminate_signal != 0) {
            close(client);
            return st;
        }
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
        if (wait->callback.state == NULL ||
            strcmp(wait->callback.state, state) != 0) {
            reply_to_browser(client, "400 Bad Request",
                             "This sign-in response is not for the current "
                             "attempt.\n");
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

static int callback_is_acceptable(const struct chatgpt_flow *flow,
                                  char *message, size_t message_size) {
    const struct oi_cli_oauth_callback *callback = &flow->wait.callback;
    const char *known = flow->saved.openai_client_id;
    const char *reason = NULL;
    if (callback->error != NULL) {
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
        flow->api.host, responses_path, flow->api.port,
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

    st = wait_for_callback(composer, signal_fd, listen_fd, flow.state,
                           &flow.wait, out_terminate_signal);
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
    int cancelled = 0;

    snprintf(message, message_size, "%s", cancelled_text);
    oi_status st = oi_cli_composer_select(
        composer, signal_fd, provider_header, COUNT(provider_header),
        provider_options, COUNT(provider_options), 0, &provider, &cancelled,
        out_terminate_signal);
    if (st != OI_OK || *out_terminate_signal != 0 || cancelled) {
        return st;
    }
    if (provider == PROVIDER_OPENAI) {
        return log_in_with_chatgpt(login, composer, signal_fd,
                                   out_terminate_signal, message,
                                   message_size);
    }

    char *key = NULL;
    size_t len = 0;
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
    st = log_in_to_opencode_go(login, key, message, message_size);

done:
    free_text_secret(&key);
    return st;
}

oi_status oi_cli_login_run(struct oi_cli_login *login,
                           struct oi_cli_composer *composer, int signal_fd,
                           int *out_terminate_signal) {
    char message[768];
    *out_terminate_signal = 0;
    int isig = oi_cli_terminal_set_isig(&composer->terminal, 1) == OI_OK;
    finish_sent_renewal(login);
    oi_status st =
        isig ? oi_cli_terminal_set_isig(&composer->terminal, 0) : OI_OK;
    renewal_free(login->renewal);
    login->renewal = NULL;
    if (st != OI_OK) {
        return st;
    }
    st = run_dialog(login, composer, signal_fd, out_terminate_signal,
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
