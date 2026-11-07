#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

// Fails only when out of memory.
static Upstream *add_upstream(Config *config, UpstreamKind kind,
                              const char *name, size_t line) {
    Upstream **upstreams = realloc(
        config->upstreams, (config->upstream_count + 1) * sizeof(Upstream *));
    if (!upstreams) {
        return NULL;
    }
    config->upstreams = upstreams;

    Upstream *upstream = malloc(sizeof(Upstream));
    char *copy = copy_text(name, strlen(name));
    if (!upstream || !copy) {
        free(upstream);
        free(copy);
        return NULL;
    }
    upstream->kind = kind;
    upstream->name = copy;
    upstream->line = line;
    upstream->index = config->upstream_count;
    upstream->endpoints = NULL;
    upstream->endpoint_count = 0;
    upstream->method.kind = BALANCE_ROUND_ROBIN;
    upstream->method.key = NULL;
    upstream->keepalive.max_idle = 0;
    upstream->keepalive.max_requests = DEFAULT_KEEPALIVE_MAX_REQUESTS;
    upstream->keepalive.lifetime = DEFAULT_KEEPALIVE_LIFETIME;
    upstream->keepalive.idle_timeout = DEFAULT_UPSTREAM_KEEPALIVE_TIMEOUT;
    config->upstreams[config->upstream_count++] = upstream;
    return upstream;
}

// Names match ignoring case, as nginx's do.
static Upstream *find_upstream(const Config *config, UpstreamKind kind,
                               const char *name) {
    for (size_t i = 0; i < config->upstream_count; i++) {
        Upstream *upstream = config->upstreams[i];

        if (upstream->kind == kind && strcasecmp(upstream->name, name) == 0) {
            return upstream;
        }
    }
    return NULL;
}

int handle_upstream(Parser *parser, const Scope *scope,
                    const Directive *directive,
                    const DirectiveSpec *spec) {
    Config *config = parser->config;
    const char *name = directive->arguments[0];

    (void)scope;
    (void)spec;
    if (find_upstream(config, UPSTREAM_BLOCK, name)) {
        return parse_error(parser, directive->line,
                           "duplicate upstream \"%s\"", name);
    }
    Upstream *upstream = add_upstream(config, UPSTREAM_BLOCK, name,
                                      directive->line);
    if (!upstream) {
        return out_of_memory(parser, directive);
    }

    UpstreamSettings written;
    memset(&written, 0, sizeof(written));
    Scope inner = {CONTEXT_UPSTREAM, NULL, NULL, NULL, upstream, &written};
    if (!parse_block(parser, &inner)) {
        return 0;
    }
    if (upstream->endpoint_count == 0) {
        return parse_error(parser, directive->line,
                           "no hosts are inside upstream");
    }

    UpstreamKeepalive *keepalive = &upstream->keepalive;
    if (written.keepalive_max_idle.is_set &&
        written.last == WROTE_KEEPALIVE_LAST) {
        keepalive->max_idle = written.keepalive_max_idle.count;
    }
    if (written.keepalive_max_requests.is_set) {
        keepalive->max_requests = written.keepalive_max_requests.count;
    }
    if (written.keepalive_lifetime.is_set) {
        keepalive->lifetime = written.keepalive_lifetime.seconds;
    }
    if (written.keepalive_timeout.is_set) {
        keepalive->idle_timeout = written.keepalive_timeout.seconds;
    }
    return 1;
}

// weight=, max_conns= or down on a host, into options.
static int take_host_option(Parser *parser, const Directive *directive,
                            const char *option, Endpoint *options) {
    if (strncmp(option, "weight=", 7) == 0) {
        if (!parse_count(option + 7, &options->weight) ||
            options->weight == 0) {
            return invalid_parameter(parser, directive, option);
        }
        return 1;
    }
    if (strncmp(option, "max_conns=", 10) == 0) {
        if (!parse_count(option + 10, &options->max_conns)) {
            return invalid_parameter(parser, directive, option);
        }
        return 1;
    }
    if (strcmp(option, "down") == 0) {
        options->down = 1;
        return 1;
    }
    if (strncmp(option, "max_fails=", 10) == 0 ||
        strncmp(option, "fail_timeout=", 13) == 0 ||
        strcmp(option, "backup") == 0) {
        return parse_error(parser, directive->line,
                           "\"%.*s\" is not supported until stage 7",
                           (int)strcspn(option, "="), option);
    }
    return invalid_parameter(parser, directive, option);
}

// What a host's endpoints have unless its options say otherwise.
static Endpoint default_host_options(const char *host) {
    Endpoint options;

    memset(&options, 0, sizeof(options));
    options.host = host;
    options.weight = 1;
    return options;
}

// "host name:port;" in an upstream block.
int handle_host(Parser *parser, const Scope *scope,
                const Directive *directive,
                const DirectiveSpec *spec) {
    const char *address = directive->arguments[0];
    char name[256];
    unsigned short port = 80;

    Endpoint options = default_host_options(address);

    (void)spec;
    for (size_t i = 1; i < directive->argument_count; i++) {
        if (!take_host_option(parser, directive, directive->arguments[i],
                              &options)) {
            return 0;
        }
    }
    if (strlen(address) >= sizeof(name) ||
        !split_host_port(address, name, &port)) {
        return parse_error(parser, directive->line,
                           "invalid port in upstream \"%s\"", address);
    }
    switch (upstream_add_host(scope->upstream, name, port, &options)) {
        case HOST_RESOLVED:
            return 1;
        case HOST_UNKNOWN:
            return parse_error(parser, directive->line,
                               "host not found in upstream \"%s\"",
                               address);
        case HOST_OUT_OF_MEMORY:
            return out_of_memory(parser, directive);
    }
    return 0;
}

// As nginx: a second method, or one after keepalive_max_idle, which it
// turns off, is only a warning.
static void set_method(Parser *parser, const Scope *scope,
                       const Directive *directive, BalanceKind kind,
                       const char *key) {
    UpstreamSettings *written = scope->upstream_settings;

    if (written->last != WROTE_NEITHER) {
        fprintf(stderr, "tinyproxy: [warn] load balancing method redefined "
                        "in %s:%zu\n", parser->path, directive->line);
    }
    written->last = WROTE_METHOD_LAST;
    scope->upstream->method.kind = kind;
    scope->upstream->method.key = key;
}

// least_conn and ip_hash, which take no arguments.
int handle_plain_method(Parser *parser, const Scope *scope,
                        const Directive *directive,
                        const DirectiveSpec *spec) {
    (void)spec;
    set_method(parser, scope, directive,
               strcmp(directive->name, "least_conn") == 0
               ? BALANCE_LEAST_CONN
               : BALANCE_IP_HASH,
               NULL);
    return 1;
}

// "hash key;" or "hash key consistent;".
int handle_hash(Parser *parser, const Scope *scope,
                const Directive *directive,
                const DirectiveSpec *spec) {
    const char *key = directive->arguments[0];
    BalanceKind kind = BALANCE_HASH;

    (void)spec;
    if (directive->argument_count > 1) {
        if (strcmp(directive->arguments[1], "consistent") != 0) {
            return invalid_parameter(parser, directive,
                                     directive->arguments[1]);
        }
        kind = BALANCE_HASH_CONSISTENT;
    }
    if (!check_variables(parser, directive, key)) {
        return 0;
    }
    set_method(parser, scope, directive, kind, key);
    return 1;
}

// Points a proxy_pass at its pool: the upstream block of that name, or an
// unnamed one for the host and port, shared by every proxy_pass to them.
static int link_proxy_pass(Parser *parser, ProxyPass *pass) {
    Config *config = parser->config;
    char host[256];
    unsigned short port = 80;

    // handle_proxy_pass checked it splits.
    split_host_port(pass->host, host, &port);
    Upstream *upstream = find_upstream(config, UPSTREAM_BLOCK, host);
    if (upstream) {
        if (strchr(pass->host, ':')) {
            return parse_error(parser, pass->line,
                               "upstream \"%s\" may not have port %d",
                               host, port);
        }
        pass->upstream = upstream;
        return 1;
    }

    upstream = find_upstream(config, UPSTREAM_IMPLICIT, pass->host);
    if (!upstream) {
        upstream = add_upstream(config, UPSTREAM_IMPLICIT, pass->host,
                                pass->line);
        if (!upstream) {
            return parse_error(parser, pass->line, "out of memory");
        }
        Endpoint options = default_host_options(pass->host);

        switch (upstream_add_host(upstream, host, port, &options)) {
            case HOST_RESOLVED:
                break;
            case HOST_UNKNOWN:
                return parse_error(parser, pass->line,
                                   "host not found in upstream \"%s\"",
                                   pass->host);
            case HOST_OUT_OF_MEMORY:
                return parse_error(parser, pass->line, "out of memory");
        }
    }
    pass->upstream = upstream;
    return 1;
}

// Done once the whole file is read, as an upstream can come after its use.
int link_proxy_passes(Parser *parser) {
    Config *config = parser->config;

    for (size_t i = 0; i < config->server_count; i++) {
        Server *server = &config->servers[i];

        for (size_t j = 0; j < server->location_count; j++) {
            Location *location = &server->locations[j];

            if (location->has_proxy_pass &&
                !link_proxy_pass(parser, &location->proxy_pass)) {
                return 0;
            }
        }
    }
    return 1;
}
