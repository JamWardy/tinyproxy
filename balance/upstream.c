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
    long score;            // Smooth round robin's.
    long effective_weight; // Its weight, lowered by failures.
    size_t active;         // Requests sent to it and not yet done.
    size_t fails;
    double failed_at;      // The last failure.
    // Its last failure, or when it was first chosen after fail_timeout had
    // passed since then: a success after that shows it's working again.
    double checked_at;
} EndpointLoad;

typedef struct {
    const Upstream *upstream;
    EndpointLoad *loads;   // In the same order as upstream->endpoints.
    size_t total_weight;   // Of every endpoint but backups, down or not.
    int single;            // Its only endpoint is never skipped.
    int has_backups;
    Ring ring;             // BALANCE_HASH_CONSISTENT's.
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
            const Endpoint *endpoint = &upstream->endpoints[j];

            balancer->loads[j].effective_weight = (long)endpoint->weight;
            if (endpoint->backup) {
                balancer->has_backups = 1;
            } else {
                balancer->total_weight += endpoint->weight;
            }
        }
        balancer->single = upstream->endpoint_count == 1;
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

// It has failed max_fails times, and fail_timeout hasn't yet passed since
// the last failure or test.
static int skipped(const Endpoint *endpoint, const EndpointLoad *load,
                   double now) {
    return endpoint->max_fails > 0 && load->fails >= endpoint->max_fails &&
           now - load->checked_at <= endpoint->fail_timeout;
}

static int usable(Balancer *balancer, const Endpoint *endpoint, double now) {
    const EndpointLoad *load = load_of(balancer, endpoint);

    return !endpoint->down &&
           (endpoint->max_conns == 0 || load->active < endpoint->max_conns) &&
           !skipped(endpoint, load, now);
}

// Whether a and b have as many active requests for their weights.
static int tied(Balancer *balancer, const Endpoint *a, const Endpoint *b) {
    return load_of(balancer, a)->active * b->weight ==
           load_of(balancer, b)->active * a->weight;
}

// Which endpoints round robin chooses among, of those that are usable.
typedef struct {
    int backups;                // The backups, or else all the others.
    const Endpoint *tied_with;  // Only those tied with it, if not NULL.
    const char *host;           // Only those from it, if not NULL.
} Among;

// Smooth weighted round robin, by each endpoint's effective weight.
static const Endpoint *round_robin(Balancer *balancer, Among among,
                                   double now) {
    const Upstream *upstream = balancer->upstream;
    const Endpoint *best = NULL;
    long total = 0;

    for (size_t i = 0; i < upstream->endpoint_count; i++) {
        const Endpoint *endpoint = &upstream->endpoints[i];
        EndpointLoad *load = load_of(balancer, endpoint);

        if (endpoint->backup != among.backups ||
            !usable(balancer, endpoint, now) ||
            (among.tied_with && !tied(balancer, endpoint, among.tied_with)) ||
            (among.host && strcmp(endpoint->host, among.host) != 0)) {
            continue;
        }
        load->score += load->effective_weight;
        total += load->effective_weight;
        if (load->effective_weight < (long)endpoint->weight) {
            load->effective_weight++;
        }
        if (!best || load->score > load_of(balancer, best)->score) {
            best = endpoint;
        }
    }
    if (best) {
        load_of(balancer, best)->score -= total;
    }
    return best;
}

static const Endpoint *least_conn(Balancer *balancer, int backups,
                                  double now) {
    const Upstream *upstream = balancer->upstream;
    const Endpoint *best = NULL;
    int many = 0;

    for (size_t i = 0; i < upstream->endpoint_count; i++) {
        const Endpoint *endpoint = &upstream->endpoints[i];

        if (endpoint->backup != backups || !usable(balancer, endpoint, now)) {
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
    if (!many) {
        return best;
    }
    Among tied_ones = {backups, best, NULL};
    return round_robin(balancer, tied_ones, now);
}

static const Endpoint *any_but_backups(Balancer *balancer, double now) {
    Among others = {0, NULL, NULL};
    return round_robin(balancer, others, now);
}

// The endpoint w falls on, counting each endpoint's weight in turn. Hash
// methods have no backups, so every endpoint counts.
static const Endpoint *endpoint_at_weight(const Upstream *upstream,
                                          size_t w) {
    size_t i = 0;

    while (w >= upstream->endpoints[i].weight) {
        w -= upstream->endpoints[i].weight;
        i++;
    }
    return &upstream->endpoints[i];
}

static const Endpoint *ip_hash(Balancer *balancer, struct in_addr client,
                               double now) {
    unsigned char address[4];
    unsigned hash = 89;

    memcpy(address, &client.s_addr, sizeof(address));
    for (size_t tries = 0; tries <= HASH_TRIES; tries++) {
        hash = ip_hash_next(hash, address);
        const Endpoint *endpoint = endpoint_at_weight(
            balancer->upstream, hash % balancer->total_weight);
        if (usable(balancer, endpoint, now)) {
            return endpoint;
        }
    }
    return any_but_backups(balancer, now);
}

static const Endpoint *key_hash(Balancer *balancer, const char *key,
                                size_t length, double now) {
    unsigned hash = 0;

    for (size_t tries = 0; tries <= HASH_TRIES; tries++) {
        hash = key_hash_next(hash, tries, key, length);
        const Endpoint *endpoint = endpoint_at_weight(
            balancer->upstream, hash % balancer->total_weight);
        if (usable(balancer, endpoint, now)) {
            return endpoint;
        }
    }
    return any_but_backups(balancer, now);
}

// The key's point on the ring, then each next point's host in turn, and
// among that host's endpoints, round robin.
static const Endpoint *consistent_hash(Balancer *balancer, const char *key,
                                       size_t length, double now) {
    const Ring *ring = &balancer->ring;
    size_t at = ring_find(ring, crc32(key, length));

    for (size_t tries = 0; tries <= HASH_TRIES; tries++, at++) {
        Among from_host = {0, NULL, ring->points[at % ring->count].host};
        const Endpoint *endpoint = round_robin(balancer, from_host, now);

        if (endpoint) {
            return endpoint;
        }
    }
    return any_but_backups(balancer, now);
}

// By the pool's method, among every endpoint but the backups.
static const Endpoint *pick(Balancer *balancer, struct in_addr client,
                            const char *key, size_t key_length,
                            double now) {
    const Upstream *upstream = balancer->upstream;
    // With one endpoint, or an empty key, there's nothing to hash, as nginx.
    int hashes = upstream->endpoint_count > 1;

    switch (upstream->method.kind) {
        case BALANCE_ROUND_ROBIN:
            break;
        case BALANCE_LEAST_CONN:
            return least_conn(balancer, 0, now);
        case BALANCE_IP_HASH:
            if (hashes) {
                return ip_hash(balancer, client, now);
            }
            break;
        case BALANCE_HASH:
            if (hashes && key_length > 0) {
                return key_hash(balancer, key, key_length, now);
            }
            break;
        case BALANCE_HASH_CONSISTENT:
            if (hashes && key_length > 0) {
                return consistent_hash(balancer, key, key_length, now);
            }
            break;
    }
    return any_but_backups(balancer, now);
}

static const Endpoint *pick_backup(Balancer *balancer, double now) {
    if (balancer->upstream->method.kind == BALANCE_LEAST_CONN) {
        return least_conn(balancer, 1, now);
    }
    Among backups = {1, NULL, NULL};
    return round_robin(balancer, backups, now);
}

static void use_endpoint(const Upstream *upstream, const Endpoint *endpoint,
                  double now) {
    EndpointLoad *load = load_of(&balancers[upstream->index], endpoint);

    load->active++;
    if (now - load->checked_at > endpoint->fail_timeout) {
        load->checked_at = now;
    }
}

const Endpoint *choose_endpoint(const Upstream *upstream,
                                struct in_addr client, const char *key,
                                size_t key_length, double now) {
    Balancer *balancer = &balancers[upstream->index];
    const Endpoint *endpoint = pick(balancer, client, key, key_length, now);

    if (!endpoint && balancer->has_backups) {
        endpoint = pick_backup(balancer, now);
    }
    if (!endpoint) {
        // As nginx: so the next request tries every one again at once.
        for (size_t i = 0; i < upstream->endpoint_count; i++) {
            balancer->loads[i].fails = 0;
        }
        return NULL;
    }
    use_endpoint(upstream, endpoint, now);
    return endpoint;
}

void release_endpoint(const Upstream *upstream, const Endpoint *endpoint) {
    EndpointLoad *load = load_of(&balancers[upstream->index], endpoint);

    load->active--;
    if (load->failed_at < load->checked_at) {
        load->fails = 0;
    }
}

int endpoint_failed(const Upstream *upstream, const Endpoint *endpoint,
                    double now) {
    Balancer *balancer = &balancers[upstream->index];
    EndpointLoad *load = load_of(balancer, endpoint);

    load->active--;
    if (balancer->single) {
        return 0;
    }
    load->fails++;
    load->failed_at = now;
    load->checked_at = now;
    if (endpoint->max_fails == 0) {
        return 0;
    }
    load->effective_weight -= (long)(endpoint->weight / endpoint->max_fails);
    if (load->effective_weight < 0) {
        load->effective_weight = 0;
    }
    return load->fails >= endpoint->max_fails;
}
