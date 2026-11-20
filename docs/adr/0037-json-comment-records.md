# json: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/json_internal.h`, `src/json_parse.c`, `src/json_write.c`, `test/fuzz/fuzz_json.c`, `test/test_json.c`.

## `oi_json_member` (src/json_internal.h)

Value-tree representation shared between json_value.c (accessors and arena-backed constructors), json_parse.c (builds the tree while parsing), and llm_responses.c (copies parsed tool schemas verbatim). json_write.c is deliberately excluded: it serializes text directly from caller calls and never touches this representation, so it doesn't need to know the tree layout.

## `oi_json_new_string` (src/json_internal.h)

Copies `len` bytes from `ptr` into arena memory (plus a trailing NUL convenience byte).

## `oi_json_array_push` (src/json_internal.h)

Appends to a growable arena-backed array, doubling capacity as needed.

## `oi_json_object_append` (src/json_internal.h)

Appends a member to the end of an object's member list. Key bytes are copied into arena memory. Does not check for/dedupe duplicate keys; oi_json_object_get resolves duplicates by returning the first match.

## `OI_JSON_MAX_DEPTH` (src/json_parse.c)

Hard cap on container nesting depth: without one, a tiny malicious input of a million '[' characters would grow the frame stack (and, via oi_json_new_array, the arena) without bound.

## `OI_JSON_MAX_NUMBER_LEN` (src/json_parse.c)

Hard cap on a single number literal's raw byte length. Real doubles never need more than a few dozen digits; this just stops a pathological "111...1" token from growing unbounded.

## `attach_completed` (src/json_parse.c)

A value (scalar, or a container just popped) is complete: attach it to the enclosing array/object, or -- if the stack is empty -- finish the whole document.

## `c_numeric_locale` (src/json_parse.c)

JSON numbers always use '.', but strtod reads the radix of the current locale. An embedding program can set a comma locale, so the parse runs under a thread-local "C" locale.

## `oi_json_parser_feed` (src/json_parse.c)

Inside a string, copy each run of plain bytes in one call; only a quote, a backslash, or a control byte needs the state machine.

## `oi_wipe` (src/json_write.c)

The buffer can hold a secret, for example saved credentials.

## `ensure_buf.new_buf` (src/json_write.c)

Not realloc: a moved block would keep a copy of the text in freed memory, where oi_json_writer_destroy cannot wipe it.

## `begin_value` (src/json_write.c)

Validates/updates state for the start of any value (scalar or the opening bracket of a container), including comma insertion for array elements. Object members insert their own comma in oi_json_write_object_key, since it precedes the key, not the value.

## `end_value` (src/json_write.c)

Marks the value just written as complete: bumps the parent frame's element count (or, at top level, marks the whole document done).

## `oi_json_write_number.len` (src/json_write.c)

"%.17g" writes the radix of the current locale, which can be ',' or a multibyte sequence. Every other byte it writes is a digit, a sign, or 'e', so replace the one run of other bytes with '.'.

## `append_escaped_string` (src/json_write.c)

Each byte can expand to six bytes as a \u00XX escape, plus quotes. Reject impossible lengths before reading from caller memory.

## `append_escaped_string.run` (src/json_write.c)

Append each run of bytes that need no escape in one call.

## `file scope` (test/fuzz/fuzz_json.c)

libFuzzer harness for the JSON parser (oi/json.h).

Checks more than "doesn't crash": oi/json.h promises that feed() behaves identically regardless of how the input is chunked -- the property the whole non-blocking-socket design rests on. Every input is therefore parsed twice, once as a single feed and once one byte at a time, and the two runs must agree on their terminal state and, when a value was produced, on its serialization.

Re-serializing the value tree also drags json_write.c into the fuzzed surface, and comparing the two texts catches value-level divergence (a dropped member, a mis-decoded escape) that comparing done/failed flags alone would miss.

json_internal.h is included because the public API can look up object members by key but cannot enumerate them in order; walking the member list directly is what makes the object comparison exact.

## `FUZZ_MAX_INPUT` (test/fuzz/fuzz_json.c)

Bounds the byte-at-a-time run, which costs one feed() call per byte. libFuzzer's own default max_len is well under this; the cap only stops a hand-added corpus file from dominating the run.

## `parse_result.nomem` (test/fuzz/fuzz_json.c)

Set when a resource limit (arena block size, writer growth) was hit. Those limits are allowed to depend on chunking, so the two runs are only compared when neither tripped one.

## `capture_text` (test/fuzz/fuzz_json.c)

Serializes `root` into a freshly malloc'd NUL-terminated buffer. Ownership transfers to `out`; any failure leaves out->text NULL and sets out->nomem, which suppresses the comparison rather than reporting a bug.

## `run_parse` (test/fuzz/fuzz_json.c)

Parse errors are sticky, so stopping at the first one matches what the single-feed run reports after consuming the rest.

Deliberately called even on an already-failed parser: "finish after an error" is itself a path worth fuzzing, and both runs reach it identically.

## `parse_byte_by_byte` (test/test_json.c)

Feeds `doc` to a fresh parser, one byte at a time -- the harshest possible chunking, since a parser correct here is correct for any coarser chunking too. Returns the completed root, or NULL.

## `check_all_split_points_succeed` (test/test_json.c)

Feeds `doc` split at every possible single cut point (i.e. two feed() calls: [0,k) and [k,len)), for every k, plus the whole-buffer and fully-byte-by-byte cases. All must agree the value is done and equal in shape to what byte-by-byte parsing produced (checked by caller via CHECK_EQ on scalar fields, since a general deep-equal isn't needed for these tests). This exercises "split mid-escape / mid-number / etc." without hand-writing every split point per test.

## `accessors_reject_wrong_type_and_null.p` (test/test_json.c)

"1" alone needs finish() in real use; here we just want a NUMBER value to probe wrong-type accessors against, so reparse using finish() directly.
