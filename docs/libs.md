# Sea External Libraries Reference

External libraries live under `libs/`. Native libraries (`.so`) must be built before use;
pure-Sea libraries work out of the box.

---

## Library Overview

| Library | Path | Kind | Build |
|---------|------|------|-------|
| `json` | `libs/json/json` | native `.so` | `make -C libs/json` |
| `xml` | `libs/xml/xml` | native `.so` | `make -C libs/xml` |
| `console` | `libs/console/console` | native `.so` | `make -C libs/console` |
| `http` | `libs/http/http` | Sea + native | `make -C libs/http` |
| `ssl` | `libs/ssl/ssl` | Sea + native | `make -C libs/ssl` |
| `jwt` | `libs/jwt/jwt` | Sea + native | `make -C libs/jwt` |
| `flags` | `libs/flags/flags` | pure Sea | — |
| `parser` | `libs/parser/parser` | native `.so` | `make -C libs/parser` |

Require paths are relative to the calling file. Examples within a lib's `examples/` directory use `"../libname"`.

Writing a native library: see `src/sea.h` for the module convention and
[memory.md](memory.md#writing-native-functions) for the ownership rules. In
short, functions borrow their arguments and return an owned value
(`sea_val_ref()` for an argument or a container element).

---

## `json`

```sea
local json = require("./libs/json/json")
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `json.encode` | `encode(v) → string\|nil` | Serialize a Sea value to JSON. Returns `nil` on circular reference or encoding error |
| `json.decode` | `decode(s) → value\|nil` | Parse a JSON string. Returns `nil` on any parse error |

**Type mapping — encode:**

| Sea type | JSON output |
|----------|-------------|
| `nil` | `null` |
| `bool` | `true` / `false` |
| `int` | integer |
| `number` | float (`NaN`/`Inf` → `null`) |
| `string` | `"..."` with standard escapes + `\uXXXX` |
| table `{0..n-1}` | array `[...]` |
| table with string keys | object `{...}` |
| `array` (SeaArray) | array `[...]` |

**Type mapping — decode:**

| JSON | Sea type |
|------|----------|
| `null` | `nil` |
| `true`/`false` | `bool` |
| integer | `int` |
| float | `number` |
| `"string"` | `string` |
| `[...]` | `array` (**0-indexed**) |
| `{...}` | table (TABLE_MAP, string keys) |

---

## `xml`

```sea
local xml = require("./libs/xml/xml")
```

Node table format: `{ tag="name", attrs={k=v,...}, children=[...], text="..." }`

- `children` — element-type children only (no text nodes)
- `text` — concatenation of all text content inside the element (including CDATA)

| Function | Signature | Description |
|----------|-----------|-------------|
| `xml.parse` | `parse(s) → node\|nil` | Parse an XML string into a node tree |
| `xml.encode` | `encode(node) → string\|nil` | Serialize a node tree back to XML |
| `xml.find` | `find(node, tag) → node\|nil` | First direct child element with the given tag |
| `xml.findall` | `findall(node, tag) → array` | All direct child elements with the given tag (0-indexed) |
| `xml.attr` | `attr(node, name) → string\|nil` | Value of an attribute, or `nil` if absent |
| `xml.text` | `text(node) → string` | Text content of the node (empty string if none) |

```sea
local root = xml.parse("<items><item id='1'>Hello</item></items>")
local item = xml.find(root, "item")
print(xml.attr(item, "id"))   -- "1"
print(xml.text(item))         -- "Hello"
```

---

## `console`

```sea
local console = require("./libs/console/console")
```

### Cursor

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.goto` | `goto(row, col)` | Move cursor to absolute position (1-based) |
| `console.up` | `up([n])` | Move cursor up `n` lines (default 1) |
| `console.down` | `down([n])` | Move cursor down `n` lines |
| `console.left` | `left([n])` | Move cursor left `n` columns |
| `console.right` | `right([n])` | Move cursor right `n` columns |
| `console.save` | `save()` | Save cursor position |
| `console.restore` | `restore()` | Restore saved cursor position |
| `console.home` | `home()` | Move cursor to top-left (1,1) |
| `console.clear` | `clear()` | Clear entire screen |
| `console.clear_line` | `clear_line()` | Clear current line |
| `console.clear_eol` | `clear_eol()` | Clear from cursor to end of line |
| `console.hide_cursor` | `hide_cursor()` | Hide cursor |
| `console.show_cursor` | `show_cursor()` | Show cursor |
| `console.size` | `size() → {width, height}` | Terminal dimensions |

### Text & colors

#### `console.write(text [, opts])` / `console.writeln(text [, opts])`

Print with optional styling. `writeln` appends a newline.

**`opts` fields:**

| Key | Type | Description |
|-----|------|-------------|
| `color` | string / `"#RRGGBB"` / `{r,g,b}` | Foreground color |
| `bg` | string / `"#RRGGBB"` / `{r,g,b}` | Background color |
| `bold` | bool | Bold |
| `dim` | bool | Dim/faint |
| `italic` | bool | Italic |
| `underline` | bool | Underline |
| `blink` | bool | Blink |
| `reverse` | bool | Swap fg/bg |

Named colors: `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`,
`bright_black` … `bright_white`, `default`.

#### `console.reset()`

Reset all text attributes to default.

### Input

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.readline` | `readline([prompt]) → string\|nil` | Read a line from stdin with optional prompt |
| `console.readkey` | `readkey() → string` | Read a single keypress (raw mode, no Enter needed) |
| `console.readpass` | `readpass([prompt]) → string\|nil` | Read a line with echo disabled (passwords) |

### Progress bar

#### `console.progress(current, total [, opts])`

Renders a progress bar inline (no newline). Overwrite with `\r` in a loop.

| Option | Default | Description |
|--------|---------|-------------|
| `width` | `30` | Bar width in characters |
| `fill` | `"█"` | Filled character |
| `empty` | `"░"` | Empty character |
| `color` | `"green"` | Bar color |
| `show_percent` | `true` | Show percentage |
| `show_count` | `false` | Show `current/total` |

### Shimmer / glow

#### `console.shimmer(text [, opts])`

One-shot animation: a brightness wave sweeps across the text, then leaves it dim.

| Option | Default | Description |
|--------|---------|-------------|
| `duration` | `1.5` | Animation length in seconds |
| `fps` | `30` | Frames per second |
| `wave_width` | `0.30` | Wave width as fraction of text length |
| `color` | `{r=255,g=255,b=255}` | Peak (brightest) color |
| `base` | `{r=70,g=70,b=70}` | Trough (darkest) color |

#### Background shimmer (non-blocking)

Runs in a background thread while your code does other work.

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.shimmer_start` | `shimmer_start(text [, opts])` | Start background shimmer. `opts`: `fps`, `wave_width`, `bar_width`, `bar_total`, `color`, `base` |
| `console.shimmer_update` | `shimmer_update(text [, cur, total])` | Update text and optional progress counters |
| `console.shimmer_stop` | `shimmer_stop()` | Stop the background thread and clear the line |
| `console.shimmer_run` | `shimmer_run(text [, opts])` | Alias for `shimmer_start` |

```sea
console.shimmer_start("Loading model...", { bar_total = 100 })
for i = 0, 99 do
    -- do real work
    console.shimmer_update("Loading model...", i + 1, 100)
end
console.shimmer_stop()
```

---

## `http`

HTTP/1.1 client and server. Requires `http_native.so` (strict request/response parsers, URL coding), plus `libs/ssl` for HTTPS and `libs/json` for the JSON helpers. Build: `make -C libs/http libs/ssl libs/json` (one `make -C` each).

```sea
local http = require("../libs/http/http")   -- relative to the requiring file
```

Sockets come from the built-in `net` module: every network wait parks only the goroutine doing it (netpoller), so a slow or idle peer never stalls the others. All timeouts are in milliseconds.

### Client

| Function | Signature | Description |
|----------|-----------|-------------|
| `http.request` | `request(method, url [, opts]) → response` | Any method. The older `request(method, url, body, content_type, headers)` form still works |
| `http.get` / `head` / `delete` | `get(url [, opts]) → response` | Requests without a body |
| `http.post` / `put` / `patch` | `post(url, body [, content_type [, opts]]) → response` | `content_type` defaults to `application/octet-stream` when a `body` is given; `body` may be `nil` when `opts.json`/`opts.form` is used |

**Client `opts`** (table or map):

| Key | Default | Description |
|-----|---------|-------------|
| `headers` | — | Extra request headers (a value may be an array for a repeated header). CR/LF in names or values is rejected |
| `body` | — | Request body (string) |
| `json` | — | Value to send JSON-encoded (sets `Content-Type: application/json`) |
| `form` | — | Map sent as `application/x-www-form-urlencoded` |
| `content_type` | — | Content-Type of `body` |
| `query` | — | Map appended to the URL as a query string |
| `timeout` | `30000` | Per network operation (connect, TLS handshake, each read/write) |
| `follow_redirects` | `true` | `false`, `true` (up to 5 hops) or a max hop count. 303 — and 301/302 after a POST — become GET; `Authorization`/`Cookie` are dropped when redirected to another host |
| `keep_alive` | `true` | Reuse connections (pool of up to 8 idle per host, 30 s max age; a stale reused connection is retried once) |
| `basic_auth` | — | `{user, password}` |
| `verify` | `true` | `false` skips TLS certificate checks |
| `tls` | — | Extra `ssl.connect` opts (`cert`, `key`, `sni`, `min_version`) |
| `max_body_bytes` | 100 MB | Larger responses fail |

The client sends `Host` (with the port when it is not the default), `User-Agent: sea-http/1.1` and `Accept: */*` (unless given) and `Content-Length` (a non-string body is converted with `tostring`); it decodes chunked responses and skips 1xx interim responses.

**Response:**

| Field | Description |
|-------|-------------|
| `status` | Status code; `0` when the request failed (connection, TLS, timeout, malformed response) |
| `ok` | `true` for 2xx |
| `reason` | Reason phrase |
| `headers` | Map with lowercase keys (repeated headers joined with `", "`) |
| `body` | Body string (`""` for HEAD / 204 / 304) |
| `url` | Final URL after redirects |
| `error` | Message, only when `status == 0` |
| `resp:json()` | Decodes the body as JSON |

### Server

| Function | Signature | Description |
|----------|-----------|-------------|
| `http.listen` | `listen(port, handler [, opts]) → server\|error` | Start serving in the background and return at once. `port` 0 picks a free port |
| `http.serve` | `serve(port, handler [, opts])` | `listen` + print a "listening" line (unless `opts.quiet`) + block until the server is closed |

`handler(req) → response` is a function or a router. Every connection runs in its own goroutine.

**Server object:** `srv.port` (bound port), `srv.tls` (bool), `srv.close()` (stop accepting; idle keep-alive connections are closed, busy ones finish their request), `srv.wait()` (block until closed).

**Server `opts`:**

| Key | Default | Description |
|-----|---------|-------------|
| `host` | all interfaces | Address to bind |
| `tls` | — | Enable HTTPS: `{cert, key [, ca]}` (`ca` requires client certificates) |
| `read_timeout` | `30000` | Time to receive a whole request once it starts (also the TLS handshake); 408 after it |
| `idle_timeout` | `60000` | Time a keep-alive connection may wait for its next request |
| `write_timeout` | `30000` | Per response write |
| `max_header_bytes` | `65536` | Larger request heads get 431 |
| `max_body_bytes` | 10 MB | Larger bodies get 413 |
| `max_requests` | `1000` | Requests per connection before it is closed |
| `keep_alive` | `true` | `false` closes after every response |
| `log` | `false` | Print one line per request: method, target, status, time |
| `quiet` | `false` | (`serve` only) no "listening" line |

The server supports keep-alive and pipelining, chunked request bodies, `Expect: 100-continue`, and HEAD (headers only). It adds `Date`, `Content-Length` and `Connection`. Malformed requests get 400, 501 (unknown Transfer-Encoding) or 505, including request-smuggling shapes: `Content-Length` together with `Transfer-Encoding`, conflicting lengths, obsolete line folding, and bare CR/LF.

**Request** (the handler's argument, a map):

| Field | Description |
|-------|-------------|
| `method` | `"GET"`, `"POST"`, … |
| `target` | Raw request target, e.g. `"/search?q=a%20b"` |
| `path` | Path **without** the query string (`"/search"`), not percent-decoded |
| `query` | Map of decoded query parameters (`req.query["q"] == "a b"`) |
| `headers` | Map with lowercase keys |
| `body` | Body string (chunked bodies already decoded) |
| `version` | `"HTTP/1.1"` or `"HTTP/1.0"` |
| `remote_addr` | `"ip:port"` of the peer |
| `tls` | `true` over HTTPS |
| `cookies` | Map from the `Cookie` header |
| `params` | Route parameters (router only) |
| `req:json()` | Decodes the body as JSON |

**Response** (what the handler returns): a table/map `{status = 200, body = "", headers = {...}, close = false}`, or just a string (200, `text/plain`). A non-string `body` is converted with `tostring`. Header values may be arrays (repeated headers). `close = true` closes the connection after the response. Anything else — `nil`, an invalid status, a header with CR/LF — becomes a 500 and is logged. Without a `Content-Type`, `text/plain; charset=utf-8` is used.

**Response helpers:**

| Function | Description |
|----------|-------------|
| `http.text(body [, status])` | `text/plain` response |
| `http.html(body [, status])` | `text/html` response |
| `http.json(value [, status [, headers]])` | JSON-encoded `value`, `application/json` |
| `http.redirect(url [, status])` | Redirect (302 by default) with `Location` |
| `http.set_cookie(resp, name, value [, opts]) → resp` | Adds a `Set-Cookie`; `opts`: `path, domain, max_age, expires, secure, http_only, same_site` |
| `http.static(dir [, opts]) → handler` | Serves files under `dir` with a MIME type by extension; `opts`: `index` (`"index.html"`), `prefix` (URL prefix to strip). GET/HEAD only; `..` and NUL in paths are refused |

### Router

```sea
local r = http.router()
r.use(function(req, next) ... return next(req) end)
r.get("/users/:id", function(req) return http.json({ id = req.params["id"] }) end)
http.serve(8080, r)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `router.get` / `post` / `put` / `delete` / `patch` / `head` | `get(path, [mws,] fn)` | Register a handler for that method |
| `router.any` | `any(path, [mws,] fn)` | Handler for any method |
| `router.use` | `use(mw)` | Global middleware: `mw(req, next) → resp` |
| `router.group` | `group(prefix, {mws}) → group` | Sub-router with a shared prefix and middlewares (same methods) |
| `router.handle` | `handle(req) → resp` | Dispatch (what the server calls) |

**Path patterns:** exact segments (`/users`), parameters (`:name` → `req.params["name"]`, percent-decoded), and a trailing `*` (the rest of the path in `req.params["*"]`).

**Automatic responses:** 404 when no path matches; 405 with an `Allow` header when the path matches but the method does not. HEAD falls back to the GET route.

**Middleware:** call `next(req)` to continue, or return a response to short-circuit. Responses are turned into mutable maps, so middleware can change `status`, `body` and `headers`; a `nil` from a handler becomes 500.

### Utilities

| Function | Description |
|----------|-------------|
| `http.parse_url(url) → {scheme, host, port, path, query, target}\|nil` | Splits an http/https URL (IPv6 literals in `[...]`, default ports, fragment dropped) |
| `http.parse_query(s) → map` | Decodes `a=1&b=x%20y` (the part after `?` if `s` is a path or URL); `+` means space |
| `http.build_query(t) → string` | Encodes a map (keys sorted; an array value repeats the key) |
| `http.url_encode(s)` / `http.url_decode(s)` | Percent-encoding (everything but `A-Z a-z 0-9 - _ . ~`); decoding turns `+` into a space |
| `http.status_text(code) → string` | Reason phrase, e.g. `"Not Found"` |

```sea
-- Client
local r = http.get("https://api.example.com/items", { query = { page = 2 }, timeout = 5000 })
if r.ok then print(r:json().total) else print(r.status, r.error) end

local r2 = http.post("https://api.example.com/items", nil, nil, { json = { name = "sea" } })

-- Server, stopped from code
local srv = http.listen(8080, function(req)
    if req.path == "/hello" then
        return http.text("Hello, " .. (req.query["name"] or "world") .. "!\n")
    end
    return { status = 404, body = "Not found\n" }
end, { log = true })
-- ...
srv.close()

-- HTTPS
http.serve(8443, function(req) return "Secure hello!\n" end,
    { tls = { cert = "cert.pem", key = "key.pem" } })
```

**Breaking changes (2026-10):** `req.path` no longer includes the query string (use `req.query`, or `req.target` for the raw form). Handlers returning `nil` now produce a 500 instead of stopping the server.

---

## `ssl`

Requires `ssl_native.so` (OpenSSL). Build: `make -C libs/ssl`.

```sea
local ssl = require("./libs/ssl/ssl")
```

Full SSL/TLS + cryptography library. All handles (connections, certs, keys, CSRs) are opaque integers; call the matching `_free` function when done.

TLS runs over sockets from the built-in `net` module (see `docs/reference.md`): every wait — handshake, read, write, accept — parks only the calling goroutine through the netpoller (on the main thread it blocks), so one slow peer never holds up the others. Timeouts are in milliseconds and apply per operation.

### TLS Client

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.connect` | `connect(host, port [, opts]) → conn\|nil` | Open a TLS client connection. `opts` (table or map): `{verify, cert, key, sni, min_version, timeout}`. On failure `ssl.last_error()` says why |
| `ssl.dial` | `dial(host, port [, opts]) → conn\|error` | Same as `connect`, but returns an error value (`is_error`) — safe from many goroutines at once |
| `ssl.last_error` | `last_error() → string\|nil` | Message of the last failure of a `nil`-returning function (shared, not per goroutine) |
| `ssl.read` | `read(conn [, n [, timeout]]) → string\|nil` | Without `n`: read until EOF. With `n`: read exactly `n` bytes (fewer if the stream ends) |
| `ssl.recv` | `recv(conn [, max [, timeout]]) → string\|error` | One read of whatever is available (up to `max`, default 65536); `""` at EOF |
| `ssl.read_request` | `read_request(conn [, timeout]) → string\|nil` | Read one full HTTP request (headers + Content-Length body). Prefer `libs/http` for real servers |
| `ssl.write` | `write(conn, data [, timeout]) → n\|nil` | Write all of `data` |
| `ssl.send` | `send(conn, data [, timeout]) → n\|error` | As `write`, with an error value on failure |
| `ssl.close` | `close(conn)` | Send close_notify (without waiting) and close the socket; wakes any goroutine waiting on it |
| `ssl.peer_cert` | `peer_cert(conn) → cert\|nil` | Peer certificate handle (inspect with `cert_info`, free with `cert_free`) |
| `ssl.version` | `version(conn) → string` | Negotiated TLS version, e.g. `"TLSv1.3"` |
| `ssl.cipher` | `cipher(conn) → string` | Negotiated cipher suite |

### TLS Server

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.server` | `server(opts) → ctx\|nil` | Create server context. `opts`: `{cert, key, ca, min_version}`. `ca` enables mTLS |
| `ssl.server_free` | `server_free(ctx)` | Free server context |
| `ssl.listen` | `listen(port [, host]) → fd\|nil` | Bind a TCP socket (a `net` fd) |
| `ssl.accept` | `accept(ctx, fd [, timeout]) → conn\|nil` | Accept one connection + TLS handshake (10 s default) |
| `ssl.handshake_server` | `handshake_server(ctx, fd [, timeout]) → conn\|error` | TLS server handshake on a socket from `net.accept`; closes it on failure |
| `ssl.wrap_server` | `wrap_server(ctx, fd [, timeout]) → conn\|nil` | As `handshake_server`, `nil` + `last_error()` on failure |
| `ssl.serve` | `serve(ctx, port, handler)` | Blocking accept loop; runs `handler(conn)` in a new goroutine per connection |

### Certificates

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.cert_load` | `cert_load(path) → cert\|nil` | Load X.509 cert from PEM file |
| `ssl.cert_parse` | `cert_parse(pem) → cert\|nil` | Parse X.509 cert from PEM string |
| `ssl.cert_info` | `cert_info(cert) → table` | `{subject, issuer, not_before, not_after, serial, fingerprint, san}`. `san` is a 0-indexed array |
| `ssl.cert_pem` | `cert_pem(cert) → string\|nil` | Serialize cert to PEM |
| `ssl.cert_save` | `cert_save(cert, path) → bool` | Save cert to PEM file |
| `ssl.cert_free` | `cert_free(cert)` | Free cert handle |
| `ssl.cert_verify` | `cert_verify(cert, ca_cert) → bool` | Verify cert was signed by ca_cert |
| `ssl.cert_self_signed` | `cert_self_signed(key, opts) → cert\|nil` | Self-signed cert. `opts`: `{cn, org, country, state, city, email, san=[], days=365, is_ca=false}` |
| `ssl.cert_sign` | `cert_sign(csr, ca_key, ca_cert, opts) → cert\|nil` | CA-sign a CSR. `opts`: `{days=365, serial=1}` |

### Keys

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.keygen` | `keygen(opts) → key\|nil` | Generate key pair. `opts`: `{type="rsa"\|"ec", bits=2048, curve="P-256"\|"P-384"\|"P-521"}` |
| `ssl.key_load` | `key_load(path [, password]) → key\|nil` | Load private key from PEM file |
| `ssl.key_parse` | `key_parse(pem [, password]) → key\|nil` | Parse private key from PEM string |
| `ssl.key_pem` | `key_pem(key [, password]) → string\|nil` | Serialize private key to PEM (AES-256-CBC encrypted if password given) |
| `ssl.key_pub_pem` | `key_pub_pem(key) → string\|nil` | Serialize public key to PEM |
| `ssl.key_save` | `key_save(key, path [, password]) → bool` | Save private key to PEM file |
| `ssl.key_free` | `key_free(key)` | Free key handle |

### CSR

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.csr_new` | `csr_new(key, opts) → csr\|nil` | Create a CSR. `opts`: `{cn, org, country, state, city, email, san=[]}` |
| `ssl.csr_pem` | `csr_pem(csr) → string\|nil` | Serialize CSR to PEM |
| `ssl.csr_free` | `csr_free(csr)` | Free CSR handle |

### Hash / HMAC

Supported `algo` strings: `"sha256"`, `"sha384"`, `"sha512"`, `"sha1"`, `"md5"`, `"sha3-256"`, `"sha3-512"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.hash` | `hash(algo, data) → string\|nil` | Hex-encoded digest |
| `ssl.hash_raw` | `hash_raw(algo, data) → string\|nil` | Binary digest |
| `ssl.hmac` | `hmac(algo, key, data) → string\|nil` | Hex-encoded HMAC |
| `ssl.hmac_raw` | `hmac_raw(algo, key, data) → string\|nil` | Binary HMAC |

### Symmetric Encryption

Supported `algo` strings: `"aes-128-gcm"`, `"aes-256-gcm"`, `"aes-128-cbc"`, `"aes-256-cbc"`, `"chacha20-poly1305"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.encrypt` | `encrypt(algo, key, plaintext [, iv]) → {data, iv, tag}\|nil` | Encrypt. For GCM/ChaCha20: IV auto-generated if omitted; `tag` is always filled |
| `ssl.decrypt` | `decrypt(algo, key, iv, ciphertext [, tag]) → string\|nil` | Decrypt. `tag` required for authenticated ciphers |

### Asymmetric Sign / Verify

Supported `algo` strings: `"rsa-sha256"`, `"rsa-sha384"`, `"rsa-sha512"`, `"rsa-pss-sha256"`, `"rsa-pss-sha384"`, `"rsa-pss-sha512"`, `"ecdsa-sha256"`, `"ecdsa-sha384"`, `"ecdsa-sha512"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.sign` | `sign(algo, key, data) → string\|nil` | Sign data; returns binary DER signature |
| `ssl.verify` | `verify(algo, key, data, sig) → bool` | Verify signature |

### Utilities

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.rand_bytes` | `rand_bytes(n) → string` | `n` cryptographically random bytes |
| `ssl.pbkdf2` | `pbkdf2(algo, password, salt, iterations, key_len) → string\|nil` | PKCS#5 PBKDF2 key derivation; returns binary |
| `ssl.hkdf` | `hkdf(algo, ikm, salt, info, length) → string\|nil` | RFC 5869 HKDF; returns binary |
| `ssl.base64_encode` | `base64_encode(data) → string` | Base64 encode (no newlines) |
| `ssl.base64_decode` | `base64_decode(str) → string\|nil` | Base64 decode |
| `ssl.hex_encode` | `hex_encode(data) → string` | Hex encode (lowercase) |
| `ssl.hex_decode` | `hex_decode(str) → string\|nil` | Hex decode |

```sea
-- TLS client
local conn = ssl.connect("example.com", 443)
ssl.write(conn, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
print(ssl.read(conn))
ssl.close(conn)

-- TLS server with mTLS
local ctx = ssl.server({cert="server.pem", key="server.key", ca="ca.pem"})
ssl.serve(ctx, 8443, function(conn)
    local data = ssl.read_request(conn)
    ssl.write(conn, "HTTP/1.0 200 OK\r\n\r\nHello\n")
    ssl.close(conn)
end)

-- Self-signed cert + key
local key = ssl.keygen({type="rsa", bits=2048})
local san = []; san[] = "localhost"; san[] = "127.0.0.1"
local cert = ssl.cert_self_signed(key, {cn="localhost", san=san, days=365})
ssl.key_save(key, "server.key")
ssl.cert_save(cert, "server.pem")
ssl.cert_free(cert)
ssl.key_free(key)

-- AES-256-GCM round-trip
local k = ssl.rand_bytes(32)
local r = ssl.encrypt("aes-256-gcm", k, "secret")
local plain = ssl.decrypt("aes-256-gcm", k, r.iv, r.data, r.tag)

-- SHA-256 hash
print(ssl.hash("sha256", "hello"))

-- RSA sign / verify
local k2 = ssl.keygen({type="rsa", bits=2048})
local sig = ssl.sign("rsa-sha256", k2, "data")
print(ssl.verify("rsa-sha256", k2, "data", sig))  -- true
ssl.key_free(k2)
```

---

## `jwt`

Requires `jwt_native.so` (OpenSSL). Build: `make -C libs/jwt`.

```sea
local jwt = require("./libs/jwt/jwt")
```

Supported algorithms: `HS256`, `HS384`, `HS512` (symmetric HMAC), `RS256` (RSA).

| Function | Signature | Description |
|----------|-----------|-------------|
| `jwt.sign` | `sign(claims, key [, opts]) → string\|nil` | Create a signed JWT token |
| `jwt.verify` | `verify(token, key [, opts]) → claims, err` | Verify and decode a token |
| `jwt.decode` | `decode(token) → header, payload` | Decode without verifying signature |
| `jwt.algs` | `algs() → array` | List of supported algorithm names |

**`sign` opts:**

| Key | Default | Description |
|-----|---------|-------------|
| `alg` | `"HS256"` | Algorithm |
| `exp` | — | Expiry in seconds from now (adds `exp` claim) |
| `iss` | — | Issuer |
| `sub` | — | Subject |
| `aud` | — | Audience |
| `nbf` | — | Not-before Unix timestamp |

**`verify` opts:**

| Key | Default | Description |
|-----|---------|-------------|
| `verify_exp` | `true` | Validate `exp` claim |

`verify` returns `claims, nil` on success and `nil, "reason"` on failure.

```sea
-- HS256
local token = jwt.sign({ sub = "user123", role = "admin" }, "my-secret", {
    alg = "HS256",
    exp = 3600,
})

local claims, err = jwt.verify(token, "my-secret")
if claims then
    print(claims.sub)   -- "user123"
else
    print("invalid:", err)
end

-- Inspect without verifying
local header, payload = jwt.decode(token)
print(header.alg)   -- "HS256"
```

---

## `flags`

Pure Sea. No build required.

```sea
local flags = require("./libs/flags/flags")
```

Modeled after Go's `flag` package. All flag definitions must come before `flags.parse()`.

### Flag definition

| Function | Signature | Description |
|----------|-----------|-------------|
| `flags.string` | `string(name, default, usage) → ref` | Define a string flag |
| `flags.int` | `int(name, default, usage) → ref` | Define an integer flag |
| `flags.bool` | `bool(name, default, usage) → ref` | Define a boolean flag |
| `flags.number` | `number(name, default, usage) → ref` | Define a float flag |

Each function returns a **flagref** table with a `.value` field that holds the parsed value after `flags.parse()` is called.

### Parsing

```sea
flags.parse()
```

Reads `os.args[1..n]`. Recognized syntax:
- `-flag value` or `--flag value`
- `-flag=value` or `--flag=value`
- `-boolFlag` (sets to `true`, no value needed)
- `--` ends flag parsing; remaining arguments become positional
- `-h` / `--help` prints usage and exits with code 0

Unknown flags call `flags.usage()` and exit with code 2.

### Accessing results

| Function | Signature | Description |
|----------|-----------|-------------|
| `flags.get` | `get(name) → value\|nil` | Current value of flag `name` |
| `flags.is_set` | `is_set(name) → bool` | `true` if the flag was explicitly passed |
| `flags.args` | `args() → table` | Positional arguments after flag parsing (0-indexed) |
| `flags.narg` | `narg() → int` | Number of positional arguments |
| `flags.nflag` | `nflag() → int` | Number of defined flags |

### Usage / help

| Function | Signature | Description |
|----------|-----------|-------------|
| `flags.usage` | `usage()` | Print default Go-style help to stderr |
| `flags.set_usage` | `set_usage(fn)` | Replace the usage function with a custom one |
| `flags.reset` | `reset()` | Clear all state (useful in tests) |

```sea
local host    = flags.string("host",    "localhost", "server address")
local port    = flags.int("port",       8080,        "server port")
local verbose = flags.bool("verbose",   false,       "verbose output")

flags.parse()

print(host.value)     -- "localhost" (or whatever was passed)
print(flags.narg())   -- number of positional args
```

---

## `parser`

Expõe o parser interno do Sea para código Sea. Ideal para linters, formatadores e ferramentas de análise estática.

**Require:** `require("./libs/parser/parser")`
**Build:** `make -C libs/parser`

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `parse` | `parse(source: string) -> result` | Faz o parse de um string de código Sea |
| `parse_file` | `parse_file(path: string) -> result` | Lê um arquivo e faz o parse |

O objeto `result` retornado tem os campos:

| Campo | Tipo | Descrição |
|-------|------|-----------|
| `ok` | bool | `true` em caso de sucesso |
| `ast` | node\|nil | Nó raiz da AST (sempre `"block"`) |
| `error` | string\|nil | Mensagem de erro (apenas quando `ok == false`) |
| `line` | int\|nil | Linha do erro (apenas quando `ok == false`) |

### AST Node Format

Cada nó é uma tabela com campos obrigatórios `type` (string) e `line` (int), mais campos específicos por tipo:

| `type` | Campos extras |
|--------|--------------|
| `"nil"`, `"break"`, `"continue"`, `"vararg"`, `"map_lit"` | — |
| `"bool"` | `value: bool` |
| `"number"` | `value: int\|number`, `is_float: bool` |
| `"string"` | `value: string` |
| `"ident"` | `name: string` |
| `"unop"` | `op: string`, `operand: node` |
| `"binop"` | `op: string`, `left: node`, `right: node` |
| `"index"` | `object: node`, `index: node` |
| `"field"` | `object: node`, `field: string` |
| `"call"` | `callee: node`, `args: [node]` |
| `"method_call"` | `object: node`, `method: string`, `args: [node]` |
| `"func"` | `name: string\|nil`, `params: [{name, is_vararg}]`, `body: node` |
| `"table"` | `fields: [{key: node\|nil, value: node}]` |
| `"array_lit"` | `cap: node\|nil` |
| `"chan_recv"` | `channel: node` |
| `"chan_send"` | `channel: node`, `value: node` |
| `"propagate"` | `expr: node` |
| `"block"` | `stmts: [node]` |
| `"do"` | `stmts: [node]` |
| `"local"` | `names: [string]`, `values: [node]` |
| `"assign"` | `targets: [node]`, `values: [node]` |
| `"if"` | `cond: node`, `then_body: node`, `else_body: node\|nil` |
| `"while"` | `cond: node`, `body: node` |
| `"for_num"` | `var: string`, `start: node`, `limit: node`, `step: node\|nil`, `body: node` |
| `"for_in"` | `vars: [string]`, `iter: node`, `body: node` |
| `"return"` | `values: [node]` |
| `"go"` | `call: node` |
| `"select"` | `cases: [{stmt: node\|nil, body: node, is_default: bool}]` |
| `"defer"` | `call: node` |
| `"panic"` | `value: node` |
| `"del"` | `target: node` |
| `"expr_stmt"` | `expr: node` |

Strings de `op` para `"binop"`: `+`, `-`, `*`, `/`, `//`, `%`, `^`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `and`, `or`, `..`, `&`, `|`, `~`, `<<`, `>>`, `<-`

Strings de `op` para `"unop"`: `-`, `not`, `~`, `<-`

Nota: `function name() ... end` na raiz é representado como `"assign"` com `values[0].type == "func"`.

### Example

```sea
local parser = require("./libs/parser/parser")

local r = parser.parse_file("mycode.sea")
if not r.ok then
    print("Erro na linha " .. r.line .. ": " .. r.error)
    return
end

-- percorre todos os statements do bloco raiz
for i = 0, len(r.ast.stmts) - 1 do
    local stmt = r.ast.stmts[i]
    print(stmt.line .. ": " .. stmt.type)
end
```
