#ifndef OI_LLM_H
#define OI_LLM_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/reactor.h"
#include "oi/status.h"

/*
 * A streaming client for the OpenAI-compatible Chat Completions wire
 * format. Scope is deliberately narrow: this module owns HTTP+SSE
 * transport mechanics (connection, request framing, incremental
 * response parsing) and nothing about message-history or tool-schema
 * modeling -- the request body is JSON the caller already builds with
 * oi/json.h (whatever shape "messages"/"tools" end up being is a
 * session/tool-registry concern, not this module's).
 *
 * Each request opens its own connection (HTTP "Connection: close");
 * there is no keep-alive/connection pooling in this first cut.
 */

typedef struct oi_llm_client oi_llm_client;
typedef struct oi_llm_request oi_llm_request;

/* Fires for each streamed assistant-text delta. May fire zero or more
 * times over the life of a request. `text` is borrowed -- copy it if
 * you need it past the callback's return. */
typedef void (*oi_llm_delta_cb)(const char *text, size_t len,
                                 void *user_data);

/*
 * Fires exactly once, terminating the request (oi_llm_request itself is
 * freed right after this returns -- don't touch it from inside). `status`
 * is OI_OK on a clean completion of the stream. Otherwise: OI_ERR_IO for
 * a network/TLS failure, OI_ERR_PARSE for a malformed response.
 * `http_status` is the HTTP status code (0 if a response was never
 * received at all). `error_body`/`error_body_len` hold the raw response
 * body when the server returned a non-2xx status (truncated at 64KiB;
 * NULL/0 if there is none, e.g. on a connect failure, or on success).
 */
typedef void (*oi_llm_done_cb)(oi_status status, int http_status,
                                const char *error_body, size_t error_body_len,
                                void *user_data);

struct oi_llm_config {
    const char *host;    /* e.g. "api.openai.com" */
    unsigned short port; /* e.g. 443 */
    int use_tls;
    const char *ca_file; /* NULL = system default trust store */
    const char *api_key; /* sent as "Authorization: Bearer <key>"; NULL to omit */
    const char *path;    /* e.g. "/v1/chat/completions" */
};

/* Copies everything out of `cfg`; it need not outlive this call. Returns
 * NULL on allocation failure, a missing/invalid host or path, or control
 * characters in an API key (request-header injection is rejected at this
 * boundary). The returned client must outlive every request started
 * against it. */
oi_llm_client *oi_llm_client_create(const struct oi_llm_config *cfg);
void oi_llm_client_destroy(oi_llm_client *client);

/*
 * Starts one streaming chat-completion request. `body` is a complete
 * JSON request object (model/messages/tools/etc., with "stream":true
 * already set by the caller) -- it's copied, so it need not outlive this
 * call. `arena` is used for the small JSON values parsed out of each
 * streamed chunk; this module never resets it, that's the caller's
 * turn/session boundary to manage, and it must outlive the request.
 *
 * Returns the in-flight request via *out_request; `on_delta`/`on_done`
 * fire later, driven by `reactor`.
 */
oi_status oi_llm_request_start(oi_llm_client *client, oi_reactor *reactor,
                                oi_arena *arena, const char *body,
                                size_t body_len, oi_llm_delta_cb on_delta,
                                oi_llm_done_cb on_done, void *user_data,
                                oi_llm_request **out_request);

/*
 * Aborts an in-flight request; on_done will NOT fire. NULL-safe and a
 * no-op if the request already finished. Safe to call from within
 * on_delta; never call it from within on_done (the request is already
 * gone by then).
 */
void oi_llm_request_cancel(oi_llm_request *req);

#endif /* OI_LLM_H */
