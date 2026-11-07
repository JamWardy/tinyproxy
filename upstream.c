#include "upstream.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "hash.h"

static int add_peer(Upstream *upstream, const struct sockaddr_in *address,
                    const Peer *options) {
    Peer *peers = realloc(upstream->peers,
                          (upstream->peer_count + 1) * sizeof(Peer));
    char text[INET_ADDRSTRLEN];

    if (!peers) {
        return 0;
    }
    upstream->peers = peers;
    Peer *peer = &peers[upstream->peer_count++];
    *peer = *options;
    peer->address = *address;
    inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    snprintf(peer->text, sizeof(peer->text), "%s:%d", text,
             ntohs(address->sin_port));
    return 1;
}

AddPeers upstream_add_peers(Upstream *upstream, const char *host,
                            unsigned short port, const Peer *options) {
    struct addrinfo hints;
    struct addrinfo *found;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &found) != 0) {
        return PEERS_NOT_FOUND;
    }

    AddPeers result = PEERS_ADDED;
    for (const struct addrinfo *each = found; each; each = each->ai_next) {
        struct sockaddr_in address;

        memcpy(&address, each->ai_addr, sizeof(address));
        address.sin_port = htons(port);
        if (!add_peer(upstream, &address, options)) {
            result = PEERS_OUT_OF_MEMORY;
            break;
        }
    }
    freeaddrinfo(found);
    return result;
}

void upstream_free(Upstream *upstream) {
    free(upstream->name);
    free(upstream->peers);
    free(upstream);
}

// ---- Choosing ----

// How many times a hash may land on a server it can't use before falling
// back to round robin, as nginx's.
#define HASH_TRIES 20

typedef struct {
    long current;  // Smooth round robin's score.
    size_t conns;  // Active connections.
} PeerState;

typedef struct {
    const Upstream *upstream;
    PeerState *peers;
    size_t total_weight;  // Of every server, down or not, as nginx's.
    Ring ring;            // BALANCE_HASH_CONSISTENT's.
} Balancer;

// One for each upstream, in Config.upstreams' order.
static Balancer *balancers = NULL;
static size_t balancer_count = 0;

int balancers_init(Upstream *const *upstreams, size_t count) {
    balancers = calloc(count ? count : 1, sizeof(Balancer));
    if (!balancers) {
        return 0;
    }
    balancer_count = count;
    for (size_t i = 0; i < count; i++) {
        const Upstream *upstream = upstreams[i];
        Balancer *balancer = &balancers[i];

        balancer->upstream = upstream;
        balancer->peers = calloc(upstream->peer_count, sizeof(PeerState));
        if (!balancer->peers) {
            balancers_free();
            return 0;
        }
        for (size_t j = 0; j < upstream->peer_count; j++) {
            balancer->total_weight += upstream->peers[j].weight;
        }
        if (upstream->balance.kind == BALANCE_HASH_CONSISTENT &&
            !ring_build(&balancer->ring, upstream)) {
            balancers_free();
            return 0;
        }
    }
    return 1;
}

void balancers_free(void) {
    for (size_t i = 0; i < balancer_count; i++) {
        free(balancers[i].peers);
        ring_free(&balancers[i].ring);
    }
    free(balancers);
    balancers = NULL;
    balancer_count = 0;
}

static PeerState *state_of(Balancer *balancer, const Peer *peer) {
    return &balancer->peers[peer - balancer->upstream->peers];
}

static int usable(Balancer *balancer, const Peer *peer) {
    return !peer->down && (peer->max_conns == 0 ||
                           state_of(balancer, peer)->conns < peer->max_conns);
}

// Whether a and b have as many connections for their weights.
static int tied(Balancer *balancer, const Peer *a, const Peer *b) {
    return state_of(balancer, a)->conns * b->weight ==
           state_of(balancer, b)->conns * a->weight;
}

// Smooth weighted round robin among the usable servers, only those tied
// with tied_with if it isn't NULL, and only those written as server if it
// isn't NULL.
static const Peer *round_robin(Balancer *balancer, const Peer *tied_with,
                               const char *server) {
    const Upstream *upstream = balancer->upstream;
    const Peer *best = NULL;
    long total = 0;

    for (size_t i = 0; i < upstream->peer_count; i++) {
        const Peer *peer = &upstream->peers[i];
        PeerState *state = state_of(balancer, peer);

        if (!usable(balancer, peer) ||
            (tied_with && !tied(balancer, peer, tied_with)) ||
            (server && strcmp(peer->server, server) != 0)) {
            continue;
        }
        state->current += (long)peer->weight;
        total += (long)peer->weight;
        if (!best || state->current > state_of(balancer, best)->current) {
            best = peer;
        }
    }
    if (best) {
        state_of(balancer, best)->current -= total;
    }
    return best;
}

static const Peer *least_conn(Balancer *balancer) {
    const Upstream *upstream = balancer->upstream;
    const Peer *best = NULL;
    int many = 0;

    for (size_t i = 0; i < upstream->peer_count; i++) {
        const Peer *peer = &upstream->peers[i];

        if (!usable(balancer, peer)) {
            continue;
        }
        if (!best || state_of(balancer, peer)->conns * best->weight <
                         state_of(balancer, best)->conns * peer->weight) {
            best = peer;
            many = 0;
        } else if (tied(balancer, peer, best)) {
            many = 1;
        }
    }
    return many ? round_robin(balancer, best, NULL) : best;
}

// The server w falls on, counting each server's weight in turn.
static const Peer *by_weight(const Upstream *upstream, size_t w) {
    size_t i = 0;

    while (w >= upstream->peers[i].weight) {
        w -= upstream->peers[i].weight;
        i++;
    }
    return &upstream->peers[i];
}

static const Peer *ip_hash(Balancer *balancer, struct in_addr client) {
    unsigned char address[4];
    unsigned hash = 89;

    memcpy(address, &client.s_addr, sizeof(address));
    for (size_t tries = 0; tries <= HASH_TRIES; tries++) {
        hash = ip_hash_next(hash, address);
        const Peer *peer = by_weight(balancer->upstream,
                                     hash % balancer->total_weight);
        if (usable(balancer, peer)) {
            return peer;
        }
    }
    return round_robin(balancer, NULL, NULL);
}

static const Peer *key_hash(Balancer *balancer, const char *key,
                            size_t length) {
    unsigned hash = 0;

    for (size_t tries = 0; tries <= HASH_TRIES; tries++) {
        hash = key_hash_next(hash, tries, key, length);
        const Peer *peer = by_weight(balancer->upstream,
                                     hash % balancer->total_weight);
        if (usable(balancer, peer)) {
            return peer;
        }
    }
    return round_robin(balancer, NULL, NULL);
}

// The key's point on the ring, then each next point's server in turn, and
// among that server's addresses, round robin.
static const Peer *consistent_hash(Balancer *balancer, const char *key,
                                   size_t length) {
    const Ring *ring = &balancer->ring;
    size_t at = ring_find(ring, crc32(key, length));

    for (size_t tries = 0; tries <= HASH_TRIES; tries++, at++) {
        const char *server = ring->points[at % ring->count].server;
        const Peer *peer = round_robin(balancer, NULL, server);

        if (peer) {
            return peer;
        }
    }
    return round_robin(balancer, NULL, NULL);
}

static const Peer *pick(Balancer *balancer, struct in_addr client,
                        const char *key, size_t key_length) {
    const Upstream *upstream = balancer->upstream;
    // With one server, or an empty key, there's nothing to hash, as nginx.
    int hashes = upstream->peer_count > 1;

    switch (upstream->balance.kind) {
        case BALANCE_ROUND_ROBIN:
            break;
        case BALANCE_LEAST_CONN:
            return least_conn(balancer);
        case BALANCE_IP_HASH:
            if (hashes) {
                return ip_hash(balancer, client);
            }
            break;
        case BALANCE_HASH:
            if (hashes && key_length > 0) {
                return key_hash(balancer, key, key_length);
            }
            break;
        case BALANCE_HASH_CONSISTENT:
            if (hashes && key_length > 0) {
                return consistent_hash(balancer, key, key_length);
            }
            break;
    }
    return round_robin(balancer, NULL, NULL);
}

const Peer *choose_peer(const Upstream *upstream, struct in_addr client,
                        const char *key, size_t key_length) {
    Balancer *balancer = &balancers[upstream->number];
    const Peer *peer = pick(balancer, client, key, key_length);

    if (peer) {
        state_of(balancer, peer)->conns++;
    }
    return peer;
}

void release_peer(const Upstream *upstream, const Peer *peer) {
    state_of(&balancers[upstream->number], peer)->conns--;
}
