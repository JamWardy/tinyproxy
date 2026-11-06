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
#include "proxy.h"
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

static int has_upstream(ConnState state) {
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
        case CONN_SENDING_ERROR:
        case CONN_LINGERING:
            return 0;
    }
    return 0;
}

// Moves to state, closing the upstream connection if state has none.
static void enter(Connection *conn, ConnState state) {
    if (has_upstream(conn->state) && !has_upstream(state)) {
        close(conn->upstream_fd);
    }
    conn->state = state;
}

static void release(Connection *conn) {
    enter(conn, CONN_UNUSED);
    close(conn->client_fd);
    buffer_free(&conn->from_client);
    buffer_free(&conn->upstream_uri);
    buffer_free(&conn->to_upstream);
    buffer_free(&conn->from_upstream);
    buffer_free(&conn->to_client);
    request_free(&conn->request);
}

void conn_close(Connection *conn) {
    release(conn);
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
    entry.body_bytes_sent = conn->sent > conn->reply_head_length
                                ? conn->sent - conn->reply_head_length
                                : 0;
    entry.referer = request_find_header(&conn->request, "Referer");
    entry.user_agent = request_find_header(&conn->request, "User-Agent");
    access_log_write(log->fd, &entry);
}

// An error about this request, with nginx's details after it.
static void log_conn_error(const Connection *conn, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

static void log_conn_error(const Connection *conn, const char *format, ...) {
    char message[512];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // As nginx's, there's an upstream only once a server's been chosen.
    char upstream[1024] = "";
    if (conn->peer) {
        snprintf(upstream, sizeof(upstream), ", upstream: \"http://%s%s\"",
                 conn->peer->text, buffer_bytes(&conn->upstream_uri));
    }

    const Request *request = &conn->request;
    error_log("error",
              "*%llu %s, client: %s, request: \"%s\"%s, host: \"%s\"",
              conn->number, message, conn->remote_addr,
              request->line ? request->line : "", upstream,
              request->host ? request->host : "");
}

// The request has its answer, or never will: logs it, and closes.
static void finish(Connection *conn) {
    log_request(conn);
    release(conn);
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

// tinyproxy's own reply in place of anything else, closing the upstream
// connection if there is one. redirect is a 301's Location, or NULL.
static void reply_own(Connection *conn, int status, const char *redirect,
                      double now) {
    const char *method = conn->request.method;
    int with_body = !(method && strcmp(method, "HEAD") == 0);

    enter(conn, CONN_SENDING_ERROR);
    buffer_clear(&conn->to_client);
    if (!build_error_reply(&conn->to_client, status, with_body, redirect)) {
        release(conn);
        return;
    }
    conn->status = status == STATUS_HEADER_TOO_LARGE ? 400 : status;
    conn->reply_head_length = find_reply_head(buffer_bytes(&conn->to_client),
                                              buffer_length(&conn->to_client));
    conn->client_io_at = now;
}

static void reply_error(Connection *conn, int status, double now) {
    reply_own(conn, status, NULL, now);
}

// Something went wrong upstream before the reply's head was in.
static void upstream_failed(Connection *conn, int status, int error_number,
                            const char *what, const char *doing,
                            double now) {
    if (error_number) {
        log_conn_error(conn, "%s (%d: %s) while %s", what, error_number,
                       strerror(error_number), doing);
    } else {
        log_conn_error(conn, "%s while %s", what, doing);
    }
    reply_error(conn, status, now);
}

// Shuts the sending side, then waits for the client to close, so the reply
// isn't lost to a reset for data it sent that was never read.
static void start_lingering(Connection *conn, double now) {
    log_request(conn);
    shutdown(conn->client_fd, SHUT_WR);
    conn->lingering_since = now;
    conn->client_io_at = now;
    enter(conn, CONN_LINGERING);
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
            finish(conn);  // The client has gone.
        }
        return;
    }
    buffer_consume(out, (size_t)count);
    conn->sent += (size_t)count;
    conn->client_io_at = now;
    if (buffer_length(out) > 0) {
        return;
    }

    if (conn->state == CONN_SENDING_REPLY) {
        finish(conn);
    } else if (conn->state == CONN_SENDING_ERROR) {
        start_lingering(conn, now);
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
           (!request->args || !*request->args ||
            buffer_appendf(out, "?%s", request->args)) &&
           buffer_append(out, "", 1);
}

static void reply_redirect(Connection *conn, const Location *location,
                           double now) {
    Buffer url;

    buffer_init(&url);
    if (build_redirect(conn, location, &url)) {
        reply_own(conn, 301, buffer_bytes(&url), now);
    } else {
        reply_error(conn, 500, now);
    }
    buffer_free(&url);
}

static int body_received(const Connection *conn) {
    return buffer_length(&conn->from_client) - conn->head_length >=
           conn->request.content_length;
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

static int build_request(Connection *conn) {
    const Location *location = conn->location;
    Buffer forwarded;

    buffer_init(&forwarded);
    // The trailing NUL makes forwarded's bytes a string.
    int ok = build_forwarded_for(conn, &forwarded) &&
             buffer_append(&forwarded, "", 1);
    if (ok) {
        Variables variables;
        variables.host = host_variable(conn);
        variables.remote_addr = conn->remote_addr;
        variables.proxy_host = location->proxy_pass.host;
        variables.proxy_add_x_forwarded_for = buffer_bytes(&forwarded);
        ok = build_upstream_request(
            &conn->to_upstream, &conn->request,
            buffer_bytes(&conn->upstream_uri),
            conn->settings->proxy_http_version, &location->proxy_headers,
            &variables, buffer_bytes(&conn->from_client) + conn->head_length);
    }
    buffer_free(&forwarded);
    return ok;
}

static void connect_upstream(Connection *conn, double now) {
    const char *connecting = "connecting to upstream";

    if (!build_request(conn)) {
        reply_error(conn, 500, now);
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        upstream_failed(conn, 502, errno, "socket() failed", connecting, now);
        return;
    }
    if (fd >= FD_SETSIZE || !set_non_blocking(fd)) {
        close(fd);
        upstream_failed(conn, 502, 0, "no descriptor select() can watch",
                        connecting, now);
        return;
    }

    conn->upstream_fd = fd;
    conn->upstream_io_at = now;
    enter(conn, CONN_CONNECTING);
    conn->peer = choose_peer(conn->location->proxy_pass.upstream);
    const struct sockaddr_in *address = &conn->peer->address;
    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0) {
        enter(conn, CONN_SENDING_REQUEST);
    } else if (errno != EINPROGRESS) {
        upstream_failed(conn, 502, errno, "connect() failed", connecting, now);
    }
}

static void start_request(Connection *conn, double now) {
    conn->server = find_server(conn->group, conn->request.host);
    conn->settings = &conn->server->settings;

    Route route = find_route(conn->server, conn->request.uri);

    if (route.kind == ROUTE_NONE) {
        reply_error(conn, 404, now);
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
        reply_error(conn, 404, now);
        return;
    }
    // Built now, so the error log can show it from here on. The NUL makes
    // its bytes a string.
    if (!build_upstream_uri(&conn->upstream_uri, &conn->request, location) ||
        !buffer_append(&conn->upstream_uri, "", 1)) {
        reply_error(conn, 500, now);
        return;
    }

    size_t limit = conn->settings->client_max_body_size;
    if (limit > 0 && conn->request.content_length > limit) {
        log_conn_error(conn, "client intended to send too large body: %zu "
                             "bytes", conn->request.content_length);
        reply_error(conn, 413, now);
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
    enter(conn, CONN_READING_BODY);
}

static void read_head(Connection *conn, double now) {
    int error_number;

    switch (receive(conn->client_fd, &conn->from_client, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            release(conn);  // Gone before asking anything.
            return;
        case RECEIVED_DATA:
            break;
    }

    HeadScan scan = scan_request_head(buffer_bytes(&conn->from_client),
                                      buffer_length(&conn->from_client));
    switch (scan.state) {
        case HEAD_INCOMPLETE:
            return;
        case HEAD_REJECTED:
            reply_error(conn, scan.status, now);
            return;
        case HEAD_COMPLETE:
            break;
    }

    conn->head_length = scan.length;
    int status = request_parse(&conn->request,
                               buffer_bytes(&conn->from_client), scan.length);
    if (status) {
        reply_error(conn, status, now);
        return;
    }
    start_request(conn, now);
}

static void read_body(Connection *conn, double now) {
    int error_number;

    switch (receive(conn->client_fd, &conn->from_client, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            release(conn);
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

    switch (receive(conn->client_fd, NULL, &error_number)) {
        case RECEIVED_DATA:  // More than the request; it's dropped.
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
        case RECEIVED_ERROR:
            conn->status = STATUS_CLIENT_CLOSED;
            finish(conn);
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
            release(conn);
            return;
    }
}

// ---- The upstream ----

static void send_request(Connection *conn, double now) {
    Buffer *out = &conn->to_upstream;
    ssize_t count = send(conn->upstream_fd, buffer_bytes(out),
                         buffer_length(out), 0);

    if (count < 0) {
        if (!would_block(errno)) {
            upstream_failed(conn, 502, errno, "send() failed",
                            "sending request to upstream", now);
        }
        return;
    }
    buffer_consume(out, (size_t)count);
    conn->upstream_io_at = now;
    if (buffer_length(out) == 0) {
        enter(conn, CONN_READING_REPLY_HEAD);
    }
}

static void finish_connecting(Connection *conn, double now) {
    int error_number = 0;
    socklen_t length = sizeof(error_number);

    if (getsockopt(conn->upstream_fd, SOL_SOCKET, SO_ERROR, &error_number,
                   &length) != 0) {
        error_number = errno;
    }
    if (error_number) {
        upstream_failed(conn, 502, error_number, "connect() failed",
                        "connecting to upstream", now);
        return;
    }
    conn->upstream_io_at = now;
    enter(conn, CONN_SENDING_REQUEST);
    send_request(conn, now);
}

// Adds a piece of the reply's body to what goes to the client.
static ChunksResult pass_on(Connection *conn, const char *data, size_t size) {
    switch (conn->reply_body) {
        case REPLY_BODY_AS_IS:
            return buffer_append(&conn->to_client, data, size)
                       ? CHUNKS_OK
                       : CHUNKS_OUT_OF_MEMORY;
        case REPLY_BODY_DECODED:
            return chunk_decode(&conn->chunks, data, size, &conn->to_client);
    }
    return CHUNKS_OK;
}

static void read_reply_head(Connection *conn, double now) {
    const char *reading = "reading response header from upstream";
    Buffer *in = &conn->from_upstream;
    int error_number;

    switch (receive(conn->upstream_fd, in, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_END:
            upstream_failed(conn, 502, 0,
                            "upstream prematurely closed connection",
                            reading, now);
            return;
        case RECEIVED_ERROR:
            upstream_failed(conn, 502, error_number, "recv() failed",
                            reading, now);
            return;
        case RECEIVED_DATA:
            break;
    }
    conn->upstream_io_at = now;

    size_t head = find_reply_head(buffer_bytes(in), buffer_length(in));
    if (head == 0 ? buffer_length(in) > REPLY_HEAD_LIMIT
                  : head > REPLY_HEAD_LIMIT) {
        upstream_failed(conn, 502, 0, "upstream sent too big header",
                        reading, now);
        return;
    }
    if (head == 0) {
        return;
    }

    ReplyHead reply;
    int status = rewrite_reply_head(&conn->to_client, buffer_bytes(in), head,
                                    conn->request.version, &reply);
    if (status) {
        upstream_failed(conn, status, 0, "upstream sent invalid header",
                        reading, now);
        return;
    }
    conn->status = reply.status;
    conn->reply_head_length = buffer_length(&conn->to_client);
    conn->reply_body = reply.decode_chunks ? REPLY_BODY_DECODED
                                           : REPLY_BODY_AS_IS;
    chunk_decoder_init(&conn->chunks);

    // Nothing has gone to the client yet, so a failure can still be answered.
    switch (pass_on(conn, buffer_bytes(in) + head, buffer_length(in) - head)) {
        case CHUNKS_OK:
            break;
        case CHUNKS_INVALID:
            upstream_failed(conn, 502, 0,
                            "upstream sent invalid chunked response",
                            reading, now);
            return;
        case CHUNKS_OUT_OF_MEMORY:
            reply_error(conn, 500, now);
            return;
    }
    buffer_clear(in);
    conn->client_io_at = now;
    enter(conn, CONN_READING_REPLY);
}

// The head has gone on, so a failure now can only cut the reply off.
static void read_reply(Connection *conn, double now) {
    Buffer *in = &conn->from_upstream;
    int was_empty = buffer_length(&conn->to_client) == 0;
    int error_number;

    switch (receive(conn->upstream_fd, in, &error_number)) {
        case RECEIVED_NOTHING:
            return;
        case RECEIVED_DATA:
            break;
        case RECEIVED_END:
            enter(conn, CONN_SENDING_REPLY);
            if (buffer_length(&conn->to_client) == 0) {
                finish(conn);
            }
            return;
        case RECEIVED_ERROR:
            log_conn_error(conn, "recv() failed (%d: %s) while reading "
                                 "upstream", error_number,
                           strerror(error_number));
            finish(conn);
            return;
    }

    ChunksResult result = pass_on(conn, buffer_bytes(in), buffer_length(in));
    buffer_clear(in);
    if (result != CHUNKS_OK) {
        if (result == CHUNKS_INVALID) {
            log_conn_error(conn, "upstream sent invalid chunked response "
                                 "while reading upstream");
        }
        finish(conn);
        return;
    }
    conn->upstream_io_at = now;
    if (was_empty && buffer_length(&conn->to_client) > 0) {
        conn->client_io_at = now;  // send_timeout starts now.
    }
}

// ---- The loop's side ----

void conn_start(Connection *conn, int fd, const struct sockaddr_in *peer,
                const AddressGroup *group, double now) {
    const Server *server = group->default_server;

    conn->state = CONN_READING_HEAD;
    conn->number = ++connections_accepted;
    conn->client_fd = fd;
    conn->upstream_fd = -1;
    inet_ntop(AF_INET, &peer->sin_addr, conn->remote_addr,
              sizeof(conn->remote_addr));
    conn->group = group;
    conn->server = server;
    conn->location = NULL;
    conn->peer = NULL;
    conn->settings = &server->settings;
    buffer_init(&conn->from_client);
    conn->head_length = 0;
    request_init(&conn->request);
    buffer_init(&conn->upstream_uri);
    buffer_init(&conn->to_upstream);
    buffer_init(&conn->from_upstream);
    buffer_init(&conn->to_client);
    conn->reply_body = REPLY_BODY_AS_IS;
    chunk_decoder_init(&conn->chunks);
    conn->status = 0;
    conn->reply_head_length = 0;
    conn->sent = 0;
    conn->accepted_at = now;
    conn->client_io_at = now;
    conn->upstream_io_at = now;
    conn->lingering_since = now;
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
            wanted.client_read = 1;
            break;
        case CONN_CONNECTING:
        case CONN_SENDING_REQUEST:
            wanted.upstream_write = 1;
            wanted.client_read = 1;  // To see it hang up.
            break;
        case CONN_READING_REPLY_HEAD:
            wanted.upstream_read = 1;
            wanted.client_read = 1;
            break;
        case CONN_READING_REPLY:
            wanted.upstream_read = 1;
            wanted.client_write = buffer_length(&conn->to_client) > 0;
            break;
        case CONN_SENDING_REPLY:
        case CONN_SENDING_ERROR:
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
    watch_fd(conn->upstream_fd, wanted.upstream_read, reads, highest);
    watch_fd(conn->upstream_fd, wanted.upstream_write, writes, highest);
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
        case CONN_SENDING_ERROR:
            send_to_client(conn, now);
            break;
        case CONN_LINGERING:
            linger(conn, now);
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
        case CONN_SENDING_ERROR:
        case CONN_LINGERING:
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
        (wanted.upstream_read && FD_ISSET(conn->upstream_fd, reads)) ||
        (wanted.upstream_write && FD_ISSET(conn->upstream_fd, writes));

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
            *at = conn->accepted_at +
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
                                       settings->send_timeout);
            }
            return 1;
        case CONN_SENDING_REPLY:
        case CONN_SENDING_ERROR:
            *at = conn->client_io_at + settings->send_timeout;
            return 1;
        case CONN_LINGERING:
            *at = earlier(conn->lingering_since + LINGERING_TIME,
                          conn->client_io_at + LINGERING_TIMEOUT);
            return 1;
    }
    return 0;
}

static void upstream_timed_out(Connection *conn, const char *doing,
                               double now) {
    upstream_failed(conn, 504, ETIMEDOUT, "upstream timed out", doing, now);
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
            reply_error(conn, 408, now);
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
                log_conn_error(conn, "upstream timed out (%d: %s) while "
                                     "reading upstream", ETIMEDOUT,
                               strerror(ETIMEDOUT));
            }
            finish(conn);
            break;
        case CONN_SENDING_REPLY:
        case CONN_SENDING_ERROR:
            finish(conn);
            break;
        case CONN_LINGERING:
            release(conn);
            break;
    }
}
