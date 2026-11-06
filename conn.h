#ifndef TINYPROXY_CONN_H
#define TINYPROXY_CONN_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>

#include "buffer.h"
#include "chunked.h"
#include "config.h"
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
    CONN_SENDING_ERROR,      // tinyproxy's own reply.
    CONN_LINGERING,          // Reading and dropping what the client still
                             // sends, so it gets the reply before the close.
} ConnState;

typedef enum {
    REPLY_BODY_AS_IS,
    REPLY_BODY_DECODED,  // From chunks, for a client that can't read them.
} ReplyBody;

typedef struct {
    ConnState state;
    unsigned long long number;  // nginx's *N in the error log.
    int client_fd;
    int upstream_fd;            // From CONN_CONNECTING to CONN_READING_REPLY.
    char remote_addr[INET_ADDRSTRLEN];

    // Borrowed from the config. The settings are the server's until the
    // request has a location, then the location's.
    const Server *server;
    const Location *location;  // NULL until then.
    const Settings *settings;

    Buffer from_client;  // The request's head, then its body.
    size_t head_length;  // Of the head in from_client, once it's all in.
    Request request;
    Buffer upstream_uri;  // With a NUL after it, once there's a location.
    Buffer to_upstream;
    Buffer from_upstream;  // What's come from the upstream, until it's used.
    Buffer to_client;
    ReplyBody reply_body;
    ChunkDecoder chunks;  // REPLY_BODY_DECODED's.

    int status;                // What the access log shows.
    size_t reply_head_length;  // So the log can count only the body.
    size_t sent;               // Bytes sent to the client.

    double accepted_at;
    // When each side last moved, or when there was first something to send.
    double client_io_at;
    double upstream_io_at;
    double lingering_since;  // CONN_LINGERING's.
} Connection;

int set_non_blocking(int fd);

void conn_start(Connection *conn, int fd, const struct sockaddr_in *peer,
                const Server *server, double now);

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
