#ifndef TINYPROXY_ROUTE_H
#define TINYPROXY_ROUTE_H

#include "config.h"

typedef enum {
    ROUTE_NONE,      // No location matches: 404.
    ROUTE_LOCATION,
    ROUTE_REDIRECT,  // To the location's path, which is uri with a '/'.
} RouteKind;

typedef struct {
    RouteKind kind;
    const Location *location;  // Borrowed from the config; NULL for none.
} Route;

// Which of server's locations takes uri, nginx's $uri: an exact match, then
// the redirect for a missing trailing '/', then the longest prefix.
Route find_route(const Server *server, const char *uri);

#endif
