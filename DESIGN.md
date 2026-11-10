# How tinyproxy works

A reverse proxy in C, for learning how real ones such as nginx work,
as tinyweb is for Flask. Understanding comes first. It behaves as nginx
does, so what it teaches holds for real proxies, and where this file
doesn't say, it does what nginx does. But it isn't a copy: where a name
or an accident of nginx's code would make the system harder to
understand, it uses a clearer one and gives nginx's alongside. Its
config is in nginx's style, but needn't be valid nginx. It uses ideas
from nginx and OpenResty, never their code: C99, the C library and
POSIX, built with `make` as `../tinysql/` is, and from stage 8
CPython's embedding API (`Python.h`), which comes with Python.

`../tinygateway/` runs inside it, as Kong runs inside OpenResty.

## Terms

- **Server:** a `server { ... }` block, the addresses tinyproxy listens
  on and the names it answers to (a virtual server).
- **Upstream:** a pool of endpoints that all serve the same thing, such
  as the shop's search service: an `upstream name { ... }` block, or
  one made for a `proxy_pass` to an address.
- **Host:** one `host name:port;` line in an upstream (nginx's
  `server`). Its name may resolve to several addresses.
- **Endpoint:** one address and port a host resolved to, where a
  request is sent (nginx's peer). The load balancer picks an endpoint
  for each request. Two services on one machine are two endpoints;
  nothing stands for the machine.

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
  ─► READING_REPLY ─► SENDING_REPLY ─► closed, or IDLE ─► READING_HEAD
```

`READING_REPLY` passes the reply on as it comes, and `SENDING_REPLY`
sends what's left once the upstream's reply is all in. Any state up to
`READING_REPLY_HEAD` can instead go to `SENDING_OWN_REPLY`, such as a
`502` or a redirect, then `LINGERING`, its gentle close, or with
keep-alive (stage 5), `DISCARDING_BODY` or `IDLE`. A request on an idle
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
  endpoint only ever sees whole requests. It's held in memory, where
  nginx would move a large one to a file.
- `client_header_timeout` (60 s) for the whole head, and
  `client_body_timeout` (60 s) between two reads of the body; either
  gets `408`.

## Sending it on

- As the HTTP version `proxy_http_version` says: `1.0`, nginx's
  default, or `1.1`, which the shop's config sets. It opens a new
  connection for each request, unless stage 5's `keepalive_max_idle`
  has an idle one.
- With nginx's default headers, `Host: $proxy_host` (the host and port
  in `proxy_pass`) and `Connection: close`, and the client's others as
  they are. `proxy_set_header` adds or replaces one, and an empty value
  removes it. Its values can use `$host`, `$remote_addr`, `$proxy_host`,
  `$proxy_add_x_forwarded_for`, and from stage 6, `$path_and_query`
  (as the client sent them; nginx's `$request_uri`), `$path` (stage 2's
  normalised path; nginx's `$uri`) and `$query` (nginx's `$args`). The
  shop's config sets `Host $host`
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
  would move it to a file. `client_send_timeout` (60 s; nginx's
  `send_timeout`) between two writes to the client, then it closes.
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
   - Locations match the normalised path, `$path`: `%XX`
     decoded, `//` merged, and `.` and `..` resolved, so
     `/search/../orders` is `/orders`. A path that climbs above `/`, a
     bad `%` escape or a `%00` gets `400`. The query isn't part of it.
   - Matching is case-sensitive, as nginx's on Linux; on macOS nginx
     ignores case, as the file system does.
   - `^~ /path` is a prefix like any other, as there are no regex
     locations for it to stop. Two locations with the same match stop
     it starting: `duplicate location "/path"`.
   - With a path in `proxy_pass`, what's sent is that path, then the
     rest of `$path` after the location's own path (all of it, for
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
4. **Upstream pools:** `upstream name { host name:port; ... }`, and
   `proxy_pass http://name;`. As in nginx:
   - `proxy_pass` always sends to a pool. `http://127.0.0.1:8003` is
     an unnamed one with that one host, shared by every `proxy_pass`
     that names it. A name is looked up once the whole file is read,
     so an `upstream` can come after the `proxy_pass` that uses it,
     and names are matched ignoring case. `$proxy_host` is what's
     written after `http://` in `proxy_pass`, up to any path: the
     pool's name, or the host and port.
   - A name that resolves to several addresses is one endpoint per
     address, in a pool's `host` or in `proxy_pass`.
   - Until stage 6 balances between them, every request goes to the
     pool's first endpoint.
   - These stop it starting: two pools with one name (`duplicate
     upstream "x"`), a pool with no hosts, a port after a pool's name
     in `proxy_pass` (`upstream "x" may not have port 8080`), and a
     `host` option other than the ones later stages add, which say
     which stage until then.
   - The error log's `upstream:` is the endpoint's address, such as
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
     `Keep-Alive: timeout=60` to kept replies. `keepalive_max_requests`
     (1000; nginx's `keepalive_requests`) and `keepalive_lifetime` (1 h;
     nginx's `keepalive_time`) make the reply that reaches that many
     requests, or that long since the connection was accepted, the
     last. `client_header_timeout` starts at the next request's
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
   - `keepalive_max_idle N;` (nginx's `keepalive N`) in an `upstream`
     block keeps up to N idle connections to its endpoints, closing the
     least recently used to make room for another. It needs
     `proxy_http_version 1.1` and `proxy_set_header Connection ""`, so
     the upstream isn't told to close. A pool with no
     `keepalive_max_idle`, such as a `proxy_pass` to an address, closes
     each connection after its reply.
   - A request goes on the most recently idle connection to the
     endpoint chosen, or else a new one.
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
     connection, and `keepalive_max_requests` (1000) and
     `keepalive_lifetime` (1 h) close one, instead of keeping it, after
     that many requests or that long since it was opened. An idle one
     that turns readable, as when the endpoint closes it, is closed.
   - A request that fails with an error on a reused connection, before
     its reply's head is in, is sent again on another idle connection
     or a new one, as the endpoint may have closed it just as it was
     reused. Never for `POST`, `PATCH` or `LOCK`, which get `502`, as
     they might have been acted on. Each failure is logged.
   - It needs tinyhttp's keep-alive, in `../tinyhttp/DESIGN.md`, whose
     idle time in the shop is longer, so tinyproxy always closes an
     idle connection first.
6. **Load balancing:** as nginx's, an endpoint is chosen for each
   request, not each connection; then stage 5's `keepalive_max_idle`
   looks for an idle connection to it.
   - **Round robin**, the default, is nginx's smooth weighted round
     robin. `weight=N` on a `host` (1 by default) is given to each of
     its endpoints. For each request every endpoint's score goes up by
     its weight, the highest is chosen (the first in the block on a
     tie), and its score goes down by the total of the weights, so
     weights 5, 1 and 1 give `a a b a c a a`. A `proxy_pass` to a name
     with several addresses is balanced the same way.
   - **`least_conn;`** chooses the endpoint with the fewest active
     requests for its weight, comparing `active_a × weight_b` with
     `active_b × weight_a`, and round robin among those that tie. A
     request is active from when its endpoint is chosen until its
     reply is all in or it fails; idle kept connections don't count.
   - **`ip_hash;`** keeps a client on one endpoint: the first three
     parts of its IPv4 address are hashed as nginx does, and the hash,
     modulo the total weight, picks an endpoint by weight.
   - **`hash key;`** does the same with `key`, which can use the
     variables `proxy_set_header` can, such as `$path_and_query`,
     hashed with CRC32 as nginx's (compatible with Cache::Memcached).
     **`hash key consistent;`** is ketama consistent hashing, as
     nginx's (compatible with Cache::Memcached::Fast): each host has
     160 points per unit of weight on a ring, and the key goes to the
     first point at or after its hash, so adding or removing a host
     moves only its share of keys. Among a host's endpoints, round
     robin picks one.
   - `down` on a `host` skips its endpoints for good, and `max_conns=N`
     skips each while it has N active requests; idle kept connections
     don't count, so with `keepalive_max_idle` the total open can be
     more. A hash that lands on one it can't use hashes again, and
     after 20 tries falls back to round robin. With none left, the
     client gets `502`, and the error log nginx's `no live upstreams
     while connecting to upstream`.
   - These stop it starting: `weight=` that isn't a whole number from 1
     (`invalid parameter "weight=0"`), `max_conns=` that isn't a whole
     number, `hash` without a key or with anything but `consistent`
     after it, and `least_conn` or `ip_hash` with arguments.
   - As in nginx, a pool has one method, and a second gets `[warn] load
     balancing method redefined`, the last one winning. A method after
     `keepalive_max_idle` in the block also turns it off for the pool,
     with the same warning, as nginx's docs say to write the method
     first.
7. **Passive health checks:** as nginx's, an endpoint that fails
   requests is skipped for a while. tinyproxy learns this from the
   requests themselves, without probing; active checks, which probe
   each endpoint on a timer, are the gateway's.
   - A failure is an error or a timeout while connecting, sending the
     request or reading the reply's head, or a head that isn't HTTP or
     is too big: the upstream's own `502`s and `504`s. A `500` reply
     isn't one (stage 12 can add it), and nor is an error after the
     head, or tinyproxy's own trouble, such as running out of
     descriptors. An error on a reused idle connection counts too,
     before stage 5 sends the request again, as in nginx.
   - `max_fails=N` (1) and `fail_timeout=T` (10 s) on a `host`: once an
     endpoint has `max_fails` failures, it's skipped for
     `fail_timeout`, with nginx's `[warn] upstream server temporarily
     disabled while connecting to upstream` (or whatever it was doing).
     Then one request is let through to test it: if it succeeds, the
     count goes back to 0; if it fails, the endpoint is skipped for
     another `fail_timeout`. `max_fails=0` never skips it.
   - Each failure also lowers the endpoint's weight in round robin by
     `weight / max_fails`, and it climbs back by 1 with each request
     after, so a recovered endpoint gets its share back gradually, as
     nginx's effective weight does.
   - A pool with only one endpoint, and no backup, never skips it, as
     in nginx: there's nowhere else to send the request, so a blip would
     only turn into `502`s for `fail_timeout`.
   - With every endpoint skipped, `down` or full, the client gets `502`
     and the error log `no live upstreams`, and every endpoint's count
     goes back to 0, so the next request tries them all again.
   - `backup` on a `host` keeps its endpoints for when every other one
     is skipped, `down` or full; among them, the pool's method chooses
     as usual. It stops tinyproxy starting with `hash` or `ip_hash`
     (`invalid parameter "backup"`), whose keys must always land on the
     same endpoints.
8. **Phases:** hooks at nginx's phases, which the gateway's Python
   runs in, as OpenResty's `*_by_lua` directives do: access (allow it,
   or answer here), balancer (pick the endpoint), header filter, body
   filter and log. `error_page` sends tinyproxy's own errors to a hook,
   as Kong's nginx config sends them to its Lua. The hooks run inside
   the loop, so, as in OpenResty, they must never block.
11. **Caching:** nginx's `proxy_cache`, following the reply's
    `Cache-Control` and `Expires`.
12. **Retries:** nginx's `proxy_next_upstream error timeout`: on a
    connection error or a timeout before any of the reply came back, it
    tries the pool's next endpoint. Never for `POST`, `PATCH` or `LOCK`,
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
- No `random` balancing method, nginx's `random` and `random two`.
- No `$cookie_<name>` or `$http_<name>` variables, so `hash` can't keep a
  session on one endpoint. The shop has no sessions: its JWT is sent with
  every request, and any service can answer any request.
- No `keepalive_disable`, which turns keep-alive off for old browsers,
  or `chunked_transfer_encoding off`.
- `select()` can't wait on a file descriptor over 1023, so about a
  thousand connections at once is the limit.
- No temporary files: bodies and replies are held in memory.
- No TLS, as in the rest of the project.
