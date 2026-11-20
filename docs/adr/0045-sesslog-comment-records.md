# sesslog: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/sesslog.c`, `test/fuzz/fuzz_sesslog.c`, `test/test_sesslog.c`.

## `OI_SESSLOG_VERSION_LENGTH_ONLY` (src/sesslog.c)

Version 1 records are [u32 length][payload]. Version 2 records are [u32 length][u32 CRC-32 of the payload][payload]. A new log is version 2; a version 1 log keeps its own record format for its whole life.

## `oi_sesslog.size` (src/sesslog.c)

The file size after the last complete record. The lifetime flock keeps other appenders out, so only this handle changes it.

## `crc32_update` (src/sesslog.c)

CRC-32 (IEEE 802.3, reflected), four bits at a time.

## `crc32_of_range` (src/sesslog.c)

CRC-32 of `len` bytes at `pos`, read in chunks so a large record needs no buffer of its own size.

## `scan_for_valid_end` (src/sesslog.c)

Walks records from just after the header, bounding every offset against the file's actual size (rather than relying on read() EOF detection), so a length prefix claiming more than what's really there is detected without ever reading past the real end of file.

A record that runs past the end of the file is the tail of a write that a crash cut short. An implausible length or a checksum mismatch on a complete record is damage: *out_damaged is set, because the bytes after it can hold more records.

## `keep_damaged_copy` (src/sesslog.c)

Copies the whole log to "<path>.corrupt" before a recovery cut removes damaged bytes, so the records after the damage stay recoverable by hand. Replaces an older copy.

## `best_effort_truncate` (src/sesslog.c)

Keep a failed first write from leaving a file that looks permanently corrupt to the next open attempt.

The handle stays usable after a short/error write: remove any partial tail now instead of waiting for a future reopen to discover and recover it.

## `oi_sesslog_open` (src/sesslog.c)

No cut without the copy: a failed copy keeps every byte.

## `oi_sesslog_replay` (src/sesslog.c)

A record damaged after open is reported, never delivered.

## `file scope` (test/fuzz/fuzz_sesslog.c)

libFuzzer harness for session-log record framing (oi/sesslog.h).

The log is the one persistent input the harness parses, and a crashed process can leave arbitrary trailing bytes behind -- so recovery has to cope with a header and record stream it did not write. Each input is dropped on disk verbatim and opened as a log.

The interesting property is not just "open() doesn't crash" but that recovery leaves a *usable* log: oi_sesslog_open documents that it truncates a torn trailing record and preserves everything before it. So whenever open succeeds, this replays the log, appends a fresh record, and replays again -- the second pass must return exactly the first pass's records plus the appended one, intact and in order. A recovery that truncated to the wrong offset shows up here as a record count or payload mismatch rather than as silently lost history.

## `probe_record` (test/fuzz/fuzz_sesslog.c)

Payload of the record appended between the two replay passes.

## `replay_ctx.digest` (test/fuzz/fuzz_sesslog.c)

Digest of every record seen, so the two passes can be compared without retaining unbounded payload.

## `replay_ctx.last` (test/fuzz/fuzz_sesslog.c)

Payload of the most recent record, capped -- only used to confirm the appended probe survived the round trip.

## `on_record` (test/fuzz/fuzz_sesslog.c)

FNV-1a over the length and the bytes, so record boundaries are part of the digest and not just the concatenated payload.

A zero-length record replays as (NULL, 0), and memcpy from a null pointer is undefined even for a zero count.

## `log_path` (test/fuzz/fuzz_sesslog.c)

One fixed path per process: libFuzzer runs single-threaded, so each iteration can reuse and truncate the same file.

## `LLVMFuzzerTestOneInput` (test/fuzz/fuzz_sesslog.c)

Recovery has run and the log validated, so an append must land cleanly on the truncated tail.

## `records_persist_across_reopen` (test/test_sesslog.c)

Simulates a process restart: a fresh open() must replay what was written before, exercising the "resume mid-conversation" use case from PLAN.md.

## `recovery_discards_truncated_length_prefix` (test/test_sesslog.c)

The truncated bytes must have been physically discarded (not just skipped in memory), so a subsequent append lands cleanly and a later replay doesn't see phantom data.
