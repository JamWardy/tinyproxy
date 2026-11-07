#ifndef TINYPROXY_KEEPALIVE_H
#define TINYPROXY_KEEPALIVE_H

// Each pool's idle connections to its endpoints, kept for the next request
// (nginx's upstream keepalive).

#include <stddef.h>
#include <sys/select.h>

#include "config.h"

// A connection to an endpoint.
typedef struct {
    int fd;           // -1 for none.
    size_t requests;  // Sent on it, the current one included.
    double opened_at;
} UpstreamConn;

// Fails only when out of memory.
int keepalive_init(const Config *config);

// Closes every idle connection.
void keepalive_free(void);

// Takes the most recently kept idle connection to endpoint in upstream.
// Fails if there's none.
int keepalive_take(const Upstream *upstream, const Endpoint *endpoint,
                   UpstreamConn *conn);

// Keeps conn idle for the next request to endpoint, closing the pool's least
// recently used one if it's full. Closes conn instead if the pool keeps
// none, or conn has reached keepalive_max_requests or keepalive_lifetime.
void keepalive_keep(const Upstream *upstream, const Endpoint *endpoint,
                    const UpstreamConn *conn, double now);

void keepalive_watch(fd_set *reads, int *highest);

// Closes those that turned readable: the endpoint closed them, or sent what
// no request asked for.
void keepalive_handle_ready(const fd_set *reads);

// When the nearest idle connection's keepalive_timeout runs out. Fails if
// there are none.
int keepalive_deadline(double *at);

void keepalive_expire(double now);

#endif
