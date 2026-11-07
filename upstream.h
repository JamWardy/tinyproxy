#ifndef TINYPROXY_UPSTREAM_H
#define TINYPROXY_UPSTREAM_H

#include <netinet/in.h>
#include <stddef.h>

// One server in a pool, nginx's peer: one address of a "server".
typedef struct {
    struct sockaddr_in address;
    char text[32];       // "127.0.0.1:8003", for the error log.
    const char *server;  // As written in its "server", for hash consistent.
    size_t weight;
    size_t max_conns;  // 0 for no limit.
    int down;
} Peer;

typedef enum {
    UPSTREAM_BLOCK,     // An "upstream name { ... }" block.
    UPSTREAM_IMPLICIT,  // Made for a proxy_pass to an address, as nginx does.
} UpstreamKind;

// How its idle connections are kept, from the upstream block.
typedef struct {
    size_t connections;  // keepalive's N; 0, with none, keeps none.
    size_t requests;     // keepalive_requests.
    double time;         // keepalive_time.
    double timeout;      // keepalive_timeout.
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
} Balance;

// A pool of servers, nginx's upstream.
typedef struct {
    UpstreamKind kind;
    char *name;  // The block's name, or the host and port.
    size_t line;
    size_t number;  // Its place in Config.upstreams.
    Peer *peers;
    size_t peer_count;
    Balance balance;
    UpstreamKeepalive keepalive;
} Upstream;

typedef enum {
    PEERS_ADDED,
    PEERS_NOT_FOUND,
    PEERS_OUT_OF_MEMORY,
} AddPeers;

// Adds a server for each IPv4 address host resolves to, once, at startup,
// with server as written and the server's options.
AddPeers upstream_add_peers(Upstream *upstream, const char *host,
                            unsigned short port, const Peer *options);

void upstream_free(Upstream *upstream);

// ---- Choosing ----

// Each pool's counts and scores, which change as requests come. Fails only
// when out of memory.
int balancers_init(Upstream *const *upstreams, size_t count);
void balancers_free(void);

// The server for the next request, by the pool's method, counted as active
// until release_peer. client is the client's address, for ip_hash, and key
// the hash's key with its variables filled. NULL if none can be used.
const Peer *choose_peer(const Upstream *upstream, struct in_addr client,
                        const char *key, size_t key_length);

void release_peer(const Upstream *upstream, const Peer *peer);

#endif
