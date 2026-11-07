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

// Where the reply's body ends.
typedef enum {
    REPLY_NO_BODY,      // To HEAD, and for 1xx, 204 and 304.
    REPLY_LENGTH,       // After Content-Length bytes.
    REPLY_CHUNKED,      // At the last chunk.
    REPLY_UNTIL_CLOSE,  // With neither, when the upstream closes.
} ReplyFraming;

typedef struct {
    int status;
    ReplyFraming framing;
    size_t length;  // REPLY_LENGTH's.
    // HTTP/1.1 without Connection: close, so the upstream connection may be
    // kept once the reply's in.
    int upstream_keeps;
} ReplyHead;

// Reads the upstream's reply head, for a request that was HEAD or not.
// Returns 0 and fills reply, or else the status to answer with instead.
int parse_reply_head(const char *head, size_t length, int to_head,
                     ReplyHead *reply);

// How the body goes on to the client.
typedef enum {
    REPLY_BODY_AS_IS,
    REPLY_BODY_DECODED,  // From chunks, for a client that can't read them.
    REPLY_BODY_ENCODED,  // Into chunks, as it has no length of its own.
} ReplyBody;

// A head that parse_reply_head took, as the client gets it: with
// tinyproxy's own Server and Date, the Transfer-Encoding body needs, and
// connection, the Connection header lines. Fails only when out of memory.
int write_reply_head(Buffer *out, const char *head, size_t length,
                     ReplyBody body, const char *connection);

#endif
