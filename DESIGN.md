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
  ─► READING_REPLY ─► SENDING_REPLY ─► closed
```

`READING_REPLY` passes the reply on as it comes, and `SENDING_REPLY`
sends what's left once the upstream has closed. Any state up to
`READING_REPLY_HEAD` can instead go to `SENDING_ERROR`, tinyproxy's
own reply, then `LINGERING`, its gentle close.

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

Until a stage adds them, it refuses what that stage brings in the same
way: a second `location` in a `server` or a path in `proxy_pass`
before stage 2, or a second
`server` on the same port before stage 3. Several `server`s on
different ports are fine from stage 1. A request that matches no
`location` gets `404`.

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
  default, or `1.1`, which the shop's config sets. Either way it opens
  a new connection for each request and sends `Connection: close`,
  until stage 5 adds keep-alive.
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
  `proxy_hide_header` defaults, and `Connection: close`.

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
reads the reply before the connection drops. The gateway swaps these
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
   URI that matched the location is replaced by it.
3. **Host routing:** several `server`s on one port. `server_name`
   picks one by the `Host` header; with no match, the one marked
   `default_server` on its `listen`, or else the first.
4. **Upstream pools:** `upstream name { server host:port; ... }`, and
   `proxy_pass http://name;`.
5. **Keep-alive:** as nginx's, `proxy_http_version 1.1`,
   `proxy_set_header Connection ""` and `keepalive N;` in an
   `upstream` block keep up to N idle connections to its servers open
   for the next request, and `keepalive_timeout` (75 s) keeps a
   client's open for its next one. It needs tinyhttp's keep-alive, in
   `../tinyhttp/TODO.md`.
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
- No regex `location`s (`~`) and no wildcard `server_name`s.
- One `access_log` per block; nginx can write to several.
- No keep-alive, with the client or the upstream, until stage 5.
- `select()` can't wait on a file descriptor over 1023, so about a
  thousand connections at once is the limit.
- No temporary files: bodies and replies are held in memory.
- No TLS, as in the rest of the project.
