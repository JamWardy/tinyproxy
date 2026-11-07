#include "keepalive.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    const Peer *peer;
    UpstreamConn conn;
    double idle_since;
} IdleConn;

typedef struct {
    const Upstream *upstream;
    IdleConn *idle;  // Least recently kept first.
    size_t count;
} Pool;

// One for each upstream, in Config.upstreams' order.
static Pool *pools = NULL;
static size_t pool_count = 0;

int keepalive_init(const Config *config) {
    pools = calloc(config->upstream_count ? config->upstream_count : 1,
                   sizeof(Pool));
    if (!pools) {
        return 0;
    }
    pool_count = config->upstream_count;
    for (size_t i = 0; i < pool_count; i++) {
        const Upstream *upstream = config->upstreams[i];
        size_t size = upstream->keepalive.connections;

        pools[i].upstream = upstream;
        if (size > 0) {
            pools[i].idle = malloc(size * sizeof(IdleConn));
            if (!pools[i].idle) {
                keepalive_free();
                return 0;
            }
        }
    }
    return 1;
}

void keepalive_free(void) {
    for (size_t i = 0; i < pool_count; i++) {
        for (size_t j = 0; j < pools[i].count; j++) {
            close(pools[i].idle[j].conn.fd);
        }
        free(pools[i].idle);
    }
    free(pools);
    pools = NULL;
    pool_count = 0;
}

static void remove_idle(Pool *pool, size_t index) {
    memmove(&pool->idle[index], &pool->idle[index + 1],
            (pool->count - index - 1) * sizeof(IdleConn));
    pool->count--;
}

static void close_idle(Pool *pool, size_t index) {
    close(pool->idle[index].conn.fd);
    remove_idle(pool, index);
}

int keepalive_take(const Upstream *upstream, const Peer *peer,
                   UpstreamConn *conn) {
    Pool *pool = &pools[upstream->number];

    for (size_t i = pool->count; i-- > 0;) {
        if (pool->idle[i].peer == peer) {
            *conn = pool->idle[i].conn;
            remove_idle(pool, i);
            return 1;
        }
    }
    return 0;
}

void keepalive_keep(const Upstream *upstream, const Peer *peer,
                    const UpstreamConn *conn, double now) {
    Pool *pool = &pools[upstream->number];
    const UpstreamKeepalive *limits = &upstream->keepalive;

    if (limits->connections == 0 || conn->requests >= limits->requests ||
        now - conn->opened_at > limits->time) {
        close(conn->fd);
        return;
    }
    if (pool->count == limits->connections) {
        close_idle(pool, 0);
    }
    IdleConn *idle = &pool->idle[pool->count++];
    idle->peer = peer;
    idle->conn = *conn;
    idle->idle_since = now;
}

void keepalive_watch(fd_set *reads, int *highest) {
    for (size_t i = 0; i < pool_count; i++) {
        for (size_t j = 0; j < pools[i].count; j++) {
            int fd = pools[i].idle[j].conn.fd;

            FD_SET(fd, reads);
            if (fd > *highest) {
                *highest = fd;
            }
        }
    }
}

void keepalive_handle_ready(const fd_set *reads) {
    for (size_t i = 0; i < pool_count; i++) {
        Pool *pool = &pools[i];

        for (size_t j = pool->count; j-- > 0;) {
            int fd = pool->idle[j].conn.fd;
            char byte;

            if (!FD_ISSET(fd, reads)) {
                continue;
            }
            // Peeked, as the set may be from before this descriptor was
            // kept, when its reply was still coming.
            ssize_t count = recv(fd, &byte, 1, MSG_PEEK);
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                              errno == EINTR)) {
                continue;
            }
            close_idle(pool, j);
        }
    }
}

int keepalive_deadline(double *at) {
    int found = 0;

    for (size_t i = 0; i < pool_count; i++) {
        double timeout = pools[i].upstream->keepalive.timeout;

        for (size_t j = 0; j < pools[i].count; j++) {
            double due = pools[i].idle[j].idle_since + timeout;

            if (!found || due < *at) {
                *at = due;
                found = 1;
            }
        }
    }
    return found;
}

void keepalive_expire(double now) {
    for (size_t i = 0; i < pool_count; i++) {
        Pool *pool = &pools[i];
        double timeout = pool->upstream->keepalive.timeout;

        for (size_t j = pool->count; j-- > 0;) {
            if (now >= pool->idle[j].idle_since + timeout) {
                close_idle(pool, j);
            }
        }
    }
}
