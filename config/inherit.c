#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

// nginx's own proxy headers, under any proxy_set_header.
static const Header DEFAULT_PROXY_HEADERS[] = {
    {"Host", "$proxy_host"},
    {"Connection", "close"},
    {"TE", ""},
    {"Keep-Alive", ""},
    {"Expect", ""},
    {"Upgrade", ""},
};

static double inherit_seconds(const OptionalSeconds *own, double outer) {
    return own->is_set ? own->seconds : outer;
}

static Settings inherit(const BlockSettings *own, const Settings *outer) {
    Settings settings;

    settings.client_header_timeout =
        inherit_seconds(&own->client_header_timeout,
                        outer->client_header_timeout);
    settings.client_body_timeout =
        inherit_seconds(&own->client_body_timeout, outer->client_body_timeout);
    settings.proxy_connect_timeout =
        inherit_seconds(&own->proxy_connect_timeout,
                        outer->proxy_connect_timeout);
    settings.proxy_send_timeout =
        inherit_seconds(&own->proxy_send_timeout, outer->proxy_send_timeout);
    settings.proxy_read_timeout =
        inherit_seconds(&own->proxy_read_timeout, outer->proxy_read_timeout);
    settings.client_send_timeout =
        inherit_seconds(&own->client_send_timeout, outer->client_send_timeout);
    settings.client_max_body_size = own->client_max_body_size.is_set
                                        ? own->client_max_body_size.bytes
                                        : outer->client_max_body_size;
    settings.proxy_http_version = own->proxy_http_version.is_set
                                      ? own->proxy_http_version.version
                                      : outer->proxy_http_version;
    if (own->keepalive_timeout.is_set) {
        settings.keepalive_timeout = own->keepalive_timeout.seconds;
        settings.keepalive_header_timeout = own->keepalive_timeout.header;
    } else {
        settings.keepalive_timeout = outer->keepalive_timeout;
        settings.keepalive_header_timeout = outer->keepalive_header_timeout;
    }
    settings.keepalive_max_requests = own->keepalive_max_requests.is_set
                                      ? own->keepalive_max_requests.count
                                      : outer->keepalive_max_requests;
    settings.keepalive_lifetime =
        inherit_seconds(&own->keepalive_lifetime, outer->keepalive_lifetime);
    settings.proxy_set_headers = own->proxy_set_headers.count > 0
                                     ? own->proxy_set_headers
                                     : outer->proxy_set_headers;
    settings.access_log = own->access_log.kind != ACCESS_LOG_UNSET
                              ? own->access_log
                              : outer->access_log;
    return settings;
}

static Settings default_settings(void) {
    Settings settings;

    settings.client_header_timeout = DEFAULT_TIMEOUT;
    settings.client_body_timeout = DEFAULT_TIMEOUT;
    settings.proxy_connect_timeout = DEFAULT_TIMEOUT;
    settings.proxy_send_timeout = DEFAULT_TIMEOUT;
    settings.proxy_read_timeout = DEFAULT_TIMEOUT;
    settings.client_send_timeout = DEFAULT_TIMEOUT;
    settings.client_max_body_size = DEFAULT_MAX_BODY_SIZE;
    settings.proxy_http_version = HTTP_1_0;
    settings.keepalive_timeout = DEFAULT_KEEPALIVE_TIMEOUT;
    settings.keepalive_header_timeout.is_set = 0;
    settings.keepalive_header_timeout.seconds = 0;
    settings.keepalive_max_requests = DEFAULT_KEEPALIVE_MAX_REQUESTS;
    settings.keepalive_lifetime = DEFAULT_KEEPALIVE_LIFETIME;
    settings.proxy_set_headers.items = NULL;
    settings.proxy_set_headers.count = 0;
    settings.access_log.kind = ACCESS_LOG_OFF;
    settings.access_log.fd = -1;
    return settings;
}

// proxy_set_header's headers, then nginx's defaults that none of them replace.
static int build_proxy_headers(Location *location) {
    const Headers *set = &location->settings.proxy_set_headers;
    size_t default_count =
        sizeof(DEFAULT_PROXY_HEADERS) / sizeof(DEFAULT_PROXY_HEADERS[0]);
    Header *items = malloc((set->count + default_count) * sizeof(Header));
    size_t count = 0;

    if (!items) {
        return 0;
    }
    for (size_t i = 0; i < set->count; i++) {
        items[count++] = set->items[i];
    }
    for (size_t i = 0; i < default_count; i++) {
        int replaced = 0;

        for (size_t j = 0; j < set->count; j++) {
            replaced |= strcasecmp(set->items[j].name,
                                   DEFAULT_PROXY_HEADERS[i].name) == 0;
        }
        if (!replaced) {
            items[count++] = DEFAULT_PROXY_HEADERS[i];
        }
    }
    location->proxy_headers.items = items;
    location->proxy_headers.count = count;
    return 1;
}

// Done once the whole file is read, so a directive after a block still
// reaches into it, as in nginx.
int inherit_all(Config *config) {
    Settings defaults = default_settings();
    Settings http = inherit(&config->http, &defaults);

    for (size_t i = 0; i < config->server_count; i++) {
        Server *server = &config->servers[i];

        server->settings = inherit(&server->written, &http);
        for (size_t j = 0; j < server->location_count; j++) {
            Location *location = &server->locations[j];

            location->settings = inherit(&location->written,
                                         &server->settings);
            if (!build_proxy_headers(location)) {
                return 0;
            }
        }
    }
    return 1;
}

// ---- Grouping by address and port ----

static ListenPort *find_or_add_port(Config *config, unsigned short number) {
    for (size_t i = 0; i < config->port_count; i++) {
        if (config->ports[i].port == number) {
            return &config->ports[i];
        }
    }
    ListenPort *ports = realloc(config->ports,
                                (config->port_count + 1) * sizeof(ListenPort));
    if (!ports) {
        return NULL;
    }
    config->ports = ports;
    ListenPort *port = &ports[config->port_count++];
    port->port = number;
    port->groups = NULL;
    port->group_count = 0;
    return port;
}

static AddressGroup *find_or_add_group(ListenPort *port,
                                       const Listen *listen) {
    for (size_t i = 0; i < port->group_count; i++) {
        if (port->groups[i].address.sin_addr.s_addr ==
            listen->address.sin_addr.s_addr) {
            return &port->groups[i];
        }
    }
    AddressGroup *groups = realloc(
        port->groups, (port->group_count + 1) * sizeof(AddressGroup));
    if (!groups) {
        return NULL;
    }
    port->groups = groups;
    AddressGroup *group = &groups[port->group_count++];
    memset(group, 0, sizeof(*group));
    group->address = listen->address;
    memcpy(group->text, listen->text, sizeof(group->text));
    return group;
}

// The group's servers so far came first, so they keep a name both have.
static void warn_name_conflicts(const AddressGroup *group,
                                const Server *server) {
    for (size_t i = 0; i < server->name_count; i++) {
        const char *name = server->names[i].name;
        int clash = 0;

        for (size_t j = 0; j < group->server_count && !clash; j++) {
            const Server *earlier = group->servers[j];

            for (size_t k = 0; k < earlier->name_count && !clash; k++) {
                clash = strcmp(earlier->names[k].name, name) == 0;
            }
        }
        if (clash) {
            fprintf(stderr, "tinyproxy: [warn] conflicting server name "
                            "\"%s\" on %s, ignored\n", name, group->text);
        }
    }
}

static int add_to_group(Parser *parser, const Server *server,
                        const Listen *listen) {
    Config *config = parser->config;
    ListenPort *port = find_or_add_port(config,
                                        ntohs(listen->address.sin_port));
    AddressGroup *group = port ? find_or_add_group(port, listen) : NULL;

    if (!group) {
        return parse_error(parser, listen->line, "out of memory");
    }
    if (listen->default_server) {
        if (group->default_server) {
            return parse_error(parser, listen->line,
                               "a duplicate default server for %s",
                               group->text);
        }
        group->default_server = server;
    }
    warn_name_conflicts(group, server);

    const Server **servers = realloc(
        group->servers, (group->server_count + 1) * sizeof(Server *));
    if (!servers) {
        return parse_error(parser, listen->line, "out of memory");
    }
    group->servers = servers;
    group->servers[group->server_count++] = server;
    return 1;
}

// Done once every server is read, as config->servers moves while it grows.
int group_servers(Parser *parser) {
    Config *config = parser->config;

    for (size_t i = 0; i < config->server_count; i++) {
        const Server *server = &config->servers[i];

        for (size_t j = 0; j < server->listen_count; j++) {
            if (!add_to_group(parser, server, &server->listens[j])) {
                return 0;
            }
        }
    }
    // With none marked default_server, a group's first server is.
    for (size_t i = 0; i < config->port_count; i++) {
        for (size_t j = 0; j < config->ports[i].group_count; j++) {
            AddressGroup *group = &config->ports[i].groups[j];

            if (!group->default_server) {
                group->default_server = group->servers[0];
            }
        }
    }
    return 1;
}
