#ifndef OI_LLM_CONN_H
#define OI_LLM_CONN_H

#include <stddef.h>

#include "oi/reactor.h"
#include "oi/status.h"

/*
 * A single non-blocking TCP connection, optionally wrapped in TLS,
 * multiplexed on the caller's reactor. Knows nothing about HTTP -- it's
 * just a byte pipe with connect/data/error callbacks.
 *
 * DNS resolution runs on a short-lived worker thread and reports its result
 * back through the reactor. TCP connect, TLS handshake, reads, and writes
 * are likewise non-blocking from the reactor thread's perspective.
 */

typedef struct oi_llm_conn oi_llm_conn;

/*
 * TLS settings that many connections share: the trust store, and the
 * SSL_CTX made from it at the first handshake. Sharing it saves loading
 * the CA bundle for each connection and lets a later connection to the
 * same host and port resume the last TLS session. `ca_file` NULL uses the
 * system default trust store. A CA file that cannot be loaded fails the
 * handshake that needs it, with a cause, and the next handshake tries
 * again. A connection borrows its oi_llm_tls, so the oi_llm_tls must stay
 * alive until that connection is connected or closed.
 */
typedef struct oi_llm_tls oi_llm_tls;
oi_status oi_llm_tls_create(const char *ca_file, oi_llm_tls **out_tls);
void oi_llm_tls_destroy(oi_llm_tls *tls); /* NULL-safe */

typedef void (*oi_llm_conn_connected_cb)(oi_llm_conn *c, void *user_data);
typedef void (*oi_llm_conn_data_cb)(oi_llm_conn *c, const void *data,
                                     size_t len, void *user_data);
/* Fires on any terminal condition: peer close (OI_ERR_CLOSED) or a
 * TCP/TLS error (OI_ERR_IO), including a failed connect or handshake. If
 * the callback doesn't call oi_llm_conn_close itself, the connection
 * closes itself right after the callback returns -- either way, `c` is
 * invalid once this fires unless the callback is still executing. */
typedef void (*oi_llm_conn_error_cb)(oi_llm_conn *c, oi_status reason,
                                      void *user_data);

struct oi_llm_conn_callbacks {
    oi_llm_conn_connected_cb on_connected;
    oi_llm_conn_data_cb on_data;
    oi_llm_conn_error_cb on_error;
};

/*
 * Begins asynchronous resolution of `host`, then a non-blocking connect,
 * wrapping the connection in TLS with `tls` once the TCP handshake
 * completes; `tls` NULL means plain TCP. Certificate verification is
 * always on (hostname + chain); there is no way to disable it.
 *
 * Returns the new connection via *out_conn immediately, before it's
 * connected; `cbs->on_connected` (or `cbs->on_error` on failure) fires
 * later from the reactor. `cbs` is copied; the struct it points to need
 * not outlive this call.
 */
oi_status oi_llm_conn_connect(oi_reactor *r, const char *host,
                               unsigned short port, oi_llm_tls *tls,
                               const struct oi_llm_conn_callbacks *cbs,
                               void *user_data, oi_llm_conn **out_conn);

/*
 * Queues `len` bytes for writing and returns once queued, not once sent.
 * Only valid once `on_connected` has fired and before the connection is
 * closed; OI_ERR_INVAL otherwise. Not compacted between partial sends --
 * fine for a write-the-request-then-only-read access pattern, not
 * intended for long-lived bidirectional streaming.
 */
oi_status oi_llm_conn_write(oi_llm_conn *c, const void *data, size_t len);

/*
 * Deregisters from the reactor, tears down the socket/TLS state, and
 * frees `c`. NULL-safe. Safe to call reentrantly from within one of `c`'s
 * own callbacks (the conn machinery checks for this internally); not
 * safe to call twice on the same pointer otherwise, per ordinary
 * ownership rules.
 */
void oi_llm_conn_close(oi_llm_conn *c);

/*
 * The reason for the failure that on_error reports, as one line of text:
 * for example the getaddrinfo error, the connect errno, or the TLS
 * certificate verify result. NULL when there is no specific reason. Valid
 * only inside on_error.
 */
const char *oi_llm_conn_error_cause(const oi_llm_conn *c);

#endif /* OI_LLM_CONN_H */
