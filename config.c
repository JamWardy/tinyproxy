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
    const char *arguments[MAX_ARGUMENTS];
    size_t argument_count;
    size_t line;
    int opens_block;
} Directive;

// The block being read. server and location are NULL outside them.
typedef struct {
    Context context;
    Server *server;
    Location *location;
    BlockSettings *settings;
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
    static const Variables NO_VALUES = {"", "", "", ""};

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
    size_t offset;  // Into BlockSettings, for handle_seconds and handle_size.
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
    Scope inner = {CONTEXT_HTTP, NULL, NULL, &config->http};

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
    server->listens = listen;
    server->listen_count = 1;
    return 1;
}

// Several servers on one port need server_name to choose between them.
static int check_ports_unshared(Parser *parser, const Server *server) {
    const Config *config = parser->config;

    for (size_t i = 0; i < config->server_count; i++) {
        const Server *other = &config->servers[i];

        if (other == server) {
            continue;
        }
        for (size_t j = 0; j < other->listen_count; j++) {
            for (size_t k = 0; k < server->listen_count; k++) {
                unsigned short port = server->listens[k].address.sin_port;

                if (other->listens[j].address.sin_port == port) {
                    return parse_error(
                        parser, server->line,
                        "a second \"server\" on port %d is not supported "
                        "until stage 3", ntohs(port));
                }
            }
        }
    }
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

    Scope inner = {CONTEXT_SERVER, server, NULL, &server->written};
    if (!parse_block(parser, &inner)) {
        return 0;
    }
    if (server->listen_count == 0 && !add_default_listen(server)) {
        return out_of_memory(parser, directive);
    }
    return check_ports_unshared(parser, server);
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

    Scope inner = {CONTEXT_LOCATION, server, location, &location->written};
    return parse_block(parser, &inner);
}

// "8000", "127.0.0.1:8000", "*:8000" or "localhost", as nginx's listen.
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

// "http://host:port", and perhaps a path after it.
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
    const char *uri = strchr(url + 7, '/');
    size_t origin_length = uri ? (size_t)(uri - url) : strlen(url);
    char *origin = malloc(origin_length + 1);
    if (!origin) {
        return out_of_memory(parser, directive);
    }
    memcpy(origin, url, origin_length);
    origin[origin_length] = '\0';
    // Owned by the location from here, so freed with it even on failure.
    location->proxy_pass.origin = origin;

    const char *authority = origin + 7;
    if (strlen(authority) >= sizeof(host) ||
        !split_host_port(authority, host, &port)) {
        return parse_error(parser, directive->line,
                           "invalid port in upstream \"%s\"", authority);
    }
    if (host[0] == '\0') {
        return parse_error(parser, directive->line, "no host in upstream "
                                                    "\"%s\"", url);
    }
    if (!resolve(host, port, &location->proxy_pass.address)) {
        return parse_error(parser, directive->line,
                           "host not found in upstream \"%s\"", authority);
    }
    location->proxy_pass.host = authority;
    location->proxy_pass.uri = uri;
    location->has_proxy_pass = 1;
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

static int handle_seconds(Parser *parser, const Scope *scope,
                          const Directive *directive,
                          const DirectiveSpec *spec) {
    OptionalSeconds *setting =
        (OptionalSeconds *)((char *)scope->settings + spec->offset);

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
    OptionalSize *setting =
        (OptionalSize *)((char *)scope->settings + spec->offset);

    if (setting->is_set) {
        return duplicate(parser, directive);
    }
    if (!parse_size(directive->arguments[0], &setting->bytes)) {
        return invalid_value(parser, directive);
    }
    setting->is_set = 1;
    return 1;
}

#define SECONDS(name, contexts)                                             \
    {#name, contexts, 0, 1, 1, handle_seconds, offsetof(BlockSettings, name)}

static const DirectiveSpec DIRECTIVES[] = {
    {"http", CONTEXT_MAIN, 1, 0, 0, handle_http, 0},
    {"server", CONTEXT_HTTP, 1, 0, 0, handle_server, 0},
    {"listen", CONTEXT_SERVER, 0, 1, 1, handle_listen, 0},
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
};

static const DirectiveSpec *find_directive(const char *name) {
    for (size_t i = 0; i < sizeof(DIRECTIVES) / sizeof(DIRECTIVES[0]); i++) {
        if (strcmp(DIRECTIVES[i].name, name) == 0) {
            return &DIRECTIVES[i];
        }
    }
    return NULL;
}

static int run_directive(Parser *parser, const Scope *scope,
                         const Directive *directive) {
    const DirectiveSpec *spec = find_directive(directive->name);
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

    Scope top = {CONTEXT_MAIN, NULL, NULL, NULL};
    int ok = parse_block(&parser, &top);
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
    for (size_t i = 0; i < config->server_count; i++) {
        Server *server = &config->servers[i];

        for (size_t j = 0; j < server->location_count; j++) {
            free(server->locations[j].proxy_pass.origin);
            free(server->locations[j].proxy_headers.items);
            block_settings_free(&server->locations[j].written);
        }
        free(server->locations);
        free(server->listens);
        block_settings_free(&server->written);
    }
    free(config->servers);
    block_settings_free(&config->http);
    free(config->arena);
    memset(config, 0, sizeof(*config));
}
