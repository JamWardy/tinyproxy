#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "internal.h"
#include "variables.h"

// Checks every $variable in a proxy_set_header value is one there is.
int check_variables(Parser *parser, const Directive *directive,
                    const char *value) {
    static const Variables NO_VALUES = {"", "", "", "", "", "", ""};

    for (const char *at = strchr(value, '$'); at; at = strchr(at, '$')) {
        const char *name;
        size_t length;
        size_t used = variable_reference(at, &name, &length);

        if (used == 0) {
            return parse_error(parser, directive->line,
                               "invalid variable name");
        }
        if (!variable_value(&NO_VALUES, name, length)) {
            return parse_error(parser, directive->line,
                               "unknown \"%.*s\" variable", (int)length,
                               name);
        }
        at += used;
    }
    return 1;
}

void block_settings_init(BlockSettings *settings) {
    memset(settings, 0, sizeof(*settings));
    settings->proxy_set_headers.items = NULL;
    settings->access_log.kind = ACCESS_LOG_UNSET;
}

// ---- http, server and location ----

static int handle_http(Parser *parser, const Scope *scope,
                       const Directive *directive,
                       const DirectiveSpec *spec) {
    Config *config = parser->config;
    Scope inner = {CONTEXT_HTTP, NULL, NULL, &config->http, NULL, NULL};

    (void)scope;
    (void)spec;
    if (config->has_http) {
        return duplicate(parser, directive);
    }
    config->has_http = 1;
    return parse_block(parser, &inner);
}

// nginx listens on port 80 when run as root, and 8000 otherwise.
static int add_default_listen(Server *server) {
    Listen *listen = malloc(sizeof(Listen));

    if (!listen) {
        return 0;
    }
    memset(listen, 0, sizeof(*listen));
    listen->address.sin_family = AF_INET;
    listen->address.sin_addr.s_addr = htonl(INADDR_ANY);
    listen->address.sin_port = htons(geteuid() == 0 ? 80 : 8000);
    snprintf(listen->text, sizeof(listen->text), "0.0.0.0:%d",
             geteuid() == 0 ? 80 : 8000);
    listen->line = server->line;
    server->listens = listen;
    server->listen_count = 1;
    return 1;
}

static int add_server_name(Server *server, ServerNameKind kind,
                           const char *name) {
    ServerName *names = realloc(server->names,
                                (server->name_count + 1) * sizeof(ServerName));

    if (!names) {
        return 0;
    }
    server->names = names;
    server->names[server->name_count].kind = kind;
    server->names[server->name_count].name = name;
    server->name_count++;
    return 1;
}

static int handle_server(Parser *parser, const Scope *scope,
                         const Directive *directive,
                         const DirectiveSpec *spec) {
    Config *config = parser->config;
    Server *servers = realloc(config->servers,
                              (config->server_count + 1) * sizeof(Server));

    (void)scope;
    (void)spec;
    if (!servers) {
        return out_of_memory(parser, directive);
    }
    config->servers = servers;
    // A server can't hold another, so this pointer lasts the whole block.
    Server *server = &servers[config->server_count++];
    memset(server, 0, sizeof(*server));
    server->line = directive->line;
    block_settings_init(&server->written);

    Scope inner = {CONTEXT_SERVER, server, NULL, &server->written, NULL,
                   NULL};
    if (!parse_block(parser, &inner)) {
        return 0;
    }
    // nginx's default server_name is "", which matches no Host.
    if ((server->listen_count == 0 && !add_default_listen(server)) ||
        (server->name_count == 0 &&
         !add_server_name(server, NAME_EXACT, ""))) {
        return out_of_memory(parser, directive);
    }
    return 1;
}

// Names are compared lowercased, as nginx's are.
static void lowercase(char *text) {
    for (char *c = text; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') {
            *c = (char)(*c - 'A' + 'a');
        }
    }
}

// Which kind of name text is, if it's one nginx takes.
static int classify_server_name(const char *text, ServerNameKind *kind) {
    const char *star = strchr(text, '*');
    size_t length = strlen(text);

    if (!star) {
        *kind = text[0] == '.' ? NAME_DOMAIN_AND_SUBDOMAINS : NAME_EXACT;
        return text[0] != '.' || length > 1;
    }
    if (strchr(star + 1, '*')) {
        return 0;
    }
    if (star == text && text[1] == '.' && length > 2) {
        *kind = NAME_SUBDOMAINS;
        return 1;
    }
    if (star == text + length - 1 && length > 2 && text[length - 2] == '.') {
        *kind = NAME_PREFIX;
        return 1;
    }
    return 0;
}

static int handle_server_name(Parser *parser, const Scope *scope,
                              const Directive *directive,
                              const DirectiveSpec *spec) {
    (void)spec;
    for (size_t i = 0; i < directive->argument_count; i++) {
        char *name = directive->arguments[i];
        ServerNameKind kind;

        if (name[0] == '~') {
            return parse_error(parser, directive->line,
                               "regex server names are not supported");
        }
        lowercase(name);
        if (!classify_server_name(name, &kind)) {
            return parse_error(parser, directive->line,
                               "invalid server name or wildcard \"%s\"",
                               name);
        }
        if (!add_server_name(scope->server, kind, name)) {
            return out_of_memory(parser, directive);
        }
    }
    return 1;
}

static int handle_location(Parser *parser, const Scope *scope,
                           const Directive *directive,
                           const DirectiveSpec *spec) {
    Server *server = scope->server;
    LocationMatch match = LOCATION_PREFIX;
    const char *path = directive->arguments[0];

    (void)spec;
    if (scope->context == CONTEXT_LOCATION) {
        return parse_error(parser, directive->line,
                           "a \"location\" inside another is not supported");
    }
    // With no regex locations, ^~ has none to stop, so it's a plain prefix.
    if (directive->argument_count == 2) {
        if (strcmp(directive->arguments[0], "=") == 0) {
            match = LOCATION_EXACT;
        } else if (strcmp(directive->arguments[0], "^~") != 0) {
            return parse_error(parser, directive->line,
                               "location modifier \"%s\" is not supported",
                               directive->arguments[0]);
        }
        path = directive->arguments[1];
    }
    for (size_t i = 0; i < server->location_count; i++) {
        if (server->locations[i].match == match &&
            strcmp(server->locations[i].path, path) == 0) {
            return parse_error(parser, directive->line,
                               "duplicate location \"%s\"", path);
        }
    }

    Location *locations = realloc(
        server->locations, (server->location_count + 1) * sizeof(Location));
    if (!locations) {
        return out_of_memory(parser, directive);
    }
    server->locations = locations;
    // Locations can't nest, so no more are added while this block is read.
    Location *location = &locations[server->location_count++];
    memset(location, 0, sizeof(*location));
    location->match = match;
    location->path = path;
    block_settings_init(&location->written);

    Scope inner = {CONTEXT_LOCATION, server, location, &location->written,
                   NULL, NULL};
    return parse_block(parser, &inner);
}

// "8000", "127.0.0.1:8000", "*:8000" or "localhost", as nginx's listen, and
// perhaps "default_server" after it.
static int handle_listen(Parser *parser, const Scope *scope,
                         const Directive *directive,
                         const DirectiveSpec *spec) {
    const char *text = directive->arguments[0];
    Server *server = scope->server;
    char host[256];
    unsigned short port = 80;
    Listen listen;

    (void)spec;
    memset(&listen, 0, sizeof(listen));
    listen.line = directive->line;
    if (directive->argument_count > 1) {
        if (strcmp(directive->arguments[1], "default_server") != 0) {
            return parse_error(parser, directive->line,
                               "invalid parameter \"%s\"",
                               directive->arguments[1]);
        }
        listen.default_server = 1;
    }
    if (strlen(text) >= sizeof(host)) {
        return invalid_value(parser, directive);
    }
    if (parse_port(text, &port)) {
        strcpy(host, "*");
    } else if (!split_host_port(text, host, &port)) {
        return parse_error(parser, directive->line,
                           "invalid port in \"%s\" of the \"listen\" "
                           "directive", text);
    }

    if (strcmp(host, "*") == 0) {
        listen.address.sin_family = AF_INET;
        listen.address.sin_addr.s_addr = htonl(INADDR_ANY);
        listen.address.sin_port = htons(port);
    } else if (!resolve(host, port, &listen.address)) {
        return parse_error(parser, directive->line,
                           "host not found in \"%s\" of the \"listen\" "
                           "directive", text);
    }
    char address[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &listen.address.sin_addr, address, sizeof(address));
    snprintf(listen.text, sizeof(listen.text), "%s:%d", address, port);

    for (size_t i = 0; i < server->listen_count; i++) {
        if (strcmp(server->listens[i].text, listen.text) == 0) {
            return parse_error(parser, directive->line,
                               "a duplicate listen %s", listen.text);
        }
    }
    Listen *listens = realloc(server->listens,
                              (server->listen_count + 1) * sizeof(Listen));
    if (!listens) {
        return out_of_memory(parser, directive);
    }
    server->listens = listens;
    server->listens[server->listen_count++] = listen;
    return 1;
}

// "http://" and a pool's name or a host and port, and perhaps a path after
// it. Which it is waits until the whole file is read.
static int handle_proxy_pass(Parser *parser, const Scope *scope,
                             const Directive *directive,
                             const DirectiveSpec *spec) {
    Location *location = scope->location;
    const char *url = directive->arguments[0];
    char host[256];
    unsigned short port = 80;

    (void)spec;
    if (location->has_proxy_pass) {
        return duplicate(parser, directive);
    }
    if (strncasecmp(url, "http://", 7) != 0) {
        return parse_error(parser, directive->line,
                           "invalid URL prefix in \"%s\"", url);
    }
    const char *authority = url + 7;
    const char *uri = strchr(authority, '/');
    size_t length = uri ? (size_t)(uri - authority) : strlen(authority);
    if (length >= sizeof(host)) {
        return invalid_value(parser, directive);
    }
    // Owned by the location from here, so freed with it even on failure.
    location->proxy_pass.host = copy_text(authority, length);
    if (!location->proxy_pass.host) {
        return out_of_memory(parser, directive);
    }
    if (!split_host_port(location->proxy_pass.host, host, &port)) {
        return parse_error(parser, directive->line,
                           "invalid port in upstream \"%s\"",
                           location->proxy_pass.host);
    }
    if (host[0] == '\0') {
        return parse_error(parser, directive->line, "no host in upstream "
                                                    "\"%s\"", url);
    }
    location->proxy_pass.uri = uri;
    location->proxy_pass.line = directive->line;
    location->has_proxy_pass = 1;
    return 1;
}

// ---- Settings ----

static int handle_proxy_set_header(Parser *parser, const Scope *scope,
                                   const Directive *directive,
                                   const DirectiveSpec *spec) {
    Headers *headers = &scope->settings->proxy_set_headers;

    (void)spec;
    if (!check_variables(parser, directive, directive->arguments[1])) {
        return 0;
    }
    Header *items = realloc(headers->items,
                            (headers->count + 1) * sizeof(Header));
    if (!items) {
        return out_of_memory(parser, directive);
    }
    headers->items = items;
    headers->items[headers->count].name = directive->arguments[0];
    headers->items[headers->count].value = directive->arguments[1];
    headers->count++;
    return 1;
}

// "off", or a file and, optionally, "combined", the only format.
static int handle_access_log(Parser *parser, const Scope *scope,
                             const Directive *directive,
                             const DirectiveSpec *spec) {
    AccessLog *log = &scope->settings->access_log;
    const char *path = directive->arguments[0];

    (void)spec;
    if (log->kind != ACCESS_LOG_UNSET) {
        return duplicate(parser, directive);
    }
    if (strcmp(path, "off") == 0) {
        if (directive->argument_count > 1) {
            return parse_error(parser, directive->line,
                               "invalid parameter \"%s\"",
                               directive->arguments[1]);
        }
        log->kind = ACCESS_LOG_OFF;
        return 1;
    }
    if (directive->argument_count > 1 &&
        strcmp(directive->arguments[1], "combined") != 0) {
        return parse_error(parser, directive->line,
                           "unknown log format \"%s\"",
                           directive->arguments[1]);
    }

    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) {
        return parse_error(parser, directive->line,
                           "open() \"%s\" failed (%d: %s)", path, errno,
                           strerror(errno));
    }
    log->kind = ACCESS_LOG_FILE;
    log->fd = fd;
    return 1;
}

// Where spec's offset points into: the upstream's settings in an upstream
// block, or else the block's.
static void *setting_at(const Scope *scope, const DirectiveSpec *spec) {
    char *base = scope->context == CONTEXT_UPSTREAM
                     ? (char *)scope->upstream_settings
                     : (char *)scope->settings;
    return base + spec->offset;
}

static int handle_seconds(Parser *parser, const Scope *scope,
                          const Directive *directive,
                          const DirectiveSpec *spec) {
    OptionalSeconds *setting = setting_at(scope, spec);

    if (setting->is_set) {
        return duplicate(parser, directive);
    }
    if (!parse_seconds(directive->arguments[0], &setting->seconds)) {
        return invalid_value(parser, directive);
    }
    setting->is_set = 1;
    return 1;
}

// "1.0" or "1.1".
static int handle_http_version(Parser *parser, const Scope *scope,
                               const Directive *directive,
                               const DirectiveSpec *spec) {
    OptionalHttpVersion *setting = &scope->settings->proxy_http_version;
    const char *text = directive->arguments[0];

    (void)spec;
    if (setting->is_set) {
        return duplicate(parser, directive);
    }
    if (strcmp(text, "1.0") == 0) {
        setting->version = HTTP_1_0;
    } else if (strcmp(text, "1.1") == 0) {
        setting->version = HTTP_1_1;
    } else {
        return invalid_value(parser, directive);
    }
    setting->is_set = 1;
    return 1;
}

static int handle_size(Parser *parser, const Scope *scope,
                       const Directive *directive,
                       const DirectiveSpec *spec) {
    OptionalSize *setting = setting_at(scope, spec);

    if (setting->is_set) {
        return duplicate(parser, directive);
    }
    if (!parse_size(directive->arguments[0], &setting->bytes)) {
        return invalid_value(parser, directive);
    }
    setting->is_set = 1;
    return 1;
}

// A whole number. keepalive_max_idle's must be at least 1.
static int handle_count(Parser *parser, const Scope *scope,
                        const Directive *directive,
                        const DirectiveSpec *spec) {
    OptionalCount *setting = setting_at(scope, spec);
    int is_keepalive = strcmp(directive->name, "keepalive_max_idle") == 0;
    size_t count;

    if (setting->is_set) {
        return duplicate(parser, directive);
    }
    if (!parse_count(directive->arguments[0], &count) ||
        (is_keepalive && count == 0)) {
        return invalid_value(parser, directive);
    }
    if (is_keepalive) {
        scope->upstream_settings->last = WROTE_KEEPALIVE_LAST;
    }
    setting->count = count;
    setting->is_set = 1;
    return 1;
}

// "keepalive_timeout 75s;" or, with "Keep-Alive: timeout=60" in kept
// replies, "keepalive_timeout 75s 60s;".
static int handle_keepalive_timeout(Parser *parser, const Scope *scope,
                                    const Directive *directive,
                                    const DirectiveSpec *spec) {
    OptionalKeepaliveTimeout *setting = &scope->settings->keepalive_timeout;

    (void)spec;
    if (setting->is_set) {
        return duplicate(parser, directive);
    }
    if (!parse_seconds(directive->arguments[0], &setting->seconds)) {
        return invalid_value(parser, directive);
    }
    if (directive->argument_count > 1) {
        if (!parse_seconds(directive->arguments[1],
                           &setting->header.seconds)) {
            return invalid_value(parser, directive);
        }
        setting->header.is_set = 1;
    }
    setting->is_set = 1;
    return 1;
}

// ---- The table ----

#define SECONDS(name, contexts)                                             \
    {#name, contexts, 0, 1, 1, handle_seconds, offsetof(BlockSettings, name)}
#define UPSTREAM(name, handler)                                             \
    {#name, CONTEXT_UPSTREAM, 0, 1, 1, handler,                             \
     offsetof(UpstreamSettings, name)}

static const DirectiveSpec DIRECTIVES[] = {
    {"http", CONTEXT_MAIN, 1, 0, 0, handle_http, 0},
    {"server", CONTEXT_HTTP, 1, 0, 0, handle_server, 0},
    {"upstream", CONTEXT_HTTP, 1, 1, 1, handle_upstream, 0},
    {"host", CONTEXT_UPSTREAM, 0, 1, MAX_ARGUMENTS, handle_host, 0},
    {"listen", CONTEXT_SERVER, 0, 1, 2, handle_listen, 0},
    {"server_name", CONTEXT_SERVER, 0, 1, MAX_ARGUMENTS, handle_server_name,
     0},
    {"location", CONTEXT_SERVER | CONTEXT_LOCATION, 1, 1, 2, handle_location,
     0},
    {"proxy_pass", CONTEXT_LOCATION, 0, 1, 1, handle_proxy_pass, 0},
    {"proxy_set_header", ANY_HTTP_BLOCK, 0, 2, 2, handle_proxy_set_header, 0},
    {"access_log", ANY_HTTP_BLOCK, 0, 1, 2, handle_access_log, 0},
    {"client_max_body_size", ANY_HTTP_BLOCK, 0, 1, 1, handle_size,
     offsetof(BlockSettings, client_max_body_size)},
    {"proxy_http_version", ANY_HTTP_BLOCK, 0, 1, 1, handle_http_version, 0},
    SECONDS(client_header_timeout, CONTEXT_HTTP | CONTEXT_SERVER),
    SECONDS(client_body_timeout, ANY_HTTP_BLOCK),
    SECONDS(proxy_connect_timeout, ANY_HTTP_BLOCK),
    SECONDS(proxy_send_timeout, ANY_HTTP_BLOCK),
    SECONDS(proxy_read_timeout, ANY_HTTP_BLOCK),
    SECONDS(client_send_timeout, ANY_HTTP_BLOCK),
    {"keepalive_timeout", ANY_HTTP_BLOCK, 0, 1, 2, handle_keepalive_timeout,
     0},
    {"keepalive_max_requests", ANY_HTTP_BLOCK, 0, 1, 1, handle_count,
     offsetof(BlockSettings, keepalive_max_requests)},
    SECONDS(keepalive_lifetime, ANY_HTTP_BLOCK),
    UPSTREAM(keepalive_max_idle, handle_count),
    {"least_conn", CONTEXT_UPSTREAM, 0, 0, 0, handle_plain_method, 0},
    {"ip_hash", CONTEXT_UPSTREAM, 0, 0, 0, handle_plain_method, 0},
    {"hash", CONTEXT_UPSTREAM, 0, 1, 2, handle_hash, 0},
    UPSTREAM(keepalive_timeout, handle_seconds),
    UPSTREAM(keepalive_max_requests, handle_count),
    UPSTREAM(keepalive_lifetime, handle_seconds),
};

// The spec for name in context, or else any for name, for the "not allowed
// here" error.
static const DirectiveSpec *find_directive(const char *name,
                                           Context context) {
    const DirectiveSpec *any = NULL;

    for (size_t i = 0; i < sizeof(DIRECTIVES) / sizeof(DIRECTIVES[0]); i++) {
        if (strcmp(DIRECTIVES[i].name, name) != 0) {
            continue;
        }
        if (DIRECTIVES[i].contexts & (int)context) {
            return &DIRECTIVES[i];
        }
        any = &DIRECTIVES[i];
    }
    return any;
}

int run_directive(Parser *parser, const Scope *scope,
                  const Directive *directive) {
    const DirectiveSpec *spec = find_directive(directive->name,
                                               scope->context);
    const char *name = directive->name;
    size_t line = directive->line;

    if (!spec) {
        return parse_error(parser, line, "unknown directive \"%s\"", name);
    }
    if (!(spec->contexts & (int)scope->context)) {
        return parse_error(parser, line,
                           "\"%s\" directive is not allowed here", name);
    }
    if (spec->is_block && !directive->opens_block) {
        return parse_error(parser, line,
                           "directive \"%s\" has no opening \"{\"", name);
    }
    if (!spec->is_block && directive->opens_block) {
        return parse_error(parser, line,
                           "directive \"%s\" is not terminated by \";\"",
                           name);
    }
    if (directive->argument_count < spec->min_arguments ||
        directive->argument_count > spec->max_arguments) {
        return parse_error(parser, line,
                           "invalid number of arguments in \"%s\" directive",
                           name);
    }
    return spec->handle(parser, scope, directive, spec);
}
