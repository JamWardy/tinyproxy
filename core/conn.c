#include "conn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "error_page.h"
#include "http.h"
#include "log.h"
#include "route.h"
#include "variables.h"

// nginx's lingering_time and lingering_timeout.
#define LINGERING_TIME 30.0
#define LINGERING_TIMEOUT 5.0

static unsigned long long connections_accepted = 0;

int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int would_block(int error_number) {
    return error_number == EAGAIN || error_number == EWOULDBLOCK ||
           error_number == EINTR;
}

static int uses_upstream(ConnState state) {
    switch (state) {
        case CONN_CONNECTING:
        case CONN_SENDING_REQUEST:
        case CONN_READING_REPLY_HEAD:
        case CONN_READING_REPLY:
            return 1;
        case CONN_UNUSED:
        case CONN_READING_HEAD:
        case CONN_READING_BODY:
        case CONN_SENDING_REPLY:
        case CONN_SENDING_OWN_REPLY:
        case CONN_LINGERING:
        case CONN_DISCARDING_BODY:
        case CONN_IDLE:
            return 0;
    }
    return 0;
}

static void close_upstream(Connection *conn) {
    if (conn->upstream.fd >= 0) {
        close(conn->upstream.fd);
        conn->upstream.fd = -1;
    }
}

// Moves to state. Leaving the upstream's states ends the endpoint's active
// request, and closes the upstream connection if it wasn't kept.
static void set_state(Connection *conn, ConnState state) {
    if (uses_upstream(conn->state) && !uses_upstream(state) &&
        conn->endpoint) {
        release_endpoint(conn->location->proxy_pass.upstream, conn->endpoint);
    }
    if (!uses_upstream(state)) {
        close_upstream(conn);
    }
    conn->state = state;
}

static void close_conn(Connection *conn) {
    set_state(conn, CONN_UNUSED);
    close(conn->client_fd);
    buffer_free(&conn->from_client);
    buffer_free(&conn->upstream_uri);
    buffer_free(&conn->to_upstream);
    buffer_free(&conn->from_upstream);
    buffer_free(&conn->to_client);
    request_free(&conn->request);
}

void conn_close(Connection *conn) {
    close_conn(conn);
}

// ---- Logs ----

static void log_request(const Connection *conn) {
    const AccessLog *log = &conn->settings->access_log;

    if (log->kind != ACCESS_LOG_FILE) {
        return;
    }
    AccessEntry entry;
    entry.remote_addr = conn->remote_addr;
    entry.time = time(NULL);
    entry.request_line = conn->request.line;
    entry.status = conn->status;
    entry.body_bytes_sent =
        conn->sent_to_client > conn->reply_head_length
            ? conn->sent_to_client - conn->reply_head_length
            : 0;
    entry.referer = request_find_header(&conn->request, "Referer");
    entry.user_agent = request_find_header(&conn->request, "User-Agent");
    access_log_write(log->fd, &entry);
}

// About this request, at level, with nginx's details after it.
static void log_conn(const Connection *conn, const char *level,
                     const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void log_conn(const Connection *conn, const char *level,
                     const char *format, ...) {
    char message[512];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // As nginx's, there's an upstream only once an endpoint's been chosen.
    char upstream[1024] = "";
    if (conn->endpoint) {
        snprintf(upstream, sizeof(upstream), ", upstream: \"http://%s%s\"",
                 conn->endpoint->text, buffer_bytes(&conn->upstream_uri));
    }

    const Request *request = &conn->request;
    error_log(level,
              "*%llu %s, client: %s, request: \"%s\"%s, host: \"%s\"",
              conn->id, message, conn->remote_addr,
              request->line ? request->line : "", upstream,
              request->host ? request->host : "");
}

// The request has its answer, or never will: logs it, and closes.
static void log_and_close(Connection *conn) {
    log_request(conn);
    close_conn(conn);
}

// ---- Reading and writing ----

typedef enum {
    RECEIVED_DATA,
    RECEIVED_NOTHING,  // Nothing there yet.
    RECEIVED_END,      // The other side closed.
    RECEIVED_ERROR,    // Including running out of memory; error_number says.
} Received;

static Received receive(int fd, Buffer *into, int *error_number) {
    char chunk[65536];
    ssize_t count = recv(fd, chunk, sizeof(chunk), 0);

    if (count == 0) {
        return RECEIVED_END;
    }
    if (count < 0) {
        *error_number = errno;
        return would_block(errno) ? RECEIVED_NOTHING : RECEIVED_ERROR;
    }
    if (into && !buffer_append(into, chunk, (size_t)count)) {
        *error_number = ENOMEM;
        return RECEIVED_ERROR;
    }
    return RECEIVED_DATA;
}

// ---- Keeping the client's connection ----

// nginx's checks once the request has its location, before anything about
// the reply: the client asked, and keepalive_timeout,
// keepalive_max_requests and keepalive_lifetime allow it.
static int may_keep_client(const Connection *conn, double now) {
    const Settings *settings = conn->settings;

    return conn->request.wants_keepalive && settings->keepalive_timeout > 0 &&
           conn->request_count < settings->keepalive_max_requests &&
           now - conn->accepted_at <= settings->keepalive_lifetime;
}

// tinyproxy's own replies that keep it, as nginx's do.
static int own_reply_keeps_client(int status) {
    switch (status) {
        case 400:
        case 408:
        case 413:
        case 414:
        case 500:
        case 505:
        case STATUS_HEADER_TOO_LARGE:
            return 0;
        default:
            return 1;
    }
}

// The Connection header lines for the reply, as conn->keep_client says.
static void connection_headers(const Connection *conn, char *out,
                               size_t size) {
    const OptionalSeconds *header = &conn->settings->keepalive_header_timeout;

    if (!conn->keep_client) {
        snprintf(out, size, "Connection: close\r\n");
    } else if (header->is_set) {
        snprintf(out, size,
                 "Connection: keep-alive\r\nKeep-Alive: timeout=%lld\r\n",
                 (long long)header->seconds);
    } else {
        snprintf(out, size, "Connection: keep-alive\r\n");
    }
}

static int body_received(const Connection *conn) {
    return buffer_length(&conn->from_client) - conn->head_length >=
           conn->request.content_length;
}

// Bytes past this request: the next one, sent early.
static int has_more_requests(const Connection *conn) {
    return conn->pipelined ||
           buffer_length(&conn->from_client) >
               conn->head_length + conn->request.content_length;
}

static void handle_head(Connection *conn, double now);

// Starts a request on the connection, from the group's default server.
static void reset_for_request(Connection *conn, double now) {
    conn->server = conn->group->default_server;
    conn->settings = &conn->server->settings;
    conn->location = NULL;
    conn->endpoint = NULL;
    conn->head_length = 0;
    conn->pipelined = 0;
    conn->discard_left = 0;
    conn->body_rewrite = REWRITE_NONE;
    conn->keep_client = 0;
    conn->status = 0;
    conn->reply_head_length = 0;
    conn->sent_to_client = 0;
    conn->request_count++;
    conn->request_at = now;
    conn->client_io_at = now;
    set_state(conn, CONN_READING_HEAD);
}

// Reads the next request if it's already been sent, or else waits for it.
static void wait_for_request(Connection *conn, double now) {
    if (buffer_length(&conn->from_client) > 0) {
        reset_for_request(conn, now);
        handle_head(conn, now);
        return;
    }
    conn->client_io_at = now;
    set_state(conn, CONN_IDLE);
}

// The request's done and the connection kept: drops the request, then the
// rest of its body if it's still to come.
static void next_request(Connection *conn, double now) {
    size_t used = conn->head_length + conn->request.content_length;
    size_t have = buffer_length(&conn->from_client);

    log_request(conn);
    buffer_consume(&conn->from_client, used < have ? used : have);
    request_free(&conn->request);
    buffer_clear(&conn->upstream_uri);
    buffer_clear(&conn->to_upstream);
    buffer_clear(&conn->from_upstream);
    if (used > have) {
        conn->discard_left = used - have;
        conn->lingering_since = now;
        conn->client_io_at = now;
        set_state(conn, CONN_DISCARDING_BODY);
        return;
    }
    wait_for_request(conn, now);
}

// Shuts the sending side, then waits for the client to close, so the reply
// isn't lost to a reset for data it sent that was never read.
static void start_lingering(Connection *conn, double now) {
    log_request(conn);
    shutdown(conn->client_fd, SHUT_WR);
    conn->lingering_since = now;
    conn->client_io_at = now;
    set_state(conn, CONN_LINGERING);
}

// The whole reply has gone to the client.
static void reply_sent(Connection *conn, double now) {
    if (conn->keep_client) {
        next_request(conn, now);
    } else if (conn->state == CONN_SENDING_OWN_REPLY ||
               has_more_requests(conn)) {
        start_lingering(conn, now);
    } else {
        log_and_close(conn);
    }
}

// ---- Own replies ----

// tinyproxy's own reply in place of anything else, closing the upstream
// connection if there is one. redirect is a 301's Location, or NULL.
static void send_own_reply(Connection *conn, int status, const char *redirect,
                           double now) {
    const char *method = conn->request.method;
    int with_body = !(method && strcmp(method, "HEAD") == 0);
    char connection[80];

    set_state(conn, CONN_SENDING_OWN_REPLY);
    conn->keep_client =
        own_reply_keeps_client(status) && may_keep_client(conn, now);
    connection_headers(conn, connection, sizeof(connection));
    buffer_clear(&conn->to_client);
    if (!build_own_reply(&conn->to_client, status, with_body, redirect,
                         connection)) {
        close_conn(conn);
        return;
    }
    conn->status = status == STATUS_HEADER_TOO_LARGE ? 400 : status;
    conn->reply_head_length = find_reply_head(buffer_bytes(&conn->to_client),
                                              buffer_length(&conn->to_client));
    // The body will be dropped once this is sent, so a client waiting to
    // be told to send it is told, as nginx does. Short and the first thing
    // sent, the socket always takes it whole.
    if (conn->keep_client && !body_received(conn) &&
        conn->request.expects_continue) {
        static const char CONTINUE[] = "HTTP/1.1 100 Continue\r\n\r\n";
        (void)send(conn->client_fd, CONTINUE, sizeof(CONTINUE) - 1, 0);
    }
    conn->client_io_at = now;
}

static void send_error(Connection *conn, int status, double now) {
    send_own_reply(conn, status, NULL, now);
}

static void log_upstream_error(const Connection *conn, int error_number,
                               const char *what, const char *doing) {
    if (error_number) {
        log_conn(conn, "error", "%s (%d: %s) while %s", what, error_number,
                 strerror(error_number), doing);
    } else {
        log_conn(conn, "error", "%s while %s", what, doing);
    }
}

// Something went wrong upstream before the reply's head was in.
static void fail_upstream(Connection *conn, int status, int error_number,
                          const char *what, const char *doing,
                          double now) {
    log_upstream_error(conn, error_number, what, doing);
    send_error(conn, status, now);
}

static void send_to_client(Connection *conn, double now) {
    Buffer *out = &conn->to_client;

    if (buffer_length(out) == 0) {
        return;
    }
    ssize_t count = send(conn->client_fd, buffer_bytes(out),
                         buffer_length(out), 0);
    if (count < 0) {
        if (!would_block(errno)) {
            log_and_close(conn);  // The client has gone.
        }
        return;
    }
    buffer_consume(out, (size_t)count);
    conn->sent_to_client += (size_t)count;
    conn->client_io_at = now;
    if (buffer_length(out) == 0 && (conn->state == CONN_SENDING_REPLY ||
                                    conn->state == CONN_SENDING_OWN_REPLY)) {
        reply_sent(conn, now);
    }
}

// ---- The request ----

// $host: the request's host or, with none, the server's first name.
static const char *host_variable(const Connection *conn) {
    return *conn->request.host ? conn->request.host
                               : conn->server->names[0].name;
}

// nginx's absolute_redirect: "http://", $host or else the address the
// client reached, the port unless it's 80, then the location's path and the
// query. Fails only when out of memory, or if the socket has no address.
static int build_redirect(const Connection *conn, const Location *location,
                          Buffer *out) {
    const Request *request = &conn->request;
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    char address[INET_ADDRSTRLEN];
    const char *host = host_variable(conn);

    if (getsockname(conn->client_fd, (struct sockaddr *)&local, &length) !=
        0) {
        return 0;
    }
    if (*host == '\0') {
        inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address));
        host = address;
    }
    unsigned port = ntohs(local.sin_port);

    return buffer_appendf(out, "http://%s", host) &&
           (port == 80 || buffer_appendf(out, ":%u", port)) &&
           buffer_append_text(out, location->path) &&
           (!request->query || !*request->query ||
            buffer_appendf(out, "?%s", request->query)) &&
           buffer_append(out, "", 1);
}

static void reply_redirect(Connection *conn, const Location *location,
                           double now) {
    Buffer url;

    buffer_init(&url);
    if (build_redirect(conn, location, &url)) {
        send_own_reply(conn, 301, buffer_bytes(&url), now);
    } else {
        send_error(conn, 500, now);
    }
    buffer_free(&url);
}

// $proxy_add_x_forwarded_for: the client's X-Forwarded-For with its address
// added, or just its address.
static int build_forwarded_for(const Connection *conn, Buffer *out) {
    const char *forwarded = request_find_header(&conn->request,
                                                "X-Forwarded-For");

    if (forwarded) {
        return buffer_appendf(out, "%s, %s", forwarded, conn->remote_addr);
    }
    return buffer_append_text(out, conn->remote_addr);
}

// The variables' values for this request. forwarded is given
// $proxy_add_x_forwarded_for's. Fails only when out of memory.
static int fill_variables(const Connection *conn, Buffer *forwarded,
                          Variables *variables) {
    const Request *request = &conn->request;

    // The trailing NUL makes forwarded's bytes a string.
    if (!build_forwarded_for(conn, forwarded) ||
        !buffer_append(forwarded, "", 1)) {
        return 0;
    }
    variables->host = host_variable(conn);
    variables->remote_addr = conn->remote_addr;
    variables->proxy_host = conn->location->proxy_pass.host;
    variables->proxy_add_x_forwarded_for = buffer_bytes(forwarded);
    variables->path_and_query = request->path_and_query;
    variables->path = request->path;
    variables->query = request->query ? request->query : "";
    return 1;
}

// The request as it goes upstream, and the hash's key if the pool has one.
// Fails only when out of memory.
static int build_request(Connection *conn, Buffer *key) {
    const Location *location = conn->location;
    const char *key_template = location->proxy_pass.upstream->method.key;
    Buffer forwarded;
    Variables variables;

    buffer_init(&forwarded);
    buffer_clear(&conn->to_upstream);
    int ok =
        fill_variables(conn, &forwarded, &variables) &&
        build_upstream_request(
            &conn->to_upstream, &conn->request,
            buffer_bytes(&conn->upstream_uri),
            conn->settings->proxy_http_version, &location->proxy_headers,
            &variables,
            buffer_bytes(&conn->from_client) + conn->head_length) &&
        (!key_template || expand_variables(key, key_template, &variables));
    buffer_free(&forwarded);
    return ok;
}

// Sends the request on an idle connection to the endpoint chosen, or else a
// new one.
static void connect_upstream(Connection *conn, double now) {
    const char *connecting = "connecting to upstream";
    const Upstream *upstream = conn->location->proxy_pass.upstream;
    Buffer key;

    buffer_init(&key);
    int ok = build_request(conn, &key);
    if (ok) {
        conn->endpoint = choose_endpoint(upstream, conn->remote_ip,
                                         buffer_bytes(&key),
                                         buffer_length(&key));
    }
    buffer_free(&key);
    if (!ok) {
        send_error(conn, 500, now);
        return;
    }
    if (!conn->endpoint) {
        fail_upstream(conn, 502, 0, "no live upstreams", connecting, now);
        return;
    }
    conn->upstream_io_at = now;
    set_state(conn, CONN_CONNECTING);
    if (keepalive_take(upstream, conn->endpoint, &conn->upstream)) {
        conn->upstream.requests++;
        set_state(conn, CONN_SENDING_REQUEST);
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail_upstream(conn, 502, errno, "socket() failed", connecting, now);
        return;
    }
    if (fd >= FD_SETSIZE || !set_non_blocking(fd)) {
        close(fd);
        fail_upstream(conn, 502, 0, "no descriptor select() can watch",
                      connecting, now);
        return;
    }

    conn->upstream.fd = fd;
    conn->upstream.requests = 1;
    conn->upstream.opened_at = now;
    const struct sockaddr_in *address = &conn->endpoint->address;
    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0) {
        set_state(conn, CONN_SENDING_REQUEST);
    } else if (errno != EINPROGRESS) {
        fail_upstream(conn, 502, errno, "connect() failed", connecting, now);
    }
}

// nginx never sends these twice, as they might have been acted on.
static int is_idempotent(const char *method) {
    return strcmp(method, "POST") != 0 && strcmp(method, "PATCH") != 0 &&
           strcmp(method, "LOCK") != 0;
}

// An error on the upstream connection before the reply's head was in. On a
// reused connection the endpoint may have closed it just as it was taken, so
// the request is sent again, if that's safe.
static void retry_or_fail_upstream(Connection *conn, int error_number,
                                   const char *what, const char *doing,
                                   double now) {
    if (conn->upstream.requests == 1 ||
        !is_idempotent(conn->request.method)) {
        fail_upstream(conn, 502, error_number, what, doing, now);
        return;
    }
    log_upstream_error(conn, error_number, what, doing);
    close_upstream(conn);
    release_endpoint(conn->location->proxy_pass.upstream, conn->endpoint);
    conn->endpoint = NULL;
    buffer_clear(&conn->from_upstream);
    connect_upstream(conn, now);
}

static void route_request(Connection *conn, double now) {
    conn->server = find_server(conn->group, conn->request.host);
    conn->settings = &conn->server->settings;

    Route route = find_route(conn->server, conn->request.path);

    if (route.kind == ROUTE_NONE) {
        send_error(conn, 404, now);
        return;
    }
    const Location *location = route.location;
    conn->location = location;
    conn->settings = &location->settings;
    if (route.kind == ROUTE_REDIRECT) {
        reply_redirect(conn, location, now);
        return;
    }
    if (!location->has_proxy_pass) {
        send_error(conn, 404, now);
        return;
    }
    // Built now, so the error log can show it from here on. The NUL makes
    // its bytes a string.
    if (!build_upstream_uri(&conn->upstream_uri, &conn->request, location) ||
        !buffer_append(&conn->upstream_uri, "", 1)) {
        send_error(conn, 500, now);
        return;
    }

    size_t limit = conn->settings->client_max_body_size;
    if (limit > 0 && conn->request.content_length > limit) {
        log_conn(conn, "error", "client intended to send too large body: "
                                "%zu bytes", conn->request.content_length);
        send_error(conn, 413, now);
        return;
    }

    if (body_received(conn)) {
        connect_upstream(conn, now);
        return;
    }
    // A client that asked waits for this before sending the body. It's
    // short and the first thing sent, so the socket always takes it whole.
    if (conn->request.expects_continue) {
        static const char CONTINUE[] = "HTTP/1.1 100 Continue\r\n\r\n";
        (void)send(conn->client_fd, CONTINUE, sizeof(CONTINUE) - 1, 0);
    }
    conn->client_io_at = now;
    set_state(conn, CONN_READING_BODY);
}

// Acts on the head in from_client, once it's all in.
static void handle_head(Connection *conn, double now) {
    HeadScan scan = scan_request_head(buffer_bytes(&conn->from_client),
                                      buffer_length(&conn->from_client));
    switch (scan.state) {
        case HEAD_INCOMPLETE:
            return;
        case HEAD_REJECTED:
            send_error(conn, scan.status, now);
            return;
        case HEAD_COMPLETE:
            break;
    }

    conn->head_length = scan.length;
    int status = request_parse(&conn->request,
                               buffer_bytes(&conn->from_client), scan.length);
    if (status) {
        send_error(conn, status, now);
        return;
    }
    route_request(conn, now);
}

static void read_head(Connection *conn, double now) {
    int error_number;

    switch (receive(conn->client_fd, &conn->from_client, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            close_conn(conn);  // Gone before asking anything.
            return;
        case RECEIVED_DATA:
            break;
    }
    handle_head(conn, now);
}

static void read_body(Connection *conn, double now) {
    int error_number;

    switch (receive(conn->client_fd, &conn->from_client, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            close_conn(conn);
            return;
        case RECEIVED_DATA:
            break;
    }
    conn->client_io_at = now;
    if (body_received(conn)) {
        connect_upstream(conn, now);
    }
}

// While the upstream has the request, a client that hangs up has given up:
// nginx's 499.
static void check_client_closed(Connection *conn) {
    int error_number;

    switch (receive(conn->client_fd, &conn->from_client, &error_number)) {
        case RECEIVED_DATA:
            conn->pipelined = 1;
            return;
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            conn->status = STATUS_CLIENT_CLOSED;
            log_and_close(conn);
            return;
    }
}

static void linger(Connection *conn, double now) {
    int error_number;

    switch (receive(conn->client_fd, NULL, &error_number)) {
        case RECEIVED_DATA:
            conn->client_io_at = now;
            return;
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            close_conn(conn);
            return;
    }
}

static void discard_body(Connection *conn, double now) {
    int error_number;

    switch (receive(conn->client_fd, &conn->from_client, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            close_conn(conn);
            return;
        case RECEIVED_DATA:
            break;
    }
    size_t have = buffer_length(&conn->from_client);
    size_t dropped = have < conn->discard_left ? have : conn->discard_left;

    buffer_consume(&conn->from_client, dropped);
    conn->discard_left -= dropped;
    conn->client_io_at = now;
    if (conn->discard_left == 0) {
        wait_for_request(conn, now);
    }
}

// An idle connection that turned readable: the next request, or the close.
static void read_idle(Connection *conn, double now) {
    int error_number;

    switch (receive(conn->client_fd, &conn->from_client, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            close_conn(conn);
            return;
        case RECEIVED_DATA:
            reset_for_request(conn, now);
            handle_head(conn, now);
            return;
    }
}

// ---- The upstream ----

static void send_request(Connection *conn, double now) {
    Buffer *out = &conn->to_upstream;
    ssize_t count = send(conn->upstream.fd, buffer_bytes(out),
                         buffer_length(out), 0);

    if (count < 0) {
        if (!would_block(errno)) {
            retry_or_fail_upstream(conn, errno, "send() failed",
                                   "sending request to upstream", now);
        }
        return;
    }
    buffer_consume(out, (size_t)count);
    conn->upstream_io_at = now;
    if (buffer_length(out) == 0) {
        set_state(conn, CONN_READING_REPLY_HEAD);
    }
}

static void finish_connecting(Connection *conn, double now) {
    int error_number = 0;
    socklen_t length = sizeof(error_number);

    if (getsockopt(conn->upstream.fd, SOL_SOCKET, SO_ERROR, &error_number,
                   &length) != 0) {
        error_number = errno;
    }
    if (error_number) {
        fail_upstream(conn, 502, error_number, "connect() failed",
                      "connecting to upstream", now);
        return;
    }
    conn->upstream_io_at = now;
    set_state(conn, CONN_SENDING_REQUEST);
    send_request(conn, now);
}

// How the body goes to this client: chunks it can't read are decoded, and
// a body with no length of its own is chunked for one that can.
static BodyRewrite choose_body_rewrite(const Connection *conn) {
    int reads_chunks = conn->request.version >= HTTP_1_1;

    switch (conn->reply.framing) {
        case REPLY_NO_BODY:
        case REPLY_LENGTH:
            return REWRITE_NONE;
        case REPLY_CHUNKED:
            return reads_chunks ? REWRITE_NONE : REWRITE_DECHUNK;
        case REPLY_UNTIL_CLOSE:
            return reads_chunks ? REWRITE_CHUNK : REWRITE_NONE;
    }
    return REWRITE_NONE;
}

// The client can find the end of the body without the close.
static int client_finds_end(const Connection *conn) {
    return conn->body_rewrite != REWRITE_DECHUNK &&
           !(conn->reply.framing == REPLY_UNTIL_CLOSE &&
             conn->body_rewrite == REWRITE_NONE);
}

typedef enum {
    BODY_MORE,
    BODY_DONE,
    BODY_INVALID,
    BODY_OUT_OF_MEMORY,
} BodyResult;

// Adds a piece of the reply's body to what goes to the client. *used says
// how much of the piece was the body's.
static BodyResult pass_on_body(Connection *conn, const char *data, size_t size,
                               size_t *used) {
    Buffer *out = &conn->to_client;

    *used = 0;
    switch (conn->reply.framing) {
        case REPLY_NO_BODY:
            return BODY_DONE;
        case REPLY_LENGTH: {
            size_t run = size < conn->reply.length ? size : conn->reply.length;

            if (!buffer_append(out, data, run)) {
                return BODY_OUT_OF_MEMORY;
            }
            *used = run;
            conn->reply.length -= run;
            return conn->reply.length == 0 ? BODY_DONE : BODY_MORE;
        }
        case REPLY_CHUNKED: {
            // Passed on as they came, the chunks are decoded only to find
            // their end.
            int decoded = conn->body_rewrite == REWRITE_DECHUNK;

            switch (chunk_decode(&conn->chunks, data, size,
                                 decoded ? out : NULL, used)) {
                case CHUNKS_OK:
                    break;
                case CHUNKS_INVALID:
                    return BODY_INVALID;
                case CHUNKS_OUT_OF_MEMORY:
                    return BODY_OUT_OF_MEMORY;
            }
            if (!decoded && !buffer_append(out, data, *used)) {
                return BODY_OUT_OF_MEMORY;
            }
            return conn->chunks.state == CHUNK_DONE ? BODY_DONE : BODY_MORE;
        }
        case REPLY_UNTIL_CLOSE: {
            int ok = conn->body_rewrite == REWRITE_CHUNK
                         ? chunk_encode(out, data, size)
                         : buffer_append(out, data, size);

            *used = size;
            return ok ? BODY_MORE : BODY_OUT_OF_MEMORY;
        }
    }
    return BODY_MORE;
}

// What the upstream sent past the reply, which is dropped, and keeps its
// connection from being reused.
static void log_extra_data(const Connection *conn) {
    log_conn(conn, "warn", conn->reply.framing == REPLY_CHUNKED
                               ? "upstream sent data after final chunk"
                               : "upstream sent more data than specified "
                                 "in \"Content-Length\" header");
}

// The upstream's reply is all in: keeps its connection if it can, then
// sends the client what's left.
static void upstream_reply_done(Connection *conn, int reusable, double now) {
    if (reusable && conn->reply.upstream_reusable) {
        keepalive_keep(conn->location->proxy_pass.upstream, conn->endpoint,
                       &conn->upstream, now);
        conn->upstream.fd = -1;
    }
    set_state(conn, CONN_SENDING_REPLY);
    if (buffer_length(&conn->to_client) == 0) {
        reply_sent(conn, now);
    }
}

static void read_reply_head(Connection *conn, double now) {
    const char *reading = "reading response header from upstream";
    Buffer *in = &conn->from_upstream;
    int error_number;

    switch (receive(conn->upstream.fd, in, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
            retry_or_fail_upstream(conn, 0,
                                   "upstream prematurely closed connection",
                                   reading, now);
            return;
        case RECEIVED_ERROR:
            retry_or_fail_upstream(conn, error_number, "recv() failed",
                                   reading, now);
            return;
        case RECEIVED_DATA:
            break;
    }
    conn->upstream_io_at = now;

    size_t head = find_reply_head(buffer_bytes(in), buffer_length(in));
    if (head == 0 ? buffer_length(in) > REPLY_HEAD_LIMIT
                  : head > REPLY_HEAD_LIMIT) {
        fail_upstream(conn, 502, 0, "upstream sent too big header",
                      reading, now);
        return;
    }
    if (head == 0) {
        return;
    }

    int to_head = strcmp(conn->request.method, "HEAD") == 0;
    int status = parse_reply_head(buffer_bytes(in), head, to_head,
                                  &conn->reply);
    if (status) {
        fail_upstream(conn, status, 0, "upstream sent invalid header",
                      reading, now);
        return;
    }
    char connection[80];
    conn->status = conn->reply.status;
    conn->body_rewrite = choose_body_rewrite(conn);
    conn->keep_client = may_keep_client(conn, now) && client_finds_end(conn);
    connection_headers(conn, connection, sizeof(connection));
    if (!write_reply_head(&conn->to_client, buffer_bytes(in), head,
                          conn->body_rewrite, connection)) {
        send_error(conn, 500, now);
        return;
    }
    conn->reply_head_length = buffer_length(&conn->to_client);
    chunk_decoder_init(&conn->chunks);

    // Nothing has gone to the client yet, so a failure can still be answered.
    size_t used;
    buffer_consume(in, head);
    BodyResult result = pass_on_body(conn, buffer_bytes(in), buffer_length(in),
                                     &used);
    size_t extra = buffer_length(in) - used;
    buffer_clear(in);
    conn->client_io_at = now;
    switch (result) {
        case BODY_MORE:
            set_state(conn, CONN_READING_REPLY);
            return;
        case BODY_DONE:
            if (extra) {
                log_extra_data(conn);
            }
            upstream_reply_done(conn, extra == 0, now);
            return;
        case BODY_INVALID:
            fail_upstream(conn, 502, 0,
                          "upstream sent invalid chunked response",
                          reading, now);
            return;
        case BODY_OUT_OF_MEMORY:
            send_error(conn, 500, now);
            return;
    }
}

// The head has gone on, so a failure now can only cut the reply off.
static void read_reply(Connection *conn, double now) {
    Buffer *in = &conn->from_upstream;
    int was_empty = buffer_length(&conn->to_client) == 0;
    int error_number;

    switch (receive(conn->upstream.fd, in, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_DATA:
            break;
        case RECEIVED_END:
            if (conn->reply.framing != REPLY_UNTIL_CLOSE) {
                log_conn(conn, "error", "upstream prematurely closed "
                                        "connection while reading upstream");
                log_and_close(conn);
            } else if (conn->body_rewrite == REWRITE_CHUNK &&
                       !chunk_encode_end(&conn->to_client)) {
                log_and_close(conn);
            } else {
                upstream_reply_done(conn, 0, now);
            }
            return;
        case RECEIVED_ERROR:
            log_conn(conn, "error", "recv() failed (%d: %s) while reading "
                                    "upstream", error_number,
                     strerror(error_number));
            log_and_close(conn);
            return;
    }

    size_t used;
    BodyResult result = pass_on_body(conn, buffer_bytes(in), buffer_length(in),
                                     &used);
    size_t extra = buffer_length(in) - used;
    buffer_clear(in);
    switch (result) {
        case BODY_MORE:
            break;
        case BODY_DONE:
            if (extra) {
                log_extra_data(conn);
            }
            upstream_reply_done(conn, extra == 0, now);
            return;
        case BODY_INVALID:
            log_conn(conn, "error", "upstream sent invalid chunked response "
                                    "while reading upstream");
            log_and_close(conn);
            return;
        case BODY_OUT_OF_MEMORY:
            log_and_close(conn);
            return;
    }
    conn->upstream_io_at = now;
    if (was_empty && buffer_length(&conn->to_client) > 0) {
        conn->client_io_at = now;  // client_send_timeout starts now.
    }
}

// ---- The loop's side ----

void conn_start(Connection *conn, int fd, const struct sockaddr_in *client,
                const AddressGroup *group, double now) {
    conn->id = ++connections_accepted;
    conn->client_fd = fd;
    conn->upstream.fd = -1;
    conn->remote_ip = client->sin_addr;
    inet_ntop(AF_INET, &client->sin_addr, conn->remote_addr,
              sizeof(conn->remote_addr));
    conn->request_count = 0;
    conn->group = group;
    buffer_init(&conn->from_client);
    request_init(&conn->request);
    buffer_init(&conn->upstream_uri);
    buffer_init(&conn->to_upstream);
    buffer_init(&conn->from_upstream);
    buffer_init(&conn->to_client);
    chunk_decoder_init(&conn->chunks);
    conn->accepted_at = now;
    conn->upstream_io_at = now;
    conn->lingering_since = now;
    conn->state = CONN_UNUSED;
    reset_for_request(conn, now);
}

typedef struct {
    int client_read;
    int client_write;
    int upstream_read;
    int upstream_write;
} Interest;

static Interest interest(const Connection *conn) {
    Interest wanted = {0, 0, 0, 0};

    switch (conn->state) {
        case CONN_UNUSED:
            break;
        case CONN_READING_HEAD:
        case CONN_READING_BODY:
        case CONN_LINGERING:
        case CONN_DISCARDING_BODY:
        case CONN_IDLE:
            wanted.client_read = 1;
            break;
        case CONN_CONNECTING:
        case CONN_SENDING_REQUEST:
            wanted.upstream_write = 1;
            // To see it hang up. Once it's sent more, select() would only
            // report that again and again.
            wanted.client_read = !conn->pipelined;
            break;
        case CONN_READING_REPLY_HEAD:
            wanted.upstream_read = 1;
            wanted.client_read = !conn->pipelined;
            break;
        case CONN_READING_REPLY:
            wanted.upstream_read = 1;
            wanted.client_write = buffer_length(&conn->to_client) > 0;
            break;
        case CONN_SENDING_REPLY:
        case CONN_SENDING_OWN_REPLY:
            wanted.client_write = 1;
            break;
    }
    return wanted;
}

static void watch_fd(int fd, int wanted, fd_set *set, int *highest) {
    if (!wanted) {
        return;
    }
    FD_SET(fd, set);
    if (fd > *highest) {
        *highest = fd;
    }
}

void conn_watch(const Connection *conn, fd_set *reads, fd_set *writes,
                int *highest) {
    Interest wanted = interest(conn);

    watch_fd(conn->client_fd, wanted.client_read, reads, highest);
    watch_fd(conn->client_fd, wanted.client_write, writes, highest);
    watch_fd(conn->upstream.fd, wanted.upstream_read, reads, highest);
    watch_fd(conn->upstream.fd, wanted.upstream_write, writes, highest);
}

static void on_client_ready(Connection *conn, double now) {
    switch (conn->state) {
        case CONN_UNUSED:
            break;
        case CONN_READING_HEAD:
            read_head(conn, now);
            break;
        case CONN_READING_BODY:
            read_body(conn, now);
            break;
        case CONN_CONNECTING:
        case CONN_SENDING_REQUEST:
        case CONN_READING_REPLY_HEAD:
            check_client_closed(conn);
            break;
        case CONN_READING_REPLY:
        case CONN_SENDING_REPLY:
        case CONN_SENDING_OWN_REPLY:
            send_to_client(conn, now);
            break;
        case CONN_LINGERING:
            linger(conn, now);
            break;
        case CONN_DISCARDING_BODY:
            discard_body(conn, now);
            break;
        case CONN_IDLE:
            read_idle(conn, now);
            break;
    }
}

static void on_upstream_ready(Connection *conn, double now) {
    switch (conn->state) {
        case CONN_CONNECTING:
            finish_connecting(conn, now);
            break;
        case CONN_SENDING_REQUEST:
            send_request(conn, now);
            break;
        case CONN_READING_REPLY_HEAD:
            read_reply_head(conn, now);
            break;
        case CONN_READING_REPLY:
            read_reply(conn, now);
            break;
        case CONN_UNUSED:
        case CONN_READING_HEAD:
        case CONN_READING_BODY:
        case CONN_SENDING_REPLY:
        case CONN_SENDING_OWN_REPLY:
        case CONN_LINGERING:
        case CONN_DISCARDING_BODY:
        case CONN_IDLE:
            break;
    }
}

void conn_handle_ready(Connection *conn, const fd_set *reads,
                       const fd_set *writes, double now) {
    // Decided before acting, as acting may close a descriptor or open one
    // that another connection had in the sets. Sockets don't block, so
    // acting on a side the state no longer needs is harmless.
    Interest wanted = interest(conn);
    int client_ready =
        (wanted.client_read && FD_ISSET(conn->client_fd, reads)) ||
        (wanted.client_write && FD_ISSET(conn->client_fd, writes));
    int upstream_ready =
        (wanted.upstream_read && FD_ISSET(conn->upstream.fd, reads)) ||
        (wanted.upstream_write && FD_ISSET(conn->upstream.fd, writes));

    if (upstream_ready) {
        on_upstream_ready(conn, now);
    }
    if (client_ready) {
        on_client_ready(conn, now);
    }
}

static double earlier(double a, double b) {
    return a < b ? a : b;
}

int conn_deadline(const Connection *conn, double *at) {
    const Settings *settings = conn->settings;

    switch (conn->state) {
        case CONN_UNUSED:
            return 0;
        case CONN_READING_HEAD:
            *at = conn->request_at +
                  conn->server->settings.client_header_timeout;
            return 1;
        case CONN_READING_BODY:
            *at = conn->client_io_at + settings->client_body_timeout;
            return 1;
        case CONN_CONNECTING:
            *at = conn->upstream_io_at + settings->proxy_connect_timeout;
            return 1;
        case CONN_SENDING_REQUEST:
            *at = conn->upstream_io_at + settings->proxy_send_timeout;
            return 1;
        case CONN_READING_REPLY_HEAD:
            *at = conn->upstream_io_at + settings->proxy_read_timeout;
            return 1;
        case CONN_READING_REPLY:
            *at = conn->upstream_io_at + settings->proxy_read_timeout;
            if (buffer_length(&conn->to_client) > 0) {
                *at = earlier(*at, conn->client_io_at +
                                       settings->client_send_timeout);
            }
            return 1;
        case CONN_SENDING_REPLY:
        case CONN_SENDING_OWN_REPLY:
            *at = conn->client_io_at + settings->client_send_timeout;
            return 1;
        case CONN_LINGERING:
        case CONN_DISCARDING_BODY:
            *at = earlier(conn->lingering_since + LINGERING_TIME,
                          conn->client_io_at + LINGERING_TIMEOUT);
            return 1;
        case CONN_IDLE:
            *at = conn->client_io_at + settings->keepalive_timeout;
            return 1;
    }
    return 0;
}

static void upstream_timed_out(Connection *conn, const char *doing,
                               double now) {
    fail_upstream(conn, 504, ETIMEDOUT, "upstream timed out", doing, now);
}

void conn_expire(Connection *conn, double now) {
    double at;

    if (!conn_deadline(conn, &at) || now < at) {
        return;
    }
    switch (conn->state) {
        case CONN_UNUSED:
            break;
        case CONN_READING_HEAD:
        case CONN_READING_BODY:
            send_error(conn, 408, now);
            break;
        case CONN_CONNECTING:
            upstream_timed_out(conn, "connecting to upstream", now);
            break;
        case CONN_SENDING_REQUEST:
            upstream_timed_out(conn, "sending request to upstream", now);
            break;
        case CONN_READING_REPLY_HEAD:
            upstream_timed_out(conn, "reading response header from upstream",
                               now);
            break;
        case CONN_READING_REPLY:
            // Either side may have run out; only the upstream's is an error.
            if (now >= conn->upstream_io_at +
                           conn->settings->proxy_read_timeout) {
                log_conn(conn, "error", "upstream timed out (%d: %s) while "
                                        "reading upstream", ETIMEDOUT,
                         strerror(ETIMEDOUT));
            }
            log_and_close(conn);
            break;
        case CONN_SENDING_REPLY:
        case CONN_SENDING_OWN_REPLY:
            log_and_close(conn);
            break;
        case CONN_LINGERING:
        case CONN_DISCARDING_BODY:
        case CONN_IDLE:
            close_conn(conn);
            break;
    }
}
