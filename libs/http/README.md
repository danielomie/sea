# libs/http

HTTP/1.1 client and server for Sea, with HTTPS through `libs/ssl`.

| Layer | File | Role |
|-------|------|------|
| Sockets | built-in `net` module | Non-blocking TCP: a wait parks only its goroutine (netpoller) |
| TLS | `libs/ssl` | OpenSSL over `net` sockets |
| Parsing | `http_native.c` → `http_native.so` | Strict request/response head parsers, chunked coding, URL coding, dates (C, no OpenSSL) |
| API | `http.sea` | Client (pool, redirects), server (keep-alive, limits), router, helpers |

The full reference (every function, option and field) is in
[docs/libs.md](../../docs/libs.md#http).

## Build

```sh
make -C libs/http     # http_native.so
make -C libs/ssl      # needed for https:// and the tls option (OpenSSL 3 headers)
make -C libs/json     # needed for http.json, opts.json and req:json()
```

`require` paths are relative to the requiring file, e.g.
`require("../../libs/http/http")` from `examples/<name>/`.

## Client

```sea
local http = require("../http")

local r = http.get("https://api.example.com/items", {
    query = { page = 2 }, timeout = 5000,
    headers = { ["Authorization"] = "Bearer TOKEN" },
})
if r.ok then
    print(r:json().total)
else
    print("failed:", r.status, r.error)   -- status 0 = network/TLS/timeout error
end

http.post(url, nil, nil, { json = { name = "sea" } })      -- JSON body
http.post(url, nil, nil, { form = { q = "a b" } })         -- urlencoded form
http.request("PATCH", url, { body = "...", content_type = "text/plain" })
```

Connections are kept alive and pooled per host. Redirects are followed (5 hops
by default). Chunked responses are decoded. `verify = false` skips certificate
checks.

## Server

```sea
local http = require("../http")

local srv = http.listen(8080, function(req)
    -- req.method, req.path (no query), req.query, req.headers, req.body,
    -- req.cookies, req.remote_addr, req:json()
    return http.json({ hello = req.query["name"] or "world" })
end, { log = true })

print("listening on", srv.port)
srv.wait()            -- or srv.close() from another goroutine
```

`http.serve(port, handler, opts)` does the same and blocks. Each connection runs
in its own goroutine; keep-alive, pipelining, chunked request bodies,
`Expect: 100-continue` and HEAD are handled. Defaults: 30 s read timeout,
60 s idle timeout, 64 KB of headers, 10 MB of body, 1000 requests per
connection. Malformed or smuggling-shaped requests are rejected with 400/501/505.

A handler returns `{status, body, headers}` (or a plain string). Helpers:
`http.text`, `http.html`, `http.json`, `http.redirect`, `http.set_cookie`,
`http.static(dir)`. A `nil` or invalid response becomes a 500.

HTTPS: pass `tls = { cert = "cert.pem", key = "key.pem" }` (add `ca` to require
client certificates).

## Router

```sea
local r = http.router()
r.use(function(req, next)
    local resp = next(req)
    resp.headers["X-Served-By"] = "sea"
    return resp
end)
r.get("/users/:id", function(req) return http.json({ id = req.params["id"] }) end)
r.get("/files/*", function(req) return http.text(req.params["*"]) end)
local api = r.group("/api", { auth_mw })
api.post("/items", create_item)
http.serve(8080, r)
```

404 when nothing matches, 405 with `Allow` when only the method differs, HEAD
falls back to GET routes.

## Examples

```sh
bin/sea libs/http/examples/client_get.sea
bin/sea libs/http/examples/server_router.sea   # then: curl localhost:8080/...
```

`client_get`, `client_post`, `client_https`, `server_hello`, `server_echo`,
`server_router`, `server_https` (needs `cert.pem`/`key.pem`; see the file header).

## Tests

`tests/unit/108_http_lib.sea` (client + server over loopback) and
`tests/unit/107_net.sea` (the `net` module).
