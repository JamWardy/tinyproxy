#ifndef TINYPROXY_CONFIG_H
#define TINYPROXY_CONFIG_H

#include <netinet/in.h>
#include <stddef.h>

#include "http.h"

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
    int version;  // HTTP_1_0 or HTTP_1_1.
} OptionalHttpVersion;

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
    Headers proxy_set_headers;
    AccessLog access_log;  // ACCESS_LOG_OFF or ACCESS_LOG_FILE.
} Settings;

typedef struct {
    char *origin;      // "http://host:port" as written, without the path.
    const char *host;  // $proxy_host: the host and port, inside origin.
    const char *uri;   // The path written after them, or NULL.
    struct sockaddr_in address;
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
    struct sockaddr_in address;
    char text[32];  // "0.0.0.0:8000", for messages.
} Listen;

typedef struct {
    size_t line;  // Where its block starts, for messages.
    Listen *listens;
    size_t listen_count;
    Location *locations;
    size_t location_count;
    BlockSettings written;
    Settings settings;
} Server;

typedef struct {
    char *arena;  // Every string in the config points into it.
    BlockSettings http;
    int has_http;
    Server *servers;
    size_t server_count;
} Config;

// On failure, error says why, with the file and line, and config_free must
// still be called.
int config_load(Config *config, const char *path, char *error,
                size_t error_size);
void config_free(Config *config);

#endif
