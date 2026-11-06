#ifndef TINYPROXY_UPSTREAM_H
#define TINYPROXY_UPSTREAM_H

#include <netinet/in.h>
#include <stddef.h>

// One server in a pool, nginx's peer.
typedef struct {
    struct sockaddr_in address;
    char text[32];  // "127.0.0.1:8003", for the error log.
} Peer;

typedef enum {
    UPSTREAM_BLOCK,     // An "upstream name { ... }" block.
    UPSTREAM_IMPLICIT,  // Made for a proxy_pass to an address, as nginx does.
} UpstreamKind;

// A pool of servers, nginx's upstream.
typedef struct {
    UpstreamKind kind;
    char *name;  // The block's name, or the host and port.
    size_t line;
    Peer *peers;
    size_t peer_count;
} Upstream;

typedef enum {
    PEERS_ADDED,
    PEERS_NOT_FOUND,
    PEERS_OUT_OF_MEMORY,
} AddPeers;

// Adds a server for each IPv4 address host resolves to, once, at startup.
AddPeers upstream_add_peers(Upstream *upstream, const char *host,
                            unsigned short port);

void upstream_free(Upstream *upstream);

// Which server the next request goes to. Always the first until stage 6.
const Peer *choose_peer(const Upstream *upstream);

#endif
