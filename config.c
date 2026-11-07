#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "variables.h"

#define MAX_ARGUMENTS 8  // nginx's NGX_CONF_MAX_ARGS.
#define DEFAULT_TIMEOUT 60.0
#define DEFAULT_MAX_BODY_SIZE (1024 * 1024)
#define DEFAULT_KEEPALIVE_TIMEOUT 75.0
#define DEFAULT_UPSTREAM_KEEPALIVE_TIMEOUT 60.0
#define DEFAULT_KEEPALIVE_REQUESTS 1000
#define DEFAULT_KEEPALIVE_TIME 3600.0

// nginx's own proxy headers, under any proxy_set_header.
static const Header DEFAULT_PROXY_HEADERS[] = {
    {"Host", "$proxy_host"},
    {"Connection", "close"},
    {"TE", ""},
    {"Keep-Alive", ""},
    {"Expect", ""},
    {"Upgrade", ""},
};

typedef enum {
    CONTEXT_MAIN = 1,
    CONTEXT_HTTP = 2,
    CONTEXT_SERVER = 4,
    CONTEXT_LOCATION = 8,
    CONTEXT_UPSTREAM = 16,
} Context;

#define ANY_HTTP_BLOCK (CONTEXT_HTTP | CONTEXT_SERVER | CONTEXT_LOCATION)

typedef enum {
    TOKEN_WORD,
    TOKEN_OPEN,
    TOKEN_CLOSE,
    TOKEN_SEMICOLON,
    TOKEN_END_OF_FILE,
} TokenType;

typedef struct {
    TokenType type;
    char *word;  // TOKEN_WORD's, in the arena.
    size_t line;
} Token;

typedef struct {
    Config *config;
    const char *path;
    const char *text;  // The whole file.
    size_t position;
    size_t line;
    size_t arena_used;
    char *error;
    size_t error_size;
} Parser;

typedef struct {
    const char *name;
    char *arguments[MAX_ARGUMENTS];  // In the arena.
    size_t argument_count;
    size_t line;
    int opens_block;
} Directive;

// What last set nginx's init_upstream for a pool: a method, or keepalive,
// which wraps the method before it. A method after keepalive replaces it.
typedef enum {
    POOL_INIT_DEFAULT,
    POOL_INIT_METHOD,
    POOL_INIT_KEEPALIVE,
} PoolInit;

// What an upstream block writes, until it's checked and put in its
// Upstream.
typedef struct {
    PoolInit init;
    OptionalCount keepalive;
    OptionalSeconds keepalive_timeout;
    OptionalCount keepalive_requests;
    OptionalSeconds keepalive_time;
} UpstreamSettings;

// The block being read. server, location and upstream are NULL outside
// them. settings is NULL in an upstream, and upstream_settings outside one.
typedef struct {
    Context context;
    Server *server;
    Location *location;
    BlockSettings *settings;
    Upstream *upstream;
    UpstreamSettings *upstream_settings;
} Scope;

static int parse_error(Parser *parser, size_t line, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static int parse_error(Parser *parser, size_t line, const char *format,
                       ...) {
    char message[512];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    snprintf(parser->error, parser->error_size, "%s in %s:%zu", message,
             parser->path, line);
    return 0;
}

// ---- Tokens ----

static int is_special(char c) {
    return c == '{' || c == '}' || c == ';';
}

static int is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void skip_blanks_and_comments(Parser *parser) {
    const char *text = parser->text;

    while (text[parser->position]) {
        char c = text[parser->position];

        if (c == '#') {
            while (text[parser->position] && text[parser->position] != '\n') {
                parser->position++;
            }
        } else if (is_blank(c)) {
            parser->line += c == '\n';
            parser->position++;
        } else {
            return;
        }
    }
}

// What a backslash and c stand for, as in nginx.
static char unescape(char c) {
    switch (c) {
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        default:
            return c;
    }
}

// Copies a word into the arena, quoted or not, without its quotes and with
// its escapes done. The arena is twice the file's size, so it always fits.
static int read_word(Parser *parser, Token *token) {
    const char *text = parser->text;
    char quote = 0;
    char *out = parser->config->arena + parser->arena_used;

    token->type = TOKEN_WORD;
    token->word = out;
    token->line = parser->line;

    if (text[parser->position] == '"' || text[parser->position] == '\'') {
        quote = text[parser->position++];
    }
    while (1) {
        char c = text[parser->position];

        if (c == '\0') {
            if (quote) {
                return parse_error(parser, parser->line,
                                   "unexpected end of file, expecting "
                                   "\";\" or \"}\"");
            }
            break;
        }
        if (quote ? c == quote : (is_blank(c) || is_special(c))) {
            parser->position += quote ? 1 : 0;
            break;
        }
        parser->line += c == '\n';
        if (c == '\\' && text[parser->position + 1]) {
            parser->position++;
            c = unescape(text[parser->position]);
        }
        *out++ = c;
        parser->position++;
    }
    *out++ = '\0';
    parser->arena_used = (size_t)(out - parser->config->arena);
    return 1;
}

static int next_token(Parser *parser, Token *token) {
    skip_blanks_and_comments(parser);
    token->line = parser->line;

    switch (parser->text[parser->position]) {
        case '\0':
            token->type = TOKEN_END_OF_FILE;
            return 1;
        case '{':
            token->type = TOKEN_OPEN;
            break;
        case '}':
            token->type = TOKEN_CLOSE;
            break;
        case ';':
            token->type = TOKEN_SEMICOLON;
            break;
        default:
            return read_word(parser, token);
    }
    parser->position++;
    return 1;
}

// ---- Values ----

// nginx's times: a number with ms, s, m, h, d, w, M or y after it, and
// several of those, such as "1m30s". A bare number is seconds.
static int parse_seconds(const char *text, double *seconds) {
    *seconds = 0;
    if (*text == '\0') {
        return 0;
    }
    while (*text) {
        char *end;
        long number = strtol(text, &end, 10);
        double unit;

        if (end == text || number < 0 || *text == '-' || *text == '+') {
            return 0;
        }
        text = end;
        if (strncmp(text, "ms", 2) == 0) {
            unit = 0.001;
            text += 2;
        } else {
            switch (*text) {
                case '\0':
                case 's':
                    unit = 1;
                    break;
                case 'm':
                    unit = 60;
                    break;
                case 'h':
                    unit = 60 * 60;
                    break;
                case 'd':
                    unit = 24 * 60 * 60;
                    break;
                case 'w':
                    unit = 7 * 24 * 60 * 60;
                    break;
                case 'M':
                    unit = 30 * 24 * 60 * 60;
                    break;
                case 'y':
                    unit = 365 * 24 * 60 * 60;
                    break;
                default:
                    return 0;
            }
            text += *text ? 1 : 0;
        }
        *seconds += (double)number * unit;
    }
    return 1;
}

// nginx's sizes: a number of bytes, or of k, m or g.
static int parse_size(const char *text, size_t *bytes) {
    char *end;
    unsigned long long number;
    unsigned long long unit = 1;

    if (*text < '0' || *text > '9') {
        return 0;
    }
    errno = 0;
    number = strtoull(text, &end, 10);
    if (errno == ERANGE) {
        return 0;
    }
    switch (*end) {
        case '\0':
            break;
        case 'k':
        case 'K':
            unit = 1024;
            break;
        case 'm':
        case 'M':
            unit = 1024 * 1024;
            break;
        case 'g':
        case 'G':
            unit = 1024 * 1024 * 1024;
            break;
        default:
            return 0;
    }
    if (*end && end[1] != '\0') {
        return 0;
    }
    if (number > (unsigned long long)SIZE_MAX / unit) {
        return 0;
    }
    *bytes = (size_t)(number * unit);
    return 1;
}

static int parse_port(const char *text, unsigned short *port) {
    char *end;
    long number = strtol(text, &end, 10);

    if (*text < '0' || *text > '9' || *end != '\0' || number < 1 ||
        number > 65535) {
        return 0;
    }
    *port = (unsigned short)number;
    return 1;
}

// Looks a host's IPv4 address up once, at startup, as nginx does.
static int resolve(const char *host, unsigned short port,
                   struct sockaddr_in *address) {
    struct addrinfo hints;
    struct addrinfo *found;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &found) != 0) {
        return 0;
    }
    memcpy(address, found->ai_addr, sizeof(*address));
    address->sin_port = htons(port);
    freeaddrinfo(found);
    return 1;
}

// Splits "host:port" at its last ':', into host, which must have room for
// all of text. Leaves port alone if there's no ':'.
static int split_host_port(const char *text, char *host,
                           unsigned short *port) {
    const char *colon = strrchr(text, ':');

    if (!colon) {
        strcpy(host, text);
        return 1;
    }
    memcpy(host, text, (size_t)(colon - text));
    host[colon - text] = '\0';
    return parse_port(colon + 1, port);
}

// Checks every $variable in a proxy_set_header value is one there is.
static int check_variables(Parser *parser, const Directive *directive,
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

// ---- Directives ----

typedef struct DirectiveSpec DirectiveSpec;
typedef int (*DirectiveHandler)(Parser *parser, const Scope *scope,
                                const Directive *directive,
                                const DirectiveSpec *spec);

struct DirectiveSpec {
    const char *name;
    int contexts;
    int is_block;
    size_t min_arguments;
    size_t max_arguments;
    DirectiveHandler handle;
    // Into the block's BlockSettings, or UpstreamSettings in an upstream,
    // for handle_seconds, handle_size and handle_count.
    size_t offset;
};

static int parse_block(Parser *parser, const Scope *scope);

static int duplicate(Parser *parser, const Directive *directive) {
    return parse_error(parser, directive->line,
                       "\"%s\" directive is duplicate", directive->name);
}

static int invalid_value(Parser *parser, const Directive *directive) {
    return parse_error(parser, directive->line,
                       "\"%s\" directive invalid value", directive->name);
}

static int out_of_memory(Parser *parser, const Directive *directive) {
    return parse_error(parser, directive->line, "out of memory");
}

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

static void block_settings_init(BlockSettings *settings) {
    memset(settings, 0, sizeof(*settings));
    settings->proxy_set_headers.items = NULL;
    settings->access_log.kind = ACCESS_LOG_UNSET;
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

static char *copy_text(const char *text, size_t length) {
    char *copy = malloc(length + 1);

    if (copy) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
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

// ---- Upstream pools ----

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
    upstream->number = config->upstream_count;
    upstream->peers = NULL;
    upstream->peer_count = 0;
    upstream->balance.kind = BALANCE_ROUND_ROBIN;
    upstream->balance.key = NULL;
    upstream->keepalive.connections = 0;
    upstream->keepalive.requests = DEFAULT_KEEPALIVE_REQUESTS;
    upstream->keepalive.time = DEFAULT_KEEPALIVE_TIME;
    upstream->keepalive.timeout = DEFAULT_UPSTREAM_KEEPALIVE_TIMEOUT;
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

static int handle_upstream(Parser *parser, const Scope *scope,
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
    if (upstream->peer_count == 0) {
        return parse_error(parser, directive->line,
                           "no servers are inside upstream");
    }

    UpstreamKeepalive *keepalive = &upstream->keepalive;
    if (written.keepalive.is_set && written.init == POOL_INIT_KEEPALIVE) {
        keepalive->connections = written.keepalive.count;
    }
    if (written.keepalive_requests.is_set) {
        keepalive->requests = written.keepalive_requests.count;
    }
    if (written.keepalive_time.is_set) {
        keepalive->time = written.keepalive_time.seconds;
    }
    if (written.keepalive_timeout.is_set) {
        keepalive->timeout = written.keepalive_timeout.seconds;
    }
    return 1;
}

// A whole number, as nginx's ngx_atoi.
static int parse_count(const char *text, size_t *count) {
    *count = 0;
    if (*text == '\0') {
        return 0;
    }
    for (const char *c = text; *c; c++) {
        size_t digit = (size_t)(*c - '0');

        if (*c < '0' || *c > '9' || *count > (SIZE_MAX - digit) / 10) {
            return 0;
        }
        *count = *count * 10 + digit;
    }
    return 1;
}

static int invalid_parameter(Parser *parser, const Directive *directive,
                             const char *option) {
    return parse_error(parser, directive->line, "invalid parameter \"%s\"",
                       option);
}

// weight=, max_conns= or down on a server, into options.
static int take_server_option(Parser *parser, const Directive *directive,
                              const char *option, Peer *options) {
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

// What a server has unless its options say otherwise.
static Peer default_peer_options(const char *server) {
    Peer options;

    memset(&options, 0, sizeof(options));
    options.server = server;
    options.weight = 1;
    return options;
}

// "server host:port;" in an upstream block.
static int handle_upstream_server(Parser *parser, const Scope *scope,
                                  const Directive *directive,
                                  const DirectiveSpec *spec) {
    const char *address = directive->arguments[0];
    char host[256];
    unsigned short port = 80;

    Peer options = default_peer_options(address);

    (void)spec;
    for (size_t i = 1; i < directive->argument_count; i++) {
        if (!take_server_option(parser, directive, directive->arguments[i],
                                &options)) {
            return 0;
        }
    }
    if (strlen(address) >= sizeof(host) ||
        !split_host_port(address, host, &port)) {
        return parse_error(parser, directive->line,
                           "invalid port in upstream \"%s\"", address);
    }
    switch (upstream_add_peers(scope->upstream, host, port, &options)) {
        case PEERS_ADDED:
            return 1;
        case PEERS_NOT_FOUND:
            return parse_error(parser, directive->line,
                               "host not found in upstream \"%s\"",
                               address);
        case PEERS_OUT_OF_MEMORY:
            return out_of_memory(parser, directive);
    }
    return 0;
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
        Peer options = default_peer_options(pass->host);

        switch (upstream_add_peers(upstream, host, port, &options)) {
            case PEERS_ADDED:
                break;
            case PEERS_NOT_FOUND:
                return parse_error(parser, pass->line,
                                   "host not found in upstream \"%s\"",
                                   pass->host);
            case PEERS_OUT_OF_MEMORY:
                return parse_error(parser, pass->line, "out of memory");
        }
    }
    pass->upstream = upstream;
    return 1;
}

// Done once the whole file is read, as an upstream can come after its use.
static int link_proxy_passes(Parser *parser) {
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

// A whole number, as nginx's keepalive_requests. keepalive's N must be at
// least 1, and it wraps the pool's method so far.
static int handle_count(Parser *parser, const Scope *scope,
                        const Directive *directive,
                        const DirectiveSpec *spec) {
    OptionalCount *setting = setting_at(scope, spec);
    int is_keepalive = strcmp(directive->name, "keepalive") == 0;
    size_t count;

    if (setting->is_set) {
        return duplicate(parser, directive);
    }
    if (!parse_count(directive->arguments[0], &count) ||
        (is_keepalive && count == 0)) {
        return invalid_value(parser, directive);
    }
    if (is_keepalive) {
        scope->upstream_settings->init = POOL_INIT_KEEPALIVE;
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

// As nginx: a second method, or one after keepalive, which it replaces,
// is only a warning.
static void set_balance(Parser *parser, const Scope *scope,
                        const Directive *directive, BalanceKind kind,
                        const char *key) {
    UpstreamSettings *written = scope->upstream_settings;

    if (written->init != POOL_INIT_DEFAULT) {
        fprintf(stderr, "tinyproxy: [warn] load balancing method redefined "
                        "in %s:%zu\n", parser->path, directive->line);
    }
    written->init = POOL_INIT_METHOD;
    scope->upstream->balance.kind = kind;
    scope->upstream->balance.key = key;
}

// least_conn and ip_hash, which take no arguments.
static int handle_balance(Parser *parser, const Scope *scope,
                          const Directive *directive,
                          const DirectiveSpec *spec) {
    (void)spec;
    set_balance(parser, scope, directive,
                strcmp(directive->name, "least_conn") == 0
                    ? BALANCE_LEAST_CONN
                    : BALANCE_IP_HASH,
                NULL);
    return 1;
}

// "hash key;" or "hash key consistent;".
static int handle_hash(Parser *parser, const Scope *scope,
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
    set_balance(parser, scope, directive, kind, key);
    return 1;
}

#define SECONDS(name, contexts)                                             \
    {#name, contexts, 0, 1, 1, handle_seconds, offsetof(BlockSettings, name)}
#define UPSTREAM(name, handler)                                             \
    {#name, CONTEXT_UPSTREAM, 0, 1, 1, handler,                             \
     offsetof(UpstreamSettings, name)}

static const DirectiveSpec DIRECTIVES[] = {
    {"http", CONTEXT_MAIN, 1, 0, 0, handle_http, 0},
    {"server", CONTEXT_HTTP, 1, 0, 0, handle_server, 0},
    {"upstream", CONTEXT_HTTP, 1, 1, 1, handle_upstream, 0},
    {"server", CONTEXT_UPSTREAM, 0, 1, MAX_ARGUMENTS, handle_upstream_server,
     0},
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
    SECONDS(send_timeout, ANY_HTTP_BLOCK),
    {"keepalive_timeout", ANY_HTTP_BLOCK, 0, 1, 2, handle_keepalive_timeout,
     0},
    {"keepalive_requests", ANY_HTTP_BLOCK, 0, 1, 1, handle_count,
     offsetof(BlockSettings, keepalive_requests)},
    SECONDS(keepalive_time, ANY_HTTP_BLOCK),
    UPSTREAM(keepalive, handle_count),
    {"least_conn", CONTEXT_UPSTREAM, 0, 0, 0, handle_balance, 0},
    {"ip_hash", CONTEXT_UPSTREAM, 0, 0, 0, handle_balance, 0},
    {"hash", CONTEXT_UPSTREAM, 0, 1, 2, handle_hash, 0},
    UPSTREAM(keepalive_timeout, handle_seconds),
    UPSTREAM(keepalive_requests, handle_count),
    UPSTREAM(keepalive_time, handle_seconds),
};

// The spec for name in context, or else any for name, for the "not allowed
// here" error. "server" has one in http and another in upstream.
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

static int run_directive(Parser *parser, const Scope *scope,
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

// Reads a directive's arguments, up to its ";" or "{".
static int read_arguments(Parser *parser, Directive *directive) {
    Token token;

    while (1) {
        if (!next_token(parser, &token)) {
            return 0;
        }
        switch (token.type) {
            case TOKEN_WORD:
                if (directive->argument_count == MAX_ARGUMENTS) {
                    return parse_error(parser, token.line,
                                       "invalid number of arguments in "
                                       "\"%s\" directive", directive->name);
                }
                directive->arguments[directive->argument_count++] =
                    token.word;
                break;
            case TOKEN_SEMICOLON:
                directive->opens_block = 0;
                return 1;
            case TOKEN_OPEN:
                directive->opens_block = 1;
                return 1;
            case TOKEN_CLOSE:
                return parse_error(parser, token.line, "unexpected \"}\"");
            case TOKEN_END_OF_FILE:
                return parse_error(parser, token.line,
                                   "unexpected end of file, expecting "
                                   "\";\" or \"}\"");
        }
    }
}

// Reads directives up to the block's "}", or at the top, the end of the file.
static int parse_block(Parser *parser, const Scope *scope) {
    int at_top = scope->context == CONTEXT_MAIN;
    Token token;

    while (1) {
        if (!next_token(parser, &token)) {
            return 0;
        }
        switch (token.type) {
            case TOKEN_WORD:
                break;
            case TOKEN_END_OF_FILE:
                return at_top ? 1
                              : parse_error(parser, token.line,
                                            "unexpected end of file, "
                                            "expecting \"}\"");
            case TOKEN_CLOSE:
                return at_top ? parse_error(parser, token.line,
                                            "unexpected \"}\"")
                              : 1;
            case TOKEN_OPEN:
                return parse_error(parser, token.line, "unexpected \"{\"");
            case TOKEN_SEMICOLON:
                return parse_error(parser, token.line, "unexpected \";\"");
        }

        Directive directive;
        directive.name = token.word;
        directive.line = token.line;
        directive.argument_count = 0;
        if (!read_arguments(parser, &directive) ||
            !run_directive(parser, scope, &directive)) {
            return 0;
        }
    }
}

// ---- Inheriting ----

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
    settings.send_timeout =
        inherit_seconds(&own->send_timeout, outer->send_timeout);
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
    settings.keepalive_requests = own->keepalive_requests.is_set
                                      ? own->keepalive_requests.count
                                      : outer->keepalive_requests;
    settings.keepalive_time =
        inherit_seconds(&own->keepalive_time, outer->keepalive_time);
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
    settings.send_timeout = DEFAULT_TIMEOUT;
    settings.client_max_body_size = DEFAULT_MAX_BODY_SIZE;
    settings.proxy_http_version = HTTP_1_0;
    settings.keepalive_timeout = DEFAULT_KEEPALIVE_TIMEOUT;
    settings.keepalive_header_timeout.is_set = 0;
    settings.keepalive_header_timeout.seconds = 0;
    settings.keepalive_requests = DEFAULT_KEEPALIVE_REQUESTS;
    settings.keepalive_time = DEFAULT_KEEPALIVE_TIME;
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
static int inherit_all(Config *config) {
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
static int group_servers(Parser *parser) {
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

// ---- Loading ----

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    size_t used = 0;
    size_t capacity = 0;

    if (!file) {
        return NULL;
    }
    while (1) {
        if (capacity - used < 4096) {
            capacity = capacity ? capacity * 2 : 8192;
            char *grown = realloc(text, capacity);
            if (!grown) {
                free(text);
                fclose(file);
                errno = ENOMEM;
                return NULL;
            }
            text = grown;
        }
        size_t count = fread(text + used, 1, capacity - used - 1, file);
        used += count;
        if (count == 0) {
            break;
        }
    }
    int failed = ferror(file);
    fclose(file);
    if (failed) {
        free(text);
        errno = EIO;
        return NULL;
    }
    text[used] = '\0';
    *length = used;
    return text;
}

int config_load(Config *config, const char *path, char *error,
                size_t error_size) {
    Parser parser;
    size_t length;

    memset(config, 0, sizeof(*config));
    block_settings_init(&config->http);

    char *text = read_file(path, &length);
    if (!text) {
        snprintf(error, error_size, "open() \"%s\" failed (%d: %s)", path,
                 errno, strerror(errno));
        return 0;
    }
    // Each word is copied with a NUL after it, and is no longer than in the
    // file, so twice the file always holds them all.
    config->arena = malloc(2 * length + 1);
    if (!config->arena) {
        free(text);
        snprintf(error, error_size, "out of memory");
        return 0;
    }

    parser.config = config;
    parser.path = path;
    parser.text = text;
    parser.position = 0;
    parser.line = 1;
    parser.arena_used = 0;
    parser.error = error;
    parser.error_size = error_size;

    Scope top = {CONTEXT_MAIN, NULL, NULL, NULL, NULL, NULL};
    int ok = parse_block(&parser, &top) && link_proxy_passes(&parser) &&
             group_servers(&parser);
    free(text);
    if (ok && !inherit_all(config)) {
        snprintf(error, error_size, "out of memory");
        return 0;
    }
    return ok;
}

static void block_settings_free(BlockSettings *settings) {
    free(settings->proxy_set_headers.items);
    if (settings->access_log.kind == ACCESS_LOG_FILE) {
        close(settings->access_log.fd);
    }
}

void config_free(Config *config) {
    for (size_t i = 0; i < config->port_count; i++) {
        for (size_t j = 0; j < config->ports[i].group_count; j++) {
            free(config->ports[i].groups[j].servers);
        }
        free(config->ports[i].groups);
    }
    free(config->ports);
    for (size_t i = 0; i < config->upstream_count; i++) {
        upstream_free(config->upstreams[i]);
    }
    free(config->upstreams);
    for (size_t i = 0; i < config->server_count; i++) {
        Server *server = &config->servers[i];

        for (size_t j = 0; j < server->location_count; j++) {
            free(server->locations[j].proxy_pass.host);
            free(server->locations[j].proxy_headers.items);
            block_settings_free(&server->locations[j].written);
        }
        free(server->locations);
        free(server->listens);
        free(server->names);
        block_settings_free(&server->written);
    }
    free(config->servers);
    block_settings_free(&config->http);
    free(config->arena);
    memset(config, 0, sizeof(*config));
}
