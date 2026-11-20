#ifndef OI_LLM_HTTP_H
#define OI_LLM_HTTP_H

#include <stddef.h>

#include "oi/status.h"

typedef struct oi_llm_http_parser oi_llm_http_parser;

typedef void (*oi_llm_http_headers_done_cb)(int status_code, void *user_data);
typedef void (*oi_llm_http_body_cb)(const void *data, size_t len,
                                     void *user_data);

oi_llm_http_parser *oi_llm_http_parser_create(
    oi_llm_http_headers_done_cb on_headers_done, oi_llm_http_body_cb on_body,
    void *user_data);
void oi_llm_http_parser_destroy(oi_llm_http_parser *p);

oi_status oi_llm_http_parser_feed(oi_llm_http_parser *p, const void *bytes,
                                   size_t len);

oi_status oi_llm_http_parser_close(oi_llm_http_parser *p);

int oi_llm_http_parser_body_done(const oi_llm_http_parser *p);
int oi_llm_http_parser_failed(const oi_llm_http_parser *p);

int oi_llm_http_parser_reusable(const oi_llm_http_parser *p);

#endif
