#ifndef TINYPROXY_URI_H
#define TINYPROXY_URI_H

#include "buffer.h"

// nginx's $uri, from a path that may have a query after it: %XX decoded,
// "//" merged, and "." and ".." resolved. Returns 0, or 400 for a bad escape,
// a %00 or a climb above "/", or 500 when out of memory.
int normalise_uri(const char *path, char **uri);

// Appends uri with what can't go in a request line escaped as %XX, as
// nginx's ngx_escape_uri does. Fails only when out of memory.
int append_escaped_uri(Buffer *out, const char *uri);

#endif
