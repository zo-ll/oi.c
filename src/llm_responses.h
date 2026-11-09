#ifndef OI_LLM_RESPONSES_H
#define OI_LLM_RESPONSES_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/json.h"
#include "oi/status.h"

/*
 * Converts a Chat Completions request body to a Responses API body. Only
 * "model", "messages", and function "tools" carry over; every other member
 * is dropped because the plan-usage route rejects most sampling and
 * lifecycle fields. The result always sets "store":false and "stream":true,
 * which that route requires. System messages become developer messages
 * because it also rejects the system role.
 *
 * Parsed values live on `arena`. On OI_OK the caller owns *out_writer.
 * Returns OI_ERR_PARSE when `body` is not a well-formed chat request.
 */
oi_status oi_llm_responses_translate_body(oi_arena *arena, const char *body,
                                          size_t body_len,
                                          oi_json_writer **out_writer);

#endif /* OI_LLM_RESPONSES_H */
