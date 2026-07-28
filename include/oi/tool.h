#ifndef OI_TOOL_H
#define OI_TOOL_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/json.h"
#include "oi/reactor.h"
#include "oi/status.h"

/*
 * Tool declaration (a static registry of name/schema/argv-builder) and
 * tool execution (every call runs as a child process, stdin/stdout
 * registered as non-blocking fds on the reactor -- no blocking calls
 * anywhere). Permission policy is entirely the embedder's: this module
 * only provides the allow/deny/ask hook and, for "ask", a way to defer
 * and later resolve the decision.
 */

typedef struct oi_tool_registry oi_tool_registry;

/*
 * Builds the argv (execvp-style, NUL-terminated array of NUL-terminated
 * strings) for one invocation from its parsed JSON arguments. Allocated
 * on `arena`. Returning a non-OK status aborts the call before any
 * process is spawned.
 */
typedef oi_status (*oi_tool_build_argv)(const oi_json_value *args,
                                         oi_arena *arena, void *user_data,
                                         char ***out_argv);

oi_tool_registry *oi_tool_registry_create(void);
void oi_tool_registry_destroy(oi_tool_registry *reg);

/* `name` and `json_schema` are copied. `json_schema` is the tool's
 * parameter schema as a JSON Schema string -- opaque to this module,
 * exposed only via oi_tool_registry_schema for the caller to embed
 * verbatim when building an LLM request's "tools" field. OI_ERR_EXISTS
 * if `name` is already registered. */
oi_status oi_tool_registry_add(oi_tool_registry *reg, const char *name,
                                const char *json_schema,
                                oi_tool_build_argv build_argv,
                                void *user_data);

/* NULL if `name` isn't registered. */
const char *oi_tool_registry_schema(const oi_tool_registry *reg,
                                     const char *name);

/* --- execution --- */

typedef struct oi_tool_call oi_tool_call;

typedef enum {
    OI_TOOL_ALLOW,
    OI_TOOL_DENY,
    OI_TOOL_ASK /* defer -- see oi_tool_call_resolve */
} oi_tool_decision;

typedef oi_tool_decision (*oi_tool_permission_cb)(const char *tool_name,
                                                   const oi_json_value *args,
                                                   void *user_data);

/* Fires zero or more times as the child's stdout (and, merged in,
 * stderr) bytes arrive. `data` is borrowed. */
typedef void (*oi_tool_output_cb)(const void *data, size_t len,
                                   void *user_data);

typedef enum {
    OI_TOOL_EXIT_NORMAL,   /* process ran; `code` is its exit status */
    OI_TOOL_EXIT_SIGNALED, /* process was killed; `code` is the signal */
    OI_TOOL_EXIT_FAILED,   /* execvp itself failed; `code` is the errno */
    OI_TOOL_EXIT_TIMEOUT   /* configured deadline expired; `code` is zero */
} oi_tool_exit_kind;

/* Fires exactly once, terminating the call (oi_tool_call is freed right
 * after this returns -- don't touch it from inside). Fires only once a
 * process has actually been spawned; everything before that is reported
 * synchronously via oi_tool_call_start/oi_tool_call_resolve's return
 * value instead (see below), with no call created and neither this nor
 * on_output ever firing. */
typedef void (*oi_tool_done_cb)(oi_tool_exit_kind kind, int code,
                                 void *user_data);

/*
 * Starts (or, for an OI_TOOL_ASK decision, stages) one invocation of
 * `tool_name` with `args`. `arena` backs the argv built for it and must
 * outlive the call.
 *
 * Synchronous outcomes are reported via the return value, with
 * *out_call left untouched and neither callback ever firing:
 *   OI_ERR_NOTFOUND  -- no such tool registered
 *   OI_ERR_DENIED     -- permission_cb returned OI_TOOL_DENY
 *   (build_argv/fork/exec-setup failure) -- see oi_status
 *
 * On OI_OK with an OI_TOOL_ALLOW decision, a process has been spawned;
 * on_output/on_done fire later from the reactor. On OI_OK with an
 * OI_TOOL_ASK decision, *out_call is populated but nothing has run yet
 * -- call oi_tool_call_resolve to proceed, or oi_tool_call_cancel to
 * abandon it; no callback fires until you do.
 */
oi_status oi_tool_call_start(oi_tool_registry *reg, oi_reactor *r,
                              oi_arena *arena, const char *tool_name,
                              const oi_json_value *args,
                              oi_tool_permission_cb permission_cb,
                              void *permission_ud, oi_tool_output_cb on_output,
                              oi_tool_done_cb on_done, void *user_data,
                              oi_tool_call **out_call);

/*
 * Resolves a call left pending by an OI_TOOL_ASK decision. `allow`
 * nonzero proceeds to spawn the process (same synchronous-failure
 * contract as oi_tool_call_start); zero denies it. Either way `call` is
 * invalid after this returns -- on denial it's freed immediately
 * (OI_ERR_DENIED, matching oi_tool_call_start's DENY contract); on a
 * spawn failure it's likewise freed and the error returned; only a
 * successful spawn leaves it alive, now running.
 *
 * OI_ERR_INVAL if `call` isn't currently awaiting a decision.
 */
oi_status oi_tool_call_resolve(oi_tool_call *call, int allow);

/* Queues bytes for the child's stdin; non-blocking, returns once
 * queued. OI_ERR_INVAL if the call isn't a running process (e.g. still
 * awaiting an ASK decision, or already finished). */
oi_status oi_tool_call_write_stdin(oi_tool_call *call, const void *data,
                                    size_t len);

/*
 * Closes the child's stdin so a read() in the child sees EOF. If bytes
 * queued via oi_tool_call_write_stdin haven't been flushed yet, the
 * close is deferred until they have been -- the child always sees
 * everything written before it sees EOF.
 */
oi_status oi_tool_call_close_stdin(oi_tool_call *call);

/*
 * Sets a one-shot execution deadline. For an ASK-pending call it starts
 * when permission is granted and the child is spawned; for a running call
 * it starts immediately. May be set only once, with a positive value.
 */
oi_status oi_tool_call_set_timeout(oi_tool_call *call, int timeout_ms);

/*
 * Kills the process (if one is running) and frees `call`; on_done will
 * NOT fire. NULL-safe. Safe to call reentrantly from within this call's
 * own on_output (not from within on_done, which means the call is
 * already gone).
 */
void oi_tool_call_cancel(oi_tool_call *call);

#endif /* OI_TOOL_H */
