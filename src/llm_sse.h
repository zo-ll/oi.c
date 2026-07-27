#ifndef OI_LLM_SSE_H
#define OI_LLM_SSE_H

#include <stddef.h>

#include "oi/status.h"

/*
 * Splits a byte stream (an HTTP response body) into Server-Sent-Events
 * "data:" payloads. Scoped to the single-data-line-per-event form used
 * by OpenAI-compatible streaming APIs -- not the full SSE spec (no
 * multi-line data-field joining, no id:/event:/retry: fields). Anything
 * that isn't a "data:" line (comments, blank event separators, other
 * fields) is silently ignored, matching SSE's own fault-tolerant design.
 */

typedef struct oi_llm_sse_parser oi_llm_sse_parser;

typedef void (*oi_llm_sse_event_cb)(const char *data, size_t len,
                                     void *user_data);

oi_llm_sse_parser *oi_llm_sse_parser_create(oi_llm_sse_event_cb on_event,
                                             void *user_data);
void oi_llm_sse_parser_destroy(oi_llm_sse_parser *p);

oi_status oi_llm_sse_parser_feed(oi_llm_sse_parser *p, const void *bytes,
                                  size_t len);

#endif /* OI_LLM_SSE_H */
