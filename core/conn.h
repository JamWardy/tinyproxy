#ifndef TINYPROXY_CONN_H
#define TINYPROXY_CONN_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>

#include "buffer.h"
#include "chunked.h"
#include "config.h"
#include "keepalive.h"
#include "proxy.h"
#include "request.h"

// Each has a descriptor for the client and one upstream, and select can't
// watch any at or above FD_SETSIZE (often 1024).
#define MAX_CONNECTIONS 500

typedef enum {
    CONN_UNUSED,             // A free slot.
    CONN_READING_HEAD,
    CONN_READING_BODY,
    CONN_CONNECTING,         // Waiting for connect() to finish.
    CONN_SENDING_REQUEST,
    CONN_READING_REPLY_HEAD,
    CONN_READING_REPLY,      // Passing it on as it comes.
    CONN_SENDING_REPLY,      // The upstream's done; sending what's left.
    CONN_SENDING_OWN_REPLY,      // tinyproxy's own reply.
    CONN_LINGERING,          // Reading and dropping what the client still
                             // sends, so it gets the reply before the close.
    CONN_DISCARDING_BODY,    // Dropping the body of a request answered
                             // before it came, to keep the connection.
    CONN_IDLE,          // Waiting for the next request.
} ConnState;

typedef struct {
    ConnState state;
    unsigned long long id;  // nginx's *N in the error log.
    int client_fd;
    UpstreamConn upstream;  // From CONN_CONNECTING to CONN_READING_REPLY.
    struct in_addr remote_ip;  // For ip_hash.
    char remote_addr[INET_ADDRSTRLEN];
    size_t request_count;  // On this connection, the current one included.

    // Borrowed from the config. The server is the group's default until the
    // request's Host picks one. The settings are the server's until the
    // request has a location, then the location's.
    const AddressGroup *group;
    const Server *server;
    const Location *location;  // NULL until then.
    const Endpoint *endpoint;  // Where it's sent; NULL until chosen.
    const Settings *settings;

    // The request's head, then its body, then any requests sent after it.
    Buffer from_client;
    size_t head_length;  // Of the head in from_client, once it's all in.
    Request request;
    // The client sent the next request while the upstream had this one.
    // It's kept, and the client isn't read again until this one's done.
    int pipelined;
    size_t discard_left;  // CONN_DISCARDING_BODY's bytes still to come.
    Buffer upstream_uri;  // With a NUL after it, once there's a location.
    Buffer to_upstream;
    Buffer from_upstream;  // What's come from the upstream, until it's used.
    Buffer to_client;
    // Once its head is in. Its length counts down as the body comes.
    ReplyHead reply;
    BodyRewrite body_rewrite;
    ChunkDecoder chunks;  // REPLY_CHUNKED's.
    int keep_client;  // The reply keeps the client's connection.

    int status;                // What the access log shows.
    size_t reply_head_length;  // So the log can count only the body.
    size_t sent_to_client;     // In bytes, the head included.

    double accepted_at;
    double request_at;  // When the current request's head started.
    // When each side last moved, or when there was first something to send.
    double client_io_at;
    double upstream_io_at;
    // CONN_LINGERING's and CONN_DISCARDING_BODY's.
    double lingering_since;
} Connection;

int set_non_blocking(int fd);

void conn_start(Connection *conn, int fd, const struct sockaddr_in *client,
                const AddressGroup *group, double now);

// Adds the descriptors the connection is waiting on to the sets.
void conn_watch(const Connection *conn, fd_set *reads, fd_set *writes,
                int *highest);

void conn_handle_ready(Connection *conn, const fd_set *reads,
                       const fd_set *writes, double now);

// When the current state's time runs out. Fails if it has no deadline.
int conn_deadline(const Connection *conn, double *at);

// Acts on a deadline that has passed, if there is one.
void conn_expire(Connection *conn, double now);

// Closes it at once, logging nothing, as when tinyproxy stops.
void conn_close(Connection *conn);

#endif
