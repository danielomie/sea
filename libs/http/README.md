# libs/http

HTTP and HTTPS client + HTTP server for Sea.

The library is split into two layers:

| Layer | File | Role |
|-------|------|------|
| Transport | `http_native.c` → `http_native.so` | TCP/TLS sockets, HTTP parsing (C) |
| Logic | `http.sea` | URL parsing, request building, public API (Sea) |

`require("./libs/http/http")` loads `http.sea` first; `http.sea` then loads
`http_native` (resolves to `http_native.so` in the same directory).

## Build

```sh
cd libs/http
make          # produces http_native.so
```

Requires OpenSSL 3.x headers and libraries (`libssl-dev` / `openssl-devel`).

## Client API

### `http.get(url) → response`

```sea
local http = require("./libs/http/http")
local r = http.get("https://example.com/api")
print(r.status, r.ok)
print(r.body)
```

### `http.post(url, body [, content_type]) → response`

```sea
local r = http.post("https://api.example.com/data",
                    '{"key":"value"}',
                    "application/json")
```

`content_type` defaults to `"application/octet-stream"` when omitted.

### `http.request(method, url [, body [, content_type [, headers]]]) → response`

Full control — custom method and headers:

```sea
local r = http.request("PATCH", "https://api.example.com/item/1",
    '{"active":true}',
    "application/json",
    { ["Authorization"] = "Bearer TOKEN" })
```

### Response object

| Field | Type | Description |
|-------|------|-------------|
| `status` | number | HTTP status code (0 on connection error) |
| `ok` | bool | `true` when `200 ≤ status < 300` |
| `body` | string | Response body |
| `headers` | table | Response headers (lowercase keys) |
| `error` | string | Error message when `status == 0` |

## Server API

### `http.serve(port, handler)`

Blocks forever. `handler(req)` receives a request table and must return a
response table.

```sea
http.serve(8080, function(req)
    print(req.method .. " " .. req.path)
    if req.method == "GET" and req.path == "/" then
        return { status = 200, body = "Hello, Sea!\n" }
    end
    return { status = 404, body = "Not Found\n" }
end)
```

### Request object

| Field | Type | Description |
|-------|------|-------------|
| `method` | string | `"GET"`, `"POST"`, … |
| `path` | string | Request path (e.g. `"/api/v1"`) |
| `headers` | table | Request headers (lowercase keys) |
| `body` | string | Request body |

### Response object (returned by handler)

| Field | Type | Description |
|-------|------|-------------|
| `status` | number | HTTP status code (default `200`) |
| `body` | string | Response body (default `""`) |
| `headers` | table | Extra headers. When absent, `Content-Type: text/plain` is sent |

## TLS / HTTPS

HTTPS is handled transparently — just use an `https://` URL. Certificate
verification is enabled by default using the system CA bundle
(`SSL_VERIFY_PEER` + hostname check via `SSL_set1_host`). SNI is set
automatically.

The `http_native` C layer manages a single shared `SSL_CTX` (initialized once
with `pthread_once`).

## Examples

```sh
# HTTPS GET + POST
bin/sea libs/http/examples/client_https.sea

# Plain HTTP GET
bin/sea libs/http/examples/client_get.sea

# Plain HTTP POST
bin/sea libs/http/examples/client_post.sea

# Echo server (run in one terminal, curl in another)
bin/sea libs/http/examples/server_echo.sea
```
