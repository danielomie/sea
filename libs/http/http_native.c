/*
 * libs/http/http_native.c — Sea HTTP library: TCP transport + HTTP parsers.
 *
 * TLS is handled by the ssl lib (libs/ssl/ssl_native.so); this module
 * provides only raw TCP and HTTP parsing consumed by http.sea.
 * Build:  cd libs/http && make
 *
 * Module functions:
 *   tcp_connect(host, port [, timeout_ms]) → fd  or nil
 *   tcp_listen(port)                       → fd  or nil
 *   tcp_accept(fd)                         → cfd or nil
 *   net_read(fd)                           → string (reads until EOF)
 *   net_write(fd, data)                    → nil
 *   net_close(fd)                          → nil
 *   read_request(fd)                       → raw HTTP request string
 *   parse_response(raw)                    → table {status, ok, body, headers}
 *   parse_request(raw)                     → table {method, path, body, headers}
 *   url_decode(s)                          → percent-decoded string
 */

#include "sea.h"
#include "sea_rope.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- Dynamic buffer ------------------------------------------------------- */

typedef struct { char *data; size_t len; size_t cap; } Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf *b) { free(b->data); buf_init(b); }

static int buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *p = realloc(b->data, nc);
        if (!p) return 0;
        b->data = p; b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static const char *mem_find4(const char *data, size_t len,
                              char a, char b, char c, char d) {
    if (len < 4) return NULL;
    for (size_t i = 0; i <= len - 4; i++)
        if (data[i]==a && data[i+1]==b && data[i+2]==c && data[i+3]==d)
            return data + i;
    return NULL;
}

/* ---- TCP helpers ---------------------------------------------------------- */

static int do_tcp_connect(const char *host, const char *port, int timeout_ms) {
    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (timeout_ms > 0) {
            struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000L };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int do_tcp_listen(int port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd >= 0) {
        int no6only = 0, reuse = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no6only, sizeof(no6only));
        setsockopt(fd, SOL_SOCKET,   SO_REUSEADDR, &reuse,  sizeof(reuse));
        struct sockaddr_in6 sa = {0};
        sa.sin6_family = AF_INET6;
        sa.sin6_addr   = in6addr_any;
        sa.sin6_port   = htons((uint16_t)port);
        if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0 &&
            listen(fd, 128) == 0)
            return fd;
        close(fd); fd = -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in sa4 = {0};
    sa4.sin_family      = AF_INET;
    sa4.sin_addr.s_addr = INADDR_ANY;
    sa4.sin_port        = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr*)&sa4, sizeof(sa4)) < 0 ||
        listen(fd, 128) < 0) {
        close(fd); return -1;
    }
    return fd;
}

/* ---- Sea string/table helpers --------------------------------------------- */

static void tbl_set_str(SeaTable *t, const char *key,
                        const char *data, size_t len) {
    SeaString *s = sea_string_intern(data, len);
    sea_table_set_str(t, key, sea_obj_val(&s->obj));
    sea_obj_release(&s->obj);
}

/* ---- HTTP parsers (keep in C: performance-critical, binary-safe) ---------- */

static SeaValue do_parse_response(const char *data, size_t len) {
    SeaTable *t    = sea_table_new(TABLE_MAP);
    SeaTable *hdrs = sea_table_new(TABLE_MAP);
    const char *p = data, *end = data + len;

    int code = 0;
    const char *sp = memchr(p, ' ', (size_t)(end - p));
    if (sp && sp + 4 <= end) code = (int)strtol(sp + 1, NULL, 10);
    sea_table_set_str(t, "status", sea_int_val(code));
    sea_table_set_str(t, "ok",     sea_bool_val(code >= 200 && code < 300));

    const char *nl = mem_find4(p, (size_t)(end - p), '\r', '\n', 0, 0);
    if (!nl) nl = memchr(p, '\n', (size_t)(end - p));
    p = nl ? nl + (nl[1] == '\n' ? 2 : 1) : end;

    while (p < end) {
        if (p[0] == '\r' || p[0] == '\n') {
            p += (p[0] == '\r' && p + 1 < end && p[1] == '\n') ? 2 : 1;
            break;
        }
        const char *colon = memchr(p, ':', (size_t)(end - p));
        const char *eol   = mem_find4(p, (size_t)(end - p), '\r', '\n', 0, 0);
        if (!eol) eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        const char *next = eol + (eol < end && eol[0] == '\r' ? 2 : 1);
        if (colon && colon < eol) {
            size_t klen = (size_t)(colon - p);
            char key[256];
            if (klen < sizeof(key)) {
                memcpy(key, p, klen); key[klen] = '\0';
                for (size_t i = 0; i < klen; i++)
                    key[i] = (char)(key[i] >= 'A' && key[i] <= 'Z'
                                    ? key[i] | 0x20 : key[i]);
                const char *vs = colon + 1;
                while (vs < eol && (*vs == ' ' || *vs == '\t')) vs++;
                size_t vlen = (size_t)(eol - vs);
                while (vlen && (vs[vlen-1] == ' ' || vs[vlen-1] == '\r')) vlen--;
                tbl_set_str(hdrs, key, vs, vlen);
            }
        }
        p = next;
    }
    sea_table_set_str(t, "headers", sea_obj_val(&hdrs->obj));
    sea_obj_release(&hdrs->obj);
    tbl_set_str(t, "body", p, (size_t)(end - p));
    return sea_obj_val(&t->obj);
}

static SeaValue do_parse_request(const char *data, size_t len) {
    SeaTable *t    = sea_table_new(TABLE_MAP);
    SeaTable *hdrs = sea_table_new(TABLE_MAP);
    const char *p = data, *end = data + len;

    const char *sp1  = memchr(p, ' ', (size_t)(end - p));
    const char *eol1 = mem_find4(p, (size_t)(end - p), '\r', '\n', 0, 0);
    if (!eol1) eol1 = memchr(p, '\n', (size_t)(end - p));
    if (!eol1) eol1 = end;

    if (sp1 && sp1 < eol1) {
        tbl_set_str(t, "method", p, (size_t)(sp1 - p));
        p = sp1 + 1;
        const char *sp2 = memchr(p, ' ', (size_t)(eol1 - p));
        if (!sp2) sp2 = eol1;
        tbl_set_str(t, "path", p, (size_t)(sp2 - p));
    } else {
        tbl_set_str(t, "method", "UNKNOWN", 7);
        tbl_set_str(t, "path",   "/",       1);
    }
    p = eol1 + (eol1 < end && eol1[0] == '\r' ? 2 : 1);

    while (p < end) {
        if (p[0] == '\r' || p[0] == '\n') {
            p += (p[0] == '\r' && p + 1 < end && p[1] == '\n') ? 2 : 1;
            break;
        }
        const char *colon = memchr(p, ':', (size_t)(end - p));
        const char *eol   = mem_find4(p, (size_t)(end - p), '\r', '\n', 0, 0);
        if (!eol) eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        const char *next = eol + (eol < end && eol[0] == '\r' ? 2 : 1);
        if (colon && colon < eol) {
            size_t klen = (size_t)(colon - p);
            char key[256];
            if (klen < sizeof(key)) {
                memcpy(key, p, klen); key[klen] = '\0';
                for (size_t i = 0; i < klen; i++)
                    key[i] = (char)(key[i] >= 'A' && key[i] <= 'Z'
                                    ? key[i] | 0x20 : key[i]);
                const char *vs = colon + 1;
                while (vs < eol && (*vs == ' ' || *vs == '\t')) vs++;
                size_t vlen = (size_t)(eol - vs);
                while (vlen && (vs[vlen-1] == ' ' || vs[vlen-1] == '\r')) vlen--;
                tbl_set_str(hdrs, key, vs, vlen);
            }
        }
        p = next;
    }
    sea_table_set_str(t, "headers", sea_obj_val(&hdrs->obj));
    sea_obj_release(&hdrs->obj);
    tbl_set_str(t, "body", p, (size_t)(end > p ? end - p : 0));
    return sea_obj_val(&t->obj);
}

/* Two-phase read: headers first, then body based on Content-Length. */
static Buf read_http_request_buf(int fd) {
    Buf buf; buf_init(&buf);
    char tmp[8192];
    while (buf.len < (1u << 20)) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf_push(&buf, tmp, (size_t)n);
        const char *hdr_end = mem_find4(buf.data, buf.len, '\r','\n','\r','\n');
        if (!hdr_end) continue;
        size_t body_start = (size_t)(hdr_end - buf.data) + 4;
        size_t body_have  = buf.len - body_start;
        size_t body_need  = 0;
        for (const char *scan = buf.data; scan < hdr_end; ) {
            size_t rem = (size_t)(hdr_end - scan);
            if (rem >= 15 && strncasecmp(scan, "content-length:", 15) == 0) {
                const char *v = scan + 15;
                while (v < hdr_end && (*v == ' ' || *v == '\t')) v++;
                body_need = (size_t)strtoul(v, NULL, 10);
                break;
            }
            const char *nl = mem_find4(scan, rem, '\r','\n',0,0);
            if (!nl) nl = memchr(scan, '\n', rem);
            if (!nl) break;
            scan = nl + (nl[1] == '\n' ? 2 : 1);
        }
        while (body_have < body_need) {
            size_t want = body_need - body_have;
            ssize_t m = read(fd, tmp, want < sizeof(tmp) ? want : sizeof(tmp));
            if (m <= 0) break;
            buf_push(&buf, tmp, (size_t)m);
            body_have += (size_t)m;
        }
        break;
    }
    return buf;
}

/* ---- Exported Sea functions ----------------------------------------------- */

static SeaValue fn_tcp_connect(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_NIL_VAL;
    SeaString *host = sea_value_get_flat_string(args[0]);
    SeaString *port = sea_value_get_flat_string(args[1]);
    if (!host || !port) { if (host) sea_obj_release(&host->obj); if (port) sea_obj_release(&port->obj); return SEA_NIL_VAL; }
    int timeout_ms = 0;
    if (argc >= 3 && sea_is_int(args[2]))
        timeout_ms = (int)sea_as_int(args[2]);
    int fd = do_tcp_connect(host->data, port->data, timeout_ms);
    sea_obj_release(&host->obj);
    sea_obj_release(&port->obj);
    return fd >= 0 ? sea_int_val(fd) : SEA_NIL_VAL;
}

static SeaValue fn_url_decode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_NIL_VAL;
    const char *in  = s->data;
    size_t      n   = s->len;
    char       *out = (char*)malloc(n + 1);
    if (!out) { sea_obj_release(&s->obj); return SEA_NIL_VAL; }
    size_t outlen = 0;
    for (size_t i = 0; i < n; ) {
        if (in[i] == '%' && i + 2 < n) {
            char hex[3] = { in[i+1], in[i+2], '\0' };
            char *end;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) { out[outlen++] = (char)v; i += 3; continue; }
        }
        out[outlen++] = (in[i] == '+') ? ' ' : in[i];
        i++;
    }
    out[outlen] = '\0';
    sea_obj_release(&s->obj);
    SeaValue r = sea_obj_val(&sea_string_new(out, outlen)->obj);
    free(out);
    return r;
}

static SeaValue fn_tcp_listen(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    int port = sea_is_int(args[0]) ? (int)sea_as_int(args[0])
                                   : (int)sea_as_number(args[0]);
    signal(SIGPIPE, SIG_IGN);
    int fd = do_tcp_listen(port);
    return fd >= 0 ? sea_int_val(fd) : SEA_NIL_VAL;
}

static SeaValue fn_tcp_accept(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    struct sockaddr_storage sa; socklen_t sl = sizeof(sa);
    int cfd = accept((int)sea_as_int(args[0]), (struct sockaddr*)&sa, &sl);
    return cfd >= 0 ? sea_int_val(cfd) : SEA_NIL_VAL;
}

static SeaValue fn_net_read(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    int fd = (int)sea_as_int(args[0]);
    Buf b; buf_init(&b);
    char tmp[8192]; ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0)
        buf_push(&b, tmp, (size_t)n);
    SeaValue r = sea_obj_val(&sea_string_new(b.data ? b.data : "", b.len)->obj);
    buf_free(&b);
    return r;
}

static SeaValue fn_net_write(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    SeaString *str = sea_value_get_flat_string(args[1]);
    if (!str) return SEA_NIL_VAL;
    int fd = (int)sea_as_int(args[0]);
    const char *data = str->data;
    size_t len = str->len;
    for (size_t sent = 0; sent < len; ) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    sea_obj_release(&str->obj);
    return SEA_NIL_VAL;
}

static SeaValue fn_net_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc >= 1 && sea_is_int(args[0]))
        close((int)sea_as_int(args[0]));
    return SEA_NIL_VAL;
}

static SeaValue fn_read_request(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    Buf buf = read_http_request_buf((int)sea_as_int(args[0]));
    SeaValue r = sea_obj_val(&sea_string_new(buf.data ? buf.data : "", buf.len)->obj);
    buf_free(&buf);
    return r;
}

static SeaValue fn_parse_response(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *str = argc >= 1 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!str) {
        SeaTable *t = sea_table_new(TABLE_MAP);
        SeaTable *h = sea_table_new(TABLE_MAP);
        sea_table_set_str(t, "status", sea_int_val(0));
        sea_table_set_str(t, "ok",     sea_bool_val(false));
        tbl_set_str(t, "body",  "", 0);
        tbl_set_str(t, "error", "empty response", 14);
        sea_table_set_str(t, "headers", sea_obj_val(&h->obj));
        sea_obj_release(&h->obj);
        return sea_obj_val(&t->obj);
    }
    SeaValue r = do_parse_response(str->data, str->len);
    sea_obj_release(&str->obj);
    return r;
}

static SeaValue fn_parse_request(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *str = sea_value_get_flat_string(args[0]);
    if (!str) return SEA_NIL_VAL;
    SeaValue r = do_parse_request(str->data, str->len);
    sea_obj_release(&str->obj);
    return r;
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "tcp_connect",    fn_tcp_connect);
    sea_mod_set(mod, "tcp_listen",     fn_tcp_listen);
    sea_mod_set(mod, "tcp_accept",     fn_tcp_accept);
    sea_mod_set(mod, "net_read",       fn_net_read);
    sea_mod_set(mod, "net_write",      fn_net_write);
    sea_mod_set(mod, "net_close",      fn_net_close);
    sea_mod_set(mod, "read_request",   fn_read_request);
    sea_mod_set(mod, "parse_response", fn_parse_response);
    sea_mod_set(mod, "parse_request",  fn_parse_request);
    sea_mod_set(mod, "url_decode",     fn_url_decode);
    return sea_obj_val(&mod->obj);
}
