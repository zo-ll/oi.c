#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oi/config.h"
#include "oi/json.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/session.h"

/*
 * Thin standalone CLI: resolves config, opens (or resumes) a session,
 * sends one streaming chat-completion request built from a prompt
 * argument (or stdin), and prints the reply as it streams in. No
 * multi-turn loop, no tool calling -- driving an actual agent turn loop
 * is a session/tool-registry-level concern outside what issues #1-#10
 * chartered; this binary exists to prove the reactor/session/config
 * pieces compose into something runnable, per issue #9's literal scope.
 */

static const char *status_str(oi_status st) {
    switch (st) {
    case OI_OK:
        return "ok";
    case OI_ERR_INVAL:
        return "invalid argument";
    case OI_ERR_NOMEM:
        return "out of memory";
    case OI_ERR_IO:
        return "I/O error";
    case OI_ERR_AGAIN:
        return "would block";
    case OI_ERR_PARSE:
        return "parse error";
    case OI_ERR_CLOSED:
        return "closed";
    case OI_ERR_EXISTS:
        return "already exists";
    case OI_ERR_NOTFOUND:
        return "not found";
    case OI_ERR_DENIED:
        return "denied";
    case OI_ERR_TIMEOUT:
        return "timeout";
    default:
        return "unknown error";
    }
}

static void print_usage(void) {
    printf(
        "usage: oi [flags] [\"prompt\"]\n"
        "\n"
        "Sends one streaming chat-completion request and prints the reply.\n"
        "If no prompt argument is given, reads the prompt from stdin.\n"
        "\n"
        "flags:\n"
        "  --config PATH        config file to load\n"
        "  --session ID         session id (default: \"default\")\n"
        "  --session-dir DIR    directory for session logs (default: \".\")\n"
        "  --host HOST          API host (default: api.openai.com)\n"
        "  --port PORT          API port (default: 443)\n"
        "  --path PATH          API path (default: /v1/chat/completions)\n"
        "  --model MODEL        model name\n"
        "  --api-key KEY        API key (or set OI_API_KEY)\n"
        "  --ca-file PATH       custom CA bundle for TLS verification\n"
        "  --timeout-ms MS      end-to-end request timeout\n"
        "  --tls / --no-tls     use TLS (default: on)\n"
        "  --dry-run            resolve config and print the request, don't send it\n"
        "  -h, --help           show this help\n");
}

static const char *config_flag_key(const char *arg) {
    static const struct {
        const char *flag;
        const char *key;
    } table[] = {
        {"--host", "host"},       {"--port", "port"},
        {"--ca-file", "ca_file"}, {"--path", "path"},
        {"--model", "model"},     {"--timeout-ms", "timeout_ms"},
        {"--api-key", "api_key"},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(arg, table[i].flag) == 0) {
            return table[i].key;
        }
    }
    return NULL;
}

struct strbuf {
    char *data;
    size_t len;
    size_t cap;
};

static int strbuf_append(struct strbuf *b, const char *data, size_t len) {
    if (len > (size_t)-1 - b->len - 1) {
        return -1;
    }
    size_t needed = b->len + len + 1;
    if (needed > b->cap) {
        size_t new_cap = b->cap == 0 ? 256 : b->cap;
        while (new_cap < needed) {
            if (new_cap > (size_t)-1 / 2) {
                return -1;
            }
            new_cap *= 2;
        }
        char *nb = realloc(b->data, new_cap);
        if (nb == NULL) {
            return -1;
        }
        b->data = nb;
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

/* Reads all of stdin. An empty stream yields a valid, empty buffer. */
static char *read_stdin_all(oi_status *out_status) {
    struct strbuf b = {0};
    if (strbuf_append(&b, "", 0) != 0) { /* ensure non-NULL even if EOF immediately */
        *out_status = OI_ERR_NOMEM;
        return NULL;
    }
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0) {
        if (strbuf_append(&b, chunk, n) != 0) {
            free(b.data);
            *out_status = OI_ERR_NOMEM;
            return NULL;
        }
    }
    if (ferror(stdin)) {
        free(b.data);
        *out_status = OI_ERR_IO;
        return NULL;
    }
    if (b.len > 0 && b.data[b.len - 1] == '\n') {
        b.data[--b.len] = '\0';
    }
    *out_status = OI_OK;
    return b.data;
}

struct cli_ctx {
    struct strbuf response;
    oi_llm_request *request;
    int done;
    int exit_code;
};

static void on_delta(const char *text, size_t len, void *ud) {
    struct cli_ctx *ctx = ud;
    if (fwrite(text, 1, len, stdout) != len || fflush(stdout) != 0 ||
        strbuf_append(&ctx->response, text, len) != 0) {
        fprintf(stderr, "\noi: failed to handle streamed response\n");
        ctx->done = 1;
        ctx->exit_code = 1;
        oi_llm_request_cancel(ctx->request);
    }
}

static void on_done(oi_status status, int http_status, const char *error_body,
                     size_t error_body_len, void *ud) {
    struct cli_ctx *ctx = ud;
    ctx->done = 1;
    if (status != OI_OK) {
        fprintf(stderr, "\noi: request failed: %s (http status %d)",
                status_str(status), http_status);
        if (error_body != NULL && error_body_len > 0) {
            fprintf(stderr, ": %.*s", (int)error_body_len, error_body);
        }
        fprintf(stderr, "\n");
        ctx->exit_code = 1;
    }
}

struct replay_ctx {
    int index;
    int output_failed;
};

static void replay_cb(const void *data, size_t len, void *ud) {
    struct replay_ctx *ctx = ud;
    const char *role = (ctx->index % 2 == 0) ? "user" : "assistant";
    if (printf("[resumed %s] ", role) < 0 ||
        (len > 0 && fwrite(data, 1, len, stdout) != len) ||
        putchar('\n') == EOF) {
        ctx->output_failed = 1;
    }
    ctx->index++;
}

static oi_status build_request_body(const char *model, const char *prompt,
                                     oi_json_writer **out_writer) {
    oi_json_writer *w = oi_json_writer_create();
    if (w == NULL) {
        return OI_ERR_NOMEM;
    }

    oi_status st;
#define WRITE_JSON(expr)          \
    do {                          \
        st = (expr);              \
        if (st != OI_OK) {        \
            goto fail;            \
        }                         \
    } while (0)
    WRITE_JSON(oi_json_write_object_begin(w));
    WRITE_JSON(oi_json_write_object_key(w, "model", 5));
    WRITE_JSON(oi_json_write_string(w, model, strlen(model)));
    WRITE_JSON(oi_json_write_object_key(w, "stream", 6));
    WRITE_JSON(oi_json_write_bool(w, 1));
    WRITE_JSON(oi_json_write_object_key(w, "messages", 8));
    WRITE_JSON(oi_json_write_array_begin(w));
    WRITE_JSON(oi_json_write_object_begin(w));
    WRITE_JSON(oi_json_write_object_key(w, "role", 4));
    WRITE_JSON(oi_json_write_string(w, "user", 4));
    WRITE_JSON(oi_json_write_object_key(w, "content", 7));
    WRITE_JSON(oi_json_write_string(w, prompt, strlen(prompt)));
    WRITE_JSON(oi_json_write_object_end(w));
    WRITE_JSON(oi_json_write_array_end(w));
    WRITE_JSON(oi_json_write_object_end(w));
#undef WRITE_JSON

    *out_writer = w;
    return OI_OK;

fail:
#undef WRITE_JSON
    oi_json_writer_destroy(w);
    return st;
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *session_id = "default";
    const char *session_dir = ".";
    const char *prompt = NULL;
    int cli_use_tls = -1; /* -1 = not specified on the command line */
    int dry_run = 0;

    struct {
        const char *key;
        const char *value;
    } overrides[32];
    size_t n_overrides = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(arg, "--dry-run") == 0) {
            dry_run = 1;
            continue;
        }
        if (strcmp(arg, "--tls") == 0) {
            cli_use_tls = 1;
            continue;
        }
        if (strcmp(arg, "--no-tls") == 0) {
            cli_use_tls = 0;
            continue;
        }
        if (strcmp(arg, "--config") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "oi: --config requires a value\n");
                return 1;
            }
            config_path = argv[i];
            continue;
        }
        if (strcmp(arg, "--session") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "oi: --session requires a value\n");
                return 1;
            }
            session_id = argv[i];
            continue;
        }
        if (strcmp(arg, "--session-dir") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "oi: --session-dir requires a value\n");
                return 1;
            }
            session_dir = argv[i];
            continue;
        }

        const char *key = config_flag_key(arg);
        if (key != NULL) {
            if (++i >= argc) {
                fprintf(stderr, "oi: %s requires a value\n", arg);
                return 1;
            }
            if (n_overrides >= sizeof overrides / sizeof overrides[0]) {
                fprintf(stderr, "oi: too many flags\n");
                return 1;
            }
            overrides[n_overrides].key = key;
            overrides[n_overrides].value = argv[i];
            n_overrides++;
            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "oi: unrecognized flag: %s\n", arg);
            return 1;
        }
        if (prompt != NULL) {
            fprintf(stderr, "oi: unexpected extra argument: %s\n", arg);
            return 1;
        }
        prompt = arg;
    }

    oi_config cfg;
    if (oi_config_init_defaults(&cfg) != OI_OK) {
        fprintf(stderr, "oi: out of memory\n");
        return 1;
    }

    if (config_path != NULL) {
        oi_status st = oi_config_load_file(&cfg, config_path);
        if (st != OI_OK) {
            fprintf(stderr, "oi: failed to load config file '%s': %s\n",
                    config_path, status_str(st));
            oi_config_free(&cfg);
            return 1;
        }
    }

    if (oi_config_load_env(&cfg) != OI_OK) {
        fprintf(stderr, "oi: out of memory loading environment\n");
        oi_config_free(&cfg);
        return 1;
    }

    for (size_t i = 0; i < n_overrides; i++) {
        oi_status st = oi_config_set(&cfg, overrides[i].key, overrides[i].value);
        if (st != OI_OK) {
            fprintf(stderr, "oi: invalid value for --%s: %s (%s)\n",
                    overrides[i].key, overrides[i].value, status_str(st));
            oi_config_free(&cfg);
            return 1;
        }
    }
    if (cli_use_tls >= 0) {
        cfg.use_tls = cli_use_tls;
    }

    if (!dry_run && (cfg.api_key == NULL || cfg.api_key[0] == '\0')) {
        fprintf(stderr,
                "oi: no API key set (use OI_API_KEY or --api-key)\n");
        oi_config_free(&cfg);
        return 1;
    }

    int owns_prompt = 0;
    if (prompt == NULL) {
        oi_status read_status;
        char *stdin_prompt = read_stdin_all(&read_status);
        if (stdin_prompt == NULL) {
            fprintf(stderr, "oi: failed to read stdin: %s\n",
                    status_str(read_status));
            oi_config_free(&cfg);
            return 1;
        }
        if (stdin_prompt[0] == '\0') {
            fprintf(stderr,
                    "oi: no prompt given (pass one as an argument or pipe "
                    "it on stdin)\n");
            free(stdin_prompt);
            oi_config_free(&cfg);
            return 1;
        }
        prompt = stdin_prompt;
        owns_prompt = 1;
    }

    oi_json_writer *w = NULL;
    oi_status st = build_request_body(cfg.model, prompt, &w);
    if (st != OI_OK) {
        fprintf(stderr, "oi: failed to build request: %s\n", status_str(st));
        oi_config_free(&cfg);
        if (owns_prompt) {
            free((char *)prompt);
        }
        return 1;
    }
    size_t body_len;
    const char *body = oi_json_writer_data(w, &body_len);

    if (dry_run) {
        printf("host: %s\nport: %d\nuse_tls: %s\npath: %s\nmodel: %s\n",
               cfg.host, cfg.port, cfg.use_tls ? "true" : "false", cfg.path,
               cfg.model);
        printf("request body: %s\n", body);
        oi_json_writer_destroy(w);
        oi_config_free(&cfg);
        if (owns_prompt) {
            free((char *)prompt);
        }
        return 0;
    }

    char log_path[4096];
    int log_path_len = snprintf(log_path, sizeof log_path, "%s/%s.oilog",
                                session_dir, session_id);
    if (log_path_len < 0 || (size_t)log_path_len >= sizeof log_path) {
        fprintf(stderr, "oi: session log path is too long\n");
        oi_json_writer_destroy(w);
        oi_config_free(&cfg);
        if (owns_prompt) {
            free((char *)prompt);
        }
        return 1;
    }

    /* All of oi_reactor_destroy/oi_session_registry_destroy/
     * oi_llm_client_destroy are documented NULL-safe, so a single
     * cascading cleanup at the end handles every early-failure point
     * below without each one needing its own partial-teardown code --
     * whatever wasn't reached yet is just NULL and skipped. */
    oi_reactor *reactor = NULL;
    oi_session_registry *sessions = NULL;
    oi_llm_client *client = NULL;
    struct cli_ctx ctx = {0};
    int exit_code = 0;

    reactor = oi_reactor_create();
    sessions = oi_session_registry_create();
    if (reactor == NULL || sessions == NULL) {
        fprintf(stderr, "oi: out of memory\n");
        exit_code = 1;
        goto cleanup;
    }

    oi_session *session;
    st = oi_session_create(sessions, session_id, log_path, 0, &session);
    if (st != OI_OK) {
        fprintf(stderr, "oi: failed to open session '%s' at '%s': %s\n",
                session_id, log_path, status_str(st));
        exit_code = 1;
        goto cleanup;
    }
    struct replay_ctx rctx = {0};
    st = oi_sesslog_replay(oi_session_log(session), replay_cb, &rctx);
    if (st != OI_OK || rctx.output_failed) {
        fprintf(stderr, "oi: failed to replay session: %s\n",
                status_str(st != OI_OK ? st : OI_ERR_IO));
        exit_code = 1;
        goto cleanup;
    }

    struct oi_llm_config llm_cfg = {
        cfg.host, (unsigned short)cfg.port, cfg.use_tls,
        cfg.ca_file, cfg.api_key,           cfg.path,
        cfg.timeout_ms,
    };
    client = oi_llm_client_create(&llm_cfg);
    if (client == NULL) {
        fprintf(stderr, "oi: out of memory\n");
        exit_code = 1;
        goto cleanup;
    }

    oi_llm_request *req;
    st = oi_llm_request_start(client, reactor, oi_session_arena(session),
                               body, body_len, on_delta, on_done, &ctx, &req);
    if (st != OI_OK) {
        fprintf(stderr, "oi: failed to start request: %s\n", status_str(st));
        exit_code = 1;
        goto cleanup;
    }
    ctx.request = req;

    while (!ctx.done) {
        oi_status step_st;
        oi_reactor_step(reactor, -1, &step_st);
        if (step_st != OI_OK) {
            fprintf(stderr, "oi: reactor error: %s\n", status_str(step_st));
            ctx.exit_code = 1;
            break;
        }
    }
    printf("\n");

    if (ctx.exit_code == 0) {
        st = oi_sesslog_append(oi_session_log(session), prompt,
                                strlen(prompt));
        if (st == OI_OK) {
            st = oi_sesslog_append(oi_session_log(session), ctx.response.data,
                                    ctx.response.len);
        }
        if (st != OI_OK) {
            fprintf(stderr, "oi: failed to persist session: %s\n",
                    status_str(st));
            oi_session_fail(session);
            ctx.exit_code = 1;
        }
    }
    exit_code = ctx.exit_code;

cleanup:
    free(ctx.response.data);
    oi_llm_client_destroy(client);
    oi_session_registry_destroy(sessions);
    oi_reactor_destroy(reactor);
    oi_json_writer_destroy(w);
    oi_config_free(&cfg);
    if (owns_prompt) {
        free((char *)prompt);
    }
    return exit_code;
}
