#include "route.h"

#include <string.h>

// ---- The server ----

static int ends_with(const char *text, const char *suffix) {
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix);

    return length >= suffix_length &&
           strcmp(text + length - suffix_length, suffix) == 0;
}

// How much of host a leading wildcard matches, or 0 if it doesn't.
static size_t leading_match(const ServerName *name, const char *host) {
    switch (name->kind) {
        case NAME_SUBDOMAINS: {
            const char *suffix = name->name + 1;  // ".shop.local"
            return strlen(host) > strlen(suffix) && ends_with(host, suffix)
                       ? strlen(suffix)
                       : 0;
        }
        case NAME_DOMAIN_AND_SUBDOMAINS:
            return ends_with(host, name->name) ||
                           strcmp(host, name->name + 1) == 0
                       ? strlen(name->name)
                       : 0;
        case NAME_EXACT:
        case NAME_PREFIX:
            return 0;
    }
    return 0;
}

// How much of host a trailing wildcard matches, or 0 if it doesn't.
static size_t trailing_match(const ServerName *name, const char *host) {
    size_t prefix_length = strlen(name->name) - 1;  // "mail." of "mail.*"

    return name->kind == NAME_PREFIX && strlen(host) > prefix_length &&
                   strncmp(host, name->name, prefix_length) == 0
               ? prefix_length
               : 0;
}

typedef size_t (*NameMatch)(const ServerName *name, const char *host);

// The server whose name matches the most of host, the first on a tie.
static const Server *longest_match(const AddressGroup *group,
                                   const char *host, NameMatch match) {
    const Server *best = NULL;
    size_t best_length = 0;

    for (size_t i = 0; i < group->server_count; i++) {
        const Server *server = group->servers[i];

        for (size_t j = 0; j < server->name_count; j++) {
            size_t length = match(&server->names[j], host);
            if (length > best_length) {
                best = server;
                best_length = length;
            }
        }
    }
    return best;
}

const Server *find_server(const AddressGroup *group, const char *host) {
    for (size_t i = 0; i < group->server_count; i++) {
        const Server *server = group->servers[i];

        for (size_t j = 0; j < server->name_count; j++) {
            if (server->names[j].kind == NAME_EXACT &&
                strcmp(server->names[j].name, host) == 0) {
                return server;
            }
        }
    }
    const Server *server = longest_match(group, host, leading_match);
    if (!server) {
        server = longest_match(group, host, trailing_match);
    }
    return server ? server : group->default_server;
}

// ---- The location ----

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
