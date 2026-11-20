#ifndef OI_LLM_SSE_H
#define OI_LLM_SSE_H

#include <stddef.h>

#include "oi/status.h"

typedef struct oi_llm_sse_parser oi_llm_sse_parser;

typedef void (*oi_llm_sse_event_cb)(const char *data, size_t len,
                                     void *user_data);

oi_llm_sse_parser *oi_llm_sse_parser_create(oi_llm_sse_event_cb on_event,
                                             void *user_data);
void oi_llm_sse_parser_destroy(oi_llm_sse_parser *p);

oi_status oi_llm_sse_parser_feed(oi_llm_sse_parser *p, const void *bytes,
                                  size_t len);

oi_status oi_llm_sse_parser_finish(oi_llm_sse_parser *p);

#endif
