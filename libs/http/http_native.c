/*
 * libs/http/http_native.c — HTTP/1.1 parsing and encoding helpers for http.sea.
 *
 * Sockets come from the built-in `net` module and TLS from the ssl lib; this
 * module only parses and encodes, strictly (RFC 9110/9112), so a malformed or
 * ambiguous message is rejected rather than guessed at.
 * Build:  make -C libs/http
 *
 *   parse_request_head(buf [, max])   → nil (incomplete) | head | {error=code, message}
 *   parse_response_head(buf [, max])  → nil (incomplete) | head | {error=code, message}
 *       head: {method, target, version | version, status, reason;
 *              headers (lowercase names; repeats joined with ", ",
 *              set-cookie with "\n"), head_len (bytes, incl. the blank line),
 *              content_length (-1 if absent), chunked, keep_alive,
 *              expect_continue}
 *   chunked_scan(data [, max])  → bytes of a complete chunked body at the start
 *                                 of data | 0 (incomplete) | -1 (malformed) |
 *                                 -2 (larger than max)
 *   chunked_decode(data)        → the decoded body (data holds a complete one)
 *   url_decode(s [, plus])      → percent-decoded string ('+' → ' ' unless plus=false)
 *   url_encode(s)               → percent-encoded (RFC 3986 unreserved kept)
 *   http_date()                 → current date, IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT")
 *   base64(s)                   → base64 encoding
 *   monotonic_ms()              → monotonic clock, milliseconds
 *   valid_token(s)              → true if s is an HTTP token (header name, method)
 *   valid_field_value(s)        → true if s has no CR, LF or other control bytes
 */

#include "sea.h"
#include "sea_rope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- Small helpers -------------------------------------------------------- */

static void tbl_set_buf(SeaTable *t, const char *key, const char *data, size_t len) {
    SeaString *s = sea_string_new(data, len);
    sea_table_set_str(t, key, sea_obj_val(&s->obj));
    sea_obj_release(&s->obj);
}

static SeaValue error_tbl(int code, const char *msg) {
    SeaTable *t = sea_table_new(TABLE_MAP);
    sea_table_set_str(t, "error", sea_int_val(code));
    tbl_set_buf(t, "message", msg, strlen(msg));
    return sea_obj_val(&t->obj);
}

/* tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
 *         "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA */
static bool is_tchar(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool is_token(const char *s, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) if (!is_tchar((unsigned char)s[i])) return false;
    return true;
}

/* field-value bytes: VCHAR, obs-text, SP, HTAB — no CR/LF/other controls */
static bool is_field_value(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
    }
    return true;
}

static SeaString *arg_str(int argc, SeaValue *args, int i) {
    return argc > i ? sea_value_get_flat_string(args[i]) : NULL;
}

/* Find the end of the head: offset just past the blank line (CRLFCRLF or
 * LFLF), or 0 if not there yet. */
static size_t head_end(const char *d, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (d[i] != '\n') continue;
        if (d[i + 1] == '\n') return i + 2;
        if (d[i + 1] == '\r' && i + 2 < n && d[i + 2] == '\n') return i + 3;
    }
    return 0;
}

/* Next line in [p, end): returns its length without CR/LF and sets *next. */
static size_t next_line(const char *p, const char *end, const char **next) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) { *next = end; return (size_t)(end - p); }
    *next = nl + 1;
    size_t len = (size_t)(nl - p);
    if (len && p[len - 1] == '\r') len--;
    return len;
}

/* Header block → table + framing facts. Returns 0 or an HTTP error code
 * with *msg set. */
typedef struct {
    int64_t content_length;   /* -1 = none */
    bool    chunked, has_te;
    bool    conn_close, conn_keep_alive;
    bool    expect_continue;
} Framing;

static int parse_headers(const char *p, const char *end, SeaTable *hdrs, Framing *f, const char **msg) {
    f->content_length = -1;
    while (p < end) {
        const char *next;
        size_t len = next_line(p, end, &next);
        if (len == 0) break;                         /* the blank line */
        if (p[0] == ' ' || p[0] == '\t') { *msg = "obsolete header line folding"; return 400; }
        const char *colon = memchr(p, ':', len);
        if (!colon || !is_token(p, (size_t)(colon - p))) { *msg = "malformed header line"; return 400; }
        size_t klen = (size_t)(colon - p);
        if (klen > 255) { *msg = "header name too long"; return 431; }
        char key[256];
        for (size_t i = 0; i < klen; i++) {
            char c = p[i];
            key[i] = (c >= 'A' && c <= 'Z') ? (char)(c | 0x20) : c;
        }
        key[klen] = '\0';
        const char *vs = colon + 1, *ve = p + len;
        while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
        while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t vlen = (size_t)(ve - vs);
        if (!is_field_value(vs, vlen)) { *msg = "invalid byte in header value"; return 400; }

        if (!strcmp(key, "content-length")) {
            if (vlen == 0 || vlen > 18) { *msg = "invalid Content-Length"; return 400; }
            int64_t v = 0;
            for (size_t i = 0; i < vlen; i++) {
                if (vs[i] < '0' || vs[i] > '9') { *msg = "invalid Content-Length"; return 400; }
                v = v * 10 + (vs[i] - '0');
            }
            if (f->content_length >= 0 && f->content_length != v) {
                *msg = "conflicting Content-Length headers"; return 400;
            }
            f->content_length = v;
        } else if (!strcmp(key, "transfer-encoding")) {
            if (f->has_te || vlen != 7 || strncasecmp(vs, "chunked", 7) != 0) {
                *msg = "unsupported Transfer-Encoding"; return 501;
            }
            f->has_te = f->chunked = true;
        } else if (!strcmp(key, "connection")) {
            /* comma-separated options */
            const char *q = vs;
            while (q < ve) {
                const char *c = memchr(q, ',', (size_t)(ve - q));
                const char *oe = c ? c : ve;
                while (q < oe && (*q == ' ' || *q == '\t')) q++;
                const char *te = oe;
                while (te > q && (te[-1] == ' ' || te[-1] == '\t')) te--;
                size_t ol = (size_t)(te - q);
                if (ol == 5 && !strncasecmp(q, "close", 5)) f->conn_close = true;
                if (ol == 10 && !strncasecmp(q, "keep-alive", 10)) f->conn_keep_alive = true;
                q = c ? c + 1 : ve;
            }
        } else if (!strcmp(key, "expect")) {
            if (vlen == 12 && !strncasecmp(vs, "100-continue", 12)) f->expect_continue = true;
        }

        /* store: repeats are joined (", "; set-cookie keeps one per line) */
        SeaValue old;
        if (sea_table_get_str(hdrs, key, &old) && sea_is_string(old)) {
            SeaString *os = SEA_AS_STRING(old);
            const char *sep = strcmp(key, "set-cookie") ? ", " : "\n";
            size_t sl = strlen(sep);
            size_t total = os->len + sl + vlen;
            char *buf = malloc(total);
            if (!buf) { *msg = "out of memory"; return 500; }
            memcpy(buf, os->data, os->len);
            memcpy(buf + os->len, sep, sl);
            memcpy(buf + os->len + sl, vs, vlen);
            tbl_set_buf(hdrs, key, buf, total);
            free(buf);
        } else {
            tbl_set_buf(hdrs, key, vs, vlen);
        }
        p = next;
    }
    if (f->chunked && f->content_length >= 0) {
        *msg = "both Content-Length and Transfer-Encoding";   /* request smuggling */
        return 400;
    }
    return 0;
}

static void set_framing(SeaTable *t, const Framing *f, bool keep_alive) {
    sea_table_set_str(t, "content_length", sea_int_val(f->content_length));
    sea_table_set_str(t, "chunked", sea_bool_val(f->chunked));
    sea_table_set_str(t, "keep_alive", sea_bool_val(keep_alive));
    sea_table_set_str(t, "expect_continue", sea_bool_val(f->expect_continue));
}

static int64_t opt_max(int argc, SeaValue *args, int i, int64_t def) {
    if (argc > i && sea_is_int(args[i]) && sea_as_int(args[i]) > 0) return sea_as_int(args[i]);
    return def;
}

/* ---- Heads ---------------------------------------------------------------- */

static SeaValue fn_parse_request_head(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) return error_tbl(400, "no data");
    int64_t max = opt_max(argc, args, 1, 65536);
    const char *d = s->data;
    size_t n = s->len;
    /* tolerate empty lines before the request line (RFC 9112 2.2) */
    size_t skip = 0;
    while (skip < n && (d[skip] == '\r' || d[skip] == '\n')) skip++;
    size_t he = head_end(d + skip, n - skip);
    SeaValue result;
    if (!he) {
        result = (int64_t)(n - skip) > max ? error_tbl(431, "request header too large") : SEA_NIL_VAL;
        sea_obj_release(&s->obj);
        return result;
    }
    if ((int64_t)he > max) { sea_obj_release(&s->obj); return error_tbl(431, "request header too large"); }

    const char *p = d + skip, *end = d + skip + he, *next;
    size_t ll = next_line(p, end, &next);
    const char *sp1 = memchr(p, ' ', ll);
    const char *sp2 = sp1 ? memchr(sp1 + 1, ' ', ll - (size_t)(sp1 + 1 - p)) : NULL;
    if (!sp1 || !sp2 || !is_token(p, (size_t)(sp1 - p))) {
        sea_obj_release(&s->obj);
        return error_tbl(400, "malformed request line");
    }
    const char *tg = sp1 + 1;
    size_t tl = (size_t)(sp2 - tg);
    const char *ver = sp2 + 1;
    size_t vl = ll - (size_t)(ver - p);
    if (tl == 0 || !is_field_value(tg, tl) || memchr(tg, ' ', tl) || memchr(tg, '\t', tl)) {
        sea_obj_release(&s->obj);
        return error_tbl(400, "malformed request target");
    }
    if (vl != 8 || strncmp(ver, "HTTP/1.", 7) != 0 || (ver[7] != '0' && ver[7] != '1')) {
        sea_obj_release(&s->obj);
        return error_tbl(505, "HTTP version not supported");
    }
    bool http11 = ver[7] == '1';

    SeaTable *t = sea_table_new(TABLE_MAP);
    SeaTable *hdrs = sea_table_new(TABLE_MAP);
    Framing f = {0};
    const char *msg = NULL;
    int code = parse_headers(next, end, hdrs, &f, &msg);
    if (code) {
        sea_obj_release(&hdrs->obj);
        sea_obj_release(&t->obj);
        sea_obj_release(&s->obj);
        return error_tbl(code, msg);
    }
    tbl_set_buf(t, "method", p, (size_t)(sp1 - p));
    tbl_set_buf(t, "target", tg, tl);
    tbl_set_buf(t, "version", ver, vl);
    sea_table_set_str(t, "headers", sea_obj_val(&hdrs->obj));
    sea_obj_release(&hdrs->obj);
    sea_table_set_str(t, "head_len", sea_int_val((int64_t)(skip + he)));
    bool ka = http11 ? !f.conn_close : (f.conn_keep_alive && !f.conn_close);
    set_framing(t, &f, ka);
    sea_obj_release(&s->obj);
    return sea_obj_val(&t->obj);
}

static SeaValue fn_parse_response_head(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) return error_tbl(502, "no data");
    int64_t max = opt_max(argc, args, 1, 1 << 20);
    size_t he = head_end(s->data, s->len);
    if (!he) {
        SeaValue r = (int64_t)s->len > max ? error_tbl(502, "response header too large") : SEA_NIL_VAL;
        sea_obj_release(&s->obj);
        return r;
    }
    const char *p = s->data, *end = s->data + he, *next;
    size_t ll = next_line(p, end, &next);
    /* HTTP/1.x SP 3DIGIT [SP reason] */
    if (ll < 12 || strncmp(p, "HTTP/1.", 7) != 0 || p[8] != ' ' ||
        p[9] < '1' || p[9] > '5' || p[10] < '0' || p[10] > '9' || p[11] < '0' || p[11] > '9' ||
        (ll > 12 && p[12] != ' ')) {
        sea_obj_release(&s->obj);
        return error_tbl(502, "malformed status line");
    }
    int status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    bool http11 = p[7] == '1';

    SeaTable *t = sea_table_new(TABLE_MAP);
    SeaTable *hdrs = sea_table_new(TABLE_MAP);
    Framing f = {0};
    const char *msg = NULL;
    int code = parse_headers(next, end, hdrs, &f, &msg);
    if (code) {
        sea_obj_release(&hdrs->obj);
        sea_obj_release(&t->obj);
        sea_obj_release(&s->obj);
        return error_tbl(502, msg);
    }
    tbl_set_buf(t, "version", p, 8);
    sea_table_set_str(t, "status", sea_int_val(status));
    tbl_set_buf(t, "reason", ll > 13 ? p + 13 : "", ll > 13 ? ll - 13 : 0);
    sea_table_set_str(t, "headers", sea_obj_val(&hdrs->obj));
    sea_obj_release(&hdrs->obj);
    sea_table_set_str(t, "head_len", sea_int_val((int64_t)he));
    bool ka = http11 ? !f.conn_close : (f.conn_keep_alive && !f.conn_close);
    set_framing(t, &f, ka);
    sea_obj_release(&s->obj);
    return sea_obj_val(&t->obj);
}

/* ---- Chunked transfer coding ---------------------------------------------- */

/* Walk a chunked body at d[0..n): returns the bytes it occupies if complete,
 * 0 if more data is needed, -1 if malformed, -2 if the decoded size > max.
 * If out != NULL, appends the chunk data to it (decode pass). */
typedef struct { char *data; size_t len, cap; } Out;

static int out_push(Out *o, const char *s, size_t n) {
    if (o->len + n > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 4096;
        while (nc < o->len + n) nc *= 2;
        char *p = realloc(o->data, nc);
        if (!p) return 0;
        o->data = p; o->cap = nc;
    }
    memcpy(o->data + o->len, s, n);
    o->len += n;
    return 1;
}

static int64_t chunked_walk(const char *d, size_t n, int64_t max, Out *out) {
    size_t pos = 0;
    int64_t total = 0;
    for (;;) {
        /* chunk-size [ ; extensions ] CRLF */
        const char *nl = memchr(d + pos, '\n', n - pos);
        if (!nl) return (n - pos) > 1024 ? -1 : 0;
        size_t line_end = (size_t)(nl - d);
        size_t i = pos;
        int64_t size = 0;
        int digits = 0;
        while (i < line_end) {
            char c = d[i];
            int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) break;
            if (++digits > 15) return -1;
            size = size * 16 + v;
            i++;
        }
        if (digits == 0) return -1;
        while (i < line_end && (d[i] == ' ' || d[i] == '\t')) i++;
        if (i < line_end && d[i] != ';' && !(d[i] == '\r' && i + 1 == line_end)) return -1;
        pos = line_end + 1;
        if (size == 0) {
            /* trailer fields until an empty line */
            for (;;) {
                const char *tl = memchr(d + pos, '\n', n - pos);
                if (!tl) return (n - pos) > 8192 ? -1 : 0;
                size_t len = (size_t)(tl - (d + pos));
                if (len && d[pos + len - 1] == '\r') len--;
                pos = (size_t)(tl - d) + 1;
                if (len == 0) return (int64_t)pos;
            }
        }
        total += size;
        if (total > max) return -2;
        if ((int64_t)(n - pos) < size + 1) return 0;   /* data + LF at least */
        if (out && !out_push(out, d + pos, (size_t)size)) return -1;
        pos += (size_t)size;
        if (d[pos] == '\r') {
            if (pos + 1 >= n) return 0;
            pos++;
        }
        if (d[pos] != '\n') return -1;
        pos++;
    }
}

static SeaValue fn_chunked_scan(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) return sea_int_val(-1);
    int64_t r = chunked_walk(s->data, s->len, opt_max(argc, args, 1, INT64_MAX / 2), NULL);
    sea_obj_release(&s->obj);
    return sea_int_val(r);
}

static SeaValue fn_chunked_decode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) return SEA_NIL_VAL;
    Out o = {0};
    int64_t r = chunked_walk(s->data, s->len, INT64_MAX / 2, &o);
    sea_obj_release(&s->obj);
    SeaValue v = r > 0 ? sea_obj_val(&sea_string_new(o.data ? o.data : "", o.len)->obj) : SEA_NIL_VAL;
    free(o.data);
    return v;
}

/* ---- Encoding helpers ----------------------------------------------------- */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static SeaValue fn_url_decode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) return SEA_NIL_VAL;
    bool plus = !(argc > 1 && sea_is_bool(args[1]) && !sea_as_bool(args[1]));
    char *out = malloc(s->len + 1);
    if (!out) { sea_obj_release(&s->obj); return SEA_NIL_VAL; }
    size_t o = 0;
    for (size_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (c == '%' && i + 2 < s->len &&
            hexval(s->data[i + 1]) >= 0 && hexval(s->data[i + 2]) >= 0) {
            out[o++] = (char)(hexval(s->data[i + 1]) * 16 + hexval(s->data[i + 2]));
            i += 2;
        } else {
            out[o++] = (plus && c == '+') ? ' ' : c;
        }
    }
    sea_obj_release(&s->obj);
    SeaValue r = sea_obj_val(&sea_string_new(out, o)->obj);
    free(out);
    return r;
}

static SeaValue fn_url_encode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) {
        if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0]))) {
            SeaString *t = sea_vm_tostring(vm, args[0]);
            return sea_obj_val(&t->obj);
        }
        return SEA_NIL_VAL;
    }
    static const char hx[] = "0123456789ABCDEF";
    char *out = malloc(s->len * 3 + 1);
    if (!out) { sea_obj_release(&s->obj); return SEA_NIL_VAL; }
    size_t o = 0;
    for (size_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hx[c >> 4];
            out[o++] = hx[c & 15];
        }
    }
    sea_obj_release(&s->obj);
    SeaValue r = sea_obj_val(&sea_string_new(out, o)->obj);
    free(out);
    return r;
}

static SeaValue fn_http_date(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             days[tm.tm_wday], tm.tm_mday, mons[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return sea_obj_val(&sea_string_from_cstr(buf)->obj);
}

static SeaValue fn_base64(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) return SEA_NIL_VAL;
    static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = s->len, olen = 4 * ((n + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) { sea_obj_release(&s->obj); return SEA_NIL_VAL; }
    const unsigned char *d = (const unsigned char*)s->data;
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        out[o++] = tb[d[i] >> 2];
        out[o++] = tb[((d[i] & 3) << 4) | (d[i + 1] >> 4)];
        out[o++] = tb[((d[i + 1] & 15) << 2) | (d[i + 2] >> 6)];
        out[o++] = tb[d[i + 2] & 63];
    }
    if (i < n) {
        out[o++] = tb[d[i] >> 2];
        if (i + 1 < n) {
            out[o++] = tb[((d[i] & 3) << 4) | (d[i + 1] >> 4)];
            out[o++] = tb[(d[i + 1] & 15) << 2];
        } else {
            out[o++] = tb[(d[i] & 3) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    sea_obj_release(&s->obj);
    SeaValue r = sea_obj_val(&sea_string_new(out, o)->obj);
    free(out);
    return r;
}

static SeaValue fn_monotonic_ms(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    return sea_int_val(sea_now_ms());
}

static SeaValue fn_valid_token(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) return SEA_FALSE_VAL;
    bool ok = is_token(s->data, s->len);
    sea_obj_release(&s->obj);
    return sea_bool_val(ok);
}

static SeaValue fn_valid_field_value(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = arg_str(argc, args, 0);
    if (!s) return SEA_FALSE_VAL;
    bool ok = is_field_value(s->data, s->len);
    sea_obj_release(&s->obj);
    return sea_bool_val(ok);
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "parse_request_head",  fn_parse_request_head);
    sea_mod_set(mod, "parse_response_head", fn_parse_response_head);
    sea_mod_set(mod, "chunked_scan",        fn_chunked_scan);
    sea_mod_set(mod, "chunked_decode",      fn_chunked_decode);
    sea_mod_set(mod, "url_decode",          fn_url_decode);
    sea_mod_set(mod, "url_encode",          fn_url_encode);
    sea_mod_set(mod, "http_date",           fn_http_date);
    sea_mod_set(mod, "base64",              fn_base64);
    sea_mod_set(mod, "monotonic_ms",        fn_monotonic_ms);
    sea_mod_set(mod, "valid_token",         fn_valid_token);
    sea_mod_set(mod, "valid_field_value",   fn_valid_field_value);
    return sea_obj_val(&mod->obj);
}
