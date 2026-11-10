#ifndef TINYPROXY_ERROR_PAGE_H
#define TINYPROXY_ERROR_PAGE_H

#include "buffer.h"
#include "http.h"

// One of tinyproxy's replies, before it's written: a Python hook's, from
// request.respond or error, or tinyproxy's own page.
typedef struct {
    int status;
    // nginx's own words for an own page's status, such as "408 Request
    // Time-out", or NULL for the status and its usual words.
    const char *status_line;
    Headers headers;  // Owning their names and values.
    Buffer body;
} HookReply;

void hook_reply_init(HookReply *reply);
void hook_reply_free(HookReply *reply);

// tinyproxy's own reply for status, with nginx's HTML page, and a Location
// header unless location is NULL. Fails only when out of memory.
int own_page_reply(HookReply *reply, int status, const char *location);

// The reply: its status line, its headers, then
// tinyproxy's own Server, Date and Content-Length, connection, the Connection
// header lines, and the body unless the request was a HEAD. Fails only when
// out of memory.
int build_hook_reply(Buffer *out, const HookReply *reply, int with_body,
                     const char *connection);

#endif
