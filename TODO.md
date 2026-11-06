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
      (`502`); a server that never answers (`504`); idle `nc`s while
      `curl` still gets answers; a slow order while a search goes
      through

## Stages 2 to 7

- [ ] 2: path routing, `location =` and prefixes, and the path in
      `proxy_pass`
- [ ] 3: host routing, `server_name` and `default_server`
- [ ] 4: `upstream` blocks
- [ ] 5: keep-alive, `keepalive` in `upstream` blocks and
      `keepalive_timeout`, once tinyhttp has it
- [ ] 6: round robin with `weight=`, and `least_conn`
- [ ] 7: `max_fails` and `fail_timeout`

## Stage 8: phases

- [ ] Embed Python, and call its hooks at the phases in `DESIGN.md`
- [ ] `error_page` to a hook

## Stages 11 to 13

- [ ] 11: `proxy_cache`
- [ ] 12: `proxy_next_upstream`
- [ ] 13: `stub_status`
