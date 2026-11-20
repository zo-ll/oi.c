# llm_conn: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/llm_conn.c`, `src/llm_conn.h`, `test/test_llm_conn.c`.

## `OI_LLM_CONN_ADDRESS_TIMEOUT_MS` (src/llm_conn.c)

A connect() that has no answer after this time moves to the next address, so a dead route (often IPv6) does not use the full request deadline. The last address has no such timer: it gets the remainder of the request deadline.

## `resolver_job.conn` (src/llm_conn.c)

protected by mutex; NULL once abandoned

## `oi_llm_conn.addresses_copied` (src/llm_conn.c)

from copy_addresses, not from getaddrinfo

## `oi_llm_conn.cache` (src/llm_conn.c)

borrowed; NULL = resolve every time

## `oi_llm_conn.tls` (src/llm_conn.c)

borrowed; NULL = plain TCP

## `oi_llm_conn.destroyed_flag` (src/llm_conn.c)

Set by the top-level reactor callback before invoking any of *cbs, so a reentrant oi_llm_conn_close() from within a callback can signal back to still-executing internal code that `c` is gone. See the call-site pattern in fail_conn/read_available.

## `OI_LLM_ADDR_CACHE_MS` (src/llm_conn.c)

How long a resolved address list serves later connections.

## `copy_addresses` (src/llm_conn.c)

A deep copy that this module frees with free_addresses: getaddrinfo's own lists may only go to freeaddrinfo. ai_canonname is not kept.

## `cache_store` (src/llm_conn.c)

the cache is an optimization; this lookup still works

## `oi_llm_conn_close` (src/llm_conn.c)

One nonblocking close_notify. Without it, SSL_free marks the session as not resumable, and the next connection pays a full handshake.

## `oi_wipe` (src/llm_conn.c)

The request head holds the Authorization header.

## `set_tls_cause` (src/llm_conn.c)

Names the TLS failure: an untrusted certificate, an OpenSSL error, or the socket errno under SSL_ERROR_SYSCALL.

## `fail_conn` (src/llm_conn.c)

Invokes on_error, then closes `c` unless the callback already did so reentrantly. Callers must treat `c` as potentially freed once this returns.

## `nosigpipe_method` (src/llm_conn.c)

OpenSSL's socket BIO writes with write(), and a write to a closed peer raises SIGPIPE in the whole process. This BIO sends with MSG_NOSIGNAL, so SSL_write gets EPIPE and the caller's SIGPIPE disposition is never touched. The BIO data is the socket fd; the connection owns the fd.

## `tls_session_slot` (src/llm_conn.c)

The last session that a server issued on a context, for resumption. One slot: a client talks to one host.

## `remember_session` (src/llm_conn.c)

OpenSSL calls this when a server issues a session (for TLS 1.3, after the handshake). Returns 1 when the slot takes the reference.

## `SSL_CTX_set_verify` (src/llm_conn.c)

From here, SSL_CTX_free frees the slot through tls_slot_free.

## `conn_raw_read` (src/llm_conn.c)

Returns >0 bytes transferred, 0 on clean EOF (read only), -1 if it would block (*would_block=1), -2 on a hard error.

## `flush_write_queue` (src/llm_conn.c)

Returns OI_OK if `c` is still alive afterward (queue drained or still pending), or an error status if `c` was closed (by fail_conn) due to a write error.

## `connect_next_address` (src/llm_conn.c)

Starts a non-blocking connect to the next address that accepts one and registers it for OI_EV_WRITE.

Every cached address failed: resolve afresh next time.

## `handle_established` (src/llm_conn.c)

An error or hangup with no other bit leaves the descriptor ready. A read reports the error or the EOF and stops the connection.

## `on_fd_event` (src/llm_conn.c)

GCC's escape analysis for -Wdangling-pointer can't see that this is safe: c->destroyed_flag is always cleared before this frame returns (see below) unless `destroyed` was set, in which case `c` itself was freed and nothing ever dereferences the stale pointer again.

## `start_from_cache` (src/llm_conn.c)

A cache hit skips the resolver thread; the connect still starts from the reactor, so that every callback stays asynchronous.

## `oi_llm_conn_write.nb` (src/llm_conn.c)

Not realloc: a moved block would keep a copy of the Authorization header in freed memory.

## `oi_llm_conn` (src/llm_conn.h)

A single non-blocking TCP connection, optionally wrapped in TLS, multiplexed on the caller's reactor. Knows nothing about HTTP -- it's just a byte pipe with connect/data/error callbacks.

DNS resolution runs on a short-lived worker thread and reports its result back through the reactor. TCP connect, TLS handshake, reads, and writes are likewise non-blocking from the reactor thread's perspective.

## `oi_llm_conn.oi_llm_tls` (src/llm_conn.h)

TLS settings that many connections share: the trust store, and the SSL_CTX made from it at the first handshake. Sharing it saves loading the CA bundle for each connection and lets a later connection to the same host and port resume the last TLS session. `ca_file` NULL uses the system default trust store. A CA file that cannot be loaded fails the handshake that needs it, with a cause, and the next handshake tries again. A connection borrows its oi_llm_tls, so the oi_llm_tls must stay alive until that connection is connected or closed.

## `void` (src/llm_conn.h)

Fires on any terminal condition: peer close (OI_ERR_CLOSED) or a TCP/TLS error (OI_ERR_IO), including a failed connect or handshake. If the callback doesn't call oi_llm_conn_close itself, the connection closes itself right after the callback returns -- either way, `c` is invalid once this fires unless the callback is still executing.

## `oi_llm_addr_cache` (src/llm_conn.h)

The last address list resolved for one host and port, kept for 60 seconds, so that a later connection skips the resolver thread. A connection that cannot reach any cached address clears the cache. A connection borrows its cache, so the cache must stay alive until that connection is closed.

## `oi_llm_conn_connect` (src/llm_conn.h)

Begins asynchronous resolution of `host`, then a non-blocking connect, wrapping the connection in TLS with `tls` once the TCP handshake completes; `tls` NULL means plain TCP. Certificate verification is always on (hostname + chain); there is no way to disable it. `cache` NULL resolves `host` every time.

Returns the new connection via *out_conn immediately, before it's connected; `cbs->on_connected` (or `cbs->on_error` on failure) fires later from the reactor. `cbs` is copied; the struct it points to need not outlive this call.

## `oi_llm_conn_close` (src/llm_conn.h)

Deregisters from the reactor, tears down the socket/TLS state, and frees `c`. NULL-safe. Safe to call reentrantly from within one of `c`'s own callbacks (the conn machinery checks for this internally); not safe to call twice on the same pointer otherwise, per ordinary ownership rules.

## `oi_llm_conn_set_callbacks` (src/llm_conn.h)

Moves an established connection to a new owner: later callbacks go to `cbs` and `user_data`. Safe from inside one of the connection's own callbacks.

## `oi_llm_conn_error_cause` (src/llm_conn.h)

The reason for the failure that on_error reports, as one line of text: for example the getaddrinfo error, the connect errno, or the TLS certificate verify result. NULL when there is no specific reason. Valid only inside on_error.

## `start_echo_server` (test/test_llm_conn.c)

Starts a blocking echo server in a forked child, listening on loopback. Returns the child pid (caller must waitpid) and the port it bound to.

## `unused_port` (test/test_llm_conn.c)

A listening socket that never accepts, so connect() to it fills the kernel backlog behavior predictably is NOT what we want for a "connection refused" test -- instead we bind a socket, learn its port, and close it immediately so nothing is listening there.

## `start_write_then_close_server` (test/test_llm_conn.c)

Listens on an ephemeral loopback port, accepts one connection, writes `data` to it, then closes immediately -- for testing the "peer sent something and then hung up" path.

## `peer_close_reports_closed` (test/test_llm_conn.c)

fail_conn already closed `conn` via its fallback path, since half_on_error doesn't close it itself -- closing it again here would be a double free.

## `write_before_connected_rejected.st` (test/test_llm_conn.c)

Use a no-op connected callback for this test by overriding: skip writing in on_connected and instead try to write immediately, before the reactor has had a chance to establish the connection.

## `start_self_signed_tls_server` (test/test_llm_conn.c)

Serves `connections` TLS connections, one after another, with a fresh self-signed certificate, which the system trust store does not contain. With `ca_path`, it also writes the certificate there as PEM, so a client can trust it, and answers the first read before it closes the socket with no TLS close_notify: with an echo, or with `report_reuse`, with "1" when the client resumed a session and "0" when it did not.

## `sends_total` (test/test_llm_conn.c)

The test links with -Wl,--wrap=send and -Wl,--wrap=SSL_CTX_new. liboi's own sends come here; libssl's built-in socket BIO uses write() and never does.
