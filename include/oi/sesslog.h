#ifndef OI_SESSLOG_H
#define OI_SESSLOG_H

#include <stddef.h>

#include "oi/status.h"

/*
 * Durable append-only log for session state: one writev() system call per
 * record. Each record carries a CRC-32 of its payload. A short/error
 * write is rolled back before returning; a genuine mid-write process
 * crash is recovered by discarding the partial tail on the next open.
 * No fsync by default: crash-safe against the *process* dying, not
 * against a power loss losing the page cache.
 *
 * Synchronous, local-disk-only I/O; unlike the reactor-driven modules,
 * this one makes no non-blocking-I/O claim (PLAN.md's "no blocking
 * calls anywhere" is scoped to the LLM/tool I/O paths, not local disk).
 */

typedef struct oi_sesslog oi_sesslog;

/* Payload length cap per record; a length prefix claiming more than
 * this during recovery/replay is treated as corruption. */
#define OI_SESSLOG_MAX_RECORD (64u * 1024 * 1024)

/*
 * Opens `path`, creating it (with a fresh versioned header) if it
 * doesn't exist or is empty. If it already has content, validates the
 * header -- OI_ERR_PARSE on a bad magic or an unrecognized version --
 * and recovers by truncating off a trailing record left incomplete by
 * a prior crash; everything before that point is preserved untouched.
 *
 * A record with a bad checksum or an impossible length is damage, not a
 * crash tail. Open then copies the whole file to "<path>.corrupt" first,
 * cuts the log before the damaged record, and reports the count of bytes
 * cut through oi_sesslog_damaged_bytes. If the copy fails, open fails
 * with OI_ERR_IO (or OI_ERR_NOMEM) and changes nothing.
 *
 * Takes an exclusive advisory lock (flock) for the life of the handle,
 * so two processes can't both append to the same log concurrently;
 * OI_ERR_EXISTS if another process already holds it.
 */
oi_status oi_sesslog_open(const char *path, oi_sesslog **out_log);

/* NULL-safe. Releases the lock and closes the underlying fd. */
void oi_sesslog_close(oi_sesslog *log);

/* Appends one record in a single writev() call. OI_ERR_INVAL if `len`
 * exceeds OI_SESSLOG_MAX_RECORD. OI_ERR_IO leaves the prior valid log
 * contents intact whenever the filesystem permits rollback. */
oi_status oi_sesslog_append(oi_sesslog *log, const void *data, size_t len);

/* Replays every record currently in the log, in write order, via `cb`.
 * `data` is borrowed, valid only for the duration of one callback
 * call. */
typedef void (*oi_sesslog_record_cb)(const void *data, size_t len,
                                      void *user_data);
oi_status oi_sesslog_replay(oi_sesslog *log, oi_sesslog_record_cb cb,
                             void *user_data);

/* The count of bytes that oi_sesslog_open cut because of damage; 0 when
 * the log was intact or lost only a crash tail. NULL-safe. */
size_t oi_sesslog_damaged_bytes(const oi_sesslog *log);

#endif /* OI_SESSLOG_H */
