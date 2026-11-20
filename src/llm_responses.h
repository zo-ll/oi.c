#ifndef OI_LLM_RESPONSES_H
#define OI_LLM_RESPONSES_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/json.h"
#include "oi/status.h"

oi_status oi_llm_responses_translate_body(oi_arena *arena, const char *body,
                                          size_t body_len,
                                          oi_json_writer **out_writer);

#endif
