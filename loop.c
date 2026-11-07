#include "loop.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "conn.h"
#include "keepalive.h"
#include "log.h"

#define LISTEN_BACKLOG 511  // nginx's default on Linux.

typedef struct {
    int fd;
    const ListenPort *port;
    // NULL for a socket on every address, where each connection's group is
    // found by the address it arrived on.
    const AddressGroup *group;
} Listener;

typedef struct {
    Listener *items;
    size_t count;
} Listeners;

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static double now_seconds(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int open_listener(const AddressGroup *where) {
    int yes = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        error_log("emerg", "socket() failed (%d: %s)", errno,
                  strerror(errno));
        return -1;
    }
    // Lets a restarted proxy bind while old connections are in TIME_WAIT.
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(fd, (const struct sockaddr *)&where->address,
             sizeof(where->address)) != 0 ||
        listen(fd, LISTEN_BACKLOG) != 0 || !set_non_blocking(fd)) {
        error_log("emerg", "bind() to %s failed (%d: %s)", where->text,
                  errno, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void close_listeners(Listeners *listeners) {
    for (size_t i = 0; i < listeners->count; i++) {
        close(listeners->items[i].fd);
    }
    free(listeners->items);
}

static const AddressGroup *every_address_group(const ListenPort *port) {
    for (size_t i = 0; i < port->group_count; i++) {
        if (port->groups[i].address.sin_addr.s_addr == htonl(INADDR_ANY)) {
            return &port->groups[i];
        }
    }
    return NULL;
}

static int add_listener(Listeners *listeners, const ListenPort *port,
                        const AddressGroup *bound, const AddressGroup *group) {
    int fd = open_listener(bound);

    if (fd < 0) {
        return 0;
    }
    listeners->items[listeners->count].fd = fd;
    listeners->items[listeners->count].port = port;
    listeners->items[listeners->count].group = group;
    listeners->count++;
    return 1;
}

// As nginx: a port with any server on every address gets one socket there,
// which takes the other addresses' connections too; otherwise each address
// gets its own.
static int open_port(Listeners *listeners, const ListenPort *port) {
    const AddressGroup *every = every_address_group(port);

    if (every) {
        return add_listener(listeners, port, every, NULL);
    }
    for (size_t i = 0; i < port->group_count; i++) {
        if (!add_listener(listeners, port, &port->groups[i],
                          &port->groups[i])) {
            return 0;
        }
    }
    return 1;
}

static int open_listeners(const Config *config, Listeners *listeners) {
    size_t total = 0;

    for (size_t i = 0; i < config->port_count; i++) {
        total += config->ports[i].group_count;
    }
    listeners->count = 0;
    listeners->items = malloc((total ? total : 1) * sizeof(Listener));
    if (!listeners->items) {
        error_log("emerg", "out of memory");
        return 0;
    }

    for (size_t i = 0; i < config->port_count; i++) {
        if (!open_port(listeners, &config->ports[i])) {
            close_listeners(listeners);
            return 0;
        }
    }
    return 1;
}

// The group for the address a connection on an every-address socket
// arrived on, or the every-address group if that address has none.
static const AddressGroup *arrival_group(const ListenPort *port, int fd) {
    struct sockaddr_in local;
    socklen_t length = sizeof(local);

    if (getsockname(fd, (struct sockaddr *)&local, &length) == 0) {
        for (size_t i = 0; i < port->group_count; i++) {
            if (port->groups[i].address.sin_addr.s_addr ==
                local.sin_addr.s_addr) {
                return &port->groups[i];
            }
        }
    }
    return every_address_group(port);
}

static Connection *free_slot(Connection *connections) {
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].state == CONN_UNUSED) {
            return &connections[i];
        }
    }
    return NULL;
}

// With none free, as nginx: closes the connection that has waited longest
// for its next request, and takes its slot.
static Connection *reuse_idle(Connection *connections) {
    Connection *oldest = NULL;

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        Connection *conn = &connections[i];

        if (conn->state == CONN_KEEPALIVE &&
            (!oldest || conn->client_io_at < oldest->client_io_at)) {
            oldest = conn;
        }
    }
    if (oldest) {
        error_log("warn", "%d worker_connections are not enough, reusing "
                          "connections", MAX_CONNECTIONS);
        conn_close(oldest);
    }
    return oldest;
}

// Several clients may be waiting in the backlog, so accepts until none are.
static void accept_connections(const Listener *listener,
                               Connection *connections, double now) {
    while (1) {
        struct sockaddr_in peer;
        socklen_t length = sizeof(peer);
        int fd = accept(listener->fd, (struct sockaddr *)&peer, &length);

        if (fd < 0) {
            return;  // EAGAIN: no more for now; anything else, try later.
        }
        Connection *conn = free_slot(connections);
        if (!conn && fd < FD_SETSIZE) {
            conn = reuse_idle(connections);
        }
        if (!conn || fd >= FD_SETSIZE) {
            error_log("alert", "%d worker_connections are not enough",
                      MAX_CONNECTIONS);
            close(fd);
            continue;
        }
        if (!set_non_blocking(fd)) {
            close(fd);
            continue;
        }
        const AddressGroup *group = listener->group
                                        ? listener->group
                                        : arrival_group(listener->port, fd);
        conn_start(conn, fd, &peer, group, now);
    }
}

// Until the nearest deadline, a connection's or an idle upstream one's, or
// no limit if none has one.
static struct timespec *select_timeout(const Connection *connections,
                                       struct timespec *timeout) {
    double nearest = 0;
    int has_deadline = keepalive_deadline(&nearest);

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        double at;

        if (conn_deadline(&connections[i], &at) &&
            (!has_deadline || at < nearest)) {
            nearest = at;
            has_deadline = 1;
        }
    }
    if (!has_deadline) {
        return NULL;
    }

    double wait = nearest - now_seconds();
    if (wait < 0) {
        wait = 0;
    }
    timeout->tv_sec = (time_t)wait;
    timeout->tv_nsec = (long)((wait - (double)timeout->tv_sec) * 1e9);
    return timeout;
}

// A signal is let in only while pselect waits, so one that arrives between
// checking stop_requested and waiting can't be missed: it ends the wait.
static int catch_stop_signals(sigset_t *while_waiting) {
    struct sigaction action;
    sigset_t blocked;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);

    // A peer that hangs up mid-send would otherwise kill the proxy.
    signal(SIGPIPE, SIG_IGN);

    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0 &&
           sigprocmask(SIG_BLOCK, &blocked, while_waiting) == 0;
}

static int watch(const Listeners *listeners, const Connection *connections,
                 fd_set *reads, fd_set *writes) {
    int highest = -1;

    FD_ZERO(reads);
    FD_ZERO(writes);
    for (size_t i = 0; i < listeners->count; i++) {
        FD_SET(listeners->items[i].fd, reads);
        if (listeners->items[i].fd > highest) {
            highest = listeners->items[i].fd;
        }
    }
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        conn_watch(&connections[i], reads, writes, &highest);
    }
    keepalive_watch(reads, &highest);
    return highest;
}

static void handle_ready(const Listeners *listeners, Connection *connections,
                         const fd_set *reads, const fd_set *writes) {
    double now = now_seconds();

    // First, so a connection can't take an idle one that has just closed.
    keepalive_handle_ready(reads);
    for (size_t i = 0; i < listeners->count; i++) {
        if (FD_ISSET(listeners->items[i].fd, reads)) {
            accept_connections(&listeners->items[i], connections, now);
        }
    }
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        conn_handle_ready(&connections[i], reads, writes, now);
    }

    now = now_seconds();
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        conn_expire(&connections[i], now);
    }
    keepalive_expire(now);
}

int loop_run(const Config *config) {
    static Connection connections[MAX_CONNECTIONS];  // Too big for the stack.
    Listeners listeners;
    sigset_t while_waiting;

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        connections[i].state = CONN_UNUSED;
    }
    if (!catch_stop_signals(&while_waiting)) {
        perror("tinyproxy: signals");
        return 1;
    }
    if (!open_listeners(config, &listeners)) {
        return 1;
    }
    if (!keepalive_init(config) ||
        !balancers_init(config->upstreams, config->upstream_count)) {
        error_log("emerg", "out of memory");
        keepalive_free();
        close_listeners(&listeners);
        return 1;
    }

    while (!stop_requested) {
        fd_set reads;
        fd_set writes;
        struct timespec timeout;
        int highest = watch(&listeners, connections, &reads, &writes);

        if (pselect(highest + 1, &reads, &writes, NULL,
                    select_timeout(connections, &timeout),
                    &while_waiting) < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_log("emerg", "select() failed (%d: %s)", errno,
                      strerror(errno));
            break;
        }
        handle_ready(&listeners, connections, &reads, &writes);
    }

    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].state != CONN_UNUSED) {
            conn_close(&connections[i]);
        }
    }
    keepalive_free();
    balancers_free();
    close_listeners(&listeners);
    return 0;
}
