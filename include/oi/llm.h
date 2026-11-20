#ifndef OI_LLM_H
#define OI_LLM_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/reactor.h"
#include "oi/status.h"

typedef struct oi_llm_client oi_llm_client;
typedef struct oi_llm_request oi_llm_request;

typedef void (*oi_llm_delta_cb)(const char *text, size_t len,
                                 void *user_data);

typedef enum {
    OI_LLM_EVENT_TEXT,
    OI_LLM_EVENT_TOOL_CALL,
    OI_LLM_EVENT_USAGE
} oi_llm_event_type;

typedef struct {
    oi_llm_event_type type;
    union {
        struct {
            const char *data;
            size_t len;
        } text;
        struct {
            size_t index;
            const char *id;
            size_t id_len;
            const char *name;
            size_t name_len;
            const char *arguments;
            size_t arguments_len;
        } tool_call;
        struct {
            size_t input_tokens;
            size_t cached_input_tokens;
            size_t output_tokens;
            int has_cached_input_tokens;
        } usage;
    } as;
} oi_llm_event;

typedef void (*oi_llm_event_cb)(const oi_llm_event *event, void *user_data);

typedef void (*oi_llm_done_cb)(oi_status status, int http_status,
                                const char *error_body, size_t error_body_len,
                                void *user_data);

struct oi_llm_config {
    const char *host;
    unsigned short port;
    int use_tls;
    const char *ca_file;
    const char *api_key;
    const char *path;
    int timeout_ms;
};

oi_llm_client *oi_llm_client_create(const struct oi_llm_config *cfg);

oi_llm_client *oi_llm_client_create_responses(const struct oi_llm_config *cfg);
void oi_llm_client_destroy(oi_llm_client *client);

oi_status oi_llm_client_reconfigure(oi_llm_client *client,
                                    const struct oi_llm_config *cfg,
                                    int responses_api);

void oi_llm_client_set_keep_alive(oi_llm_client *client, int enabled);

typedef oi_status (*oi_llm_before_request_cb)(void *user_data);
void oi_llm_client_set_before_request(oi_llm_client *client,
                                      oi_llm_before_request_cb hook,
                                      void *user_data);

oi_status oi_llm_request_start(oi_llm_client *client, oi_reactor *reactor,
                                oi_arena *arena, const char *body,
                                size_t body_len, oi_llm_delta_cb on_delta,
                                oi_llm_done_cb on_done, void *user_data,
                                oi_llm_request **out_request);

oi_status oi_llm_request_start_events(
    oi_llm_client *client, oi_reactor *reactor, oi_arena *arena,
    const char *body, size_t body_len, oi_llm_event_cb on_event,
    oi_llm_done_cb on_done, void *user_data, oi_llm_request **out_request);

void oi_llm_request_cancel(oi_llm_request *req);

#endif
