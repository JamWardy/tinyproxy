#ifndef TINYPROXY_ROUTE_H
#define TINYPROXY_ROUTE_H

#include "config.h"

typedef enum {
    ROUTE_NONE,      // No location matches: 404.
    ROUTE_LOCATION,
    ROUTE_REDIRECT,  // To the location's path, which is path with a '/'.
} RouteKind;

typedef struct {
    RouteKind kind;
    const Location *location;  // Borrowed from the config; NULL for none.
} Route;

// Which of the group's servers takes host, nginx's $host, as nginx picks: an
// exact name, then the longest leading wildcard, then the longest trailing
// one, then the group's default.
const Server *find_server(const AddressGroup *group, const char *host);

// Which of server's locations takes the normalised path: an exact match, then
// the redirect for a missing trailing '/', then the longest prefix.
Route find_route(const Server *server, const char *path);

#endif
