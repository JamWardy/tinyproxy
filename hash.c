#include "hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t crc32_init(void) {
    return 0xffffffffu;
}

// The reflected IEEE polynomial, a bit at a time, as nothing hashes much.
uint32_t crc32_update(uint32_t crc, const void *data, size_t size) {
    const unsigned char *bytes = data;

    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t crc32_final(uint32_t crc) {
    return crc ^ 0xffffffffu;
}

uint32_t crc32(const void *data, size_t size) {
    return crc32_final(crc32_update(crc32_init(), data, size));
}

unsigned ip_hash_next(unsigned hash, const unsigned char address[4]) {
    for (int i = 0; i < 3; i++) {
        hash = (hash * 113 + address[i]) % 6271;
    }
    return hash;
}

unsigned key_hash_next(unsigned hash, size_t rehash, const char *key,
                       size_t length) {
    uint32_t crc = crc32_init();

    // The tries after the first hash the try's number before the key.
    if (rehash > 0) {
        char number[24];
        int size = snprintf(number, sizeof(number), "%zu", rehash);
        crc = crc32_update(crc, number, (size_t)size);
    }
    crc = crc32_final(crc32_update(crc, key, length));
    return hash + ((crc >> 16) & 0x7fff);
}

// The host, a NUL, then the port: the digits after the last ':', or none.
static uint32_t server_hash(const char *server) {
    size_t host_length = strlen(server);
    const char *port = "";

    for (size_t i = host_length; i-- > 0;) {
        if (server[i] == ':') {
            host_length = i;
            port = server + i + 1;
            break;
        }
        if (server[i] < '0' || server[i] > '9') {
            break;
        }
    }
    uint32_t crc = crc32_update(crc32_init(), server, host_length);

    crc = crc32_update(crc, "", 1);
    return crc32_update(crc, port, strlen(port));
}

static int compare_points(const void *a, const void *b) {
    uint32_t x = ((const RingPoint *)a)->hash;
    uint32_t y = ((const RingPoint *)b)->hash;

    return x < y ? -1 : x > y;
}

int ring_build(Ring *ring, const Upstream *upstream) {
    size_t total = 0;

    for (size_t i = 0; i < upstream->peer_count; i++) {
        total += upstream->peers[i].weight * 160;
    }
    ring->points = malloc((total ? total : 1) * sizeof(RingPoint));
    ring->count = 0;
    if (!ring->points) {
        return 0;
    }

    for (size_t i = 0; i < upstream->peer_count; i++) {
        const Peer *peer = &upstream->peers[i];
        uint32_t base = server_hash(peer->server);
        uint32_t previous = 0;

        for (size_t j = 0; j < peer->weight * 160; j++) {
            // The last hash, little-endian, whatever the machine's order.
            unsigned char bytes[4] = {
                (unsigned char)previous, (unsigned char)(previous >> 8),
                (unsigned char)(previous >> 16),
                (unsigned char)(previous >> 24)};
            uint32_t hash = crc32_final(crc32_update(base, bytes, 4));

            ring->points[ring->count].hash = hash;
            ring->points[ring->count].server = peer->server;
            ring->count++;
            previous = hash;
        }
    }
    qsort(ring->points, ring->count, sizeof(RingPoint), compare_points);

    // Each address of one server gives the same points; one of each stays.
    size_t kept = 0;
    for (size_t i = 0; i < ring->count; i++) {
        if (kept == 0 || ring->points[kept - 1].hash != ring->points[i].hash) {
            ring->points[kept++] = ring->points[i];
        }
    }
    ring->count = kept;
    return 1;
}

void ring_free(Ring *ring) {
    free(ring->points);
    ring->points = NULL;
    ring->count = 0;
}

size_t ring_find(const Ring *ring, uint32_t hash) {
    size_t low = 0;
    size_t high = ring->count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;

        if (hash > ring->points[middle].hash) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}
