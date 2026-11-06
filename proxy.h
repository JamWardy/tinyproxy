#ifndef TINYPROXY_PROXY_H
#define TINYPROXY_PROXY_H

// The request as it goes upstream, and the reply as it comes back.

#include <stddef.h>

#include "buffer.h"
#include "config.h"
#include "http.h"
#include "request.h"
#include "variables.h"

// nginx's proxy_buffer_size on Linux, one page.
#define REPLY_HEAD_LIMIT 4096

// The URI sent upstream. With a path in proxy_pass, it's that path in place
// of the part of $uri the location matched, then the query; without one,
// it's the URI as the client sent it. Fails only when out of memory.
int build_upstream_uri(Buffer *out, const Request *request,
                       const Location *location);

// The request as it goes upstream, for uri: proxy_headers first, with their
// variables filled in and any that come out empty left off, then the
// client's headers that none of them replace, then the body. Every variable
// in proxy_headers must already have been checked. Fails only when out of
// memory.
int build_upstream_request(Buffer *out, const Request *request,
                           const char *uri, int version,
                           const Headers *proxy_headers,
                           const Variables *variables, const char *body);

// The length of the reply's head in data, or 0 if it isn't all in yet.
size_t find_reply_head(const char *data, size_t size);

typedef struct {
    int status;
    // A chunked body for a client older than HTTP/1.1, which can't read
    // chunks, so they're decoded and the body ends at the close.
    int decode_chunks;
} ReplyHead;

// The upstream's reply head as a client of client_version gets it, with
// tinyproxy's own Server, Date and Connection. Returns 0 and fills reply, or
// else the status to answer with instead.
int rewrite_reply_head(Buffer *out, const char *head, size_t length,
                       int client_version, ReplyHead *reply);

#endif
