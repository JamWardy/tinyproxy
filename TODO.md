# TODO

How it works is in `DESIGN.md`. The stages are the ones in `../TODO.md`.

```
tinyproxy/
  Makefile
  main.c              -c, reading the config, starting the loop
  config/
    config.c/.h       loading the nginx-style config file, and freeing it
    internal.h        what the files below share; not for the rest
    parse.c           tokens, blocks, and values such as times and sizes
    directives.c      the directive table and the handlers for
                      http, server and location blocks
    upstreams.c       upstream blocks, their hosts and methods, and
                      linking each proxy_pass to its pool
    inherit.c         settings inherited block to block, and servers
                      grouped by the address they listen on
  core/
    loop.c/.h         the listening sockets, select() and deadlines
    conn.c/.h         each connection's state machine
    route.c/.h        choosing the server block and the location
  http/
    request.c/.h      finding and parsing the request's head, with its
                      limits
    proxy.c/.h        the request as it goes upstream, the reply as it
                      comes back
    chunked.c/.h      decoding and encoding chunked bodies
    uri.c/.h          normalising the path, and escaping it again
    error_page.c/.h   tinyproxy's own replies and nginx's pages
    http.c/.h         what those share: header types, versions, line
                      helpers
  balance/
    upstream.c/.h     upstream pools and choosing an endpoint from one
    hash.c/.h         ip_hash, hash and hash consistent, with CRC32
    keepalive.c/.h    idle upstream connections, kept for the next
                      request
  python/
    embed.c/.h        the interpreter, and calling each phase's hook
    request_object.c/.h
                      the request object, as the hooks see the
                      request: its type and its life
    request_attributes.c
                      its attributes, such as request.path
    request_methods.c its methods, such as request.respond()
    convert.c/.h      values between C and Python, checked when they
                      come from a hook
  util/
    buffer.c/.h       growing byte buffers, consumed from the front
    log.c/.h          the access log and the error log
    variables.c/.h    $host and the others, for proxy_set_header and hash
```

- [x] Split `config.c` and move the files into the folders above

## Stage 1: forwarding

- [x] The `Makefile` and `-c`
- [x] The config parser, for `http`, `server`, `listen`, `location`,
      `proxy_pass`, `proxy_set_header`, `access_log` and the timeouts
- [x] The loop: non-blocking listening socket, `select()`, accept,
      deadlines
- [x] Reading the request, with its limits and timeouts
- [x] Connecting upstream and sending the request on, with its headers
- [x] Reading the reply and sending it back as it comes
- [x] `proxy_http_version`, and chunked replies passed on or decoded
- [x] The error pages and the gentle close
- [x] The access log
- [x] Try it by hand: `curl` through to search; a stopped service
      (`502`); a service that never answers (`504`); idle `nc`s while
      `curl` still gets answers; a slow order while a search goes
      through

## Stages 2 to 7

- [x] 2: path routing, as in `DESIGN.md`:
  - [x] Several `location`s, `location =`, `^~`, duplicates and
        nested ones refused
  - [x] A path in `proxy_pass`
  - [x] Normalising the path into `$path`, in `uri.c`
  - [x] Matching, exact then the longest prefix, and the trailing-slash
        `301`, in `route.c`
  - [x] Rewriting the URI sent upstream
  - [x] Try it by hand: exact against prefix, the longest prefix,
        `/search/../orders`, `%2e%2e`, a path in `proxy_pass` with a
        query, no match (`404`), the `301`, a duplicate location, then
        the shop's routes with `curl` to each service
- [x] 3: host routing, as in `DESIGN.md`:
  - [x] `server_name`: exact names, `""` and wildcards, with regex
        names refused
  - [x] `default_server` on `listen`, and two in a group refused
  - [x] Servers grouped by address and port, with one socket on every
        address when a server on the port asks for it, and the group
        picked by the address a connection arrived on
  - [x] Choosing the server by `Host`, and `$host` from its first name
        when there's no `Host`
  - [x] The `[warn]` for conflicting names
  - [x] Try it by hand: two names on one port, `*.` and `.` and `.*`
        wildcards and which wins, no `Host` with and without a `""`
        server, `default_server`, a specific address beside the
        every-address one, and each config error
- [x] 4: upstream pools, as in `DESIGN.md`:
  - [x] `upstream` blocks and their `host`s, with their errors, in
        `upstream.c`
  - [x] Every `proxy_pass` to a pool, named or not, looked up once the
        file is read
  - [x] A host name's every address as an endpoint
  - [x] Choosing the endpoint, the pool's first until stage 6
  - [x] The endpoint's address in the error log
  - [x] Try it by hand: a pool and a plain address side by side, an
        `upstream` after its use, each config error, a pool whose
        endpoint is down (`502`), and the shop through pools
- [x] 5: keep-alive, as in `DESIGN.md`:
  - [x] `keepalive_timeout`, `keepalive_max_requests` and
        `keepalive_lifetime`, for clients and in `upstream` blocks, and
        `keepalive_max_idle`
  - [x] Where each reply ends: `Content-Length`, the last chunk, or no
        body, and chunking one with neither for HTTP/1.1
  - [x] Clients: `IDLE` between requests, pipelined requests, own
        replies that keep the connection, `DISCARDING_BODY`, and
        reusing the longest-idle connection when they run out
  - [x] Upstreams: the idle connections in `keepalive.c`, taking and
        keeping them, and sending a request again when a reused one
        fails
  - [x] Try it by hand: several requests on one `curl` connection,
        pipelined requests with `nc`, HTTP/1.0 with and without
        `keep-alive`, `keepalive_max_requests 2`, a `404` with a body
        still to come, the shop through pools with
        `keepalive_max_idle`, one upstream
        connection reused across clients, a service restarted under
        tinyproxy (a `GET` sent again, a `POST` getting `502`), and
        idle connections timing out on both sides
- [x] 6: load balancing, as in `DESIGN.md`:
  - [x] `weight=`, `down` and `max_conns=` on a `host`, and
        `least_conn`, `ip_hash` and `hash`, with their errors and the
        `load balancing method redefined` warning
  - [x] Each pool's runtime state, as `keepalive.c` keeps it, since the
        config is `const` by then: each endpoint's score and active
        requests, in `upstream.c`, set up and freed by the loop
  - [x] Smooth weighted round robin and `least_conn`, in `upstream.c`;
        `choose_endpoint` counts the request, and a new
        `release_endpoint` uncounts it, called by `set_state` where it
        closes the upstream socket and by `retry_or_fail_upstream`
        before it chooses again
  - [x] `$path_and_query`, `$path` and `$query`, in `variables.c`
  - [x] `ip_hash`, `hash` and `hash consistent`, with CRC32 and the
        ketama ring, in `hash.c`; the key is expanded per request in
        `conn.c`
  - [x] `down`, `max_conns=`, the 20 tries and the `502`
  - [x] Try it by hand: two tinyhttp echoes with `weight=3` and
        `weight=1`, the order in their logs; `least_conn` with a slow
        request holding one; `ip_hash` from two client addresses
        (`127.0.0.1` and `127.0.0.2`); `hash $remote_addr` and
        `consistent` keeping a client as a third host is added;
        `down`, `max_conns=1` with two requests at once, all down
        (`502`); `keepalive_max_idle` reusing each endpoint's
        connections; a host with two addresses; each config error and
        the warning
- [x] 7: passive health checks, as in `DESIGN.md`:
  - [x] `max_fails=`, `fail_timeout=` and `backup` on a `host`, and
        `backup` refused with `hash` and `ip_hash`
  - [x] Each endpoint's failures, its skipping and test request, its
        effective weight, and the reset when none is left, in
        `balance/upstream.c`
  - [x] Backups, chosen when no other endpoint can be, for round robin
        and `least_conn`
  - [x] Reporting each failure from `core/conn.c`, with the `[warn]`
  - [x] Try it by hand: two echo services with one stopped (the
        failure, the warning, the other taking every request, the test
        request after `fail_timeout`, sharing again once it's back);
        `max_fails=3`; `max_fails=0`; a pool of one never skipped;
        `backup` taking over and handing back; all stopped (`502` and
        the reset); `backup` with `hash` refused
- [x] Clearer names, as `DESIGN.md`'s Terms say: `host` for nginx's
      upstream `server`, endpoints for its peers, the renamed keepalive
      directives, `client_send_timeout`, and `$path_and_query`, `$path`
      and `$query`; and the same sweep through the code's own names

## Stage 8: phases

As in `DESIGN.md`:

- [x] Python's flags in the `Makefile` and `compile_flags.txt`
- [x] `python_path`, `python_module` and `python_setting`, with their
      errors and `python_setting`'s inheriting
- [x] The interpreter, importing the module, finding its hooks, `init`,
      and tracebacks to the error log, in `python/embed.c`
- [x] `request`, the request as the hooks see it, with what each phase may
      change, in `python/request_object.c`
- [x] Calling each hook from `core/conn.c`: `access` with `set_header`
      and `respond`; `balancer`, with a `use_endpoint` in
      `balance/upstream.c` that counts the one it returns; `header_filter`;
      `body_filter`, with the filtered reply chunked or ending at the
      close; `log`; and `error` in place of tinyproxy's page
- [x] Try it by hand: a module with every hook printing what it gets;
      `access` adding a header the echo service sees, removing one, and
      answering `401`; `balancer` choosing an endpoint and returning
      `None`; `header_filter` changing the status and a header;
      `body_filter` upper-casing a body to HTTP/1.1 and HTTP/1.0;
      `error` for a `404` and a `502`; each hook raising; a module that
      won't import; `python_setting` inherited and replaced
- [x] `header_filter` and `body_filter` on tinyproxy's own replies too,
      as `DESIGN.md`'s "Its own replies too" says: `own_page_reply` in
      `http/error_page.c`, and `filter_own_reply` in `core/conn.c`
- [x] Try it by hand: a header and an upper-cased body added to a
      `respond` reply, an `error` reply, tinyproxy's own `404` page, a
      `301` and a `400` that couldn't be read; a status changed; a
      `HEAD`; each filter raising, giving a Problem Details `500` once
- [x] `python_setting` inherited by name, as `DESIGN.md` says, in
      `config/inherit.c`
- [x] Try it by hand: a name set in `http`, `server` and `location`,
      each replacing the one outside; a location setting one name and
      keeping the server's others; an empty value

## Stages 11 to 13

- [ ] 11: `proxy_cache`
- [ ] 12: `proxy_next_upstream`
- [ ] 13: `stub_status`
