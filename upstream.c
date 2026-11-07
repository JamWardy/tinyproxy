#include "upstream.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "hash.h"

static int add_endpoint(Upstream *upstream, const struct sockaddr_in *address,
                    const Endpoint *options) {
    Endpoint *endpoints = realloc(upstream->endpoints,
                          (upstream->endpoint_count + 1) * sizeof(Endpoint));
    char text[INET_ADDRSTRLEN];

    if (!endpoints) {
        return 0;
    }
    upstream->endpoints = endpoints;
    Endpoint *endpoint = &endpoints[upstream->endpoint_count++];
    *endpoint = *options;
    endpoint->address = *address;
    inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    snprintf(endpoint->text, sizeof(endpoint->text), "%s:%d", text,
             ntohs(address->sin_port));
    return 1;
}

AddHost upstream_add_host(Upstream *upstream, const char *name,
                          unsigned short port, const Endpoint *options) {
    struct addrinfo hints;
    struct addrinfo *found;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(name, NULL, &hints, &found) != 0) {
        return HOST_UNKNOWN;
    }

    AddHost result = HOST_RESOLVED;
    for (const struct addrinfo *each = found; each; each = each->ai_next) {
        struct sockaddr_in address;

        memcpy(&address, each->ai_addr, sizeof(address));
        address.sin_port = htons(port);
        if (!add_endpoint(upstream, &address, options)) {
            result = HOST_OUT_OF_MEMORY;
            break;
        }
    }
    freeaddrinfo(found);
    return result;
}

void upstream_free(Upstream *upstream) {
    free(upstream->name);
    free(upstream->endpoints);
    free(upstream);
}

// ---- Choosing ----

// How many times a hash may land on an endpoint it can't use before falling
// back to round robin, as nginx's.
#define HASH_TRIES 20

typedef struct {
    long score;     // Smooth round robin's.
    size_t active;  // Requests sent to it and not yet done.
} EndpointLoad;

typedef struct {
    const Upstream *upstream;
    EndpointLoad *loads;  // In the same order as upstream->endpoints.
    size_t total_weight;  // Of every endpoint, down or not, as nginx's.
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
        balancer->loads =
            calloc(upstream->endpoint_count, sizeof(EndpointLoad));
        if (!balancer->loads) {
            balancers_free();
            return 0;
        }
        for (size_t j = 0; j < upstream->endpoint_count; j++) {
            balancer->total_weight += upstream->endpoints[j].weight;
        }
        if (upstream->method.kind == BALANCE_HASH_CONSISTENT &&
            !ring_build(&balancer->ring, upstream)) {
            balancers_free();
            return 0;
        }
    }
    return 1;
}

void balancers_free(void) {
    for (size_t i = 0; i < balancer_count; i++) {
        free(balancers[i].loads);
        ring_free(&balancers[i].ring);
    }
    free(balancers);
    balancers = NULL;
    balancer_count = 0;
}

static EndpointLoad *load_of(Balancer *balancer, const Endpoint *endpoint) {
    return &balancer->loads[endpoint - balancer->upstream->endpoints];
}

static int usable(Balancer *balancer, const Endpoint *endpoint) {
    return !endpoint->down &&
           (endpoint->max_conns == 0 ||
            load_of(balancer, endpoint)->active < endpoint->max_conns);
}

// Whether a and b have as many active requests for their weights.
static int tied(Balancer *balancer, const Endpoint *a, const Endpoint *b) {
    return load_of(balancer, a)->active * b->weight ==
           load_of(balancer, b)->active * a->weight;
}

// Smooth weighted round robin among the usable endpoints, only those tied
// with tied_with if it isn't NULL, and only those from host if it isn't
// NULL.
static const Endpoint *round_robin(Balancer *balancer,
                                   const Endpoint *tied_with,
                                   const char *host) {
    const Upstream *upstream = balancer->upstream;
    const Endpoint *best = NULL;
    long total = 0;

    for (size_t i = 0; i < upstream->endpoint_count; i++) {
        const Endpoint *endpoint = &upstream->endpoints[i];
        EndpointLoad *load = load_of(balancer, endpoint);

        if (!usable(balancer, endpoint) ||
            (tied_with && !tied(balancer, endpoint, tied_with)) ||
            (host && strcmp(endpoint->host, host) != 0)) {
            continue;
        }
        load->score += (long)endpoint->weight;
        total += (long)endpoint->weight;
        if (!best || load->score > load_of(balancer, best)->score) {
            best = endpoint;
        }
    }
    if (best) {
        load_of(balancer, best)->score -= total;
    }
    return best;
}

static const Endpoint *least_conn(Balancer *balancer) {
    const Upstream *upstream = balancer->upstream;
    const Endpoint *best = NULL;
    int many = 0;

    for (size_t i = 0; i < upstream->endpoint_count; i++) {
        const Endpoint *endpoint = &upstream->endpoints[i];

        if (!usable(balancer, endpoint)) {
            continue;
        }
        if (!best || load_of(balancer, endpoint)->active * best->weight <
                         load_of(balancer, best)->active * endpoint->weight) {
            best = endpoint;
            many = 0;
        } else if (tied(balancer, endpoint, best)) {
            many = 1;
        }
    }
    return many ? round_robin(balancer, best, NULL) : best;
}

// The endpoint w falls on, counting each endpoint's weight in turn.
static const Endpoint *endpoint_at_weight(const Upstream *upstream, size_t w) {
    size_t i = 0;

    while (w >= upstream->endpoints[i].weight) {
        w -= upstream->endpoints[i].weight;
        i++;
    }
    return &upstream->endpoints[i];
}

static const Endpoint *ip_hash(Balancer *balancer, struct in_addr client) {
    unsigned char address[4];
    unsigned hash = 89;

    memcpy(address, &client.s_addr, sizeof(address));
    for (size_t tries = 0; tries <= HASH_TRIES; tries++) {
        hash = ip_hash_next(hash, address);
        const Endpoint *endpoint = endpoint_at_weight(
            balancer->upstream, hash % balancer->total_weight);
        if (usable(balancer, endpoint)) {
            return endpoint;
        }
    }
    return round_robin(balancer, NULL, NULL);
}

static const Endpoint *key_hash(Balancer *balancer, const char *key,
                            size_t length) {
    unsigned hash = 0;

    for (size_t tries = 0; tries <= HASH_TRIES; tries++) {
        hash = key_hash_next(hash, tries, key, length);
        const Endpoint *endpoint = endpoint_at_weight(
            balancer->upstream, hash % balancer->total_weight);
        if (usable(balancer, endpoint)) {
            return endpoint;
        }
    }
    return round_robin(balancer, NULL, NULL);
}

// The key's point on the ring, then each next point's host in turn, and
// among that host's endpoints, round robin.
static const Endpoint *consistent_hash(Balancer *balancer, const char *key,
                                   size_t length) {
    const Ring *ring = &balancer->ring;
    size_t at = ring_find(ring, crc32(key, length));

    for (size_t tries = 0; tries <= HASH_TRIES; tries++, at++) {
        const char *host = ring->points[at % ring->count].host;
        const Endpoint *endpoint = round_robin(balancer, NULL, host);

        if (endpoint) {
            return endpoint;
        }
    }
    return round_robin(balancer, NULL, NULL);
}

static const Endpoint *pick(Balancer *balancer, struct in_addr client,
                        const char *key, size_t key_length) {
    const Upstream *upstream = balancer->upstream;
    // With one endpoint, or an empty key, there's nothing to hash, as nginx.
    int hashes = upstream->endpoint_count > 1;

    switch (upstream->method.kind) {
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

const Endpoint *choose_endpoint(const Upstream *upstream,
                                struct in_addr client, const char *key,
                                size_t key_length) {
    Balancer *balancer = &balancers[upstream->index];
    const Endpoint *endpoint = pick(balancer, client, key, key_length);

    if (endpoint) {
        load_of(balancer, endpoint)->active++;
    }
    return endpoint;
}

void release_endpoint(const Upstream *upstream, const Endpoint *endpoint) {
    load_of(&balancers[upstream->index], endpoint)->active--;
}
