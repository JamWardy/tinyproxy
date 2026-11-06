#include "route.h"

#include <string.h>

static Route route(RouteKind kind, const Location *location) {
    Route found = {kind, location};
    return found;
}

static int is_prefix_of(const Location *location, const char *uri) {
    return strncmp(uri, location->path, strlen(location->path)) == 0;
}

// nginx's auto_redirect: location /api/ with a proxy_pass also answers /api,
// with a redirect to /api/, which it would otherwise never see.
static int redirects_from(const Location *location, const char *uri) {
    size_t length = strlen(location->path);

    return location->has_proxy_pass && length > 1 &&
           location->path[length - 1] == '/' && strlen(uri) == length - 1 &&
           strncmp(uri, location->path, length - 1) == 0;
}

Route find_route(const Server *server, const char *uri) {
    const Location *longest = NULL;
    const Location *redirect = NULL;

    for (size_t i = 0; i < server->location_count; i++) {
        const Location *location = &server->locations[i];

        switch (location->match) {
            case LOCATION_EXACT:
                if (strcmp(location->path, uri) == 0) {
                    return route(ROUTE_LOCATION, location);
                }
                break;
            case LOCATION_PREFIX:
                if (redirects_from(location, uri)) {
                    redirect = location;
                }
                if (is_prefix_of(location, uri) &&
                    (!longest ||
                     strlen(location->path) > strlen(longest->path))) {
                    longest = location;
                }
                break;
        }
    }

    if (redirect) {
        return route(ROUTE_REDIRECT, redirect);
    }
    return longest ? route(ROUTE_LOCATION, longest) : route(ROUTE_NONE, NULL);
}
