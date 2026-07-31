#include "cli_status.h"

#include <stdio.h>
#include <string.h>

#include "cli_bytebuf.h"
#include "cli_render_sanitize.h"
#include "cli_utf8_stream.h"

/*
 * The report under assembly, plus the scratch every sanitized field reuses.
 * One buffer pair for the whole report rather than one per field: fewer
 * allocations, and exactly one place that frees them.
 */
struct status_writer {
    struct oi_cli_bytebuf out;   /* the report being assembled */
    struct oi_cli_bytebuf fixed; /* UTF-8-repaired bytes of one field */
    struct oi_cli_bytebuf clean; /* the same field, escape-stripped */
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
    /* Widest unsigned long long is 20 digits; 32 leaves room to spare. */
    char digits[32];
    int written = snprintf(digits, sizeof digits, "%llu", value);

    if (written < 0 || (size_t)written >= sizeof digits) {
        return OI_ERR_INVAL;
    }
    return oi_cli_bytebuf_append(&writer->out, digits, (size_t)written);
}

/* U+FFFD, the same replacement the UTF-8 repair pass emits for a byte it
 * cannot show -- reused here rather than inventing a second marker. */
static const char replacement_char[] = "\xEF\xBF\xBD";

/*
 * Appends already-sanitized bytes, replacing the only control bytes the
 * sanitize pass deliberately lets through. It preserves '\n' and '\t'
 * because its usual callers render multi-line streamed output; a `Key:
 * value` line must not inherit that, or an attacker-supplied value could
 * forge a whole extra status line (or shift one into another column).
 *
 * Written against the full C0 range plus DEL rather than just those two, so
 * it stays correct if the sanitize pass' own passthrough set ever widens.
 */
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

/*
 * Appends one untrusted borrowed string: UTF-8 repair, then the shared
 * escape/control stripper, then line flattening. Bounded first, so a
 * pathological value (a forged model name in a tampered log) cannot produce
 * an unbounded line; the bound is on input bytes and the marker is added
 * only when something was actually dropped.
 *
 * Truncation cuts raw bytes, which can split a multi-byte sequence -- the
 * UTF-8 repair pass then turns that tail into a single U+FFFD, which is
 * exactly what it is for. There is deliberately no attempt to find a
 * character boundary here.
 */
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
    return NULL; /* the id itself is printed instead */
}

/* NULL means "say nothing about provenance", which is the only honest
 * rendering of an origin nobody tracked. */
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

static const char *permission_text(enum oi_cli_status_permission permission) {
    switch (permission) {
    case OI_CLI_STATUS_PERMISSION_UNKNOWN:
        return "(unknown)";
    case OI_CLI_STATUS_PERMISSION_ASK:
        return "ask";
    case OI_CLI_STATUS_PERMISSION_ALLOW:
        return "allow";
    case OI_CLI_STATUS_PERMISSION_DENY:
        return "deny";
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

enum oi_cli_status_permission oi_cli_status_permission_from_policy(
    oi_cli_tool_policy policy) {
    switch (policy) {
    case OI_CLI_TOOLS_ASK:
        return OI_CLI_STATUS_PERMISSION_ASK;
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
    /* The zero value is already "unknown" for every enum and every borrowed
     * pointer here; only the deadlines need saying, since 0 is a real state
     * for them (disabled). Still routed through this function rather than
     * left to the caller's memset, so there is one place to update when a
     * field with a meaningful zero is added. */
    snapshot->request_timeout_ms = -1;
    snapshot->tool_timeout_ms = -1;
}

/* Every line goes through this pair, so no field can silently skip its own
 * failure check and every key is spelled once. */
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

    /* An ACTIVE/FAILED state without an id would be a bug in the assembler,
     * but reporting it plainly beats printing an invalid pointer. */
    if (fixed == NULL && snapshot->session_id == NULL) {
        fixed = "(unknown)";
    }
    if (fixed != NULL) {
        return append_line(writer, "Session", fixed);
    }
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

/*
 * host/port/path plus TLS on/off, assembled here rather than as a URL: a
 * URL invites a userinfo component, and this line must never be able to
 * carry a credential.
 */
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
        /* A fixed vocabulary from oi/status.h, not borrowed text. */
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

/*
 * Reports the kind and size of what is queued, never its text. A message
 * queued mid-turn is ordinary user input, and /status is exactly the sort of
 * thing someone runs with a shoulder-surfer or a shared screen recording
 * present.
 */
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
        /* Live-only: a session with no durable storage still has its active
         * context spliced by /compact, and saying "none" would be wrong. */
        status = append_line(writer, "Checkpoint",
                             "applied this run (not persisted)");
    } else {
        status = append_line(writer, "Checkpoint", "none");
    }
    if (status != OI_OK) {
        return status;
    }
    /* Its own line rather than a qualifier on the one above: "is my context
     * compacted" is the question a user actually has, and it must be
     * answerable without knowing whether a checkpoint got persisted. */
    return append_line(writer, "Context", checkpoint->context_compacted
                                              ? "compacted"
                                              : "not compacted");
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
                             permission_text(snapshot->permission));
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
