#include "cli_status.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static char *read_stream(FILE *stream) {
    long len;
    char *text;

    CHECK_EQ(fflush(stream), 0);
    CHECK_EQ(fseek(stream, 0, SEEK_END), 0);
    len = ftell(stream);
    CHECK(len >= 0);
    CHECK_EQ(fseek(stream, 0, SEEK_SET), 0);
    text = malloc((size_t)len + 1);
    CHECK(text != NULL);
    if (text == NULL) {
        return NULL;
    }
    CHECK_EQ(fread(text, 1, (size_t)len, stream), (size_t)len);
    text[len] = '\0';
    return text;
}

/*
 * Renders `snapshot` into a fresh temporary stream and returns the text. The
 * report never contains a NUL byte, so reading it back as a C string loses
 * nothing -- the sanitize pass strips NUL along with every other C0 control.
 */
static char *render(const struct oi_cli_status_snapshot *snapshot) {
    FILE *out = tmpfile();
    char *text;

    CHECK(out != NULL);
    if (out == NULL) {
        return NULL;
    }
    CHECK_EQ(oi_cli_status_write(snapshot, out), OI_OK);
    text = read_stream(out);
    fclose(out);
    return text;
}

static size_t count_lines(const char *text) {
    size_t lines = 0;
    const char *cursor = text;

    while ((cursor = strchr(cursor, '\n')) != NULL) {
        lines++;
        cursor++;
    }
    return lines;
}

/* The report's fixed line count: one per contracted field, with Checkpoint
 * and Context counted separately. */
#define STATUS_REPORT_LINES 11u

/* A fully-populated snapshot, so each test can vary one field and leave the
 * rest realistic rather than unknown. Built on the real initializer, which is
 * what every assembler must do. */
static void init_active(struct oi_cli_status_snapshot *snapshot) {
    oi_cli_status_snapshot_init(snapshot);
    snapshot->session_state = OI_CLI_STATUS_SESSION_ACTIVE;
    snapshot->session_id = "20260731-100000";
    snapshot->model = "test-model";
    snapshot->model_origin = OI_CLI_SESSION_MODEL_HISTORY;
    snapshot->endpoint.host = "api.example.invalid";
    snapshot->endpoint.path = "/v1/chat/completions";
    snapshot->endpoint.port = 443;
    snapshot->endpoint.use_tls = 1;
    snapshot->permission = OI_CLI_STATUS_PERMISSION_ASK;
    snapshot->request_timeout_ms = 30000;
    snapshot->tool_timeout_ms = 5000;
    snapshot->cwd = "/tmp/work";
    snapshot->conversation = OI_CLI_CONVERSATION_ACTIVITY_IDLE;
    snapshot->queue = OI_CLI_STATUS_QUEUE_EMPTY;
    snapshot->checkpoint.known = 1;
}

TEST(rejects_missing_arguments) {
    struct oi_cli_status_snapshot snapshot;
    FILE *out = tmpfile();

    init_active(&snapshot);
    CHECK(out != NULL);
    CHECK_EQ(oi_cli_status_write(NULL, out), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_status_write(&snapshot, NULL), OI_ERR_INVAL);
    /* NULL-safe, so a caller cannot be punished for initializing early. */
    oi_cli_status_snapshot_init(NULL);
    fclose(out);
}

TEST(reports_every_required_field) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    text = render(&snapshot);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK(strstr(text, "Session: 20260731-100000\n") != NULL);
    CHECK(strstr(text, "Model: test-model (restored from session history)\n") !=
          NULL);
    CHECK(strstr(text,
                 "Endpoint: api.example.invalid:443/v1/chat/completions "
                 "(TLS on)\n") != NULL);
    CHECK(strstr(text, "Permissions: ask\n") != NULL);
    CHECK(strstr(text, "Request timeout: 30000 ms\n") != NULL);
    CHECK(strstr(text, "Tool timeout: 5000 ms\n") != NULL);
    CHECK(strstr(text, "CWD: /tmp/work\n") != NULL);
    CHECK(strstr(text, "Conversation: idle\n") != NULL);
    CHECK(strstr(text, "Queue: empty\n") != NULL);
    CHECK(strstr(text, "Checkpoint: none\n") != NULL);
    CHECK(strstr(text, "Context: not compacted\n") != NULL);
    free(text);
}

/*
 * The order is part of the contract: a redirected /status is something
 * scripts and tests read, and reordering the report would break them
 * silently. Asserted as one exact string rather than field by field.
 */
TEST(output_is_deterministic_and_ordered) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    text = render(&snapshot);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK_STREQ(text,
                "Session: 20260731-100000\n"
                "Model: test-model (restored from session history)\n"
                "Endpoint: api.example.invalid:443/v1/chat/completions "
                "(TLS on)\n"
                "Permissions: ask\n"
                "Request timeout: 30000 ms\n"
                "Tool timeout: 5000 ms\n"
                "CWD: /tmp/work\n"
                "Conversation: idle\n"
                "Queue: empty\n"
                "Checkpoint: none\n"
                "Context: not compacted\n");
    free(text);
}

/*
 * What dispatch hands a callback that fills nothing in. Every field must read
 * as explicitly unknown -- not as a guessed default, and not as a
 * dereferenced NULL. This is the whole reason the initializer exists rather
 * than a memset: a zeroed struct would claim no session, ask, disabled
 * deadlines, idle, and an empty queue, none of which anyone asserted.
 */
TEST(initialized_snapshot_reports_everything_as_unknown) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    oi_cli_status_snapshot_init(&snapshot);
    text = render(&snapshot);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK_STREQ(text,
                "Session: (unknown)\n"
                "Model: (unknown)\n"
                "Endpoint: (unknown)\n"
                "Permissions: (unknown)\n"
                "Request timeout: (unknown)\n"
                "Tool timeout: (unknown)\n"
                "CWD: (unknown)\n"
                "Conversation: (unknown)\n"
                "Queue: (unknown)\n"
                "Checkpoint: (unknown)\n"
                "Context: (unknown)\n");
    free(text);
}

/*
 * The states a zeroed snapshot used to claim are all real, observable states
 * an assembler does report -- so the initializer must not have made them
 * unreachable. This is the other half of the test above.
 */
TEST(really_observed_zero_like_states_still_report_themselves) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    snapshot.session_state = OI_CLI_STATUS_SESSION_NOT_CREATED;
    snapshot.permission = OI_CLI_STATUS_PERMISSION_ASK;
    snapshot.request_timeout_ms = 0;
    snapshot.tool_timeout_ms = 0;
    snapshot.conversation = OI_CLI_CONVERSATION_ACTIVITY_IDLE;
    snapshot.queue = OI_CLI_STATUS_QUEUE_EMPTY;
    text = render(&snapshot);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK(strstr(text, "Session: (not created)\n") != NULL);
    CHECK(strstr(text, "Permissions: ask\n") != NULL);
    CHECK(strstr(text, "Request timeout: disabled\n") != NULL);
    CHECK(strstr(text, "Tool timeout: disabled\n") != NULL);
    CHECK(strstr(text, "Conversation: idle\n") != NULL);
    CHECK(strstr(text, "Queue: empty\n") != NULL);
    CHECK(strstr(text, "Checkpoint: none\n") != NULL);
    CHECK(strstr(text, "Context: not compacted\n") != NULL);
    free(text);
}

TEST(missing_optional_strings_are_named) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    snapshot.model = NULL;
    snapshot.cwd = NULL;
    snapshot.endpoint.host = NULL;
    snapshot.request_timeout_ms = -1;
    snapshot.tool_timeout_ms = -1;
    text = render(&snapshot);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK(strstr(text, "Model: (unknown)\n") != NULL);
    CHECK(strstr(text, "Endpoint: (unknown)\n") != NULL);
    CHECK(strstr(text, "Request timeout: (unknown)\n") != NULL);
    CHECK(strstr(text, "Tool timeout: (unknown)\n") != NULL);
    CHECK(strstr(text, "CWD: (unknown)\n") != NULL);
    free(text);
}

/* An endpoint with no path is still a usable report, and must not print a
 * stray "(null)" from a NULL passed to printf. */
TEST(endpoint_without_a_path_still_renders) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    snapshot.endpoint.path = NULL;
    snapshot.endpoint.use_tls = 0;
    snapshot.endpoint.port = 8080;
    text = render(&snapshot);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK(strstr(text, "Endpoint: api.example.invalid:8080 (TLS off)\n") !=
          NULL);
    CHECK(strstr(text, "null") == NULL);
    free(text);
}

TEST(every_session_state_is_reported) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    snapshot.session_state = OI_CLI_STATUS_SESSION_UNKNOWN;
    text = render(&snapshot);
    CHECK(text != NULL && strstr(text, "Session: (unknown)\n") != NULL);
    /* The id is deliberately not printed for a state that has no session,
     * even when the assembler left one behind. */
    CHECK(text != NULL && strstr(text, "20260731-100000") == NULL);
    free(text);

    snapshot.session_state = OI_CLI_STATUS_SESSION_NOT_CREATED;
    text = render(&snapshot);
    CHECK(text != NULL && strstr(text, "Session: (not created)\n") != NULL);
    CHECK(text != NULL && strstr(text, "20260731-100000") == NULL);
    free(text);

    snapshot.session_state = OI_CLI_STATUS_SESSION_EPHEMERAL;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Session: (ephemeral, not persisted)\n") != NULL);
    free(text);

    snapshot.session_state = OI_CLI_STATUS_SESSION_ACTIVE;
    text = render(&snapshot);
    CHECK(text != NULL && strstr(text, "Session: 20260731-100000\n") != NULL);
    free(text);

    snapshot.session_state = OI_CLI_STATUS_SESSION_FAILED;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text,
                 "Session: 20260731-100000 (durable storage failed)\n") !=
              NULL);
    free(text);

    /* An assembler bug -- an active state with no id -- must degrade to a
     * named unknown rather than to an invalid pointer. */
    snapshot.session_state = OI_CLI_STATUS_SESSION_ACTIVE;
    snapshot.session_id = NULL;
    text = render(&snapshot);
    CHECK(text != NULL && strstr(text, "Session: (unknown)\n") != NULL);
    free(text);
}

TEST(every_model_origin_is_reported) {
    static const struct {
        enum oi_cli_session_model_origin origin;
        const char *expected;
    } cases[] = {
        {OI_CLI_SESSION_MODEL_UNKNOWN, "Model: test-model\n"},
        {OI_CLI_SESSION_MODEL_DEFAULT, "Model: test-model (startup default)\n"},
        {OI_CLI_SESSION_MODEL_EXPLICIT,
         "Model: test-model (command-line override)\n"},
        {OI_CLI_SESSION_MODEL_HISTORY,
         "Model: test-model (restored from session history)\n"},
        {OI_CLI_SESSION_MODEL_METADATA,
         "Model: test-model (restored from session metadata)\n"},
        {OI_CLI_SESSION_MODEL_COMMAND,
         "Model: test-model (changed with /model)\n"},
    };
    size_t index;

    for (index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        struct oi_cli_status_snapshot snapshot;
        char *text;

        init_active(&snapshot);
        snapshot.model_origin = cases[index].origin;
        text = render(&snapshot);
        CHECK(text != NULL && strstr(text, cases[index].expected) != NULL);
        free(text);
    }
}

TEST(every_permission_state_is_reported) {
    static const struct {
        enum oi_cli_status_permission permission;
        const char *expected;
    } cases[] = {
        {OI_CLI_STATUS_PERMISSION_UNKNOWN, "Permissions: (unknown)\n"},
        {OI_CLI_STATUS_PERMISSION_ASK, "Permissions: ask\n"},
        {OI_CLI_STATUS_PERMISSION_ALLOW, "Permissions: allow\n"},
        {OI_CLI_STATUS_PERMISSION_DENY, "Permissions: deny\n"},
    };
    size_t index;

    for (index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        struct oi_cli_status_snapshot snapshot;
        char *text;

        init_active(&snapshot);
        snapshot.permission = cases[index].permission;
        text = render(&snapshot);
        CHECK(text != NULL && strstr(text, cases[index].expected) != NULL);
        free(text);
    }
}

/* Every real policy maps to a reportable value, and none of them maps to
 * unknown -- a live policy is always one of the three. */
TEST(policy_conversion_never_yields_unknown) {
    CHECK_EQ(oi_cli_status_permission_from_policy(OI_CLI_TOOLS_ASK),
             OI_CLI_STATUS_PERMISSION_ASK);
    CHECK_EQ(oi_cli_status_permission_from_policy(OI_CLI_TOOLS_ALLOW),
             OI_CLI_STATUS_PERMISSION_ALLOW);
    CHECK_EQ(oi_cli_status_permission_from_policy(OI_CLI_TOOLS_DENY),
             OI_CLI_STATUS_PERMISSION_DENY);
}

TEST(every_conversation_state_is_reported) {
    static const struct {
        enum oi_cli_conversation_activity activity;
        const char *expected;
    } cases[] = {
        {OI_CLI_CONVERSATION_ACTIVITY_UNKNOWN, "Conversation: (unknown)\n"},
        {OI_CLI_CONVERSATION_ACTIVITY_IDLE, "Conversation: idle\n"},
        {OI_CLI_CONVERSATION_ACTIVITY_STREAMING,
         "Conversation: model streaming\n"},
        {OI_CLI_CONVERSATION_ACTIVITY_AWAITING_PERMISSION,
         "Conversation: awaiting tool permission\n"},
        {OI_CLI_CONVERSATION_ACTIVITY_TOOL_RUNNING,
         "Conversation: tool running\n"},
        {OI_CLI_CONVERSATION_ACTIVITY_CANCELLING, "Conversation: cancelling\n"},
        {OI_CLI_CONVERSATION_ACTIVITY_WORKING, "Conversation: working\n"},
    };
    size_t index;

    for (index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        struct oi_cli_status_snapshot snapshot;
        char *text;

        init_active(&snapshot);
        snapshot.conversation = cases[index].activity;
        text = render(&snapshot);
        CHECK(text != NULL && strstr(text, cases[index].expected) != NULL);
        free(text);
    }
}

/* FAILED names the cause, since "failed" alone tells the user nothing about
 * whether to retry. A failure with no status still renders. */
TEST(failed_conversation_names_its_cause) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    snapshot.conversation = OI_CLI_CONVERSATION_ACTIVITY_FAILED;
    snapshot.conversation_status = OI_ERR_TIMEOUT;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Conversation: failed (timeout)\n") != NULL);
    free(text);

    snapshot.conversation_status = OI_OK;
    text = render(&snapshot);
    CHECK(text != NULL && strstr(text, "Conversation: failed\n") != NULL);
    free(text);
}

/*
 * The queue reports a kind and a size. Content is structurally absent from
 * the snapshot, so this asserts what a caller can learn -- not that one
 * particular string was withheld.
 */
TEST(queue_states_report_size_without_content) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    snapshot.queue = OI_CLI_STATUS_QUEUE_MESSAGE;
    snapshot.queue_bytes = 42;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Queue: 1 message queued (42 bytes)\n") != NULL);
    free(text);

    snapshot.queue = OI_CLI_STATUS_QUEUE_COMMAND;
    snapshot.queue_bytes = 7;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Queue: 1 command queued (7 bytes)\n") != NULL);
    free(text);

    snapshot.steering = 1;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Queue: 1 command queued (7 bytes) (steering to a "
                       "safe boundary)\n") != NULL);
    free(text);

    /* Steering with an empty slot is reachable: a queued item can be
     * discarded by a cancel while the turn is still unwinding. */
    snapshot.queue = OI_CLI_STATUS_QUEUE_EMPTY;
    snapshot.queue_bytes = 0;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Queue: empty (steering to a safe boundary)\n") != NULL);
    free(text);

    /* An unknown queue still reports steering, which is independently
     * observable. */
    snapshot.queue = OI_CLI_STATUS_QUEUE_UNKNOWN;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Queue: (unknown) (steering to a safe boundary)\n") !=
              NULL);
    free(text);
}

TEST(checkpoint_states_are_reported) {
    struct oi_cli_status_snapshot snapshot;
    char *text;

    init_active(&snapshot);
    snapshot.checkpoint.has_durable_checkpoint = 1;
    snapshot.checkpoint.source_first_record_id = 3;
    snapshot.checkpoint.source_last_record_id = 18;
    snapshot.checkpoint.context_compacted = 1;
    text = render(&snapshot);
    CHECK(text != NULL && strstr(text, "Checkpoint: records 3-18\n") != NULL);
    CHECK(text != NULL && strstr(text, "Context: compacted\n") != NULL);
    free(text);

    snapshot.checkpoint.applied_this_run = 1;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Checkpoint: records 3-18 (applied this run)\n") !=
              NULL);
    free(text);

    /* Live-only compaction: a session with no durable storage still has a
     * spliced active context, and "none" would misreport it. */
    snapshot.checkpoint.has_durable_checkpoint = 0;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Checkpoint: applied this run (not persisted)\n") !=
              NULL);
    CHECK(text != NULL && strstr(text, "Context: compacted\n") != NULL);
    free(text);

    /* Nobody looked: distinct from "there is none". */
    snapshot.checkpoint.known = 0;
    text = render(&snapshot);
    CHECK(text != NULL && strstr(text, "Checkpoint: (unknown)\n") != NULL);
    CHECK(text != NULL && strstr(text, "Context: (unknown)\n") != NULL);
    free(text);

    /* Full record-id range, so a 64-bit id is not truncated to int. */
    init_active(&snapshot);
    snapshot.checkpoint.has_durable_checkpoint = 1;
    snapshot.checkpoint.source_first_record_id = 1;
    snapshot.checkpoint.source_last_record_id = 18446744073709551615ULL;
    text = render(&snapshot);
    CHECK(text != NULL &&
          strstr(text, "Checkpoint: records 1-18446744073709551615\n") != NULL);
    free(text);
}

/* --- sanitization of untrusted borrowed strings --- */

/*
 * The renderer's terminal-safety contract holds for values it cannot vet:
 * a model name from a tampered session log, a working directory containing
 * whatever a filesystem accepted, a session id straight off the command
 * line. Each case below puts the hostile value in every untrusted field at
 * once and then asserts the two properties that matter -- the report is
 * still exactly its contracted number of lines, and not one raw control byte
 * or escape sequence reached the stream.
 */
static char *render_with_untrusted(const char *value) {
    struct oi_cli_status_snapshot snapshot;

    init_active(&snapshot);
    snapshot.session_id = value;
    snapshot.model = value;
    snapshot.endpoint.host = value;
    snapshot.endpoint.path = value;
    snapshot.cwd = value;
    return render(&snapshot);
}

/* No ESC, no CSI/OSC introducer, and no C0 control or DEL other than the
 * report's own line terminators. */
static void check_no_terminal_controls(const char *text) {
    size_t index;

    CHECK(strchr(text, '\x1b') == NULL);
    CHECK(strstr(text, "\x9b") == NULL); /* 8-bit CSI, as raw byte */
    for (index = 0; text[index] != '\0'; index++) {
        unsigned char byte = (unsigned char)text[index];

        if (byte == '\n') {
            continue;
        }
        CHECK(byte >= 0x20U && byte != 0x7fU);
    }
}

TEST(newlines_and_carriage_returns_cannot_forge_a_line) {
    char *text = render_with_untrusted("a\nSession: FORGED\r\nb");

    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    /* The forged key survives as *text inside a value*, but it cannot start
     * a line of its own: the line count is unchanged, and no line begins
     * with the forged key. */
    CHECK_EQ(count_lines(text), (size_t)STATUS_REPORT_LINES);
    CHECK(strstr(text, "\nSession: FORGED") == NULL);
    CHECK(strncmp(text, "Session: FORGED", 15) != 0);
    check_no_terminal_controls(text);
    free(text);
}

TEST(csi_sequences_are_stripped) {
    /* Colour, cursor movement, and a screen clear -- all CSI, all removed
     * with their parameters, leaving the surrounding text. */
    char *text = render_with_untrusted("x\x1b[31mred\x1b[0m\x1b[2J\x1b[10;10Hy");

    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK_EQ(count_lines(text), (size_t)STATUS_REPORT_LINES);
    check_no_terminal_controls(text);
    /* The parameters do not leak as literal text either. */
    CHECK(strstr(text, "[31m") == NULL);
    CHECK(strstr(text, "[2J") == NULL);
    CHECK(strstr(text, "10;10H") == NULL);
    CHECK(strstr(text, "xredy") != NULL);
    free(text);
}

TEST(osc_sequences_including_clipboard_writes_are_stripped) {
    /* OSC 52 is the one that matters most: it writes the user's clipboard.
     * Both terminators are exercised -- BEL and ST (ESC backslash). */
    char *bel = render_with_untrusted("p\x1b]52;c;cGF5bG9hZA==\x07q");
    char *st = render_with_untrusted("p\x1b]52;c;cGF5bG9hZA==\x1b\\q");
    char *title = render_with_untrusted("p\x1b]0;window title\x07q");

    CHECK(bel != NULL && st != NULL && title != NULL);
    if (bel == NULL || st == NULL || title == NULL) {
        free(bel);
        free(st);
        free(title);
        return;
    }
    CHECK_EQ(count_lines(bel), (size_t)STATUS_REPORT_LINES);
    CHECK_EQ(count_lines(st), (size_t)STATUS_REPORT_LINES);
    CHECK_EQ(count_lines(title), (size_t)STATUS_REPORT_LINES);
    check_no_terminal_controls(bel);
    check_no_terminal_controls(st);
    check_no_terminal_controls(title);
    /* The payload is gone, not merely defanged. */
    CHECK(strstr(bel, "cGF5bG9hZA==") == NULL);
    CHECK(strstr(st, "cGF5bG9hZA==") == NULL);
    CHECK(strstr(bel, "52;c") == NULL);
    CHECK(strstr(title, "window title") == NULL);
    CHECK(strstr(bel, "pq") != NULL);
    CHECK(strstr(st, "pq") != NULL);
    free(bel);
    free(st);
    free(title);
}

TEST(tabs_and_other_control_bytes_are_replaced) {
    /* A tab would shift a value into another column; the C1 range encoded as
     * UTF-8 carries an 8-bit CSI. Both are removed or replaced, never
     * passed. */
    char *text =
        render_with_untrusted("a\tb\x01\x1f\x7f" "\xc2\x9b" "c");

    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK_EQ(count_lines(text), (size_t)STATUS_REPORT_LINES);
    CHECK(strchr(text, '\t') == NULL);
    check_no_terminal_controls(text);
    /* The tab became the replacement character; the rest was dropped
     * outright by the shared sanitize pass. */
    CHECK(strstr(text, "a\xEF\xBF\xBD" "b") != NULL);
    CHECK(strstr(text, "bc") != NULL);
    free(text);
}

TEST(invalid_utf8_becomes_replacement_characters) {
    /* A lone continuation byte, a truncated three-byte lead, an overlong
     * encoding, and a surrogate -- each becomes U+FFFD rather than reaching
     * the terminal as an undecodable byte. */
    char *text = render_with_untrusted("\x80" "a" "\xe2\x82" "b" "\xc0\xaf"
                                       "c" "\xed\xa0\x80" "d");

    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK_EQ(count_lines(text), (size_t)STATUS_REPORT_LINES);
    check_no_terminal_controls(text);
    /* Every byte of the report is now well-formed UTF-8, and the legible
     * characters between the bad sequences survived. */
    CHECK(strstr(text, "\xEF\xBF\xBD" "a") != NULL);
    CHECK(strstr(text, "b") != NULL);
    CHECK(strstr(text, "c") != NULL);
    CHECK(strstr(text, "d") != NULL);
    {
        /* No raw 0x80-0xBF continuation byte stands alone, and no 0xC0/0xC1
         * overlong lead survives. */
        size_t index;
        for (index = 0; text[index] != '\0'; index++) {
            unsigned char byte = (unsigned char)text[index];
            CHECK(byte != 0xc0U && byte != 0xc1U);
            CHECK(byte < 0xf5U);
        }
    }
    free(text);
}

/* Ordinary text must not be collateral damage: the point is a readable
 * report, not an escaped one. */
TEST(ordinary_utf8_survives_sanitization) {
    char *text = render_with_untrusted("caf\xc3\xa9-\xe6\xa8\xa1\xe5\x9e\x8b");

    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK_EQ(count_lines(text), (size_t)STATUS_REPORT_LINES);
    CHECK(strstr(text, "caf\xc3\xa9-\xe6\xa8\xa1\xe5\x9e\x8b") != NULL);
    CHECK(strstr(text, "\xEF\xBF\xBD") == NULL);
    free(text);
}

/* No single field can produce an unbounded line, however long the borrowed
 * string is. */
TEST(an_overlong_field_is_bounded_and_marked) {
    size_t oversized = OI_CLI_STATUS_MAX_FIELD_BYTES + 500u;
    char *value = malloc(oversized + 1);
    char *text;

    CHECK(value != NULL);
    if (value == NULL) {
        return;
    }
    memset(value, 'x', oversized);
    value[oversized] = '\0';
    text = render_with_untrusted(value);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK_EQ(count_lines(text), (size_t)STATUS_REPORT_LINES);
        CHECK(strstr(text, "x...") != NULL);
        /* Bounded per field, so the whole report stays proportionate. */
        CHECK(strlen(text) < 6u * OI_CLI_STATUS_MAX_FIELD_BYTES);
    }
    free(text);
    free(value);
}

/*
 * Secrets are kept out of /status structurally -- the snapshot simply has no
 * field for one -- so the thing worth asserting here is the inventory: the
 * report is exactly the eleven contracted lines, with nothing extra that a
 * later change could have quietly started emitting. (That a real API key
 * never reaches the terminal is asserted end to end against the actual CLI,
 * with a sentinel key, in test_cli.c.)
 */
TEST(report_is_exactly_the_contracted_lines) {
    static const char *const keys[] = {
        "Session: ",  "Model: ",        "Endpoint: ",
        "Permissions: ", "Request timeout: ", "Tool timeout: ",
        "CWD: ",      "Conversation: ", "Queue: ",
        "Checkpoint: ", "Context: ",
    };
    const size_t key_count = sizeof keys / sizeof keys[0];
    struct oi_cli_status_snapshot snapshot;
    char *text;
    size_t index;

    CHECK_EQ(key_count, (size_t)STATUS_REPORT_LINES);
    init_active(&snapshot);
    text = render(&snapshot);
    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    CHECK_EQ(count_lines(text), key_count);
    for (index = 0; index < key_count; index++) {
        CHECK(strstr(text, keys[index]) != NULL);
    }
    /* No credential-shaped line, however the snapshot was filled in. */
    CHECK(strstr(text, "Authorization") == NULL);
    CHECK(strstr(text, "Bearer") == NULL);
    CHECK(strstr(text, "api_key") == NULL);
    CHECK(strstr(text, "ca_file") == NULL);
    free(text);
}

int main(void) {
    RUN(rejects_missing_arguments);
    RUN(reports_every_required_field);
    RUN(output_is_deterministic_and_ordered);
    RUN(initialized_snapshot_reports_everything_as_unknown);
    RUN(really_observed_zero_like_states_still_report_themselves);
    RUN(missing_optional_strings_are_named);
    RUN(endpoint_without_a_path_still_renders);
    RUN(every_session_state_is_reported);
    RUN(every_model_origin_is_reported);
    RUN(every_permission_state_is_reported);
    RUN(policy_conversion_never_yields_unknown);
    RUN(every_conversation_state_is_reported);
    RUN(failed_conversation_names_its_cause);
    RUN(queue_states_report_size_without_content);
    RUN(checkpoint_states_are_reported);
    RUN(newlines_and_carriage_returns_cannot_forge_a_line);
    RUN(csi_sequences_are_stripped);
    RUN(osc_sequences_including_clipboard_writes_are_stripped);
    RUN(tabs_and_other_control_bytes_are_replaced);
    RUN(invalid_utf8_becomes_replacement_characters);
    RUN(ordinary_utf8_survives_sanitization);
    RUN(an_overlong_field_is_bounded_and_marked);
    RUN(report_is_exactly_the_contracted_lines);
    return oi_test_report();
}
