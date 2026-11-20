# llm_responses: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/llm_responses.c`, `src/llm_responses.h`.

## `write_value` (src/llm_responses.c)

The parser bounds nesting depth, so this recursion is bounded too.

## `write_input_items` (src/llm_responses.c)

A tool-calling turn stores empty content; an empty message item would add a turn the model never produced.

## `WRITE` (src/llm_responses.c)

Chat tool schemas make no strict-mode promise.

## `oi_llm_responses_translate_body` (src/llm_responses.h)

Converts a Chat Completions request body to a Responses API body. Only "model", "messages", and function "tools" carry over; every other member is dropped because the plan-usage route rejects most sampling and lifecycle fields. The result always sets "store":false and "stream":true, which that route requires. System messages become developer messages because it also rejects the system role.

Parsed values live on `arena`. On OI_OK the caller owns *out_writer. Returns OI_ERR_PARSE when `body` is not a well-formed chat request.
