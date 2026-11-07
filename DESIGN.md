# How tinyproxy works

A reverse proxy in C, as nginx is: the core of nginx, as tinyweb is
the core of Flask. It copies nginx's behaviour, directive names and
defaults, and where this file doesn't say, it does what nginx does. It
uses ideas from nginx and OpenResty, never their code: C99, the C
library and POSIX, built with `make` as `../tinysql/` is, and from
stage 8 CPython's embedding API (`Python.h`), which comes with Python.

`../tinygateway/` runs inside it, as Kong runs inside OpenResty.

## Running it

```
tinyproxy/tinyproxy -c shop/gateway.conf
```

`-c` names the config file, as nginx's does, and is required, as there's
no install folder for nginx's default; relative paths in it, such as
an `access_log`'s, are from the working folder. It stays in the
foreground, as nginx with `daemon off`, and Ctrl-C stops it.

## One process, one loop

One process with one event loop, as nginx with `master_process off`
and one worker. nginx waits with `epoll` or `kqueue`; tinyproxy uses
`select()`, as `../DESIGN.md` says. Every socket is non-blocking, and
each connection is a state machine:

```
READING_HEAD ─► READING_BODY ─► CONNECTING ─► SENDING_REQUEST ─► READING_REPLY_HEAD
  ─► READING_REPLY ─► SENDING_REPLY ─► closed, or KEEPALIVE ─► READING_HEAD
```

`READING_REPLY` passes the reply on as it comes, and `SENDING_REPLY`
sends what's left once the upstream's reply is all in. Any state up to
`READING_REPLY_HEAD` can instead go to `SENDING_ERROR`, tinyproxy's
own reply, then `LINGERING`, its gentle close, or with keep-alive
(stage 5), `DISCARDING_BODY` or `KEEPALIVE`. A request on an idle
upstream connection skips `CONNECTING`.

Each state has a deadline from the timeouts below, and the loop's
`select()` timeout is the nearest one. A non-blocking `connect()`
returns `EINPROGRESS`; the socket turns writable when it's done, and
`SO_ERROR` says whether it worked.

## Config

nginx's syntax: a directive ends in `;`, a block is in `{}`, and `#`
starts a comment. Stage 1's config:

```nginx
http {
    server {
        listen 8000;
        location / {
            proxy_pass http://127.0.0.1:8003;
        }
    }
}
```

An unknown directive, or one in the wrong block, stops it starting,
with the file and line, as nginx's `[emerg]` does. Each stage below
adds its own directives.

A request that matches no `location` gets `404`.

## The request

- The head: as nginx's `large_client_header_buffers 4 8k`, a request
  line over 8 KB gets `414`, and a header line over 8 KB or a head over
  32 KB gets `400`. Anything malformed gets `400`.
- A header whose name has anything but letters, digits and `-` is
  dropped, as nginx's `ignore_invalid_headers` and
  `underscores_in_headers off` do, so `X_User_Id` can't pass for
  `X-User-Id`, which a WSGI app reads the same way.
- The body: `Content-Length` bytes, up to `client_max_body_size`
  (default `1m`), or `413`. It's read in full before the upstream is
  connected, as nginx's `proxy_request_buffering on` does, so the
  server behind only ever sees whole requests. It's held in memory,
  where nginx would move a large one to a file.
- `client_header_timeout` (60 s) for the whole head, and
  `client_body_timeout` (60 s) between two reads of the body; either
  gets `408`.

## Sending it on

- As the HTTP version `proxy_http_version` says: `1.0`, nginx's
  default, or `1.1`, which the shop's config sets. It opens a new
  connection for each request, unless stage 5's `keepalive` has an
  idle one.
- With nginx's default headers, `Host: $proxy_host` (the host and port
  in `proxy_pass`) and `Connection: close`, and the client's others as
  they are. `proxy_set_header` adds or replaces one, and an empty value
  removes it. Its values can use `$host`, `$remote_addr`, `$proxy_host`
  and `$proxy_add_x_forwarded_for`. The shop's config sets `Host $host`
  and `X-Forwarded-For $proxy_add_x_forwarded_for`.
- A refused connection gets `502`. `proxy_connect_timeout` (60 s) to
  connect, or `504`, and `proxy_send_timeout` (60 s) between two
  writes.

## The reply

- A status line or headers that aren't HTTP, or the upstream closing
  before they're in, get `502`.
- `proxy_read_timeout` (60 s) between two reads. If it fires before the
  reply's head is in, the client gets `504`; after, the client's
  connection is closed, as nginx does, and it gets a cut-off reply.
- As nginx's `proxy_buffering on`, it reads the reply as fast as the
  upstream sends it and sends it on as fast as the client takes it.
  What the client hasn't taken yet is held in memory, where nginx
  would move it to a file. `send_timeout` (60 s) between two writes to
  the client, then it closes.
- A chunked reply (`Transfer-Encoding: chunked`, which a `1.1`
  upstream may send) goes to an HTTP/1.1 client as it came. An
  HTTP/1.0 client can't read chunks, so it gets them decoded, without
  the header, and the body ends at the close. Any other
  `Transfer-Encoding`, or one with a `Content-Length` too, gets `502`,
  as in nginx. A bad chunk gets `502` if none of the reply has gone
  yet, and otherwise cuts it off.
- The headers go back as they came, except `Date` and `Server`, which
  are tinyproxy's own (`Server: tinyproxy`), as in nginx's
  `proxy_hide_header` defaults, and `Connection` and `Keep-Alive`, which
  say whether tinyproxy keeps the client's connection (stage 5).

## Its own replies

nginx's statuses, with its small HTML page:

```html
<html>
<head><title>502 Bad Gateway</title></head>
<body>
<center><h1>502 Bad Gateway</h1></center>
<hr><center>tinyproxy</center>
</body>
</html>
```

Then it closes gently, as nginx's `lingering_close` does, so the client
reads the reply before the connection drops, unless stage 5 keeps the
connection. The gateway swaps these
for Problem Details (stage 8), as Kong swaps nginx's pages for JSON.

## Logs

- `access_log path;` writes one line per request in nginx's `combined`
  format:

  ```
  127.0.0.1 - - [06/Nov/2026:16:00:00 +0000] "GET /search?term=lamp HTTP/1.1" 200 512 "-" "curl/8.7.1"
  ```

  Without one it logs nothing, as tinyproxy has no install folder for
  nginx's `logs/access.log`. The shop's config sets `/dev/stdout`.
- Errors, such as an upstream that's down, go to stderr, as nginx's
  `error_log stderr`.

## Stages

Each stage adds nginx's directives for it.

2. **Path routing:** several `location`s in a `server`. `location =
   /path` matches exactly and is checked first; otherwise the longest
   matching prefix wins. With a path in `proxy_pass`, the part of the
   URI that matched the location is replaced by it. As in nginx:
   - Locations match the normalised path, nginx's `$uri`: `%XX`
     decoded, `//` merged, and `.` and `..` resolved, so
     `/search/../orders` is `/orders`. A path that climbs above `/`, a
     bad `%` escape or a `%00` gets `400`. The query isn't part of it.
   - Matching is case-sensitive, as nginx's on Linux; on macOS nginx
     ignores case, as the file system does.
   - `^~ /path` is a prefix like any other, as there are no regex
     locations for it to stop. Two locations with the same match stop
     it starting: `duplicate location "/path"`.
   - With a path in `proxy_pass`, what's sent is that path, then the
     rest of `$uri` after the location's own path (all of it, for
     `location =`), escaped again, then `?` and the query as it came.
     Without one, the URI goes on as the client sent it.
   - A prefix location that ends in `/` and has a `proxy_pass`, such as
     `location /api/`, answers `/api` with `301` to `/api/`, keeping the
     query. An exact `location = /api` takes it instead. The `Location`
     is absolute, as nginx's `absolute_redirect`: `http://`, `$host`
     (or the address it was reached on, if that's empty), the port
     unless it's 80, and the path.
3. **Host routing:** several `server`s on one port. `server_name`
   picks one by the `Host` header; with no match, the one marked
   `default_server` on its `listen`, or else the first. As in nginx:
   - Servers are grouped by the address and port they listen on. If
     any server on a port listens on every address (`listen 8000`),
     tinyproxy opens one socket for the port, and each connection uses
     the group for the address it arrived on, or else the every-address
     one. Otherwise each address gets its own socket. Only the group's
     servers are candidates, and its default is its own.
   - Names are matched ignoring case, in nginx's order: an exact name;
     then the longest leading wildcard, `*.shop.local` (any name ending
     `.shop.local`) or `.shop.local` (that, or `shop.local` itself);
     then the longest trailing wildcard, `mail.*`. A `*` anywhere else,
     or a regex name (`~`), stops it starting.
   - A server with no `server_name` has the empty name `""`, which
     matches a request with no `Host`, as does `server_name ""`.
   - Two servers in a group with the same name get nginx's `[warn]
     conflicting server name "x" on 0.0.0.0:8000, ignored` at startup,
     and the first keeps it. Two `default_server`s in a group stop it
     starting.
   - With no `Host`, `$host` is the server's first name.
4. **Upstream pools:** `upstream name { server host:port; ... }`, and
   `proxy_pass http://name;`. As in nginx:
   - `proxy_pass` always sends to a pool. `http://127.0.0.1:8003` is
     an unnamed one with that one server, shared by every `proxy_pass`
     that names it. A name is looked up once the whole file is read,
     so an `upstream` can come after the `proxy_pass` that uses it,
     and names are matched ignoring case. `$proxy_host` is what's
     written after `http://` in `proxy_pass`, up to any path: the
     pool's name, or the host and port.
   - A host name that resolves to several addresses is one server per
     address, in a pool's `server` or in `proxy_pass`.
   - Until stage 6 balances between them, every request goes to the
     pool's first server.
   - These stop it starting: two pools with one name (`duplicate
     upstream "x"`), a pool with no servers, a port after a pool's
     name in `proxy_pass` (`upstream "x" may not have port 8080`), and
     a `server` option other than the ones later stages add, which
     say which stage until then.
   - The error log's `upstream:` is the server's own address, such as
     `http://127.0.0.1:8003/search`.
5. **Keep-alive:** as nginx's, with the client and with the upstream.
   With the client:
   - HTTP/1.1 keeps the connection unless its `Connection` has
     `close` in it; HTTP/1.0 only if it has `keep-alive`. The reply
     says `Connection: keep-alive`, or `Connection: close` when it's
     the last.
   - `keepalive_timeout` (75 s) in `http`, `server` or `location` is
     how long it waits for the next request, and `0` turns keep-alive
     off. A second argument, such as `keepalive_timeout 75s 60s`, adds
     `Keep-Alive: timeout=60` to kept replies. `keepalive_requests`
     (1000) and `keepalive_time` (1 h) make the reply that reaches that
     many requests, or that long since the connection was accepted,
     the last. `client_header_timeout` starts at the next request's
     first byte.
   - Requests sent before the last reply came back (pipelined) are
     served in order.
   - A reply whose end the client can only find by the close ends the
     connection: a chunked reply decoded for HTTP/1.0, or one with
     neither `Content-Length` nor chunks to HTTP/1.0. To HTTP/1.1, such
     a reply goes chunked, as nginx's `chunked_transfer_encoding on`
     does, and the connection stays.
   - tinyproxy's own replies keep it too, except `400`, `408`, `413`,
     `414` and `500`, and any to a request it couldn't read. A body not
     yet read, as with a `404` before it came, is read and dropped
     first, after a `100 Continue` if the client asked for one, with
     the gentle close's limits; past them it closes.
   - A connection that closes with more requests already sent closes
     gently.
   - When all of its connections are in use, a new one closes the idle
     connection that has waited longest, with nginx's `[warn] 500
     worker_connections are not enough, reusing connections`.

   With the upstream:
   - `keepalive N;` in an `upstream` block keeps up to N idle
     connections to its servers, closing the least recently used to
     make room for another. It needs `proxy_http_version 1.1` and
     `proxy_set_header Connection ""`, so the upstream isn't told to
     close. A pool with no `keepalive`, such as a `proxy_pass` to an
     address, closes each connection after its reply.
   - A request goes on the most recently idle connection to the
     server chosen, or else a new one.
   - A connection is kept once its whole reply is in: up to its
     `Content-Length` or last chunk, or with no body for `HEAD`, `1xx`,
     `204` and `304`. It isn't after an HTTP/1.0 reply, a reply with
     `Connection: close`, an error or a timeout, or a reply the
     upstream sent more after, which gets nginx's `[warn] upstream sent
     more data than specified in "Content-Length" header` or `upstream
     sent data after final chunk`. A reply that ends early cuts the
     client's off, with `upstream prematurely closed connection while
     reading upstream`.
   - In the `upstream` block, `keepalive_timeout` (60 s) closes an idle
     connection, and `keepalive_requests` (1000) and `keepalive_time`
     (1 h) close one, instead of keeping it, after that many requests
     or that long since it was opened. An idle one that turns readable,
     as when the server closes it, is closed.
   - A request that fails with an error on a reused connection, before
     its reply's head is in, is sent again on another idle connection
     or a new one, as the server may have closed it just as it was
     reused. Never for `POST`, `PATCH` or `LOCK`, which get `502`, as
     they might have been acted on. Each failure is logged.
   - It needs tinyhttp's keep-alive, in `../tinyhttp/DESIGN.md`, whose
     idle time in the shop is longer, so tinyproxy always closes an
     idle connection first.
6. **Load balancing:** round robin by default, with `weight=` on a
   `server`; `least_conn;` picks the one with the fewest open
   connections.
7. **Passive health checks:** `max_fails` (1) and `fail_timeout`
   (10 s) on a `server`: after `max_fails` failures within
   `fail_timeout`, it's skipped for `fail_timeout`. With none left, the
   client gets `502`. Active checks are the gateway's.
8. **Phases:** hooks at nginx's phases, which the gateway's Python
   runs in, as OpenResty's `*_by_lua` directives do: access (allow it,
   or answer here), balancer (pick the server), header filter, body
   filter and log. `error_page` sends tinyproxy's own errors to a hook,
   as Kong's nginx config sends them to its Lua. The hooks run inside
   the loop, so, as in OpenResty, they must never block.
11. **Caching:** nginx's `proxy_cache`, following the reply's
    `Cache-Control` and `Expires`.
12. **Retries:** nginx's `proxy_next_upstream error timeout`: on a
    connection error or a timeout before any of the reply came back, it
    tries the pool's next server. Never for `POST`, `PATCH` or `LOCK`,
    as they might have been acted on.
13. **Observability:** the access log, and nginx's `stub_status`: a
    page of active connections, connections accepted and handled,
    requests, and how many are reading, writing and waiting.

Stages 9 and 10 are the gateway's.

## Known gaps

- No chunked request bodies; nginx accepts them. One gets `411`, as
  nginx's did before it did.
- No regex `location`s or `server_name`s (`~`).
- No `location` inside another; nginx allows them. One stops it
  starting.
- One `access_log` per block; nginx can write to several.
- No `keepalive_disable`, which turns keep-alive off for old browsers,
  or `chunked_transfer_encoding off`.
- `select()` can't wait on a file descriptor over 1023, so about a
  thousand connections at once is the limit.
- No temporary files: bodies and replies are held in memory.
- No TLS, as in the rest of the project.
