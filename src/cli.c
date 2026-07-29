#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli_loop.h"
#include "cli_repl.h"
#include "cli_sessions.h"
#include "cli_history_repair.h"
#include "cli_history_store.h"
#include "cli_tools.h"
#include "oi/config.h"
#include "oi/json.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/session.h"

/*
 * Thin standalone CLI: resolves config, opens (or resumes) a session,
 * sends a prompt argument (or stdin) through the CLI-owned model/tool
 * loop, and prints assistant replies as they stream. Configuration and
 * session lifecycle stay here; loop state and built-in tool policy live
 * in their own internal modules.
 */

static void print_usage(void) {
    printf(
        "usage: oi [flags] [\"prompt\"]\n"
        "\n"
        "Runs an interactive chat on a terminal, or one model turn when given\n"
        "a prompt argument or redirected stdin.\n"
        "\n"
        "flags:\n"
        "  --config PATH        config file to load\n"
        "  --session ID         durable named session (omitted: ephemeral)\n"
        "  --session-dir DIR    directory for session logs (default: \".\")\n"
        "  --host HOST          API host (default: api.openai.com)\n"
        "  --port PORT          API port (default: 443)\n"
        "  --path PATH          API path (default: /v1/chat/completions)\n"
        "  --model MODEL        model name\n"
        "  --api-key KEY        API key (or set OI_API_KEY)\n"
        "  --ca-file PATH       custom CA bundle for TLS verification\n"
        "  --timeout-ms MS      end-to-end request timeout\n"
        "  --max-turns N        maximum model turns (default: 8)\n"
        "  --allow-tools        run requested tools without prompting\n"
        "  --deny-tools         reject every requested tool\n"
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

struct persistence_context {
    struct oi_cli_history_store *store;
    struct oi_cli_history_replay_state *state;
    uint64_t turn_id;
    struct oi_cli_string model; /* owned; may be updated live by /model */
    const char *metadata_path;  /* borrowed, stable for the process lifetime */
    const char *session_id;    /* borrowed */
    oi_status last_error;
};

struct automatic_session_context {
    oi_session_registry *registry;
    oi_session **session;
    const char *root_override;
    struct oi_cli_session_location *location;
    struct oi_cli_history_store *store;
    struct oi_cli_history_replay_state *state;
    struct persistence_context *persistence;
    struct oi_cli_string *pending_model;
    const char *default_cwd;
    FILE *diagnostics;
};

static oi_status prepare_automatic_session(void *user_data,
                                           oi_arena **out_arena) {
    struct automatic_session_context *context = user_data;
    struct oi_cli_session_restore restore;
    oi_status status;

    if (context == NULL || context->registry == NULL ||
        context->session == NULL || context->location == NULL ||
        context->store == NULL || context->state == NULL ||
        context->persistence == NULL || context->pending_model == NULL ||
        context->pending_model->data == NULL ||
        context->default_cwd == NULL || out_arena == NULL ||
        *context->session != NULL) {
        return OI_ERR_INVAL;
    }
    status = oi_cli_session_location_create(context->root_override,
                                            context->location);
    if (status == OI_OK) {
        status = oi_session_create(
            context->registry, context->location->id,
            context->location->history_path, 0, context->session);
    }
    if (status == OI_OK) {
        status = oi_cli_history_store_load(
            oi_session_log(*context->session), context->store,
            context->state);
    }
    if (status == OI_OK && context->state->needs_transition) {
        struct oi_cli_history_record transition;

        oi_cli_history_record_init(&transition);
        status = oi_cli_history_record_set_transition(
            &transition, context->state->next_record_id,
            context->store->legacy_messages.len);
        if (status == OI_OK) {
            status = oi_cli_history_store_append(
                context->store, &transition, context->state);
        }
        oi_cli_history_record_free(&transition);
    }
    if (status != OI_OK) {
        return status;
    }

    /* Every automatic session is brand new: bare interactive startup
     * always begins a fresh timestamped directory (docs/REPL_PLAN.md),
     * so this always durably records its first-ever effective model/cwd. */
    oi_cli_session_restore_init(&restore);
    status = oi_cli_session_restore_settings(
        context->store, context->state, context->location->metadata_path,
        context->location->id, /*is_new_session=*/1, /*explicit_model=*/NULL,
        context->pending_model->data, context->default_cwd,
        context->diagnostics, &restore);
    if (status == OI_OK) {
        status = oi_cli_string_set(context->pending_model, restore.model.data,
                                   restore.model.len);
    }
    if (status == OI_OK) {
        status = oi_cli_string_set(&context->persistence->model,
                                   restore.model.data, restore.model.len);
    }
    oi_cli_session_restore_free(&restore);
    if (status != OI_OK) {
        return status;
    }

    context->persistence->store = context->store;
    context->persistence->state = context->state;
    context->persistence->turn_id = context->state->next_turn_id;
    context->persistence->metadata_path = context->location->metadata_path;
    context->persistence->session_id = context->location->id;
    context->persistence->last_error = OI_OK;
    *out_arena = oi_session_arena(*context->session);
    return *out_arena == NULL ? OI_ERR_IO : OI_OK;
}

static const char *current_session_id(void *user_data) {
    oi_session *const *session = user_data;
    return session == NULL ? NULL : oi_session_id(*session);
}

static enum oi_cli_history_tool_outcome history_tool_outcome(
    enum oi_cli_conversation_tool_outcome outcome) {
    switch (outcome) {
    case OI_CLI_CONVERSATION_TOOL_COMPLETED:
        return OI_CLI_HISTORY_TOOL_COMPLETED;
    case OI_CLI_CONVERSATION_TOOL_OUTCOME_UNKNOWN:
        return OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN;
    case OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED:
        return OI_CLI_HISTORY_TOOL_NOT_EXECUTED;
    case OI_CLI_CONVERSATION_TOOL_NONE:
        return OI_CLI_HISTORY_TOOL_OUTCOME_NONE;
    }
    return OI_CLI_HISTORY_TOOL_OUTCOME_NONE;
}

static oi_status persist_conversation_event(
    const struct oi_cli_conversation_event *event, void *user_data) {
    struct persistence_context *context = user_data;
    struct oi_cli_history_record record;
    oi_cli_history_record_init(&record);
    oi_status st = OI_OK;
    int append = 0;

    if (event->type == OI_CLI_CONVERSATION_EVENT_MESSAGE) {
        const struct oi_cli_message *message = event->as.message.value;
        const char *model =
            message->role == OI_CLI_MESSAGE_ASSISTANT
                ? event->as.message.model
                : NULL;
        size_t model_len =
            message->role == OI_CLI_MESSAGE_ASSISTANT
                ? event->as.message.model_len
                : 0;
        st = oi_cli_history_record_set_message(
            &record, context->state->next_record_id, context->turn_id,
            message,
            event->as.message.is_repair ? OI_CLI_HISTORY_MESSAGE_REPAIR
                                        : OI_CLI_HISTORY_MESSAGE_NORMAL,
            model, model_len,
            history_tool_outcome(event->as.message.tool_outcome),
            event->as.message.raw_tool_output,
            event->as.message.raw_tool_output_len,
            event->as.message.has_raw_tool_output);
        append = st == OI_OK;
    } else if (event->type ==
               OI_CLI_CONVERSATION_EVENT_TOOL_STARTING) {
        st = oi_cli_history_record_set_tool_started(
            &record, context->state->next_record_id, context->turn_id,
            event->as.tool_starting.id->data,
            event->as.tool_starting.id->len);
        append = st == OI_OK;
    } else if (event->type ==
               OI_CLI_CONVERSATION_EVENT_PARTIAL_ASSISTANT) {
        st = oi_cli_history_record_set_partial_assistant(
            &record, context->state->next_record_id, context->turn_id,
            event->as.bytes.data, event->as.bytes.len,
            context->model.data, context->model.len);
        append = st == OI_OK;
    }
    if (append) {
        st = oi_cli_history_store_append(context->store, &record,
                                         context->state);
    }
    oi_cli_history_record_free(&record);
    if (st != OI_OK) {
        context->last_error = st;
    }
    if (st == OI_OK &&
        event->type == OI_CLI_CONVERSATION_EVENT_TURN_DONE) {
        context->turn_id = context->state->next_turn_id;
    }
    return st;
}

/* Durable persistence for a live /model or /cwd change -- cli_repl.c's
 * controller only requests the change; cli.c owns what persisting it
 * actually means (a durable history record plus a metadata.json
 * refresh). NULL `store` means no durable session exists yet (nothing
 * to persist to yet; the REPL's own live update already applied). */
static oi_status persist_model_setting(void *user_data, const char *name,
                                       size_t name_len) {
    struct persistence_context *persistence = user_data;

    if (persistence->store == NULL) {
        return OI_OK;
    }
    return oi_cli_session_apply_setting(
        persistence->store, persistence->state, persistence->metadata_path,
        persistence->session_id, OI_CLI_HISTORY_SESSION_SETTING_MODEL, name,
        name_len);
}

static oi_status persist_cwd_setting(void *user_data, const char *path,
                                     size_t path_len) {
    struct persistence_context *persistence = user_data;

    if (persistence->store == NULL) {
        return OI_OK;
    }
    return oi_cli_session_apply_setting(
        persistence->store, persistence->state, persistence->metadata_path,
        persistence->session_id, OI_CLI_HISTORY_SESSION_SETTING_CWD, path,
        path_len);
}

/*
 * Durable persistence for the one-slot queued-input mechanism. Unlike
 * persist_model_setting/persist_cwd_setting, these append a plain history
 * record directly (oi_cli_history_store_append), the same way
 * persist_conversation_event does -- there is no metadata.json refresh
 * involved. context->state->next_record_id/next_turn_id are exactly right
 * for a QUEUED_INPUT record (cli_history_replay.c requires its turn_id to
 * be the *next* turn, the one that hasn't started yet, not the currently
 * active one tracked by context->turn_id); a successful append then has
 * oi_cli_history_store_append itself replay context->state fresh, which is
 * what populates pending_input_turn_id below for the matching
 * QUEUE_RESOLVED record -- cli_repl.c never needs to track or pass a
 * turn_id itself, only the opaque record id persist_queued_input hands
 * back.
 */
static oi_status persist_queued_input(void *user_data, const char *content,
                                      size_t content_len,
                                      uint64_t *out_record_id) {
    struct persistence_context *persistence = user_data;
    struct oi_cli_history_record record;
    oi_status st;

    *out_record_id = 0;
    if (persistence->store == NULL) {
        return OI_OK;
    }
    oi_cli_history_record_init(&record);
    st = oi_cli_history_record_set_queued_input(
        &record, persistence->state->next_record_id,
        persistence->state->next_turn_id, content, content_len);
    if (st == OI_OK) {
        *out_record_id = persistence->state->next_record_id;
        st = oi_cli_history_store_append(persistence->store, &record,
                                         persistence->state);
    }
    oi_cli_history_record_free(&record);
    if (st != OI_OK) {
        persistence->last_error = st;
    }
    return st;
}

static oi_status persist_queue_resolved(void *user_data,
                                        uint64_t queued_record_id,
                                        int consumed) {
    struct persistence_context *persistence = user_data;
    struct oi_cli_history_record record;
    oi_status st;

    if (persistence->store == NULL) {
        return OI_OK;
    }
    oi_cli_history_record_init(&record);
    st = oi_cli_history_record_set_queue_resolved(
        &record, persistence->state->next_record_id,
        persistence->state->pending_input_turn_id, queued_record_id,
        consumed ? OI_CLI_HISTORY_QUEUE_CONSUMED
                : OI_CLI_HISTORY_QUEUE_DISCARDED);
    if (st == OI_OK) {
        st = oi_cli_history_store_append(persistence->store, &record,
                                         persistence->state);
    }
    oi_cli_history_record_free(&record);
    if (st != OI_OK) {
        persistence->last_error = st;
    }
    return st;
}

/* Wired unconditionally into oi_cli_repl_config, even for an ephemeral
 * session with no durable persistence at all: persist_conversation_event
 * (the only writer of persistence->last_error) is then never registered as
 * on_event, so last_error naturally, permanently stays OI_OK. */
static int persistence_has_failed(void *user_data) {
    const struct persistence_context *persistence = user_data;
    return persistence->last_error != OI_OK;
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *session_id = NULL;
    const char *session_dir = ".";
    int session_dir_set = 0;
    const char *prompt = NULL;
    int cli_use_tls = -1; /* -1 = not specified on the command line */
    int dry_run = 0;
    int max_turns = 8;
    oi_cli_tool_policy tool_policy = OI_CLI_TOOLS_ASK;

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
        if (strcmp(arg, "--allow-tools") == 0) {
            tool_policy = OI_CLI_TOOLS_ALLOW;
            continue;
        }
        if (strcmp(arg, "--deny-tools") == 0) {
            tool_policy = OI_CLI_TOOLS_DENY;
            continue;
        }
        if (strcmp(arg, "--max-turns") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "oi: --max-turns requires a value\n");
                return 1;
            }
            char *end = NULL;
            long value = strtol(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || value <= 0 ||
                value > 1000) {
                fprintf(stderr, "oi: invalid --max-turns value: %s\n",
                        argv[i]);
                return 1;
            }
            max_turns = (int)value;
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
            session_dir_set = 1;
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
                    config_path, oi_status_str(st));
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
                    overrides[i].key, overrides[i].value, oi_status_str(st));
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

    int interactive =
        prompt == NULL && !dry_run && isatty(STDIN_FILENO) &&
        isatty(STDOUT_FILENO);
    int automatic_session = interactive && session_id == NULL;
    int owns_prompt = 0;
    if (prompt == NULL && !interactive) {
        oi_status read_status;
        char *stdin_prompt = read_stdin_all(&read_status);
        if (stdin_prompt == NULL) {
            fprintf(stderr, "oi: failed to read stdin: %s\n",
                    oi_status_str(read_status));
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
    oi_status st = OI_OK;
    if (!interactive) {
        st = build_request_body(cfg.model, prompt, &w);
        if (st != OI_OK) {
            fprintf(stderr, "oi: failed to build request: %s\n",
                    oi_status_str(st));
            oi_config_free(&cfg);
            if (owns_prompt) {
                free((char *)prompt);
            }
            return 1;
        }
    }

    if (dry_run) {
        size_t body_len;
        const char *body = oi_json_writer_data(w, &body_len);
        (void)body_len;
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

    char log_path[4096] = {0};
    if (session_id != NULL) {
        int log_path_len = snprintf(log_path, sizeof log_path, "%s/%s.oilog",
                                    session_dir, session_id);
        if (log_path_len < 0 ||
            (size_t)log_path_len >= sizeof log_path) {
            fprintf(stderr, "oi: session log path is too long\n");
            oi_json_writer_destroy(w);
            oi_config_free(&cfg);
            if (owns_prompt) {
                free((char *)prompt);
            }
            return 1;
        }
    }

    /* All of oi_reactor_destroy/oi_session_registry_destroy/
     * oi_llm_client_destroy are documented NULL-safe, so a single
     * cascading cleanup at the end handles every early-failure point
     * below without each one needing its own partial-teardown code --
     * whatever wasn't reached yet is just NULL and skipped. */
    oi_reactor *reactor = NULL;
    oi_session_registry *sessions = NULL;
    oi_llm_client *client = NULL;
    oi_tool_registry *tools = NULL;
    oi_arena *ephemeral_arena = NULL;
    oi_arena *turn_arena = NULL;
    oi_session *session = NULL;
    struct oi_cli_history_store history_store;
    struct oi_cli_history_replay_state replay_state;
    struct oi_cli_message_list initial_context;
    struct persistence_context persistence = {0};
    struct oi_cli_session_location automatic_location;
    struct automatic_session_context automatic_context;
    oi_cli_session_location_init(&automatic_location);
    memset(&automatic_context, 0, sizeof automatic_context);
    oi_cli_history_store_init(&history_store);
    oi_cli_history_replay_state_init(&replay_state);
    oi_cli_message_list_init(&initial_context);
    struct oi_cli_loop_result loop_result = {0};
    int exit_code = 0;
    char *default_cwd = getcwd(NULL, 0);
    struct oi_cli_string pending_model = {0};
    char *explicit_metadata_path = NULL;
    int model_flag_explicit = 0;

    for (size_t i = 0; i < n_overrides; i++) {
        if (strcmp(overrides[i].key, "model") == 0) {
            model_flag_explicit = 1;
            break;
        }
    }
    if (default_cwd == NULL) {
        fprintf(stderr, "oi: failed to resolve current working directory\n");
        exit_code = 1;
        goto cleanup;
    }
    st = oi_cli_string_set(&pending_model, cfg.model, strlen(cfg.model));
    if (st != OI_OK) {
        fprintf(stderr, "oi: out of memory\n");
        exit_code = 1;
        goto cleanup;
    }

    reactor = oi_reactor_create();
    sessions = oi_session_registry_create();
    tools = oi_tool_registry_create();
    if (reactor == NULL || sessions == NULL || tools == NULL) {
        fprintf(stderr, "oi: out of memory\n");
        exit_code = 1;
        goto cleanup;
    }
    st = oi_cli_tools_register(tools);
    if (st != OI_OK) {
        fprintf(stderr, "oi: failed to register built-in tools: %s\n",
                oi_status_str(st));
        exit_code = 1;
        goto cleanup;
    }

    if (session_id != NULL) {
        st = oi_session_create(sessions, session_id, log_path, 0, &session);
        if (st != OI_OK) {
            fprintf(stderr, "oi: failed to open session '%s' at '%s': %s\n",
                    session_id, log_path, oi_status_str(st));
            exit_code = 1;
            goto cleanup;
        }
        st = oi_cli_history_store_load(oi_session_log(session),
                                       &history_store, &replay_state);
        if (st != OI_OK) {
            fprintf(stderr, "oi: failed to replay session: %s\n",
                    oi_status_str(st));
            exit_code = 1;
            goto cleanup;
        }
        /* Captured before this run appends anything (including its own
         * transition record below), so it reflects whether this ID had
         * any pre-existing records at all. */
        int is_new_history = history_store.typed_history.len == 0;
        if (replay_state.needs_transition) {
            struct oi_cli_history_record transition;
            oi_cli_history_record_init(&transition);
            st = oi_cli_history_record_set_transition(
                &transition, replay_state.next_record_id,
                history_store.legacy_messages.len);
            if (st == OI_OK) {
                st = oi_cli_history_store_append(
                    &history_store, &transition, &replay_state);
            }
            oi_cli_history_record_free(&transition);
        }
        if (st == OI_OK && replay_state.needs_repair) {
            struct oi_cli_history repairs;
            oi_cli_history_init(&repairs);
            st = oi_cli_history_build_repairs(&replay_state, &repairs);
            for (size_t i = 0; st == OI_OK && i < repairs.len; i++) {
                st = oi_cli_history_store_append(
                    &history_store, &repairs.records[i], &replay_state);
            }
            oi_cli_history_free(&repairs);
        }
        if (st != OI_OK) {
            fprintf(stderr, "oi: failed to prepare session history: %s\n",
                    oi_status_str(st));
            exit_code = 1;
            goto cleanup;
        }
        for (size_t i = 0; i < replay_state.context_len; i++) {
            st = oi_cli_message_list_append_clone(
                &initial_context, &replay_state.context[i].message);
            if (st != OI_OK) {
                fprintf(stderr, "oi: out of memory restoring context\n");
                exit_code = 1;
                goto cleanup;
            }
        }
        turn_arena = oi_session_arena(session);
        st = oi_cli_session_metadata_path_for_log(log_path, 0,
                                                  &explicit_metadata_path);
        if (st == OI_OK) {
            struct oi_cli_session_restore restore;

            oi_cli_session_restore_init(&restore);
            st = oi_cli_session_restore_settings(
                &history_store, &replay_state, explicit_metadata_path,
                session_id, is_new_history,
                model_flag_explicit ? cfg.model : NULL, cfg.model,
                default_cwd, stderr, &restore);
            if (st == OI_OK) {
                st = oi_config_set(&cfg, "model", restore.model.data);
            }
            if (st == OI_OK) {
                st = oi_cli_string_set(&persistence.model, restore.model.data,
                                       restore.model.len);
            }
            oi_cli_session_restore_free(&restore);
        }
        if (st != OI_OK) {
            fprintf(stderr, "oi: failed to restore session settings: %s\n",
                    oi_status_str(st));
            exit_code = 1;
            goto cleanup;
        }
        persistence.store = &history_store;
        persistence.state = &replay_state;
        persistence.turn_id = replay_state.next_turn_id;
        persistence.metadata_path = explicit_metadata_path;
        persistence.session_id = session_id;
        persistence.last_error = OI_OK;
    } else if (!automatic_session) {
        ephemeral_arena = oi_arena_create(0);
        if (ephemeral_arena == NULL) {
            fprintf(stderr, "oi: out of memory\n");
            exit_code = 1;
            goto cleanup;
        }
        turn_arena = ephemeral_arena;
    }
    if (automatic_session) {
        automatic_context.registry = sessions;
        automatic_context.session = &session;
        automatic_context.root_override =
            session_dir_set ? session_dir : NULL;
        automatic_context.location = &automatic_location;
        automatic_context.store = &history_store;
        automatic_context.state = &replay_state;
        automatic_context.persistence = &persistence;
        automatic_context.pending_model = &pending_model;
        automatic_context.default_cwd = default_cwd;
        automatic_context.diagnostics = stderr;
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

    struct oi_cli_permission permission = {tool_policy};
    struct oi_cli_loop_config loop_config = {
        .model = cfg.model,
        .max_turns = max_turns,
        .tool_timeout_ms = cfg.timeout_ms,
        .permission = &permission,
        .out = stdout,
        .err = stderr,
        .initial_context =
            session_id != NULL ? &initial_context : NULL,
        .on_event =
            session_id != NULL ? persist_conversation_event : NULL,
        .event_user_data =
            session_id != NULL ? &persistence : NULL,
    };
    if (interactive) {
        struct oi_cli_repl_config repl_config = {
            .model = cfg.model,
            .max_turns = max_turns,
            .tool_timeout_ms = cfg.timeout_ms,
            .permission = &permission,
            .input_fd = STDIN_FILENO,
            .output_fd = STDOUT_FILENO,
            .out = stdout,
            .err = stderr,
            .initial_context =
                session_id != NULL ? &initial_context : NULL,
            .on_event =
                session_id != NULL || automatic_session
                    ? persist_conversation_event
                    : NULL,
            .event_user_data =
                session_id != NULL || automatic_session ? &persistence
                                                        : NULL,
            .prepare =
                automatic_session ? prepare_automatic_session : NULL,
            .prepare_user_data =
                automatic_session ? &automatic_context : NULL,
            .session_id = current_session_id,
            .session_id_user_data = &session,
            .pending_model = automatic_session ? &pending_model : NULL,
            .persist_model = persist_model_setting,
            .persist_model_user_data = &persistence,
            .persist_cwd = persist_cwd_setting,
            .persist_cwd_user_data = &persistence,
            .is_durably_failed = persistence_has_failed,
            .is_durably_failed_user_data = &persistence,
            .persist_queued_input = persist_queued_input,
            .persist_queued_input_user_data = &persistence,
            .persist_queue_resolved = persist_queue_resolved,
            .persist_queue_resolved_user_data = &persistence,
        };
        st = oi_cli_repl_run(client, reactor, turn_arena, tools,
                             &repl_config);
    } else {
        st = oi_cli_loop_run(client, reactor, turn_arena, tools, &loop_config,
                             prompt, &loop_result);
    }
    if (st != OI_OK) {
        fprintf(stderr, "oi: agent loop failed: %s\n", oi_status_str(st));
        if (persistence.last_error != OI_OK && session != NULL) {
            oi_session_fail(session);
        }
        exit_code = 1;
    }

cleanup:
    free(loop_result.assistant_text);
    oi_cli_message_list_free(&initial_context);
    oi_cli_history_replay_state_free(&replay_state);
    oi_cli_history_store_free(&history_store);
    oi_cli_session_location_free(&automatic_location);
    oi_arena_destroy(ephemeral_arena);
    oi_tool_registry_destroy(tools);
    oi_llm_client_destroy(client);
    oi_session_registry_destroy(sessions);
    oi_reactor_destroy(reactor);
    oi_json_writer_destroy(w);
    oi_config_free(&cfg);
    oi_cli_string_free(&persistence.model);
    oi_cli_string_free(&pending_model);
    free(explicit_metadata_path);
    free(default_cwd);
    if (owns_prompt) {
        free((char *)prompt);
    }
    return exit_code;
}
