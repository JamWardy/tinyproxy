#ifndef TINYPROXY_HASH_H
#define TINYPROXY_HASH_H

// The hashes behind ip_hash, hash and hash consistent, as nginx's.

#include <stddef.h>
#include <stdint.h>

#include "upstream.h"

// CRC-32 in steps, as nginx's ngx_crc32_init, _update and _final, so a
// part-done hash can be copied and carried on.
uint32_t crc32_init(void);
uint32_t crc32_update(uint32_t crc, const void *data, size_t size);
uint32_t crc32_final(uint32_t crc);
uint32_t crc32(const void *data, size_t size);

// ip_hash's next hash, from the last one (89 to start) and the first three
// bytes of the client's IPv4 address.
unsigned ip_hash_next(unsigned hash, const unsigned char address[4]);

// hash's next hash, from the last one (0 to start) and how many times it
// has hashed (0 to start): compatible with Cache::Memcached.
unsigned key_hash_next(unsigned hash, size_t rehash, const char *key,
                       size_t length);

// hash consistent's ring: points sorted by hash, each naming a server as
// written in its "server" directive.
typedef struct {
    uint32_t hash;
    const char *server;
} RingPoint;

typedef struct {
    RingPoint *points;
    size_t count;
} Ring;

// 160 points per unit of weight for each server, compatible with
// Cache::Memcached::Fast. Fails only when out of memory.
int ring_build(Ring *ring, const Upstream *upstream);
void ring_free(Ring *ring);

// The first point at or after hash, as an index to count on from.
size_t ring_find(const Ring *ring, uint32_t hash);

#endif
