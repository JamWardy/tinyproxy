#ifndef TINYPROXY_ERROR_PAGE_H
#define TINYPROXY_ERROR_PAGE_H

#include "buffer.h"

// tinyproxy's own reply for status, with nginx's HTML page unless the request
// was a HEAD, and a Location header unless location is NULL. Fails only when
// out of memory.
int build_error_reply(Buffer *out, int status, int with_body,
                      const char *location);

#endif
