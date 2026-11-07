# TODO

How it works is in `DESIGN.md`. The stages are the ones in `../TODO.md`.

```
tinyproxy/
  Makefile
  main.c          -c, reading the config, starting the loop
  config.c/.h     the nginx-style config file
  loop.c/.h       the listening sockets, select() and deadlines
  conn.c/.h       each connection's state machine
  request.c/.h    finding and parsing the request's head, with its limits
  proxy.c/.h      the request as it goes upstream, the reply as it comes
                  back
  chunked.c/.h    decoding a chunked reply
  uri.c/.h        normalising the path, and escaping it again
  route.c/.h      choosing the server block and the location
  upstream.c/.h   upstream pools and choosing an endpoint from one
  keepalive.c/.h  idle upstream connections, kept for the next request
  hash.c/.h       ip_hash, hash and hash consistent, with CRC32
  variables.c/.h  $host and the others, for proxy_set_header
  error_page.c/.h tinyproxy's own replies and nginx's pages
  http.c/.h       what those share: header types, versions, line helpers
  buffer.c/.h     growing byte buffers, consumed from the front
  log.c/.h        the access log and the error log
```

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
- [ ] 6: load balancing, as in `DESIGN.md`:
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
  - [ ] Try it by hand: two tinyhttp echoes with `weight=3` and
        `weight=1`, the order in their logs; `least_conn` with a slow
        request holding one; `ip_hash` from two client addresses
        (`127.0.0.1` and `127.0.0.2`); `hash $remote_addr` and
        `consistent` keeping a client as a third host is added;
        `down`, `max_conns=1` with two requests at once, all down
        (`502`); `keepalive_max_idle` reusing each endpoint's
        connections; a host with two addresses; each config error and
        the warning
- [ ] 7: `max_fails`, `fail_timeout` and `backup`
- [x] Clearer names, as `DESIGN.md`'s Terms say: `host` for nginx's
      upstream `server`, endpoints for its peers, the renamed keepalive
      directives, `client_send_timeout`, and `$path_and_query`, `$path`
      and `$query`; and the same sweep through the code's own names

## Stage 8: phases

- [ ] Embed Python, and call its hooks at the phases in `DESIGN.md`
- [ ] `error_page` to a hook

## Stages 11 to 13

- [ ] 11: `proxy_cache`
- [ ] 12: `proxy_next_upstream`
- [ ] 13: `stub_status`
