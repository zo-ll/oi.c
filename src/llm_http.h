#ifndef OI_LLM_HTTP_H
#define OI_LLM_HTTP_H

#include <stddef.h>

#include "oi/status.h"

/*
 * Incremental HTTP/1.1 response parser: status line, headers, and a
 * chunked- or Content-Length-delimited body, fed byte-by-byte-safe from
 * a non-blocking socket. Deliberately scoped to what an OpenAI-compatible
 * API actually sends -- no HTTP/1.0 "read until close" body mode, no
 * chunk trailers beyond a single terminating CRLF. Knows nothing about
 * SSE or JSON; it just hands raw body bytes to the caller.
 */

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

int oi_llm_http_parser_body_done(const oi_llm_http_parser *p);
int oi_llm_http_parser_failed(const oi_llm_http_parser *p);

#endif /* OI_LLM_HTTP_H */
