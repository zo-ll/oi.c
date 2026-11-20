#include "cli_status.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cli_bytebuf.h"
#include "cli_render_sanitize.h"
#include "cli_utf8_stream.h"

struct status_writer {
    struct oi_cli_bytebuf out;
    struct oi_cli_bytebuf fixed;
    struct oi_cli_bytebuf clean;
};

static void status_writer_init(struct status_writer *writer) {
    oi_cli_bytebuf_init(&writer->out);
    oi_cli_bytebuf_init(&writer->fixed);
    oi_cli_bytebuf_init(&writer->clean);
}

static void status_writer_free(struct status_writer *writer) {
    oi_cli_bytebuf_free(&writer->out);
    oi_cli_bytebuf_free(&writer->fixed);
    oi_cli_bytebuf_free(&writer->clean);
}

static oi_status append_text(struct status_writer *writer, const char *text) {
    return oi_cli_bytebuf_append(&writer->out, text, strlen(text));
}

static oi_status append_number(struct status_writer *writer,
                               unsigned long long value) {
    char digits[32];
    int written = snprintf(digits, sizeof digits, "%llu", value);

    if (written < 0 || (size_t)written >= sizeof digits) {
        return OI_ERR_INVAL;
    }
    return oi_cli_bytebuf_append(&writer->out, digits, (size_t)written);
}

static const char replacement_char[] = "\xEF\xBF\xBD";

static oi_status append_flattened(struct status_writer *writer,
                                  const char *data, size_t len) {
    size_t offset = 0;

    while (offset < len) {
        size_t start = offset;
        oi_status status;

        while (offset < len && (unsigned char)data[offset] >= 0x20U &&
               (unsigned char)data[offset] != 0x7fU) {
            offset++;
        }
        if (offset > start) {
            status = oi_cli_bytebuf_append(&writer->out, data + start,
                                           offset - start);
            if (status != OI_OK) {
                return status;
            }
        }
        if (offset < len) {
            status = oi_cli_bytebuf_append(&writer->out, replacement_char,
                                           sizeof replacement_char - 1);
            if (status != OI_OK) {
                return status;
            }
            offset++;
        }
    }
    return OI_OK;
}

static oi_status append_untrusted(struct status_writer *writer,
                                  const char *value) {
    struct oi_cli_utf8_stream utf8;
    struct oi_cli_sanitize_state sanitize;
    size_t len = strlen(value);
    int truncated = len > OI_CLI_STATUS_MAX_FIELD_BYTES;
    oi_status status;

    if (truncated) {
        len = OI_CLI_STATUS_MAX_FIELD_BYTES;
    }
    oi_cli_bytebuf_reset(&writer->fixed);
    oi_cli_bytebuf_reset(&writer->clean);
    oi_cli_utf8_stream_init(&utf8);
    oi_cli_sanitize_init(&sanitize);

    status = oi_cli_utf8_stream_feed(&utf8, (const unsigned char *)value, len,
                                     &writer->fixed);
    if (status == OI_OK) {
        status = oi_cli_utf8_stream_finish(&utf8, &writer->fixed);
    }
    if (status == OI_OK && writer->fixed.len != 0) {
        status = oi_cli_sanitize_feed(
            &sanitize, (const unsigned char *)writer->fixed.data,
            writer->fixed.len, &writer->clean);
    }
    if (status == OI_OK) {
        status = oi_cli_sanitize_finish(&sanitize);
    }
    if (status == OI_OK) {
        status = append_flattened(writer, writer->clean.data,
                                  writer->clean.len);
    }
    if (status == OI_OK && truncated) {
        status = append_text(writer, "...");
    }
    return status;
}

static const char *session_state_text(
    enum oi_cli_status_session_state state) {
    switch (state) {
    case OI_CLI_STATUS_SESSION_UNKNOWN:
        return "(unknown)";
    case OI_CLI_STATUS_SESSION_NOT_CREATED:
        return "(not created)";
    case OI_CLI_STATUS_SESSION_EPHEMERAL:
        return "(ephemeral, not persisted)";
    case OI_CLI_STATUS_SESSION_ACTIVE:
    case OI_CLI_STATUS_SESSION_FAILED:
        break;
    }
    return NULL;
}

static const char *model_origin_text(
    enum oi_cli_session_model_origin origin) {
    switch (origin) {
    case OI_CLI_SESSION_MODEL_UNKNOWN:
        return NULL;
    case OI_CLI_SESSION_MODEL_DEFAULT:
        return "startup default";
    case OI_CLI_SESSION_MODEL_EXPLICIT:
        return "command-line override";
    case OI_CLI_SESSION_MODEL_HISTORY:
        return "restored from session history";
    case OI_CLI_SESSION_MODEL_METADATA:
        return "restored from session metadata";
    case OI_CLI_SESSION_MODEL_COMMAND:
        return "changed with /model";
    }
    return NULL;
}

const char *oi_cli_status_permission_text(
    enum oi_cli_status_permission permission) {
    switch (permission) {
    case OI_CLI_STATUS_PERMISSION_UNKNOWN:
        return "(unknown)";
    case OI_CLI_STATUS_PERMISSION_ASK:
        return "ask";
    case OI_CLI_STATUS_PERMISSION_ALLOW:
        return "allow";
    case OI_CLI_STATUS_PERMISSION_DENY:
        return "deny";
    case OI_CLI_STATUS_PERMISSION_ASK_FILE_ACCESS_ALLOWED:
        return "ask (file access allowed)";
    case OI_CLI_STATUS_PERMISSION_ASK_COMMANDS_ALLOWED:
        return "ask (commands allowed)";
    case OI_CLI_STATUS_PERMISSION_ASK_BOTH_ALLOWED:
        return "ask (file access and commands allowed)";
    }
    return "(unknown)";
}

static const char *activity_text(enum oi_cli_conversation_activity activity) {
    switch (activity) {
    case OI_CLI_CONVERSATION_ACTIVITY_UNKNOWN:
        return "(unknown)";
    case OI_CLI_CONVERSATION_ACTIVITY_IDLE:
        return "idle";
    case OI_CLI_CONVERSATION_ACTIVITY_STREAMING:
        return "model streaming";
    case OI_CLI_CONVERSATION_ACTIVITY_AWAITING_PERMISSION:
        return "awaiting tool permission";
    case OI_CLI_CONVERSATION_ACTIVITY_TOOL_RUNNING:
        return "tool running";
    case OI_CLI_CONVERSATION_ACTIVITY_CANCELLING:
        return "cancelling";
    case OI_CLI_CONVERSATION_ACTIVITY_WORKING:
        return "working";
    case OI_CLI_CONVERSATION_ACTIVITY_FAILED:
        return "failed";
    }
    return "(unknown)";
}

enum oi_cli_status_permission oi_cli_status_permission_from(
    const struct oi_cli_permission *permission) {
    switch (permission->policy) {
    case OI_CLI_TOOLS_ASK:
        if (permission->file_access_allowed) {
            return permission->commands_allowed
                       ? OI_CLI_STATUS_PERMISSION_ASK_BOTH_ALLOWED
                       : OI_CLI_STATUS_PERMISSION_ASK_FILE_ACCESS_ALLOWED;
        }
        return permission->commands_allowed
                   ? OI_CLI_STATUS_PERMISSION_ASK_COMMANDS_ALLOWED
                   : OI_CLI_STATUS_PERMISSION_ASK;
    case OI_CLI_TOOLS_ALLOW:
        return OI_CLI_STATUS_PERMISSION_ALLOW;
    case OI_CLI_TOOLS_DENY:
        return OI_CLI_STATUS_PERMISSION_DENY;
    }
    return OI_CLI_STATUS_PERMISSION_UNKNOWN;
}

void oi_cli_status_snapshot_init(struct oi_cli_status_snapshot *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->request_timeout_ms = -1;
    snapshot->tool_timeout_ms = -1;
}

static oi_status begin_line(struct status_writer *writer, const char *key) {
    oi_status status = append_text(writer, key);

    return status == OI_OK ? append_text(writer, ": ") : status;
}

static oi_status end_line(struct status_writer *writer) {
    return append_text(writer, "\n");
}

static oi_status append_line(struct status_writer *writer, const char *key,
                             const char *value) {
    oi_status status = begin_line(writer, key);

    if (status == OI_OK) {
        status = append_text(writer, value);
    }
    return status == OI_OK ? end_line(writer) : status;
}

static oi_status append_session(struct status_writer *writer,
                                const struct oi_cli_status_snapshot *snapshot) {
    const char *fixed = session_state_text(snapshot->session_state);
    oi_status status;

    if (fixed != NULL) {
        return append_line(writer, "Session", fixed);
    }
    assert(snapshot->session_id != NULL);
    status = begin_line(writer, "Session");
    if (status == OI_OK) {
        status = append_untrusted(writer, snapshot->session_id);
    }
    if (status == OI_OK &&
        snapshot->session_state == OI_CLI_STATUS_SESSION_FAILED) {
        status = append_text(writer, " (durable storage failed)");
    }
    return status == OI_OK ? end_line(writer) : status;
}

static oi_status append_model(struct status_writer *writer,
                              const struct oi_cli_status_snapshot *snapshot) {
    const char *origin = model_origin_text(snapshot->model_origin);
    oi_status status;

    if (snapshot->model == NULL) {
        return append_line(writer, "Model", "(unknown)");
    }
    status = begin_line(writer, "Model");
    if (status == OI_OK) {
        status = append_untrusted(writer, snapshot->model);
    }
    if (status == OI_OK && origin != NULL) {
        status = append_text(writer, " (");
        if (status == OI_OK) {
            status = append_text(writer, origin);
        }
        if (status == OI_OK) {
            status = append_text(writer, ")");
        }
    }
    return status == OI_OK ? end_line(writer) : status;
}

static oi_status append_endpoint(
    struct status_writer *writer,
    const struct oi_cli_status_snapshot *snapshot) {
    const struct oi_cli_status_endpoint *endpoint = &snapshot->endpoint;
    oi_status status;

    if (endpoint->host == NULL) {
        return append_line(writer, "Endpoint", "(unknown)");
    }
    status = begin_line(writer, "Endpoint");
    if (status == OI_OK) {
        status = append_untrusted(writer, endpoint->host);
    }
    if (status == OI_OK) {
        status = append_text(writer, ":");
    }
    if (status == OI_OK) {
        status = append_number(writer, endpoint->port);
    }
    if (status == OI_OK && endpoint->path != NULL) {
        status = append_untrusted(writer, endpoint->path);
    }
    if (status == OI_OK) {
        status = append_text(writer, endpoint->use_tls ? " (TLS on)"
                                                       : " (TLS off)");
    }
    return status == OI_OK ? end_line(writer) : status;
}

static oi_status append_timeout(struct status_writer *writer, const char *key,
                                int timeout_ms) {
    oi_status status;

    if (timeout_ms < 0) {
        return append_line(writer, key, "(unknown)");
    }
    if (timeout_ms == 0) {
        return append_line(writer, key, "disabled");
    }
    status = begin_line(writer, key);
    if (status == OI_OK) {
        status = append_number(writer, (unsigned long long)timeout_ms);
    }
    if (status == OI_OK) {
        status = append_text(writer, " ms");
    }
    return status == OI_OK ? end_line(writer) : status;
}

static oi_status append_conversation(
    struct status_writer *writer,
    const struct oi_cli_status_snapshot *snapshot) {
    const char *text = activity_text(snapshot->conversation);
    oi_status status;

    if (snapshot->conversation != OI_CLI_CONVERSATION_ACTIVITY_FAILED ||
        snapshot->conversation_status == OI_OK) {
        return append_line(writer, "Conversation", text);
    }
    status = begin_line(writer, "Conversation");
    if (status == OI_OK) {
        status = append_text(writer, text);
    }
    if (status == OI_OK) {
        status = append_text(writer, " (");
    }
    if (status == OI_OK) {
        status = append_text(writer,
                             oi_status_str(snapshot->conversation_status));
    }
    if (status == OI_OK) {
        status = append_text(writer, ")");
    }
    return status == OI_OK ? end_line(writer) : status;
}

static const char *queue_kind_text(enum oi_cli_status_queue_state queue) {
    switch (queue) {
    case OI_CLI_STATUS_QUEUE_UNKNOWN:
    case OI_CLI_STATUS_QUEUE_EMPTY:
        return NULL;
    case OI_CLI_STATUS_QUEUE_MESSAGE:
        return "message";
    case OI_CLI_STATUS_QUEUE_COMMAND:
        return "command";
    }
    return "item";
}

static oi_status append_queue(struct status_writer *writer,
                              const struct oi_cli_status_snapshot *snapshot) {
    const char *kind = queue_kind_text(snapshot->queue);
    oi_status status = begin_line(writer, "Queue");

    if (status != OI_OK) {
        return status;
    }
    if (kind == NULL) {
        status = append_text(writer,
                             snapshot->queue == OI_CLI_STATUS_QUEUE_UNKNOWN
                                 ? "(unknown)"
                                 : "empty");
    } else {
        status = append_text(writer, "1 ");
        if (status == OI_OK) {
            status = append_text(writer, kind);
        }
        if (status == OI_OK) {
            status = append_text(writer, " queued (");
        }
        if (status == OI_OK) {
            status = append_number(writer,
                                   (unsigned long long)snapshot->queue_bytes);
        }
        if (status == OI_OK) {
            status = append_text(writer, " bytes)");
        }
    }
    if (status == OI_OK && snapshot->steering) {
        status = append_text(writer, " (steering to a safe boundary)");
    }
    return status == OI_OK ? end_line(writer) : status;
}

static oi_status append_cwd(struct status_writer *writer,
                            const struct oi_cli_status_snapshot *snapshot) {
    oi_status status;

    if (snapshot->cwd == NULL) {
        return append_line(writer, "CWD", "(unknown)");
    }
    status = begin_line(writer, "CWD");
    if (status == OI_OK) {
        status = append_untrusted(writer, snapshot->cwd);
    }
    return status == OI_OK ? end_line(writer) : status;
}

static oi_status append_checkpoint(
    struct status_writer *writer,
    const struct oi_cli_status_snapshot *snapshot) {
    const struct oi_cli_status_checkpoint *checkpoint = &snapshot->checkpoint;
    oi_status status;

    if (!checkpoint->known) {
        status = append_line(writer, "Checkpoint", "(unknown)");
        return status == OI_OK ? append_line(writer, "Context", "(unknown)")
                               : status;
    }
    if (checkpoint->has_durable_checkpoint) {
        status = begin_line(writer, "Checkpoint");
        if (status == OI_OK) {
            status = append_text(writer, "records ");
        }
        if (status == OI_OK) {
            status = append_number(writer, checkpoint->source_first_record_id);
        }
        if (status == OI_OK) {
            status = append_text(writer, "-");
        }
        if (status == OI_OK) {
            status = append_number(writer, checkpoint->source_last_record_id);
        }
        if (status == OI_OK && checkpoint->applied_this_run) {
            status = append_text(writer, " (applied this run)");
        }
        if (status == OI_OK) {
            status = end_line(writer);
        }
    } else if (checkpoint->applied_this_run) {
        status = append_line(writer, "Checkpoint",
                             "applied this run (not persisted)");
    } else {
        status = append_line(writer, "Checkpoint", "none");
    }
    if (status != OI_OK) {
        return status;
    }
    return append_line(writer, "Context", checkpoint->context_compacted
                                              ? "compacted"
                                              : "not compacted");
}

int oi_cli_usage_format(const struct oi_cli_usage *usage, char *out,
                        size_t size) {
    const char *requests = usage->requests == 1 ? "request" : "requests";
    char cached[64] = "unknown";
    int len;

    if (usage->requests == 0) {
        return snprintf(out, size, "no requests yet");
    }
    if (usage->reported_requests == 0) {
        return snprintf(out, size, "unavailable (%zu %s, none reported)",
                        usage->requests, requests);
    }
    if (usage->cached_reported_requests == usage->reported_requests) {
        snprintf(cached, sizeof cached, "%zu", usage->cached_input_tokens);
    } else if (usage->cached_reported_requests > 0) {
        snprintf(cached, sizeof cached, "%zu, unknown for %zu",
                 usage->cached_input_tokens,
                 usage->reported_requests - usage->cached_reported_requests);
    }
    len = snprintf(out, size, "input %zu (cached %s), output %zu, %s %zu",
                   usage->input_tokens, cached, usage->output_tokens,
                   requests, usage->requests);
    if (len >= 0 && (size_t)len < size &&
        usage->reported_requests < usage->requests) {
        len += snprintf(out + len, size - (size_t)len,
                        " (usage unavailable for %zu)",
                        usage->requests - usage->reported_requests);
    }
    return len;
}

static oi_status append_usage(struct status_writer *writer,
                              const struct oi_cli_status_snapshot *snapshot) {
    char text[192];
    int len;

    if (!snapshot->usage_known) {
        return append_line(writer, "Tokens", "(unknown)");
    }
    len = oi_cli_usage_format(&snapshot->usage, text, sizeof text);
    if (len < 0 || (size_t)len >= sizeof text) {
        return OI_ERR_NOMEM;
    }
    return append_line(writer, "Tokens", text);
}

static oi_status assemble(struct status_writer *writer,
                          const struct oi_cli_status_snapshot *snapshot) {
    oi_status status = append_session(writer, snapshot);

    if (status == OI_OK) {
        status = append_model(writer, snapshot);
    }
    if (status == OI_OK) {
        status = append_endpoint(writer, snapshot);
    }
    if (status == OI_OK) {
        status = append_line(writer, "Permissions",
                             oi_cli_status_permission_text(
                                 snapshot->permission));
    }
    if (status == OI_OK) {
        status = append_timeout(writer, "Request timeout",
                                snapshot->request_timeout_ms);
    }
    if (status == OI_OK) {
        status = append_timeout(writer, "Tool timeout",
                                snapshot->tool_timeout_ms);
    }
    if (status == OI_OK) {
        status = append_cwd(writer, snapshot);
    }
    if (status == OI_OK) {
        status = append_conversation(writer, snapshot);
    }
    if (status == OI_OK) {
        status = append_queue(writer, snapshot);
    }
    if (status == OI_OK) {
        status = append_checkpoint(writer, snapshot);
    }
    if (status == OI_OK) {
        status = append_usage(writer, snapshot);
    }
    return status;
}

oi_status oi_cli_status_write(const struct oi_cli_status_snapshot *snapshot,
                              FILE *out) {
    struct status_writer writer;
    oi_status status;

    if (snapshot == NULL || out == NULL) {
        return OI_ERR_INVAL;
    }
    status_writer_init(&writer);
    status = assemble(&writer, snapshot);
    if (status == OI_OK && writer.out.len != 0 &&
        fwrite(writer.out.data, 1, writer.out.len, out) != writer.out.len) {
        status = OI_ERR_IO;
    }
    if (status == OI_OK && fflush(out) != 0) {
        status = OI_ERR_IO;
    }
    status_writer_free(&writer);
    return status;
}
