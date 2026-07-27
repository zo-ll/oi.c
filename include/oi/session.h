#ifndef OI_SESSION_H
#define OI_SESSION_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/sesslog.h"
#include "oi/status.h"

/*
 * The session management surface: identity (a caller-chosen ID),
 * resources (a per-turn arena, a durable on-disk log), and fault
 * containment. Deliberately does not know how to drive an LLM/tool turn
 * loop or interpret log records -- that depends on wire formats and
 * conversation structure this module has no business knowing about.
 * Sessions are fully independent: no inter-session message-passing or
 * shared task queue.
 */

typedef struct oi_session oi_session;
typedef struct oi_session_registry oi_session_registry;

typedef enum { OI_SESSION_ACTIVE, OI_SESSION_FAILED } oi_session_state;

oi_session_registry *oi_session_registry_create(void);
/* Destroys every session still registered, then the registry itself. */
void oi_session_registry_destroy(oi_session_registry *reg);

/*
 * Creates (or, if `log_path` already has prior records, resumes) a
 * session identified by `id`. `block_size` is passed through to
 * oi_arena_create (0 for a default size). OI_ERR_EXISTS if `id` is
 * already registered; otherwise whatever oi_sesslog_open returns
 * (OI_ERR_EXISTS if another process holds `log_path`, OI_ERR_PARSE on a
 * corrupt header, ...) if opening the log fails.
 *
 * Resuming means the log is opened (and recovered/truncated per
 * oi_sesslog_open) so its prior records are there to replay -- call
 * oi_sesslog_replay(oi_session_log(session), ...) afterward to rebuild
 * whatever in-memory state the embedder's conversation format needs
 * from them; this module doesn't do that itself.
 */
oi_status oi_session_create(oi_session_registry *reg, const char *id,
                             const char *log_path, size_t block_size,
                             oi_session **out_session);

/* NULL if `id` isn't registered. */
oi_session *oi_session_lookup(const oi_session_registry *reg,
                               const char *id);

/*
 * Removes `session` from `reg` and destroys its resources (arena,
 * sesslog). The caller must have already torn down anything it built
 * on top -- in-flight LLM requests/tool calls reference the session's
 * arena and don't know to stop on their own just because the session
 * did.
 */
void oi_session_destroy(oi_session_registry *reg, oi_session *session);

/* NULL-safe (returns NULL for a NULL session). */
const char *oi_session_id(const oi_session *session);

/* NULL-safe: a NULL session reads as OI_SESSION_FAILED. */
oi_session_state oi_session_state_of(const oi_session *session);

/* NULL for a NULL session, and once the session has failed (see
 * oi_session_fail). */
oi_arena *oi_session_arena(const oi_session *session);
oi_sesslog *oi_session_log(const oi_session *session);

/*
 * Fault containment: marks `session` FAILED, destroying its arena and
 * closing its log, without touching the registry, the reactor, or any
 * other session. Idempotent, NULL-safe. This is a reporting hook, not a
 * detector -- callers invoke it once *they* determine a session has hit
 * an unrecoverable error (a malformed API response, an allocator
 * failure, ...) while driving its LLM/tool calls; this module has no
 * visibility into those and can't determine that on its own. The
 * session stays in the registry (still look-up-able, e.g. for
 * diagnostics) until an explicit oi_session_destroy.
 */
void oi_session_fail(oi_session *session);

#endif /* OI_SESSION_H */
