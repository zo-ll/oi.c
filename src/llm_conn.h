#ifndef OI_LLM_CONN_H
#define OI_LLM_CONN_H

#include <stddef.h>

#include "oi/reactor.h"
#include "oi/status.h"

typedef struct oi_llm_conn oi_llm_conn;

typedef struct oi_llm_tls oi_llm_tls;
oi_status oi_llm_tls_create(const char *ca_file, oi_llm_tls **out_tls);
void oi_llm_tls_destroy(oi_llm_tls *tls);

typedef void (*oi_llm_conn_connected_cb)(oi_llm_conn *c, void *user_data);
typedef void (*oi_llm_conn_data_cb)(oi_llm_conn *c, const void *data,
                                     size_t len, void *user_data);
typedef void (*oi_llm_conn_error_cb)(oi_llm_conn *c, oi_status reason,
                                      void *user_data);

struct oi_llm_conn_callbacks {
    oi_llm_conn_connected_cb on_connected;
    oi_llm_conn_data_cb on_data;
    oi_llm_conn_error_cb on_error;
};

typedef struct oi_llm_addr_cache oi_llm_addr_cache;
oi_status oi_llm_addr_cache_create(oi_llm_addr_cache **out_cache);
void oi_llm_addr_cache_destroy(oi_llm_addr_cache *cache);

oi_status oi_llm_conn_connect(oi_reactor *r, const char *host,
                               unsigned short port, oi_llm_tls *tls,
                               oi_llm_addr_cache *cache,
                               const struct oi_llm_conn_callbacks *cbs,
                               void *user_data, oi_llm_conn **out_conn);

oi_status oi_llm_conn_write(oi_llm_conn *c, const void *data, size_t len);

void oi_llm_conn_close(oi_llm_conn *c);

void oi_llm_conn_set_callbacks(oi_llm_conn *c,
                               const struct oi_llm_conn_callbacks *cbs,
                               void *user_data);

const char *oi_llm_conn_error_cause(const oi_llm_conn *c);

#endif
