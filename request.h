#ifndef TINYPROXY_REQUEST_H
#define TINYPROXY_REQUEST_H

#include <stddef.h>

#include "http.h"

// nginx's large_client_header_buffers 4 8k: each line fits in one buffer,
// and the head in four.
#define REQUEST_LINE_LIMIT 8192
#define HEADER_LINE_LIMIT 8192
#define REQUEST_HEAD_LIMIT 32768

typedef enum {
    HEAD_INCOMPLETE,
    HEAD_COMPLETE,
    HEAD_REJECTED,
} HeadState;

typedef struct {
    HeadState state;
    size_t length;  // HEAD_COMPLETE's: where the head ends.
    int status;     // HEAD_REJECTED's: what to answer.
} HeadScan;

// Checks a request head that may not be all in yet against the limits.
HeadScan scan_request_head(const char *data, size_t size);

typedef struct {
    char *line;  // The request line as it came, for the logs.
    char *head;  // A copy of the head, cut up into the strings below.
    const char *method;
    const char *target;
    const char *path;  // The target without any "http://host" before it.
    char *uri;         // nginx's $uri: the path normalised, without a query.
    const char *args;  // nginx's $args: the query after '?', or NULL.
    int version;
    char *host;  // $host: lowercased, without the port; "" if none.
    Header *headers;  // Only those with names nginx passes on.
    size_t header_count;
    size_t content_length;
    int expects_continue;
    // The client asked to keep the connection, by its version and
    // Connection.
    int keepalive;
} Request;

void request_init(Request *request);
void request_free(Request *request);

// Returns 0, or the status to answer with. request->line is set whenever it
// could be, so the log can show what was refused.
int request_parse(Request *request, const char *head, size_t length);

const char *request_find_header(const Request *request, const char *name);

#endif
