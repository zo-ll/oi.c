#ifndef OI_JSON_H
#define OI_JSON_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/status.h"

/*
 * Dependency-free JSON parser and serializer.
 *
 * The parser is a true incremental push-parser: oi_json_parser_feed may be
 * called with any number of bytes at a time -- including one byte at a
 * time, split in the middle of a string escape, a \uXXXX surrogate pair,
 * or a number literal -- and behavior is identical regardless of how the
 * input is chunked. This is required for parsing SSE/chunked LLM API
 * responses off a non-blocking socket.
 *
 * Parsed values are allocated on a caller-supplied oi_arena and are valid
 * until that arena is reset or destroyed; the parser itself owns no
 * caller-visible heap allocations beyond its own bookkeeping.
 */

typedef struct oi_json_parser oi_json_parser;
typedef struct oi_json_value oi_json_value;

typedef enum oi_json_type {
    OI_JSON_NULL,
    OI_JSON_BOOL,
    OI_JSON_NUMBER,
    OI_JSON_STRING,
    OI_JSON_ARRAY,
    OI_JSON_OBJECT
} oi_json_type;

/* --- parsing --- */

/* `arena` is borrowed: the parser does not destroy it. Returns NULL on
 * allocation failure or if `arena` is NULL. */
oi_json_parser *oi_json_parser_create(oi_arena *arena);

/* NULL-safe. Does not touch the arena. */
void oi_json_parser_destroy(oi_json_parser *p);

/*
 * Feeds `len` bytes of input. Consumes bytes up through the end of the
 * current top-level value; whitespace after that is ignored, but any
 * other byte fed after the value is already complete is a parse error.
 * Returns OI_ERR_PARSE on malformed input, or immediately if the parser
 * is already in an error state -- errors are sticky until
 * oi_json_parser_reset. Returns OI_ERR_NOMEM if the arena or an internal
 * scratch buffer cannot grow.
 */
oi_status oi_json_parser_feed(oi_json_parser *p, const void *bytes,
                               size_t len);

/*
 * JSON numbers have no explicit terminator, so a bare top-level number
 * (e.g. "42") stays ambiguous -- and oi_json_parser_done stays false --
 * until either a following byte disambiguates it or the caller calls
 * this to confirm no more input is coming. A no-op if already done;
 * returns OI_ERR_PARSE if the input so far is incomplete for any other
 * reason (open string, unclosed container, or nothing parsed yet).
 */
oi_status oi_json_parser_finish(oi_json_parser *p);

/* True once a complete top-level value has been parsed. */
int oi_json_parser_done(const oi_json_parser *p);

/* True once the parser has hit a malformed-input error. */
int oi_json_parser_failed(const oi_json_parser *p);

/*
 * Returns the completed root value (arena-owned), or NULL if parsing
 * isn't done yet. Does not reset the parser -- call oi_json_parser_reset
 * before feeding the next value.
 */
oi_json_value *oi_json_parser_root(const oi_json_parser *p);

/* Clears parse state (done/failed/partial-token buffers) so the parser
 * can parse another top-level value. Does not touch the arena, so
 * previously parsed values remain valid until the arena itself is
 * reset. */
void oi_json_parser_reset(oi_json_parser *p);

/* --- value accessors --- */

/* `v` must be non-NULL (a programming-error precondition, not a runtime
 * one -- values that might be absent come from oi_json_array_get /
 * oi_json_object_get, which already return NULL for that case). */
oi_json_type oi_json_type_of(const oi_json_value *v);

/* The oi_json_get_* functions below return OI_ERR_INVAL if `v` is NULL or
 * not of the expected type. */
oi_status oi_json_get_bool(const oi_json_value *v, int *out);
oi_status oi_json_get_number(const oi_json_value *v, double *out);

/* `*out_ptr` points into arena memory and is valid for `*out_len` bytes.
 * A trailing NUL byte is appended as a convenience for strings with no
 * embedded NUL byte, but `*out_len` is authoritative -- a JSON string may
 * legally contain an embedded NUL via a \u0000 escape. */
oi_status oi_json_get_string(const oi_json_value *v, const char **out_ptr,
                              size_t *out_len);

size_t oi_json_array_len(const oi_json_value *v);
/* Returns NULL if `v` is not an array or `index` is out of range. */
oi_json_value *oi_json_array_get(const oi_json_value *v, size_t index);

size_t oi_json_object_len(const oi_json_value *v);
/* Linear scan by NUL-terminated key. Returns NULL if not found or `v` is
 * not an object. */
oi_json_value *oi_json_object_get(const oi_json_value *v, const char *key);

/* --- serialization ---
 * A minimal streaming writer for outgoing requests/tool results. Tracks
 * container nesting to insert commas correctly and reject structurally
 * invalid call sequences (e.g. a bare value after the top-level value is
 * already complete).
 */

typedef struct oi_json_writer oi_json_writer;

oi_json_writer *oi_json_writer_create(void);
void oi_json_writer_destroy(oi_json_writer *w);

oi_status oi_json_write_null(oi_json_writer *w);
oi_status oi_json_write_bool(oi_json_writer *w, int value);
oi_status oi_json_write_number(oi_json_writer *w, double value);
oi_status oi_json_write_string(oi_json_writer *w, const char *s, size_t len);

oi_status oi_json_write_array_begin(oi_json_writer *w);
oi_status oi_json_write_array_end(oi_json_writer *w);

oi_status oi_json_write_object_begin(oi_json_writer *w);
/* Must be called in "expecting a member" position, immediately followed
 * by exactly one value-writing call for that member. */
oi_status oi_json_write_object_key(oi_json_writer *w, const char *key,
                                    size_t len);
oi_status oi_json_write_object_end(oi_json_writer *w);

/* Returns the NUL-terminated serialized bytes written so far and, if
 * `out_len` is non-NULL, their length excluding the NUL. Valid until the
 * next write call or oi_json_writer_destroy. */
const char *oi_json_writer_data(const oi_json_writer *w, size_t *out_len);

#endif /* OI_JSON_H */
