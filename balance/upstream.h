#ifndef TINYPROXY_UPSTREAM_H
#define TINYPROXY_UPSTREAM_H

#include <netinet/in.h>
#include <stddef.h>

// One address and port a request can be sent to: one of the addresses its
// "host" line's name resolved to (nginx's peer). The balancer picks these.
typedef struct {
    struct sockaddr_in address;
    char text[32];     // "127.0.0.1:8003", for the error log.
    const char *host;  // Its "host" line's name and port, as written.
    size_t weight;
    size_t max_conns;  // 0 for no limit.
    int down;
} Endpoint;

typedef enum {
    UPSTREAM_BLOCK,     // An "upstream name { ... }" block.
    UPSTREAM_IMPLICIT,  // Made for a proxy_pass to an address, as nginx does.
} UpstreamKind;

// How its idle connections are kept, from the upstream block.
typedef struct {
    size_t max_idle;  // 0 keeps none.
    size_t max_requests;
    double lifetime;
    double idle_timeout;
} UpstreamKeepalive;

typedef enum {
    BALANCE_ROUND_ROBIN,
    BALANCE_LEAST_CONN,
    BALANCE_IP_HASH,
    BALANCE_HASH,
    BALANCE_HASH_CONSISTENT,
} BalanceKind;

typedef struct {
    BalanceKind kind;
    const char *key;  // The hashes': the key, with its variables unfilled.
} BalanceMethod;

// A pool of endpoints that all serve the same thing.
typedef struct {
    UpstreamKind kind;
    char *name;  // The block's name, or the host and port.
    size_t line;
    size_t index;  // Its place in Config.upstreams.
    Endpoint *endpoints;
    size_t endpoint_count;
    BalanceMethod method;
    UpstreamKeepalive keepalive;
} Upstream;

typedef enum {
    HOST_RESOLVED,
    HOST_UNKNOWN,
    HOST_OUT_OF_MEMORY,
} AddHost;

// Adds an endpoint for each IPv4 address name resolves to, once, at
// startup, each a copy of options with that address.
AddHost upstream_add_host(Upstream *upstream, const char *name,
                          unsigned short port, const Endpoint *options);

void upstream_free(Upstream *upstream);

// ---- Choosing ----

// Each pool's counts and scores, which change as requests come. Fails only
// when out of memory.
int balancers_init(Upstream *const *upstreams, size_t count);
void balancers_free(void);

// The endpoint for the next request, by the pool's method, counted as
// active until release_endpoint. client is the client's address, for
// ip_hash, and key the hash's key with its variables filled. NULL if none
// can be used.
const Endpoint *choose_endpoint(const Upstream *upstream,
                                struct in_addr client, const char *key,
                                size_t key_length);

void release_endpoint(const Upstream *upstream, const Endpoint *endpoint);

#endif
