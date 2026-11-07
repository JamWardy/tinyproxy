#ifndef TINYPROXY_CONFIG_INTERNAL_H
#define TINYPROXY_CONFIG_INTERNAL_H

// What the config's own files share, and nothing else uses.

#include <netinet/in.h>
#include <stddef.h>

#include "config.h"

#define MAX_ARGUMENTS 8  // nginx's NGX_CONF_MAX_ARGS.

#define DEFAULT_TIMEOUT 60.0
#define DEFAULT_MAX_BODY_SIZE (1024 * 1024)
#define DEFAULT_KEEPALIVE_TIMEOUT 75.0
#define DEFAULT_UPSTREAM_KEEPALIVE_TIMEOUT 60.0
#define DEFAULT_KEEPALIVE_MAX_REQUESTS 1000
#define DEFAULT_KEEPALIVE_LIFETIME 3600.0

typedef enum {
    CONTEXT_MAIN = 1,
    CONTEXT_HTTP = 2,
    CONTEXT_SERVER = 4,
    CONTEXT_LOCATION = 8,
    CONTEXT_UPSTREAM = 16,
} Context;

#define ANY_HTTP_BLOCK (CONTEXT_HTTP | CONTEXT_SERVER | CONTEXT_LOCATION)

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

// Which of a balancing method and keepalive_max_idle an upstream block
// wrote last. As in nginx, a method written after keepalive_max_idle turns
// it off.
typedef enum {
    WROTE_NEITHER,
    WROTE_METHOD_LAST,
    WROTE_KEEPALIVE_LAST,
} UpstreamLast;

// What an upstream block writes, until it's checked and put in its
// Upstream.
typedef struct {
    UpstreamLast last;
    OptionalCount keepalive_max_idle;
    OptionalSeconds keepalive_timeout;
    OptionalCount keepalive_max_requests;
    OptionalSeconds keepalive_lifetime;
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

// ---- parse.c ----

int parse_error(Parser *parser, size_t line, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
int duplicate(Parser *parser, const Directive *directive);
int invalid_value(Parser *parser, const Directive *directive);
int invalid_parameter(Parser *parser, const Directive *directive,
                      const char *option);
int out_of_memory(Parser *parser, const Directive *directive);

int parse_block(Parser *parser, const Scope *scope);

int parse_seconds(const char *text, double *seconds);
int parse_size(const char *text, size_t *bytes);
int parse_count(const char *text, size_t *count);
int parse_port(const char *text, unsigned short *port);
int resolve(const char *host, unsigned short port,
            struct sockaddr_in *address);
int split_host_port(const char *text, char *host, unsigned short *port);
char *copy_text(const char *text, size_t length);

// ---- directives.c ----

// Runs one directive read in scope, by the directive table.
int run_directive(Parser *parser, const Scope *scope,
                  const Directive *directive);
void block_settings_init(BlockSettings *settings);
int check_variables(Parser *parser, const Directive *directive,
                    const char *value);

// ---- upstreams.c ----

int handle_upstream(Parser *parser, const Scope *scope,
                    const Directive *directive, const DirectiveSpec *spec);
int handle_host(Parser *parser, const Scope *scope,
                const Directive *directive, const DirectiveSpec *spec);
int handle_plain_method(Parser *parser, const Scope *scope,
                        const Directive *directive,
                        const DirectiveSpec *spec);
int handle_hash(Parser *parser, const Scope *scope,
                const Directive *directive, const DirectiveSpec *spec);
int link_proxy_passes(Parser *parser);

// ---- inherit.c ----

// Fails only when out of memory.
int inherit_all(Config *config);
int group_servers(Parser *parser);

#endif
