#ifndef TINYPROXY_CONFIG_H
#define TINYPROXY_CONFIG_H

#include <netinet/in.h>
#include <stddef.h>

#include "http.h"
#include "upstream.h"

typedef struct {
    int is_set;
    double seconds;
} OptionalSeconds;

typedef struct {
    int is_set;
    size_t bytes;
} OptionalSize;

typedef struct {
    int is_set;
    size_t count;
} OptionalCount;

typedef struct {
    int is_set;
    int version;  // HTTP_1_0 or HTTP_1_1.
} OptionalHttpVersion;

// keepalive_timeout's two arguments: the wait, then the optional
// "Keep-Alive: timeout=N".
typedef struct {
    int is_set;
    double seconds;
    OptionalSeconds header;
} OptionalKeepaliveTimeout;

typedef enum {
    ACCESS_LOG_UNSET,
    ACCESS_LOG_OFF,
    ACCESS_LOG_FILE,
} AccessLogKind;

typedef struct {
    AccessLogKind kind;
    int fd;  // ACCESS_LOG_FILE's.
} AccessLog;

// What one block writes. Anything it leaves out it inherits from the block
// around it.
typedef struct {
    OptionalSeconds client_header_timeout;
    OptionalSeconds client_body_timeout;
    OptionalSeconds proxy_connect_timeout;
    OptionalSeconds proxy_send_timeout;
    OptionalSeconds proxy_read_timeout;
    OptionalSeconds send_timeout;
    OptionalSize client_max_body_size;
    OptionalHttpVersion proxy_http_version;
    OptionalKeepaliveTimeout keepalive_timeout;
    OptionalCount keepalive_requests;
    OptionalSeconds keepalive_time;
    // As in nginx, a block that sets any inherits none from around it.
    Headers proxy_set_headers;
    AccessLog access_log;
} BlockSettings;

// What applies once inherited. The headers and the log's descriptor are
// borrowed from the block that wrote them.
typedef struct {
    double client_header_timeout;
    double client_body_timeout;
    double proxy_connect_timeout;
    double proxy_send_timeout;
    double proxy_read_timeout;
    double send_timeout;
    size_t client_max_body_size;  // 0 for no limit.
    int proxy_http_version;
    double keepalive_timeout;  // 0 for no keep-alive.
    OptionalSeconds keepalive_header_timeout;
    size_t keepalive_requests;
    double keepalive_time;
    Headers proxy_set_headers;
    AccessLog access_log;  // ACCESS_LOG_OFF or ACCESS_LOG_FILE.
} Settings;

typedef struct {
    char *host;       // $proxy_host: as written after "http://", up to any
                      // path, so a pool's name or a host and port.
    const char *uri;  // The path written after it, or NULL.
    size_t line;
    const Upstream *upstream;  // Looked up once the whole file is read.
} ProxyPass;

typedef enum {
    LOCATION_PREFIX,  // "location /path" or "location ^~ /path".
    LOCATION_EXACT,   // "location = /path".
} LocationMatch;

typedef struct {
    LocationMatch match;
    const char *path;
    int has_proxy_pass;
    ProxyPass proxy_pass;  // has_proxy_pass's.
    // What's sent upstream: nginx's defaults, with proxy_set_header's on top.
    // Owned, though the strings are the config's.
    Headers proxy_headers;
    BlockSettings written;
    Settings settings;
} Location;

typedef struct {
    struct sockaddr_in address;  // INADDR_ANY for every address.
    char text[32];               // "0.0.0.0:8000", for messages.
    int default_server;
    size_t line;
} Listen;

typedef enum {
    NAME_EXACT,                  // "shop.local", or "" for no Host.
    NAME_SUBDOMAINS,             // "*.shop.local": any name ending in
                                 // ".shop.local".
    NAME_DOMAIN_AND_SUBDOMAINS,  // ".shop.local": that, or "shop.local".
    NAME_PREFIX,                 // "mail.*": any name starting "mail.".
} ServerNameKind;

typedef struct {
    ServerNameKind kind;
    const char *name;  // As written, lowercased.
} ServerName;

typedef struct {
    size_t line;  // Where its block starts, for messages.
    ServerName *names;  // At least one: "" if it has no server_name.
    size_t name_count;
    Listen *listens;
    size_t listen_count;
    Location *locations;
    size_t location_count;
    BlockSettings written;
    Settings settings;
} Server;

// The servers that listen on one address and port, nginx's
// ngx_http_conf_addr_t. The servers are borrowed from Config.servers.
typedef struct {
    struct sockaddr_in address;  // INADDR_ANY for every address.
    char text[32];
    const Server *default_server;
    const Server **servers;  // In the config's order, so the first wins.
    size_t server_count;
} AddressGroup;

typedef struct {
    unsigned short port;
    AddressGroup *groups;
    size_t group_count;
} ListenPort;

typedef struct {
    char *arena;  // Every string in the config points into it.
    BlockSettings http;
    int has_http;
    Server *servers;
    size_t server_count;
    ListenPort *ports;  // Built from the servers' listens once they're read.
    size_t port_count;
    // Each on the heap, so a proxy_pass's pointer lasts as more are added.
    Upstream **upstreams;
    size_t upstream_count;
} Config;

// On failure, error says why, with the file and line, and config_free must
// still be called.
int config_load(Config *config, const char *path, char *error,
                size_t error_size);
void config_free(Config *config);

#endif
