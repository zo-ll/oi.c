#ifndef OI_CLI_STATUS_H
#define OI_CLI_STATUS_H

#include <stdint.h>
#include <stdio.h>

#include "cli_conversation.h"
#include "cli_sessions.h"
#include "cli_tools.h"
#include "oi/status.h"

/*
 * The typed snapshot behind /status.
 *
 * Why a snapshot rather than a set of accessors reached from command
 * dispatch: /status has to report facts owned by four different modules
 * (the REPL's queue, the conversation's turn state, cli.c's endpoint and
 * durable-session state, the replay state's checkpoint range), and
 * cli_command_dispatch has direct access to none of them by design. Each
 * owner fills in its own fields, the assembler hands the finished struct
 * to oi_cli_status_write, and the renderer never dereferences a private
 * struct or asks anyone a follow-up question.
 *
 * Ownership and lifetime: every pointer here is *borrowed*. The snapshot
 * owns nothing and frees nothing, and every string it points at must stay
 * valid until rendering finishes -- which is what oi_cli_command_status_cb
 * promises by keeping them valid until the next call through the same
 * callback.
 *
 * That is strictly longer than an assembler's own call. A snapshot is filled
 * in by a callback and rendered after that callback has returned, so pointing
 * a field at storage local to the assembler -- a buffer in its own stack
 * frame, say -- leaves a dangling pointer for the renderer to read. Scratch
 * for a derived value (a working directory read at assembly time is the one
 * that needs it) belongs in the callback's own context object, which outlives
 * the call; see repl_status_context in cli_repl.c.
 *
 * A borrowed string that is unavailable is NULL, never a placeholder or an
 * invalid pointer, and oi_cli_status_write renders every NULL/unknown case
 * explicitly.
 *
 * Every field has an explicit "nobody reported this" value, established by
 * oi_cli_status_snapshot_init and *not* by zeroing: a zeroed struct would
 * otherwise claim a pile of real states (no session, ask, disabled
 * deadlines, idle, empty queue) that no assembler ever asserted. Assemblers
 * must start from that initializer, so a partially-filled snapshot reports
 * what is actually unavailable instead of a plausible-looking default.
 *
 * Secrets are structurally absent: there is no field for an API key, an
 * authorization header, a CA file, or a request body, so no assembler can
 * pass one in and no renderer can print one. Queued input is reported as a
 * kind and a byte count, never as content.
 *
 * Borrowed strings are untrusted. A model name can come from a tampered
 * session log, a working directory can contain any byte a filesystem
 * accepts, and a host or session id is whatever the command line said. The
 * renderer therefore repairs and sanitizes each of them rather than
 * trusting the assembler to have done it -- see oi_cli_status_write.
 */

/* What durable session, if any, this run is attached to. */
enum oi_cli_status_session_state {
    /* Nobody reported session state. */
    OI_CLI_STATUS_SESSION_UNKNOWN = 0,
    /* An automatic session that will be created on the first message. */
    OI_CLI_STATUS_SESSION_NOT_CREATED,
    /*
     * No durable session at all this run: nothing is being persisted. The
     * `oi` CLI never reaches this -- bare interactive startup always creates
     * an automatic session -- but an embedder that runs the REPL on its own
     * arena with no session hook does, and telling it "not created" would
     * promise a session that is never coming.
     */
    OI_CLI_STATUS_SESSION_EPHEMERAL,
    OI_CLI_STATUS_SESSION_ACTIVE,
    /* Durable storage for the active session has failed. */
    OI_CLI_STATUS_SESSION_FAILED
};

/*
 * The permission policy as /status reports it. A separate type from
 * oi_cli_tool_policy on purpose: that one gates whether a tool may run, so
 * every value it has must be a real decision and it must never grow an
 * "unknown". Reporting does need one. Convert with
 * oi_cli_status_permission_from_policy.
 */
enum oi_cli_status_permission {
    OI_CLI_STATUS_PERMISSION_UNKNOWN = 0,
    OI_CLI_STATUS_PERMISSION_ASK,
    OI_CLI_STATUS_PERMISSION_ALLOW,
    OI_CLI_STATUS_PERMISSION_DENY
};

enum oi_cli_status_permission oi_cli_status_permission_from_policy(
    oi_cli_tool_policy policy);

enum oi_cli_status_queue_state {
    OI_CLI_STATUS_QUEUE_UNKNOWN = 0,
    OI_CLI_STATUS_QUEUE_EMPTY,
    OI_CLI_STATUS_QUEUE_MESSAGE,
    OI_CLI_STATUS_QUEUE_COMMAND
};

/* host/port/path and TLS on/off, and deliberately nothing else. */
struct oi_cli_status_endpoint {
    /* NULL when unknown, which makes the whole endpoint unknown: a port and
     * a TLS flag say nothing on their own, so the renderer never reports
     * them without a host. */
    const char *host;
    const char *path; /* NULL when the endpoint has no path component */
    unsigned short port;
    int use_tls;
};

struct oi_cli_status_checkpoint {
    /*
     * Somebody reported checkpoint state. Cleared by
     * oi_cli_status_snapshot_init, since "no checkpoint" and "nobody
     * looked" are different answers and only the assembler can tell them
     * apart. Every other field here is meaningless while this is 0.
     */
    int known;
    /* A checkpoint exists in durable history. The range is meaningful only
     * then, and is expressed in record ids, matching what the checkpoint
     * record itself stores. */
    int has_durable_checkpoint;
    uint64_t source_first_record_id;
    uint64_t source_last_record_id;
    /* /compact applied a checkpoint to the live conversation this run. True
     * without has_durable_checkpoint only for a session with no durable
     * storage, where /compact still splices active context. */
    int applied_this_run;
    /* Active context currently begins with a checkpoint summary rather than
     * the session's oldest real message. */
    int context_compacted;
};

struct oi_cli_status_snapshot {
    enum oi_cli_status_session_state session_state;
    /* Borrowed; non-NULL only for ACTIVE/FAILED. */
    const char *session_id;

    const char *model; /* NULL when unknown */
    enum oi_cli_session_model_origin model_origin;

    struct oi_cli_status_endpoint endpoint;
    enum oi_cli_status_permission permission;

    /* Milliseconds. 0 means the deadline is disabled (matching
     * oi_llm_config.timeout_ms); negative means unknown, which is what
     * oi_cli_status_snapshot_init leaves behind. */
    int request_timeout_ms;
    int tool_timeout_ms;

    const char *cwd; /* NULL when it could not be resolved */

    enum oi_cli_conversation_activity conversation;
    /* The conversation's status when `conversation` is
     * OI_CLI_CONVERSATION_ACTIVITY_FAILED; ignored otherwise. */
    oi_status conversation_status;
    /*
     * A queued item has asked the active turn to stop at its next safe
     * boundary. Reported only when set, so unlike the fields above this one
     * needs no unknown: saying nothing is already the right answer when
     * nobody knows.
     */
    int steering;

    enum oi_cli_status_queue_state queue;
    /* Length of the queued text. The text itself is deliberately not part
     * of this struct: see the header comment. */
    size_t queue_bytes;

    struct oi_cli_status_checkpoint checkpoint;
};

/*
 * Establishes the "nobody reported anything" state: every field explicitly
 * unknown, every borrowed pointer NULL, both deadlines negative. This is the
 * only correct starting point for an assembler, and the only correct thing
 * for a caller to hand a callback that might fill in nothing at all -- a
 * plain memset would instead assert a handful of real states.
 */
void oi_cli_status_snapshot_init(struct oi_cli_status_snapshot *snapshot);

/*
 * Renders `snapshot` to `out` as one `Key: value` line per field, in a
 * fixed order, and flushes. The output carries no terminal control
 * sequences and no styling, so it reads the same on a TTY and in a
 * redirected stream, and is stable enough to assert on.
 *
 * Every borrowed string is repaired to well-formed UTF-8 (malformed bytes
 * become U+FFFD), stripped of escape sequences and control bytes by the
 * shared cli_render_sanitize pass, and then flattened so the two control
 * bytes that pass deliberately preserves ('\n' and '\t') cannot forge an
 * extra line or a column. Ordinary UTF-8 text survives unchanged. A value
 * longer than OI_CLI_STATUS_MAX_FIELD_BYTES is truncated with a trailing
 * "..." so no single field can produce an unbounded line.
 *
 * The report is assembled in memory and written with one fwrite, so an
 * allocation failure writes nothing at all rather than half a report.
 *
 * OI_ERR_INVAL if either argument is NULL, OI_ERR_NOMEM if the report
 * cannot be assembled, OI_ERR_IO if the write or the flush fails.
 */
oi_status oi_cli_status_write(const struct oi_cli_status_snapshot *snapshot,
                              FILE *out);

/*
 * Bound on the input bytes of one borrowed string. Matches
 * OI_CLI_HISTORY_MAX_SETTING_VALUE, the ceiling a durable model or working
 * directory is already stored under, so no legitimate value is ever
 * truncated.
 */
#define OI_CLI_STATUS_MAX_FIELD_BYTES 4096u

#endif
