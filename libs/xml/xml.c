/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * libs/xml/xml.c — XML 1.0 parser, encoder and queries for Sea.
 *
 * Build:  make -C libs/xml
 * Usage:  local xml = require("../libs/xml/xml")   -- relative to your file
 *
 *   xml.parse(s [, opts])        → element (or document) | error
 *       opts: ignore_whitespace, comments, cdata, pi, namespaces, document (bools),
 *             max_depth (512)
 *   xml.valid(s)                 → bool
 *   xml.sax(s, handlers [, opts])→ true (done) | false (a handler returned false) | error
 *       handlers: start(tag, attrs [, ns]), finish(tag), text(s), cdata(s),
 *                 comment(s), pi(target, data), doctype(s)
 *   xml.encode(node [, opts])    → string | error
 *       opts: indent (spaces or a string), declaration, sort_attrs (bools), max_depth (512)
 *   xml.escape(s [, attr])       → string
 *   xml.find / findall / descendants / query / query_one / attr / text
 *   xml.element / append / set_text / set_attr / remove
 *
 * An element is a table
 *   { tag, attrs = {name = value}, attr_order = [names], children = [elements],
 *     nodes = [content in document order], text = "direct text" }
 * where nodes holds strings (text), the same element tables as children, and
 * {type = "comment"|"cdata"|"pi", value [, target]} when those are kept.
 *
 * Parsing is strict (well-formedness): matching close tags, one root, quoted
 * and unique attributes, defined entities only (DTD entities are never
 * expanded), valid UTF-8 and XML characters, a depth limit. Failures are
 * error values with line and column: "xml.parse: expected </b>, found </a> at
 * line 1, column 8". The parser is iterative: no input can overflow the stack.
 */

#include "sea.h"
#include "sea_rope.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define XML_MAX_DEPTH 512
#define XML_DEPTH_CAP 10000   /* encode and deep text recurse on the C stack */
#define XML_NS_URI    "http://www.w3.org/XML/1998/namespace"

static inline void val_release(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

/* ---- Dynamic write buffer ------------------------------------------------- */

typedef struct { char *data; size_t len, cap; int oom; } Buf;

static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static int buf_grow(Buf *b, size_t need) {
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + need + 1) nc *= 2;
    char *p = realloc(b->data, nc);
    if (!p) { b->oom = 1; return 0; }
    b->data = p;
    b->cap = nc;
    return 1;
}

static inline int buf_reserve(Buf *b, size_t n) {
    if (b->len + n + 1 <= b->cap) return 1;
    return buf_grow(b, n);
}

static inline void buf_push(Buf *b, const char *s, size_t n) {
    if (!n || !buf_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static inline void buf_putc(Buf *b, char c) {
    if (!buf_reserve(b, 1)) return;
    b->data[b->len++] = c;
}

static void buf_puts(Buf *b, const char *s) { buf_push(b, s, strlen(s)); }

/* ---- UTF-8 ------------------------------------------------------------------ */

#define utf8_seq(s, n, cp) sea_utf8_decode((s), (n), (cp))   /* sea_utf8.h */

static void push_utf8(Buf *b, uint32_t cp) {
    char t[4];
    buf_push(b, t, (size_t)sea_utf8_encode(cp, t));
}

/* XML 1.0 Char production. */
static int xml_char(uint32_t cp) {
    return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) ||
           (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
}

/* ---- Names -------------------------------------------------------------------- */

/* ASCII per the spec; any non-ASCII byte is accepted in names. */
static inline int is_name_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80;
}
static inline int is_name_char(unsigned char c) {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}
static inline int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int valid_name(const char *s, size_t n) {
    if (!n || !is_name_start((unsigned char)s[0])) return 0;
    for (size_t i = 1; i < n; i++)
        if (!is_name_char((unsigned char)s[i])) return 0;
    return 1;
}

static int all_ws(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!is_ws(s[i])) return 0;
    return 1;
}

/* Name strings: one per distinct name per call (a document of a thousand
 * <item>s shares one "item"), reusing the program's interned instance when
 * there is one. Never interns new names: input must not grow the global
 * intern table. */
#define NAMECACHE_SIZE  256
#define NAMECACHE_PROBE 8

typedef struct { SeaString **slots; } Names;

static SeaString *name_str(Names *nc, const char *data, size_t len) {
    if (!nc->slots) nc->slots = calloc(NAMECACHE_SIZE, sizeof *nc->slots);
    if (nc->slots) {
        uint32_t h = sea_string_hash(data, len);
        for (int i = 0; i < NAMECACHE_PROBE; i++) {
            SeaString **slot = &nc->slots[(h + (uint32_t)i) & (NAMECACHE_SIZE - 1)];
            SeaString *s = *slot;
            if (s && s->len == len && memcmp(s->data, data, len) == 0) {
                sea_obj_retain(&s->obj);
                return s;
            }
            if (!s) {
                s = sea_string_find_intern(data, len);
                if (!s) s = sea_string_new(data, len);
                *slot = s;                       /* the cache's reference */
                sea_obj_retain(&s->obj);         /* the caller's */
                return s;
            }
        }
    }
    SeaString *s = sea_string_find_intern(data, len);
    return s ? s : sea_string_new(data, len);
}

static void names_free(Names *nc) {
    if (!nc->slots) return;
    for (int i = 0; i < NAMECACHE_SIZE; i++)
        if (nc->slots[i]) sea_obj_release(&nc->slots[i]->obj);
    free(nc->slots);
    nc->slots = NULL;
}

/* ---- Field names and options (table, map or struct) ---------------------- */

static SeaString *K_TAG, *K_ATTRS, *K_ATTR_ORDER, *K_CHILDREN, *K_NODES, *K_TEXT, *K_TYPE,
                 *K_VALUE, *K_TARGET, *K_NS, *K_NAME, *K_ROOT, *K_DOCTYPE, *K_VERSION,
                 *K_ENCODING, *K_STANDALONE;
static SeaString *T_COMMENT, *T_CDATA, *T_PI, *T_DOCUMENT, *T_DOCTYPE, *T_TEXT;
static pthread_once_t g_keys_once = PTHREAD_ONCE_INIT;

static void keys_init(void) {
    /* interned strings are shared (atomic RC) and live as long as the module */
#define K(var, s) var = sea_string_intern(s, sizeof(s) - 1)
    K(K_TAG, "tag"); K(K_ATTRS, "attrs"); K(K_ATTR_ORDER, "attr_order");
    K(K_CHILDREN, "children"); K(K_NODES, "nodes"); K(K_TEXT, "text"); K(K_TYPE, "type");
    K(K_VALUE, "value"); K(K_TARGET, "target"); K(K_NS, "ns"); K(K_NAME, "name");
    K(K_ROOT, "root"); K(K_DOCTYPE, "doctype"); K(K_VERSION, "version");
    K(K_ENCODING, "encoding"); K(K_STANDALONE, "standalone");
    K(T_COMMENT, "comment"); K(T_CDATA, "cdata"); K(T_PI, "pi"); K(T_DOCUMENT, "document");
    T_DOCTYPE = K_DOCTYPE; T_TEXT = K_TEXT;
#undef K
}

/* Borrowed field of a table, map or struct; false when absent or nil. */
static bool fget(SeaValue o, SeaString *k, SeaValue *out) {
    *out = SEA_NIL_VAL;
    SeaValue kv = sea_obj_val(&k->obj);
    if (sea_is_table(o)) return sea_table_get(SEA_AS_TABLE(o), kv, out) && !sea_is_nil(*out);
    if (sea_is_map(o)) return sea_map_get(SEA_AS_MAP(o), kv, out) && !sea_is_nil(*out);
    if (sea_is_struct(o)) {
        *out = sea_struct_get(SEA_AS_STRUCT(o), k);
        return !sea_is_nil(*out);
    }
    return false;
}

static bool opt_get(SeaValue opts, const char *field, SeaValue *out) {
    *out = SEA_NIL_VAL;
    if (!sea_is_table(opts) && !sea_is_map(opts) && !sea_is_struct(opts)) return false;
    SeaString *k = sea_string_intern(field, strlen(field));
    bool found = fget(opts, k, out);
    sea_obj_release(&k->obj);
    return found;
}

static bool opt_bool(SeaValue opts, const char *field) {
    SeaValue v;
    return opt_get(opts, field, &v) && sea_is_bool(v) && sea_as_bool(v);
}

static int opt_int(SeaValue opts, const char *field, int def) {
    SeaValue v;
    if (!opt_get(opts, field, &v)) return def;
    if (sea_is_int(v)) return (int)sea_as_int(v);
    if (sea_is_number(v)) return (int)sea_as_number(v);
    return def;
}

static bool is_strval(SeaValue v) { return sea_is_string(v) || sea_is_rope(v); }

/* Does string value v hold exactly s[0..n)? */
static bool str_is(SeaValue v, const char *s, size_t n) {
    if (sea_is_string(v)) {
        SeaString *x = SEA_AS_STRING(v);
        return x->len == n && memcmp(x->data, s, n) == 0;
    }
    if (sea_is_rope(v)) {
        SeaString *x = sea_rope_flatten(SEA_AS_ROPE(v));
        bool eq = x->len == n && memcmp(x->data, s, n) == 0;
        sea_obj_release(&x->obj);
        return eq;
    }
    return false;
}

static bool str_eq(SeaValue v, SeaString *s) { return str_is(v, s->data, s->len); }

/* A node's type field is one of the T_* strings. */
static bool node_type_is(SeaValue v, SeaString *t) {
    SeaValue x;
    return fget(v, K_TYPE, &x) && str_eq(x, t);
}

static bool is_element(SeaValue v) {
    SeaValue t;
    return fget(v, K_TAG, &t) && is_strval(t);
}

/* ======================================================================== */
/* Parser core: an event stream over the source, checked for well-formedness */
/* ======================================================================== */

typedef struct XP XP;

typedef struct {
    const char *name; size_t nlen;     /* in the source */
    size_t voff, vlen;                 /* decoded value in p->abuf */
} XAttr;

typedef struct { const char *prefix; size_t plen; size_t uoff, ulen; } XBind;   /* uri in p->nsbuf */

typedef struct { const char *name; size_t nlen; size_t nbinds, nsbuf_len; } XOpen;

/* Callbacks return 1 to go on, 0 to stop (the sink records why). */
typedef struct {
    int (*start)(XP *p, const char *name, size_t nlen);   /* attributes: p->attrs */
    int (*end)(XP *p, const char *name, size_t nlen);
    int (*text)(XP *p, const char *s, size_t n);
    int (*cdata)(XP *p, const char *s, size_t n);
    int (*comment)(XP *p, const char *s, size_t n);
    int (*pi)(XP *p, const char *t, size_t tn, const char *d, size_t dn);
    int (*doctype)(XP *p, const char *s, size_t n);
} XSink;

struct XP {
    const char *src;
    size_t      len, pos;
    int         max_depth;
    bool        ignore_ws, keep_comments, keep_cdata, keep_pi, namespaces;
    const XSink *sink;
    void       *ctx;
    const char *err;
    char        errbuf[200];
    size_t      err_pos;
    Buf         text, abuf, nsbuf, scratch;
    XAttr      *attrs;  size_t nattrs, cattrs;
    XOpen      *open;   size_t depth, copen;
    XBind      *binds;  size_t nbinds, cbinds;
    int         oom;
    /* the start tag being reported (namespaces) */
    const char *el_ns;  size_t el_nslen;
    const char *el_local; size_t el_locallen;
    /* the XML declaration */
    const char *ver, *enc, *sa; size_t verlen, enclen, salen;
};

#define SINK(p, cb, ...) ((p)->sink && (p)->sink->cb ? (p)->sink->cb((p), __VA_ARGS__) : 1)

static int xp_fail(XP *p, size_t at, const char *fmt, ...) {
    if (!p->err) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(p->errbuf, sizeof p->errbuf, fmt, ap);
        va_end(ap);
        p->err = p->errbuf;
        p->err_pos = at;
    }
    return 0;
}

/* Names in messages are cut at 40 bytes. */
#define NM(n) (int)((n) > 40 ? 40 : (n))

static inline bool at(XP *p, const char *lit) {
    size_t n = strlen(lit);
    return p->pos + n <= p->len && memcmp(p->src + p->pos, lit, n) == 0;
}

static size_t skip_ws(XP *p) {
    size_t s = p->pos;
    while (p->pos < p->len && is_ws(p->src[p->pos])) p->pos++;
    return p->pos - s;
}

static size_t name_len_at(XP *p, size_t i) {
    if (i >= p->len || !is_name_start((unsigned char)p->src[i])) return 0;
    size_t j = i + 1;
    while (j < p->len && is_name_char((unsigned char)p->src[j])) j++;
    return j - i;
}

/* Every byte must be valid UTF-8 and an XML character. */
static int xp_check_chars(XP *p) {
    const unsigned char *s = (const unsigned char *)p->src;
    size_t i = p->pos, n = p->len;
    while (i < n) {
        unsigned char c = s[i];
        if (c >= 0x20 && c < 0x80) { i++; continue; }
        if (c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c < 0x20) return xp_fail(p, i, "character U+%04X is not allowed in XML", c);
        uint32_t cp;
        int k = utf8_seq(s + i, n - i, &cp);
        if (!k) {
            if (p->enc && !(p->enclen == 5 && strncasecmp(p->enc, "UTF-8", 5) == 0))
                return xp_fail(p, i, "invalid UTF-8 (only UTF-8 is supported; the document declares %.*s)",
                               NM(p->enclen), p->enc);
            return xp_fail(p, i, "invalid UTF-8");
        }
        if (cp == 0xFFFE || cp == 0xFFFF) return xp_fail(p, i, "character U+%04X is not allowed in XML", cp);
        i += (size_t)k;
    }
    return 1;
}

/* Decode the reference at src[i] == '&' into out; the index after ';', or 0. */
static size_t xp_ref(XP *p, size_t i, Buf *out) {
    const char *s = p->src;
    size_t n = p->len, j = i + 1;
    if (j < n && s[j] == '#') {
        int hex = 0;
        uint32_t cp = 0;
        j++;
        if (j < n && s[j] == 'x') { hex = 1; j++; }
        size_t d0 = j;
        for (; j < n && s[j] != ';'; j++) {
            char c = s[j];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return xp_fail(p, i, "malformed character reference");
            cp = cp * (hex ? 16u : 10u) + (uint32_t)v;
            if (cp > 0x10FFFF) cp = 0x110000;            /* saturate */
        }
        if (j >= n || j == d0) return xp_fail(p, i, "malformed character reference");
        if (!xml_char(cp)) return xp_fail(p, i, "character reference &%.*s; is not an XML character",
                                          NM(j - i - 1), s + i + 1);
        push_utf8(out, cp);
        return j + 1;
    }
    size_t nl = name_len_at(p, j);
    if (!nl || j + nl >= n || s[j + nl] != ';')
        return xp_fail(p, i, "'&' must start an entity reference (write &amp;)");
    const char *e = s + j;
    if      (nl == 2 && memcmp(e, "lt", 2) == 0)   buf_putc(out, '<');
    else if (nl == 2 && memcmp(e, "gt", 2) == 0)   buf_putc(out, '>');
    else if (nl == 3 && memcmp(e, "amp", 3) == 0)  buf_putc(out, '&');
    else if (nl == 4 && memcmp(e, "apos", 4) == 0) buf_putc(out, '\'');
    else if (nl == 4 && memcmp(e, "quot", 4) == 0) buf_putc(out, '"');
    else return xp_fail(p, i, "undefined entity &%.*s;", NM(nl), e);
    return j + nl + 1;
}

/* Character data up to the next '<' into p->text: references decoded, line
 * ends normalized to "\n". */
static int xp_chars(XP *p) {
    const char *s = p->src;
    size_t i = p->pos, n = p->len;
    Buf *t = &p->text;
    while (i < n) {
        size_t start = i;
        while (i < n) {
            char c = s[i];
            if (c == '<' || c == '&' || c == '\r' || c == ']') break;
            i++;
        }
        buf_push(t, s + start, i - start);
        if (i >= n || s[i] == '<') break;
        char c = s[i];
        if (c == '&') {
            size_t k = xp_ref(p, i, t);
            if (!k) return 0;
            i = k;
        } else if (c == '\r') {
            buf_putc(t, '\n');
            i++;
            if (i < n && s[i] == '\n') i++;
        } else {
            if (i + 2 < n && s[i + 1] == ']' && s[i + 2] == '>')
                return xp_fail(p, i, "']]>' is not allowed in text (write ]]&gt;)");
            buf_putc(t, ']');
            i++;
        }
    }
    p->pos = i;
    return 1;
}

/* s[0..n) with "\r\n" and "\r" as "\n" (in p->scratch when it has a '\r'). */
static const char *norm_eol(XP *p, const char *s, size_t n, size_t *outn) {
    if (!memchr(s, '\r', n)) { *outn = n; return s; }
    p->scratch.len = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') {
            buf_putc(&p->scratch, '\n');
            if (i + 1 < n && s[i + 1] == '\n') i++;
        } else {
            buf_putc(&p->scratch, s[i]);
        }
    }
    *outn = p->scratch.len;
    return p->scratch.data ? p->scratch.data : "";
}

static int xp_flush(XP *p) {
    if (!p->text.len) return 1;
    int ok = 1;
    if (!(p->ignore_ws && all_ws(p->text.data, p->text.len)))
        ok = SINK(p, text, p->text.data, p->text.len);
    p->text.len = 0;
    return ok;
}

/* <!-- ... -->, at "<!--". */
static int xp_comment(XP *p) {
    size_t start = p->pos;
    p->pos += 4;
    const char *s = p->src;
    size_t i = p->pos;
    for (;;) {
        const char *d = i < p->len ? memmem(s + i, p->len - i, "--", 2) : NULL;
        if (!d) return xp_fail(p, start, "unterminated comment");
        size_t k = (size_t)(d - s);
        if (k + 2 >= p->len || s[k + 2] != '>')
            return xp_fail(p, k, "'--' is not allowed inside a comment");
        size_t n;
        const char *c = norm_eol(p, s + p->pos, k - p->pos, &n);
        p->pos = k + 3;
        if (!p->keep_comments) return 1;
        if (p->depth && !xp_flush(p)) return 0;
        return SINK(p, comment, c, n);
    }
}

/* <? target data ?>, at "<?". */
static int xp_pi(XP *p) {
    size_t start = p->pos;
    p->pos += 2;
    size_t tn = name_len_at(p, p->pos);
    if (!tn) return xp_fail(p, p->pos, "invalid processing instruction target");
    const char *t = p->src + p->pos;
    if (tn == 3 && strncasecmp(t, "xml", 3) == 0)
        return xp_fail(p, start, "the XML declaration is only allowed at the start of the document");
    p->pos += tn;
    const char *d = p->src + p->pos;
    size_t dn = 0;
    if (!at(p, "?>")) {
        if (!skip_ws(p)) return xp_fail(p, p->pos, "expected whitespace or '?>' after <?%.*s", NM(tn), t);
        const char *e = memmem(p->src + p->pos, p->len - p->pos, "?>", 2);
        if (!e) return xp_fail(p, start, "unterminated processing instruction");
        d = p->src + p->pos;
        dn = (size_t)(e - d);
        p->pos = (size_t)(e - p->src);
    }
    p->pos += 2;
    if (!p->keep_pi) return 1;
    if (p->depth && !xp_flush(p)) return 0;
    d = norm_eol(p, d, dn, &dn);
    return SINK(p, pi, t, tn, d, dn);
}

/* <![CDATA[ ... ]]>, at "<![CDATA[". */
static int xp_cdata(XP *p) {
    size_t start = p->pos;
    p->pos += 9;
    const char *e = memmem(p->src + p->pos, p->len - p->pos, "]]>", 3);
    if (!e) return xp_fail(p, start, "unterminated CDATA section");
    size_t n;
    const char *c = norm_eol(p, p->src + p->pos, (size_t)(e - (p->src + p->pos)), &n);
    p->pos = (size_t)(e - p->src) + 3;
    if (!p->keep_cdata) {
        buf_push(&p->text, c, n);
        return 1;
    }
    if (!xp_flush(p)) return 0;
    return SINK(p, cdata, c, n);
}

/* <!DOCTYPE ...>, at "<!DOCTYPE". The internal subset is skipped, never
 * interpreted: its entities are not expanded. */
static int xp_doctype(XP *p) {
    size_t start = p->pos;
    p->pos += 9;
    if (!skip_ws(p)) return xp_fail(p, p->pos, "expected whitespace after <!DOCTYPE");
    const char *s = p->src;
    size_t b = p->pos;
    int depth = 0;
    char q = 0;
    while (p->pos < p->len) {
        char c = s[p->pos];
        if (q) {
            if (c == q) q = 0;
        } else if (c == '"' || c == '\'') {
            q = c;
        } else if (c == '<' && at(p, "<!--")) {
            const char *e = memmem(s + p->pos + 4, p->len - p->pos - 4, "-->", 3);
            if (!e) break;
            p->pos = (size_t)(e - s) + 3;
            continue;
        } else if (c == '[') {
            depth++;
        } else if (c == ']') {
            depth--;
        } else if (c == '>' && depth <= 0) {
            break;
        }
        p->pos++;
    }
    if (p->pos >= p->len) return xp_fail(p, start, "unterminated DOCTYPE");
    size_t e = p->pos;
    while (e > b && is_ws(s[e - 1])) e--;
    p->pos++;
    size_t n;
    const char *c = norm_eol(p, s + b, e - b, &n);
    return SINK(p, doctype, c, n);
}

/* <?xml version="1.0" encoding="..." standalone="..."?>, at "<?xml". */
static int xp_decl(XP *p) {
    size_t start = p->pos;
    p->pos += 5;
    static const char *names[3] = { "version", "encoding", "standalone" };
    int next = 0;
    for (;;) {
        size_t ws = skip_ws(p);
        if (at(p, "?>")) { p->pos += 2; break; }
        if (p->pos >= p->len) return xp_fail(p, start, "unterminated XML declaration");
        if (!ws) return xp_fail(p, p->pos, "expected whitespace in the XML declaration");
        size_t nl = name_len_at(p, p->pos);
        const char *nm = p->src + p->pos;
        int which = -1;
        for (int k = next; k < 3; k++)
            if (strlen(names[k]) == nl && memcmp(nm, names[k], nl) == 0) which = k;
        if (which < 0 || (next == 0 && which != 0))
            return xp_fail(p, p->pos, next == 0 ? "the XML declaration must start with version"
                                                : "unexpected '%.*s' in the XML declaration", NM(nl), nm);
        p->pos += nl;
        skip_ws(p);
        if (!at(p, "=")) return xp_fail(p, p->pos, "expected '=' in the XML declaration");
        p->pos++;
        skip_ws(p);
        char q = p->pos < p->len ? p->src[p->pos] : 0;
        if (q != '"' && q != '\'') return xp_fail(p, p->pos, "expected a quoted value in the XML declaration");
        const char *v = p->src + ++p->pos;
        const char *e = memchr(v, q, p->len - p->pos);
        if (!e) return xp_fail(p, start, "unterminated XML declaration");
        size_t vl = (size_t)(e - v);
        p->pos += vl + 1;
        if (which == 0) {
            if (vl < 3 || v[0] != '1' || v[1] != '.' || strspn(v + 2, "0123456789") < vl - 2)
                return xp_fail(p, (size_t)(v - p->src), "unsupported XML version '%.*s'", NM(vl), v);
            p->ver = v; p->verlen = vl;
        } else if (which == 1) {
            p->enc = v; p->enclen = vl;
        } else {
            if (!(vl == 3 && memcmp(v, "yes", 3) == 0) && !(vl == 2 && memcmp(v, "no", 2) == 0))
                return xp_fail(p, (size_t)(v - p->src), "standalone must be \"yes\" or \"no\"");
            p->sa = v; p->salen = vl;
        }
        next = which + 1;
    }
    if (!p->ver) return xp_fail(p, start, "the XML declaration must start with version");
    return 1;
}

/* A quoted attribute value into p->abuf (references decoded, whitespace
 * characters normalized to spaces). */
static int xp_attr_value(XP *p, size_t *voff, size_t *vlen) {
    const char *s = p->src;
    char q = p->pos < p->len ? s[p->pos] : 0;
    if (q != '"' && q != '\'') return xp_fail(p, p->pos, "attribute values must be quoted");
    size_t start = p->pos++;
    Buf *b = &p->abuf;
    *voff = b->len;
    for (;;) {
        size_t i = p->pos;
        while (i < p->len) {
            char c = s[i];
            if (c == q || c == '<' || c == '&' || c == '\t' || c == '\n' || c == '\r') break;
            i++;
        }
        buf_push(b, s + p->pos, i - p->pos);
        p->pos = i;
        if (i >= p->len) return xp_fail(p, start, "unterminated attribute value");
        char c = s[i];
        if (c == q) { p->pos++; break; }
        if (c == '<') return xp_fail(p, i, "'<' is not allowed in attribute values (write &lt;)");
        if (c == '&') {
            size_t k = xp_ref(p, i, b);
            if (!k) return 0;
            p->pos = k;
        } else {
            buf_putc(b, ' ');
            p->pos++;
            if (c == '\r' && p->pos < p->len && s[p->pos] == '\n') p->pos++;
        }
    }
    *vlen = b->len - *voff;
    return 1;
}

static int same_name(const XAttr *a, const XAttr *b) {
    return a->nlen == b->nlen && memcmp(a->name, b->name, a->nlen) == 0;
}

/* Index of a repeated attribute name, or -1. */
static long dup_attr(XP *p) {
    size_t n = p->nattrs;
    if (n < 2) return -1;
    if (n <= 12) {
        for (size_t i = 1; i < n; i++)
            for (size_t j = 0; j < i; j++)
                if (same_name(&p->attrs[i], &p->attrs[j])) return (long)i;
        return -1;
    }
    size_t cap = 32;
    while (cap < n * 2) cap *= 2;
    uint32_t *set = calloc(cap, sizeof *set);       /* attribute index + 1 */
    if (!set) { p->oom = 1; return -1; }
    long found = -1;
    for (size_t i = 0; i < n && found < 0; i++) {
        uint32_t h = sea_string_hash(p->attrs[i].name, p->attrs[i].nlen);
        for (size_t k = h & (cap - 1);; k = (k + 1) & (cap - 1)) {
            if (!set[k]) { set[k] = (uint32_t)i + 1; break; }
            if (same_name(&p->attrs[set[k] - 1], &p->attrs[i])) { found = (long)i; break; }
        }
    }
    free(set);
    return found;
}

static const XBind *ns_lookup(XP *p, const char *prefix, size_t plen) {
    for (size_t i = p->nbinds; i-- > 0;)
        if (p->binds[i].plen == plen && memcmp(p->binds[i].prefix, prefix, plen) == 0) return &p->binds[i];
    return NULL;
}

/* Split a qualified name at its colon: the prefix length, 0 for none, or -1
 * when it is not a valid QName. */
static long qname_split(const char *s, size_t n) {
    const char *c = memchr(s, ':', n);
    if (!c) return 0;
    size_t k = (size_t)(c - s);
    if (k == 0 || k + 1 >= n || memchr(c + 1, ':', n - k - 1) || !is_name_start((unsigned char)c[1])) return -1;
    return (long)k;
}

/* Resolve a prefix (bound, or the implicit xml) into uri/ulen; 0 if unbound. */
static int ns_resolve(XP *p, const char *prefix, size_t plen, const char **uri, size_t *ulen) {
    const XBind *b = ns_lookup(p, prefix, plen);
    if (b) {
        *uri = p->nsbuf.data + b->uoff;
        *ulen = b->ulen;
        return 1;
    }
    if (plen == 3 && memcmp(prefix, "xml", 3) == 0) {
        *uri = XML_NS_URI;
        *ulen = sizeof(XML_NS_URI) - 1;
        return 1;
    }
    return plen == 0;   /* no default namespace: fine */
}

/* Namespace declarations of the start tag at `at`, then its element name. */
static int xp_namespaces(XP *p, size_t at, const char *name, size_t nlen) {
    for (size_t i = 0; i < p->nattrs; i++) {
        XAttr *a = &p->attrs[i];
        const char *pre;
        size_t pl;
        if (a->nlen == 5 && memcmp(a->name, "xmlns", 5) == 0) {
            pre = "";
            pl = 0;
        } else if (a->nlen > 6 && memcmp(a->name, "xmlns:", 6) == 0) {
            pre = a->name + 6;
            pl = a->nlen - 6;
            const char *v = p->abuf.data + a->voff;
            bool is_xml_uri = a->vlen == sizeof(XML_NS_URI) - 1 && memcmp(v, XML_NS_URI, a->vlen) == 0;
            if (memchr(pre, ':', pl)) return xp_fail(p, at, "invalid namespace prefix '%.*s'", NM(pl), pre);
            if (pl == 5 && memcmp(pre, "xmlns", 5) == 0)
                return xp_fail(p, at, "the xmlns prefix cannot be declared");
            if (!a->vlen) return xp_fail(p, at, "namespace prefix '%.*s' cannot be undeclared", NM(pl), pre);
            if ((pl == 3 && memcmp(pre, "xml", 3) == 0) != is_xml_uri)
                return xp_fail(p, at, "the xml prefix and only it is bound to " XML_NS_URI);
        } else {
            continue;
        }
        if (p->nbinds == p->cbinds) {
            size_t nc = p->cbinds ? p->cbinds * 2 : 16;
            XBind *nb = realloc(p->binds, nc * sizeof *nb);
            if (!nb) { p->oom = 1; return 0; }
            p->binds = nb;
            p->cbinds = nc;
        }
        XBind *b = &p->binds[p->nbinds++];
        b->prefix = pre;
        b->plen = pl;
        b->uoff = p->nsbuf.len;
        b->ulen = a->vlen;
        buf_push(&p->nsbuf, p->abuf.data + a->voff, a->vlen);
    }
    long k = qname_split(name, nlen);
    if (k < 0) return xp_fail(p, at, "invalid qualified name '%.*s'", NM(nlen), name);
    const char *uri = NULL;
    size_t ulen = 0;
    if (!ns_resolve(p, name, (size_t)k, &uri, &ulen))
        return xp_fail(p, at, "unbound namespace prefix '%.*s'", NM((size_t)k), name);
    p->el_ns = ulen ? uri : NULL;
    p->el_nslen = ulen;
    p->el_local = name + (k ? k + 1 : 0);
    p->el_locallen = nlen - (size_t)(k ? k + 1 : 0);
    for (size_t i = 0; i < p->nattrs; i++) {
        XAttr *a = &p->attrs[i];
        long ak = qname_split(a->name, a->nlen);
        if (ak < 0) return xp_fail(p, at, "invalid qualified name '%.*s'", NM(a->nlen), a->name);
        if (ak == 0 || (ak == 5 && memcmp(a->name, "xmlns", 5) == 0)) continue;
        if (!ns_resolve(p, a->name, (size_t)ak, &uri, &ulen))
            return xp_fail(p, at, "unbound namespace prefix '%.*s'", NM((size_t)ak), a->name);
    }
    return 1;
}

/* A start tag; p->pos is just after '<'. Reports start (and end if empty). */
static int xp_start_tag(XP *p) {
    const char *s = p->src;
    size_t tag_at = p->pos - 1;
    size_t nl = name_len_at(p, p->pos);
    if (!nl) return xp_fail(p, p->pos, "expected an element name after '<'");
    const char *name = s + p->pos;
    p->pos += nl;
    p->nattrs = 0;
    p->abuf.len = 0;
    int empty;
    for (;;) {
        size_t ws = skip_ws(p);
        if (p->pos >= p->len) return xp_fail(p, tag_at, "unterminated start tag <%.*s>", NM(nl), name);
        char c = s[p->pos];
        if (c == '>') { p->pos++; empty = 0; break; }
        if (c == '/') {
            if (p->pos + 1 < p->len && s[p->pos + 1] == '>') { p->pos += 2; empty = 1; break; }
            return xp_fail(p, p->pos, "expected '>' after '/'");
        }
        if (!ws) return xp_fail(p, p->pos, "expected whitespace, '>' or '/>' in <%.*s>", NM(nl), name);
        size_t al = name_len_at(p, p->pos);
        if (!al) return xp_fail(p, p->pos, "invalid attribute name in <%.*s>", NM(nl), name);
        const char *an = s + p->pos;
        p->pos += al;
        skip_ws(p);
        if (p->pos >= p->len || s[p->pos] != '=')
            return xp_fail(p, p->pos, "attribute '%.*s' has no value", NM(al), an);
        p->pos++;
        skip_ws(p);
        size_t voff = 0, vlen = 0;
        if (!xp_attr_value(p, &voff, &vlen)) return 0;
        if (p->nattrs == p->cattrs) {
            size_t nc = p->cattrs ? p->cattrs * 2 : 8;
            XAttr *na = realloc(p->attrs, nc * sizeof *na);
            if (!na) { p->oom = 1; return 0; }
            p->attrs = na;
            p->cattrs = nc;
        }
        p->attrs[p->nattrs++] = (XAttr){ an, al, voff, vlen };
    }
    long d = dup_attr(p);
    if (p->oom) return 0;
    if (d >= 0) return xp_fail(p, tag_at, "duplicate attribute '%.*s' in <%.*s>",
                               NM(p->attrs[d].nlen), p->attrs[d].name, NM(nl), name);
    size_t nbinds = p->nbinds, nsbuf_len = p->nsbuf.len;
    if (p->namespaces && !xp_namespaces(p, tag_at, name, nl)) return 0;
    if (!SINK(p, start, name, nl)) return 0;
    if (empty) {
        p->nbinds = nbinds;
        p->nsbuf.len = nsbuf_len;
        return SINK(p, end, name, nl);
    }
    if ((int)p->depth >= p->max_depth)
        return xp_fail(p, tag_at, "nesting too deep (more than %d levels)", p->max_depth);
    if (p->depth == p->copen) {
        size_t nc = p->copen ? p->copen * 2 : 32;
        XOpen *no = realloc(p->open, nc * sizeof *no);
        if (!no) { p->oom = 1; return 0; }
        p->open = no;
        p->copen = nc;
    }
    p->open[p->depth++] = (XOpen){ name, nl, nbinds, nsbuf_len };
    return 1;
}

/* An end tag; p->pos is at "</". */
static int xp_end_tag(XP *p) {
    size_t tag_at = p->pos;
    p->pos += 2;
    XOpen *o = &p->open[p->depth - 1];
    size_t nl = name_len_at(p, p->pos);
    const char *name = p->src + p->pos;
    if (nl != o->nlen || memcmp(name, o->name, nl) != 0) {
        if (!nl) return xp_fail(p, tag_at, "expected </%.*s>", NM(o->nlen), o->name);
        return xp_fail(p, tag_at, "expected </%.*s>, found </%.*s>", NM(o->nlen), o->name, NM(nl), name);
    }
    p->pos += nl;
    skip_ws(p);
    if (p->pos >= p->len || p->src[p->pos] != '>')
        return xp_fail(p, p->pos, "expected '>' to close </%.*s", NM(nl), name);
    p->pos++;
    if (!xp_flush(p)) return 0;
    if (!SINK(p, end, o->name, o->nlen)) return 0;
    p->nbinds = o->nbinds;
    p->nsbuf.len = o->nsbuf_len;
    p->depth--;
    return 1;
}

/* The root element and everything in it; p->pos is just after its '<'. */
static int xp_root(XP *p) {
    if (!xp_start_tag(p)) return 0;
    while (p->depth > 0) {
        if (p->pos >= p->len) {
            XOpen *o = &p->open[p->depth - 1];
            return xp_fail(p, p->pos, "expected </%.*s>, found the end of the input", NM(o->nlen), o->name);
        }
        if (p->src[p->pos] != '<') {
            if (!xp_chars(p)) return 0;
            continue;
        }
        int ok;
        if (at(p, "</"))             ok = xp_end_tag(p);
        else if (at(p, "<!--"))      ok = xp_comment(p);
        else if (at(p, "<![CDATA[")) ok = xp_cdata(p);
        else if (at(p, "<?"))        ok = xp_pi(p);
        else if (at(p, "<!"))        ok = xp_fail(p, p->pos, "unexpected '<!' inside an element");
        else {
            ok = xp_flush(p);
            p->pos++;
            ok = ok && xp_start_tag(p);
        }
        if (!ok) return 0;
    }
    return 1;
}

static int xp_document(XP *p) {
    if (p->len >= 3 && memcmp(p->src, "\xEF\xBB\xBF", 3) == 0) p->pos = 3;
    if (at(p, "<?xml") && p->pos + 5 < p->len && (is_ws(p->src[p->pos + 5]) || p->src[p->pos + 5] == '?')) {
        if (!xp_decl(p)) return 0;
    }
    if (!xp_check_chars(p)) return 0;
    bool root = false, doctype = false;
    for (;;) {
        skip_ws(p);
        if (p->pos >= p->len) break;
        int ok;
        if (p->src[p->pos] != '<')
            ok = xp_fail(p, p->pos, root ? "text after the root element" : "text before the root element");
        else if (at(p, "<!--"))
            ok = xp_comment(p);
        else if (at(p, "<?"))
            ok = xp_pi(p);
        else if (at(p, "<!DOCTYPE")) {
            if (root || doctype) ok = xp_fail(p, p->pos, "DOCTYPE is only allowed once, before the root element");
            else { doctype = true; ok = xp_doctype(p); }
        } else if (at(p, "</"))
            ok = xp_fail(p, p->pos, "unexpected closing tag");
        else if (at(p, "<!"))
            ok = xp_fail(p, p->pos, "unexpected '<!' outside the root element");
        else if (root)
            ok = xp_fail(p, p->pos, "more than one root element");
        else {
            root = true;
            p->pos++;
            ok = xp_root(p);
        }
        if (!ok) return 0;
    }
    if (!root) return xp_fail(p, p->pos, "no root element");
    return 1;
}

static void xp_init(XP *p, SeaString *s, SeaValue opts) {
    memset(p, 0, sizeof *p);
    p->src = s->data;
    p->len = s->len;
    p->max_depth = opt_int(opts, "max_depth", XML_MAX_DEPTH);
    if (p->max_depth < 1) p->max_depth = XML_MAX_DEPTH;
    if (p->max_depth > 1000000) p->max_depth = 1000000;
    p->ignore_ws = opt_bool(opts, "ignore_whitespace");
    p->keep_comments = opt_bool(opts, "comments");
    p->keep_cdata = opt_bool(opts, "cdata");
    p->keep_pi = opt_bool(opts, "pi");
    p->namespaces = opt_bool(opts, "namespaces");
}

static void xp_free(XP *p) {
    buf_free(&p->text);
    buf_free(&p->abuf);
    buf_free(&p->nsbuf);
    buf_free(&p->scratch);
    free(p->attrs);
    free(p->open);
    free(p->binds);
}

static bool xp_oom(XP *p) {
    return p->oom || p->text.oom || p->abuf.oom || p->nsbuf.oom || p->scratch.oom;
}

/* "fn: message at line L, column C" as an error value. */
static SeaValue xp_error(SeaVM *vm, XP *p, const char *fn) {
    char msg[320];
    if (xp_oom(p) && !p->err) {
        snprintf(msg, sizeof msg, "%s: out of memory", fn);
    } else {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < p->err_pos && i < p->len; i++) {
            unsigned char c = (unsigned char)p->src[i];
            if (c == '\n' || (c == '\r' && (i + 1 >= p->len || p->src[i + 1] != '\n'))) { line++; col = 1; }
            else if ((c & 0xC0) != 0x80) col++;
        }
        snprintf(msg, sizeof msg, "%s: %s at line %zu, column %zu", fn,
                 p->err ? p->err : "parse failed", line, col);
    }
    return sea_make_error_str(vm, msg);
}

/* ======================================================================== */
/* Tree builder                                                             */
/* ======================================================================== */

typedef struct {
    SeaTable  *node;          /* borrowed: the parent's arrays (or root) own it */
    SeaArray  *children, *nodes;
    size_t     toff;          /* start of this element's text in Tree.tbuf */
    SeaString *only;          /* its text node, while it has exactly one */
    int        ntext;
} TFrame;

typedef struct {
    bool       document;
    TFrame    *fr;  size_t nfr, cfr;
    Buf        tbuf;          /* text of the open elements, a segment each */
    SeaValue   root;          /* owned */
    SeaArray  *top;           /* document mode: the document's nodes (owned) */
    SeaValue   doctype;       /* owned */
    SeaString *empty;
    Names      names;
    int        oom;
} Tree;

static void tset(SeaTable *t, SeaString *k, SeaValue v) {        /* takes over v */
    sea_table_set(t, sea_obj_val(&k->obj), v);
    val_release(v);
}

static SeaValue tstr(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

/* Attach a new node (rc=1) to the open element, or to the top level. */
static void t_attach(Tree *t, SeaValue v, bool element) {
    if (t->nfr) {
        TFrame *f = &t->fr[t->nfr - 1];
        if (element) sea_array_push(f->children, v);
        sea_array_push(f->nodes, v);
        val_release(v);
    } else if (t->document) {
        sea_array_push(t->top, v);
        if (element) t->root = v; else val_release(v);
    } else if (element) {
        t->root = v;
    } else {
        val_release(v);
    }
}

static int t_start(XP *p, const char *name, size_t nlen) {
    Tree *t = p->ctx;
    if (t->nfr == t->cfr) {
        size_t nc = t->cfr ? t->cfr * 2 : 32;
        TFrame *nf = realloc(t->fr, nc * sizeof *nf);
        if (!nf) { t->oom = 1; return 0; }
        t->fr = nf;
        t->cfr = nc;
    }
    SeaTable *node = sea_table_new(TABLE_MAP);
    tset(node, K_TAG, sea_obj_val(&name_str(&t->names, name, nlen)->obj));
    SeaTable *attrs = sea_table_new(TABLE_MAP);
    SeaArray *order = sea_array_new((uint32_t)p->nattrs);
    for (size_t i = 0; i < p->nattrs; i++) {
        XAttr *a = &p->attrs[i];
        SeaString *k = name_str(&t->names, a->name, a->nlen);
        SeaValue v = tstr(p->abuf.data + a->voff, a->vlen);
        sea_table_set(attrs, sea_obj_val(&k->obj), v);
        val_release(v);
        sea_array_push_move(order, sea_obj_val(&k->obj));
    }
    tset(node, K_ATTRS, sea_obj_val(&attrs->obj));
    tset(node, K_ATTR_ORDER, sea_obj_val(&order->obj));
    SeaArray *children = sea_array_new(0), *nodes = sea_array_new(0);
    tset(node, K_CHILDREN, sea_obj_val(&children->obj));
    tset(node, K_NODES, sea_obj_val(&nodes->obj));
    if (p->namespaces) {
        tset(node, K_NAME, sea_obj_val(&name_str(&t->names, p->el_local, p->el_locallen)->obj));
        if (p->el_ns) tset(node, K_NS, sea_obj_val(&name_str(&t->names, p->el_ns, p->el_nslen)->obj));
    }
    t_attach(t, sea_obj_val(&node->obj), true);
    t->fr[t->nfr++] = (TFrame){ node, children, nodes, t->tbuf.len, NULL, 0 };
    return 1;
}

static int t_end(XP *p, const char *name, size_t nlen) {
    (void)name; (void)nlen;
    Tree *t = p->ctx;
    TFrame *f = &t->fr[--t->nfr];
    size_t n = t->tbuf.len - f->toff;
    SeaValue txt;
    if (!n) {
        sea_obj_retain(&t->empty->obj);
        txt = sea_obj_val(&t->empty->obj);
    } else if (f->ntext == 1 && f->only) {
        sea_obj_retain(&f->only->obj);
        txt = sea_obj_val(&f->only->obj);
    } else {
        txt = tstr(t->tbuf.data + f->toff, n);
    }
    tset(f->node, K_TEXT, txt);
    t->tbuf.len = f->toff;
    return 1;
}

static int t_text(XP *p, const char *s, size_t n) {
    Tree *t = p->ctx;
    if (!t->nfr) return 1;
    TFrame *f = &t->fr[t->nfr - 1];
    SeaString *str = sea_string_new(s, n);
    sea_array_push(f->nodes, sea_obj_val(&str->obj));
    f->only = str;                                  /* borrowed: nodes holds it */
    f->ntext++;
    sea_obj_release(&str->obj);
    buf_push(&t->tbuf, s, n);
    return 1;
}

/* {type = T, value = s [, target = ...]} */
static SeaValue t_typed(SeaString *type, const char *s, size_t n) {
    SeaTable *x = sea_table_new(TABLE_MAP);
    sea_obj_retain(&type->obj);
    tset(x, K_TYPE, sea_obj_val(&type->obj));
    tset(x, K_VALUE, tstr(s, n));
    return sea_obj_val(&x->obj);
}

static int t_cdata(XP *p, const char *s, size_t n) {
    Tree *t = p->ctx;
    TFrame *f = &t->fr[t->nfr - 1];
    t_attach(t, t_typed(T_CDATA, s, n), false);
    f->ntext += 2;                                  /* text is no longer one node */
    buf_push(&t->tbuf, s, n);
    return 1;
}

static int t_comment(XP *p, const char *s, size_t n) {
    t_attach(p->ctx, t_typed(T_COMMENT, s, n), false);
    return 1;
}

static int t_pi(XP *p, const char *tg, size_t tn, const char *d, size_t dn) {
    SeaValue x = t_typed(T_PI, d, dn);
    tset(SEA_AS_TABLE(x), K_TARGET, tstr(tg, tn));
    t_attach(p->ctx, x, false);
    return 1;
}

static int t_doctype(XP *p, const char *s, size_t n) {
    Tree *t = p->ctx;
    if (!t->document) return 1;
    SeaValue x = t_typed(T_DOCTYPE, s, n);
    t->doctype = tstr(s, n);
    t_attach(t, x, false);
    return 1;
}

static const XSink TREE_SINK = { t_start, t_end, t_text, t_cdata, t_comment, t_pi, t_doctype };

static SeaValue fn_parse(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return sea_make_error_str(vm, "xml.parse: string expected");
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    XP p;
    xp_init(&p, s, opts);
    Tree t = { 0 };
    t.document = opt_bool(opts, "document");
    t.root = SEA_NIL_VAL;
    t.doctype = SEA_NIL_VAL;
    t.empty = sea_string_new("", 0);
    if (t.document) t.top = sea_array_new(2);
    p.sink = &TREE_SINK;
    p.ctx = &t;
    int ok = xp_document(&p) && !xp_oom(&p) && !t.oom && !t.tbuf.oom;
    SeaValue r;
    if (!ok) {
        r = xp_error(vm, &p, "xml.parse");
        val_release(t.root);
        if (t.top) sea_obj_release(&t.top->obj);
        val_release(t.doctype);
    } else if (t.document) {
        SeaTable *doc = sea_table_new(TABLE_MAP);
        sea_obj_retain(&T_DOCUMENT->obj);
        tset(doc, K_TYPE, sea_obj_val(&T_DOCUMENT->obj));
        tset(doc, K_ROOT, t.root);                  /* takes over the root's reference */
        tset(doc, K_NODES, sea_obj_val(&t.top->obj));
        if (!sea_is_nil(t.doctype)) tset(doc, K_DOCTYPE, t.doctype);
        if (p.ver) tset(doc, K_VERSION, tstr(p.ver, p.verlen));
        if (p.enc) tset(doc, K_ENCODING, tstr(p.enc, p.enclen));
        if (p.sa) tset(doc, K_STANDALONE, sea_bool_val(p.salen == 3));
        r = sea_obj_val(&doc->obj);
    } else {
        r = t.root;
    }
    sea_obj_release(&t.empty->obj);
    names_free(&t.names);
    buf_free(&t.tbuf);
    free(t.fr);
    xp_free(&p);
    sea_obj_release(&s->obj);
    return r;
}

static SeaValue fn_valid(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return SEA_FALSE_VAL;
    XP p;
    xp_init(&p, s, SEA_NIL_VAL);
    int ok = xp_document(&p) && !xp_oom(&p);
    xp_free(&p);
    sea_obj_release(&s->obj);
    return sea_bool_val(ok);
}

/* ======================================================================== */
/* SAX: handlers called as the document is read                             */
/* ======================================================================== */

typedef struct {
    SeaVM   *vm;
    SeaValue on_start, on_finish, on_text, on_cdata, on_comment, on_pi, on_doctype;   /* owned */
    Names    names;
    bool     panicked, stopped;
} Sax;

static int sax_call(Sax *x, SeaValue fn, SeaValue *args, int n) {
    SeaValue r = sea_vm_call(x->vm, fn, args, n);
    bool stop = sea_is_bool(r) && !sea_as_bool(r);
    val_release(r);
    for (int i = 0; i < n; i++) val_release(args[i]);
    if (x->vm->panic_propagating) { x->panicked = true; return 0; }
    if (stop) { x->stopped = true; return 0; }
    return 1;
}

static int s_start(XP *p, const char *name, size_t nlen) {
    Sax *x = p->ctx;
    if (sea_is_nil(x->on_start)) return 1;
    SeaTable *attrs = sea_table_new(TABLE_MAP);
    for (size_t i = 0; i < p->nattrs; i++) {
        XAttr *a = &p->attrs[i];
        SeaString *k = name_str(&x->names, a->name, a->nlen);
        SeaValue v = tstr(p->abuf.data + a->voff, a->vlen);
        sea_table_set(attrs, sea_obj_val(&k->obj), v);
        val_release(v);
        sea_obj_release(&k->obj);
    }
    SeaValue args[3] = { sea_obj_val(&name_str(&x->names, name, nlen)->obj), sea_obj_val(&attrs->obj),
                         SEA_NIL_VAL };
    int n = 2;
    if (p->namespaces) {
        n = 3;
        if (p->el_ns) args[2] = tstr(p->el_ns, p->el_nslen);
    }
    return sax_call(x, x->on_start, args, n);
}

static int s_end(XP *p, const char *name, size_t nlen) {
    Sax *x = p->ctx;
    if (sea_is_nil(x->on_finish)) return 1;
    SeaValue a[1] = { sea_obj_val(&name_str(&x->names, name, nlen)->obj) };
    return sax_call(x, x->on_finish, a, 1);
}

static int s_text(XP *p, const char *s, size_t n) {
    Sax *x = p->ctx;
    if (sea_is_nil(x->on_text)) return 1;
    SeaValue a[1] = { tstr(s, n) };
    return sax_call(x, x->on_text, a, 1);
}

static int s_cdata(XP *p, const char *s, size_t n) {
    Sax *x = p->ctx;
    SeaValue a[1] = { tstr(s, n) };
    return sax_call(x, x->on_cdata, a, 1);
}

static int s_comment(XP *p, const char *s, size_t n) {
    Sax *x = p->ctx;
    SeaValue a[1] = { tstr(s, n) };
    return sax_call(x, x->on_comment, a, 1);
}

static int s_pi(XP *p, const char *t, size_t tn, const char *d, size_t dn) {
    Sax *x = p->ctx;
    SeaValue a[2] = { tstr(t, tn), tstr(d, dn) };
    return sax_call(x, x->on_pi, a, 2);
}

static int s_doctype(XP *p, const char *s, size_t n) {
    Sax *x = p->ctx;
    if (sea_is_nil(x->on_doctype)) return 1;
    SeaValue a[1] = { tstr(s, n) };
    return sax_call(x, x->on_doctype, a, 1);
}

static const XSink SAX_SINK = { s_start, s_end, s_text, s_cdata, s_comment, s_pi, s_doctype };

static SeaValue sax_handler(SeaValue h, const char *name) {
    SeaValue v;
    if (!opt_get(h, name, &v)) return SEA_NIL_VAL;
    return sea_val_ref(v);
}

static SeaValue fn_sax(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return sea_make_error_str(vm, "xml.sax: string expected");
    SeaValue h = argc > 1 ? args[1] : SEA_NIL_VAL;
    if (!sea_is_table(h) && !sea_is_map(h) && !sea_is_struct(h)) {
        sea_obj_release(&s->obj);
        return sea_make_error_str(vm, "xml.sax: a table of handlers expected");
    }
    XP p;
    xp_init(&p, s, argc > 2 ? args[2] : SEA_NIL_VAL);
    Sax x = { 0 };
    x.vm = vm;
    x.on_start = sax_handler(h, "start");
    x.on_finish = sax_handler(h, "finish");
    x.on_text = sax_handler(h, "text");
    x.on_cdata = sax_handler(h, "cdata");
    x.on_comment = sax_handler(h, "comment");
    x.on_pi = sax_handler(h, "pi");
    x.on_doctype = sax_handler(h, "doctype");
    p.keep_cdata = !sea_is_nil(x.on_cdata);
    p.keep_comments = !sea_is_nil(x.on_comment);
    p.keep_pi = !sea_is_nil(x.on_pi);
    p.sink = &SAX_SINK;
    p.ctx = &x;
    int ok = xp_document(&p) && !xp_oom(&p);
    SeaValue r;
    if (x.panicked) r = SEA_NIL_VAL;               /* the panic unwinds from here */
    else if (x.stopped) r = SEA_FALSE_VAL;
    else if (!ok) r = xp_error(vm, &p, "xml.sax");
    else r = SEA_TRUE_VAL;
    val_release(x.on_start); val_release(x.on_finish); val_release(x.on_text);
    val_release(x.on_cdata); val_release(x.on_comment); val_release(x.on_pi);
    val_release(x.on_doctype);
    names_free(&x.names);
    xp_free(&p);
    sea_obj_release(&s->obj);
    return r;
}

/* ======================================================================== */
/* Encoder                                                                  */
/* ======================================================================== */

typedef struct {
    SeaVM      *vm;
    Buf         b;
    const char *indent;
    size_t      indent_len;
    bool        sort_attrs;
    int         max_depth;
    const char *tag;  size_t taglen;   /* the element being written, for messages */
    char        err[240];
    bool        failed;
} Enc;

static int enc_fail(Enc *e, const char *fmt, ...) {
    if (e->failed) return 0;
    e->failed = true;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (e->tag) snprintf(e->err, sizeof e->err, "<%.*s>: %s", NM(e->taglen), e->tag, msg);
    else snprintf(e->err, sizeof e->err, "%s", msg);
    return 0;
}

/* Bytes that may be written: valid UTF-8 XML characters. */
static int enc_check(Enc *e, const char *s, size_t n, const char *what) {
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        unsigned char c = u[i];
        if (c >= 0x20 && c < 0x80) { i++; continue; }
        if (c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c < 0x20) return enc_fail(e, "character U+%04X cannot be written in XML (%s)", c, what);
        uint32_t cp;
        int k = utf8_seq(u + i, n - i, &cp);
        if (!k) return enc_fail(e, "invalid UTF-8 in %s", what);
        if (cp == 0xFFFE || cp == 0xFFFF) return enc_fail(e, "character U+%04X cannot be written in XML (%s)", cp, what);
        i += (size_t)k;
    }
    return 1;
}

/* Escaped text (attr: for a double-quoted attribute value), checked to be
 * valid UTF-8 XML characters, in one pass. */
static int enc_escaped(Enc *e, const char *s, size_t n, bool attr) {
    const unsigned char *u = (const unsigned char *)s;
    Buf *b = &e->b;
    size_t run = 0, i = 0;
    while (i < n) {
        unsigned char c = u[i];
        const char *rep = NULL;
        if (c >= 0x80) {
            uint32_t cp;
            int k = utf8_seq(u + i, n - i, &cp);
            if (!k) return enc_fail(e, "invalid UTF-8 in %s", attr ? "an attribute value" : "text");
            if (cp == 0xFFFE || cp == 0xFFFF)
                return enc_fail(e, "character U+%04X cannot be written in XML", cp);
            i += (size_t)k;
            continue;
        }
        switch (c) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '\r': rep = "&#13;"; break;
            case '"': if (attr) rep = "&quot;"; break;
            case '\n': if (attr) rep = "&#10;"; break;
            case '\t': if (attr) rep = "&#9;"; break;
            default:
                if (c < 0x20 && c != '\n' && c != '\t')
                    return enc_fail(e, "character U+%04X cannot be written in XML", c);
        }
        if (rep) {
            buf_push(b, s + run, i - run);
            buf_puts(b, rep);
            run = i + 1;
        }
        i++;
    }
    buf_push(b, s + run, n - run);
    return 1;
}

static void enc_newline(Enc *e, int depth) {
    buf_putc(&e->b, '\n');
    for (int i = 0; i < depth; i++) buf_push(&e->b, e->indent, e->indent_len);
}

typedef struct { SeaString *name; SeaValue val; bool used; } EAttr;

static int eattr_cmp(const void *a, const void *b) {
    const SeaString *x = ((const EAttr *)a)->name, *y = ((const EAttr *)b)->name;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->data, y->data, n);
    return c ? c : (x->len > y->len) - (x->len < y->len);
}

static int enc_attr(Enc *e, EAttr *a) {
    if (!valid_name(a->name->data, a->name->len))
        return enc_fail(e, "invalid attribute name '%.*s'", NM(a->name->len), a->name->data);
    SeaValue v = a->val;
    SeaString *s;
    if (is_strval(v)) s = sea_value_get_flat_string(v);
    else if (sea_is_int(v) || sea_is_number(v) || sea_is_bool(v)) s = sea_vm_tostring(e->vm, v);
    else return enc_fail(e, "attribute '%.*s': string, number or bool expected, got %s",
                         NM(a->name->len), a->name->data, sea_type_name(v));
    buf_putc(&e->b, ' ');
    buf_push(&e->b, a->name->data, a->name->len);
    buf_push(&e->b, "=\"", 2);
    int ok = enc_escaped(e, s->data, s->len, true);
    buf_putc(&e->b, '"');
    sea_obj_release(&s->obj);
    return ok;
}

/* The attributes: attr_order first (unless sort_attrs), the rest by name. */
static int enc_attrs(Enc *e, SeaValue node) {
    SeaValue attrs, order;
    if (!fget(node, K_ATTRS, &attrs)) return 1;
    EAttr local[16];
    size_t n = 0, cap = 16;
    EAttr *list = local;
    uint32_t it = 0;
    SeaValue k, v;
    int ok = 1;
    for (;;) {
        bool more;
        if (sea_is_table(attrs)) more = sea_table_next(SEA_AS_TABLE(attrs), &it, &k, &v);
        else if (sea_is_map(attrs)) more = sea_map_next(SEA_AS_MAP(attrs), &it, &k, &v);
        else if (sea_is_struct(attrs)) more = sea_struct_next(SEA_AS_STRUCT(attrs), &it, &k, &v);
        else { ok = enc_fail(e, "attrs must be a table, got %s", sea_type_name(attrs)); break; }
        if (!more) break;
        if (sea_is_nil(v)) continue;
        if (!sea_is_string(k)) { ok = enc_fail(e, "attribute names must be strings"); break; }
        if (n == cap) {
            cap *= 2;
            EAttr *nl = list == local ? malloc(cap * sizeof *nl) : realloc(list, cap * sizeof *nl);
            if (!nl) { ok = enc_fail(e, "out of memory"); break; }
            if (list == local) memcpy(nl, local, sizeof local);
            list = nl;
        }
        list[n++] = (EAttr){ SEA_AS_STRING(k), v, false };
    }
    if (ok && !n) return 1;
    if (ok && !e->sort_attrs && fget(node, K_ATTR_ORDER, &order) && sea_is_array(order)) {
        SeaArray *o = SEA_AS_ARRAY(order);
        for (uint32_t i = 0; ok && i < sea_array_len(o); i++) {
            SeaValue name = sea_array_get(o, i);
            for (size_t j = 0; j < n; j++) {
                if (!list[j].used && str_eq(name, list[j].name)) {
                    list[j].used = true;
                    ok = enc_attr(e, &list[j]);
                    break;
                }
            }
        }
    }
    if (ok) {
        size_t m = 0;
        for (size_t j = 0; j < n; j++)
            if (!list[j].used) list[m++] = list[j];
        if (m > 1) qsort(list, m, sizeof *list, eattr_cmp);
        for (size_t j = 0; ok && j < m; j++) ok = enc_attr(e, &list[j]);
    }
    if (list != local) free(list);
    return ok;
}

static int enc_element(Enc *e, SeaValue v, int depth, bool pretty);

/* Content of an element: nodes, or (when it has none) text then children. */
typedef struct { SeaArray *nodes, *children; SeaValue text; } Content;

static uint32_t content_len(const Content *c) {
    if (c->nodes) return sea_array_len(c->nodes);
    return (sea_is_nil(c->text) ? 0 : 1) + (c->children ? sea_array_len(c->children) : 0);
}

static SeaValue content_at(const Content *c, uint32_t i) {
    if (c->nodes) return sea_array_get(c->nodes, i);
    if (!sea_is_nil(c->text)) {
        if (i == 0) return c->text;
        i--;
    }
    return sea_array_get(c->children, i);
}

/* Text an item contributes to its element's `text` (strings, cdata, text). */
static bool item_text(SeaValue x, SeaValue *s) {
    if (is_strval(x)) { *s = x; return true; }
    SeaValue t;
    if (!sea_is_obj(x) || !fget(x, K_TYPE, &t)) return false;     /* elements have no type */
    if ((str_eq(t, T_CDATA) || str_eq(t, T_TEXT)) && !is_element(x))
        return fget(x, K_VALUE, s) && is_strval(*s);
    return false;
}

/* Does string value x match text[*off..] (advancing *off)? */
static bool text_part(SeaValue x, const char *td, size_t tl, size_t *off) {
    SeaString *f = sea_is_string(x) ? SEA_AS_STRING(x) : sea_value_get_flat_string(x);
    bool ok = *off + f->len <= tl && memcmp(td + *off, f->data, f->len) == 0;
    *off += f->len;
    if (!sea_is_string(x)) sea_obj_release(&f->obj);
    return ok;
}

/* nodes must agree with children (the same elements in order) and with text
 * (its strings joined), so that an edit of one view is never silently lost. */
static bool content_agrees(const Content *c) {
    uint32_t n = sea_array_len(c->nodes), ci = 0;
    uint32_t nc = c->children ? sea_array_len(c->children) : 0;
    SeaString *flat = NULL;
    const char *td = NULL;
    size_t tl = 0, toff = 0;
    bool has_text = !sea_is_nil(c->text), ok = true;
    if (has_text) {
        flat = sea_is_string(c->text) ? NULL : sea_value_get_flat_string(c->text);
        SeaString *t = flat ? flat : SEA_AS_STRING(c->text);
        td = t->data;
        tl = t->len;
    }
    for (uint32_t i = 0; ok && i < n; i++) {
        SeaValue x = sea_array_get(c->nodes, i), s;
        if (is_strval(x)) {
            if (has_text) ok = text_part(x, td, tl, &toff);
        } else if (!sea_is_obj(x)) {
            continue;
        } else if (ci < nc && sea_array_get(c->children, ci) == x) {
            ci++;                                       /* the common case: no lookups */
        } else if (item_text(x, &s)) {
            if (has_text) ok = text_part(s, td, tl, &toff);
        } else if (c->children && is_element(x)) {
            ok = false;
        }
    }
    if (c->children && ci != nc) ok = false;
    if (has_text && toff != tl) ok = false;
    if (flat) sea_obj_release(&flat->obj);
    return ok;
}

static bool is_blank(SeaValue x) {
    if (!is_strval(x)) return false;
    SeaString *s = sea_value_get_flat_string(x);
    bool b = all_ws(s->data, s->len);
    sea_obj_release(&s->obj);
    return b;
}

static int enc_string_field(Enc *e, SeaValue node, SeaString *key, SeaString **out, const char *what) {
    SeaValue v;
    if (!fget(node, key, &v)) { *out = NULL; return 1; }
    if (!is_strval(v)) return enc_fail(e, "%s must be a string, got %s", what, sea_type_name(v));
    *out = sea_value_get_flat_string(v);
    return 1;
}

/* A comment, CDATA, PI, text or doctype node, or an element. */
static int enc_item(Enc *e, SeaValue x, int depth, bool pretty, bool top) {
    if (is_strval(x)) {
        SeaString *s = sea_value_get_flat_string(x);
        int ok = enc_escaped(e, s->data, s->len, false);
        sea_obj_release(&s->obj);
        return ok;
    }
    if (!sea_is_table(x) && !sea_is_map(x) && !sea_is_struct(x))
        return enc_fail(e, "unexpected %s in the content", sea_type_name(x));
    if (is_element(x)) return enc_element(e, x, depth, pretty);
    SeaValue type;
    if (!fget(x, K_TYPE, &type)) return enc_fail(e, "a node needs a tag (element) or a type");
    SeaString *val = NULL;
    if (!enc_string_field(e, x, K_VALUE, &val, "a node's value")) return 0;
    const char *s = val ? val->data : "";
    size_t n = val ? val->len : 0;
    Buf *b = &e->b;
    int ok = 1;
    if (str_eq(type, T_TEXT)) {
        ok = enc_escaped(e, s, n, false);
    } else if (str_eq(type, T_COMMENT)) {
        if (memmem(s, n, "--", 2) || (n && s[n - 1] == '-'))
            ok = enc_fail(e, "a comment cannot contain '--' or end with '-'");
        else if ((ok = enc_check(e, s, n, "a comment"))) {
            buf_puts(b, "<!--");
            buf_push(b, s, n);
            buf_puts(b, "-->");
        }
    } else if (str_eq(type, T_CDATA)) {
        if ((ok = enc_check(e, s, n, "a CDATA section"))) {
            buf_puts(b, "<![CDATA[");
            size_t i = 0;
            const char *m;
            while ((m = memmem(s + i, n - i, "]]>", 3)) != NULL) {     /* split "]]>" */
                size_t k = (size_t)(m - s);
                buf_push(b, s + i, k + 2 - i);
                buf_puts(b, "]]><![CDATA[");
                i = k + 2;
            }
            buf_push(b, s + i, n - i);
            buf_puts(b, "]]>");
        }
    } else if (str_eq(type, T_PI)) {
        SeaString *tg = NULL;
        ok = enc_string_field(e, x, K_TARGET, &tg, "a pi's target");
        if (ok && (!tg || !valid_name(tg->data, tg->len) || (tg->len == 3 && strncasecmp(tg->data, "xml", 3) == 0)))
            ok = enc_fail(e, "a pi needs a valid target");
        else if (ok && memmem(s, n, "?>", 2))
            ok = enc_fail(e, "a pi cannot contain '?>'");
        else if (ok && (ok = enc_check(e, s, n, "a pi"))) {
            buf_puts(b, "<?");
            buf_push(b, tg->data, tg->len);
            if (n) { buf_putc(b, ' '); buf_push(b, s, n); }
            buf_puts(b, "?>");
        }
        if (tg) sea_obj_release(&tg->obj);
    } else if (str_eq(type, T_DOCTYPE)) {
        if (!top) ok = enc_fail(e, "a doctype is only allowed at the top of a document");
        else if (!n)
            ok = enc_fail(e, "a doctype needs a value");
        else if ((ok = enc_check(e, s, n, "a doctype"))) {
            buf_puts(b, "<!DOCTYPE ");
            buf_push(b, s, n);
            buf_putc(b, '>');
        }
    } else {
        ok = enc_fail(e, "unknown node type");
    }
    if (val) sea_obj_release(&val->obj);
    return ok;
}

static int enc_array_field(Enc *e, SeaValue node, SeaString *key, SeaArray **out) {
    SeaValue v;
    *out = NULL;
    if (!fget(node, key, &v)) return 1;
    if (!sea_is_array(v)) return enc_fail(e, "%s must be an array, got %s", key->data, sea_type_name(v));
    *out = SEA_AS_ARRAY(v);
    return 1;
}

static int enc_element(Enc *e, SeaValue v, int depth, bool pretty) {
    if (depth > e->max_depth)
        return enc_fail(e, "nesting too deep (more than %d levels) or a node contains itself", e->max_depth);
    SeaValue tagv;
    fget(v, K_TAG, &tagv);
    SeaString *tag = sea_value_get_flat_string(tagv);
    const char *saved = e->tag;
    size_t saved_len = e->taglen;
    e->tag = tag->data;
    e->taglen = tag->len;
    int ok = 1;
    Content c = { NULL, NULL, SEA_NIL_VAL };
    if (!valid_name(tag->data, tag->len)) ok = enc_fail(e, "invalid tag name");
    if (ok) ok = enc_array_field(e, v, K_NODES, &c.nodes) && enc_array_field(e, v, K_CHILDREN, &c.children);
    if (ok && fget(v, K_TEXT, &c.text) && !is_strval(c.text))
        ok = enc_fail(e, "text must be a string, got %s", sea_type_name(c.text));
    if (ok && c.nodes && !content_agrees(&c)) {
        /* only text in nodes: text and children say it all (a leaf whose
         * text was assigned, say); otherwise an edit would be lost */
        bool only_text = true;
        SeaValue s;
        for (uint32_t i = 0; only_text && i < sea_array_len(c.nodes); i++)
            only_text = item_text(sea_array_get(c.nodes, i), &s);
        if (only_text) c.nodes = NULL;
    }
    if (ok && c.nodes && !content_agrees(&c))
        ok = enc_fail(e, "nodes, children and text disagree: change content with xml.append, "
                         "xml.set_text and xml.remove, or set nodes to nil");
    if (ok) {
        buf_putc(&e->b, '<');
        buf_push(&e->b, tag->data, tag->len);
        ok = enc_attrs(e, v);
    }
    if (ok) {
        uint32_t n = content_len(&c);
        bool mixed = false, any = false;
        for (uint32_t i = 0; i < n && !(any && (mixed || !pretty)); i++) {
            SeaValue x = content_at(&c, i), s;
            if (sea_is_string(x)) {
                SeaString *f = SEA_AS_STRING(x);
                if (f->len) any = true;
                if (pretty && !all_ws(f->data, f->len)) mixed = true;
            } else if (sea_is_rope(x)) {
                any = mixed = true;                     /* ropes are never empty */
            } else if (!sea_is_nil(x)) {
                any = true;
                if (pretty && item_text(x, &s)) mixed = true;
            }
        }
        bool indent = pretty && !mixed;
        if (indent) {
            any = false;
            for (uint32_t i = 0; i < n && !any; i++) {
                SeaValue x = content_at(&c, i);
                any = !sea_is_nil(x) && !is_blank(x);
            }
        }
        if (!any) {
            buf_push(&e->b, "/>", 2);
        } else {
            buf_putc(&e->b, '>');
            for (uint32_t i = 0; ok && i < n; i++) {
                SeaValue x = content_at(&c, i);
                if (sea_is_nil(x) || (indent && is_blank(x))) continue;
                if (indent) enc_newline(e, depth + 1);
                ok = enc_item(e, x, depth + 1, indent, false);
            }
            if (indent) enc_newline(e, depth);
            buf_push(&e->b, "</", 2);
            buf_push(&e->b, tag->data, tag->len);
            buf_putc(&e->b, '>');
        }
    }
    e->tag = saved;
    e->taglen = saved_len;
    sea_obj_release(&tag->obj);
    return ok;
}

static int enc_document(Enc *e, SeaValue doc, bool decl) {
    SeaArray *nodes = NULL;
    if (!enc_array_field(e, doc, K_NODES, &nodes)) return 0;
    SeaValue root = SEA_NIL_VAL;
    fget(doc, K_ROOT, &root);
    bool pretty = e->indent != NULL;
    if (!nodes) {
        if (!is_element(root)) return enc_fail(e, "a document needs a root element");
        if (decl) buf_putc(&e->b, '\n');
        return enc_element(e, root, 0, pretty);
    }
    int roots = 0, ok = 1;
    bool first = true;
    uint32_t n = sea_array_len(nodes);
    for (uint32_t i = 0; ok && i < n; i++) {
        SeaValue x = sea_array_get(nodes, i);
        if (is_strval(x)) {
            if (is_blank(x)) continue;
            ok = enc_fail(e, "text is not allowed outside the root element");
            break;
        }
        if (is_element(x)) {
            roots++;
            if (!sea_is_nil(root) && x != root) ok = enc_fail(e, "the document's root is not in its nodes");
        }
        if (!ok) break;
        if (first ? decl : pretty) buf_putc(&e->b, '\n');
        first = false;
        ok = enc_item(e, x, 0, pretty, true);
    }
    if (ok && roots != 1) ok = enc_fail(e, "a document needs exactly one root element");
    return ok;
}

static SeaValue fn_encode(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    if (argc < 1 || (!is_element(args[0]) && !node_type_is(args[0], T_DOCUMENT)))
        return sea_make_error_str(vm, "xml.encode: an element or a document expected");
    Enc e = { 0 };
    e.vm = vm;
    e.max_depth = opt_int(opts, "max_depth", XML_MAX_DEPTH);
    if (e.max_depth < 1 || e.max_depth > XML_DEPTH_CAP) e.max_depth = XML_MAX_DEPTH;
    e.sort_attrs = opt_bool(opts, "sort_attrs");
    bool decl = opt_bool(opts, "declaration");
    static const char spaces[] = "                ";   /* up to 16 */
    SeaValue ind;
    SeaString *ind_s = NULL;
    if (opt_get(opts, "indent", &ind)) {
        if (sea_is_int(ind) && sea_as_int(ind) > 0) {
            int64_t n = sea_as_int(ind);
            e.indent = spaces;
            e.indent_len = (size_t)(n > 16 ? 16 : n);
        } else if ((ind_s = sea_value_get_flat_string(ind)) != NULL) {
            e.indent = ind_s->data;
            e.indent_len = ind_s->len;
        }
    }
    if (decl) buf_puts(&e.b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    int ok;
    if (is_element(args[0])) {
        if (decl) buf_putc(&e.b, '\n');
        ok = enc_element(&e, args[0], 0, e.indent != NULL);
    } else {
        ok = enc_document(&e, args[0], decl);
    }
    SeaValue r;
    if (!ok || e.b.oom) {
        char msg[300];
        snprintf(msg, sizeof msg, "xml.encode: %s", e.b.oom ? "out of memory" : e.err);
        r = sea_make_error_str(vm, msg);
    } else {
        r = tstr(e.b.data ? e.b.data : "", e.b.len);
    }
    if (ind_s) sea_obj_release(&ind_s->obj);
    buf_free(&e.b);
    return r;
}

static SeaValue fn_escape(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_vm_tostring(vm, args[0]) : sea_string_new("", 0);
    bool attr = argc > 1 && sea_is_truthy(args[1]);
    Buf b = { 0 };
    for (size_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        switch (c) {
            case '&': buf_puts(&b, "&amp;"); break;
            case '<': buf_puts(&b, "&lt;"); break;
            case '>': buf_puts(&b, "&gt;"); break;
            case '\r': buf_puts(&b, "&#13;"); break;
            case '"': if (attr) { buf_puts(&b, "&quot;"); break; } buf_putc(&b, c); break;
            case '\'': if (attr) { buf_puts(&b, "&apos;"); break; } buf_putc(&b, c); break;
            case '\n': if (attr) { buf_puts(&b, "&#10;"); break; } buf_putc(&b, c); break;
            case '\t': if (attr) { buf_puts(&b, "&#9;"); break; } buf_putc(&b, c); break;
            default: buf_putc(&b, c);
        }
    }
    sea_obj_release(&s->obj);
    SeaValue r = tstr(b.data ? b.data : "", b.len);
    buf_free(&b);
    return r;
}

/* ======================================================================== */
/* Reading helpers and queries                                              */
/* ======================================================================== */

/* A node's child elements: an element's children, a document's root. The
 * sentinel SEA_TRUE_VAL stands for the parent of the query's start node. */
typedef struct { SeaArray *arr; SeaValue one; uint32_t n; } Kids;

static void kids_of(SeaValue v, SeaValue start, Kids *k) {
    k->arr = NULL;
    k->one = SEA_NIL_VAL;
    k->n = 0;
    SeaValue x;
    if (v == SEA_TRUE_VAL) {
        if (node_type_is(start, T_DOCUMENT)) { kids_of(start, start, k); return; }
        k->one = start;
        k->n = 1;
        return;
    }
    if (node_type_is(v, T_DOCUMENT)) {
        if (fget(v, K_ROOT, &x)) { k->one = x; k->n = 1; return; }
        if (fget(v, K_NODES, &x) && sea_is_array(x)) { k->arr = SEA_AS_ARRAY(x); k->n = sea_array_len(k->arr); }
        return;
    }
    if (fget(v, K_CHILDREN, &x) && sea_is_array(x)) { k->arr = SEA_AS_ARRAY(x); k->n = sea_array_len(k->arr); }
}

static SeaValue kid(const Kids *k, uint32_t i) { return k->arr ? sea_array_get(k->arr, i) : k->one; }

static bool tag_is(SeaValue v, const char *name, size_t n) {
    SeaValue t;
    if (!fget(v, K_TAG, &t)) return false;
    if (n == 1 && name[0] == '*') return is_strval(t);
    return str_is(t, name, n);
}

/* Growable list of borrowed values. */
typedef struct { SeaValue *v; size_t n, cap; int oom; } VList;

static void vl_push(VList *l, SeaValue x) {
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        SeaValue *nv = realloc(l->v, nc * sizeof *nv);
        if (!nv) { l->oom = 1; return; }
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = x;
}

static SeaValue vl_array(VList *l) {
    SeaArray *a = sea_array_new((uint32_t)l->n);
    for (size_t i = 0; i < l->n; i++) sea_array_push(a, l->v[i]);
    return sea_obj_val(&a->obj);
}

/* Element v and the elements under it, in document order (self only when
 * with_self). Returns 0 when deeper than XML_DEPTH_CAP (or cyclic). */
static int walk(SeaValue v, SeaValue start, bool with_self, VList *out) {
    typedef struct { Kids k; uint32_t i; } W;
    size_t cap = 64, n = 0;
    W *st = malloc(cap * sizeof *st);
    if (!st) { out->oom = 1; return 1; }
    if (with_self) vl_push(out, v);
    kids_of(v, start, &st[n].k);
    st[n++].i = 0;
    int ok = 1;
    while (n) {
        W *w = &st[n - 1];
        if (w->i >= w->k.n) { n--; continue; }
        SeaValue c = kid(&w->k, w->i++);
        if (!is_element(c)) continue;
        vl_push(out, c);
        if (n >= XML_DEPTH_CAP) { ok = 0; break; }
        if (n == cap) {
            cap *= 2;
            W *ns = realloc(st, cap * sizeof *ns);
            if (!ns) { out->oom = 1; break; }
            st = ns;
        }
        kids_of(c, start, &st[n].k);
        st[n++].i = 0;
    }
    free(st);
    return ok;
}

/* ---- query: a small XPath subset ----------------------------------------- */

enum { S_NAME, S_ATTR, S_TEXT, S_SELF };
enum { P_POS, P_LAST, P_ATTR, P_CHILD, P_TEXT };

typedef struct {
    int         kind;
    long        pos;
    const char *name; size_t nlen;      /* attribute or child name */
    bool        cmp, neg;               /* = / != literal */
    const char *lit; size_t litn;
} Pred;

#define MAX_STEPS 32
#define MAX_PREDS 8

typedef struct {
    bool        desc;                   /* reached through // */
    int         kind;
    const char *name; size_t nlen;      /* "*" for any */
    Pred        preds[MAX_PREDS];
    int         np;
} Step;

typedef struct {
    Step   steps[MAX_STEPS];
    int    n;
    bool   absolute;
    char   err[160];
} Path;

static int path_fail(Path *q, const char *path, size_t i, const char *what) {
    (void)path;
    snprintf(q->err, sizeof q->err, "%s at position %zu", what, i + 1);
    return 0;
}

static size_t path_name(const char *s, size_t n, size_t i) {
    if (i >= n || !is_name_start((unsigned char)s[i])) return 0;
    size_t j = i + 1;
    while (j < n && is_name_char((unsigned char)s[j])) j++;
    return j - i;
}

static void path_ws(const char *s, size_t n, size_t *i) {
    while (*i < n && (s[*i] == ' ' || s[*i] == '\t')) (*i)++;
}

static int path_pred(Path *q, const char *s, size_t n, size_t *ip, Pred *pr) {
    size_t i = *ip;
    memset(pr, 0, sizeof *pr);
    path_ws(s, n, &i);
    if (i < n && s[i] >= '0' && s[i] <= '9') {
        long v = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); if (v > 1000000000) v = 1000000000; i++; }
        if (v < 1) return path_fail(q, s, *ip, "positions start at 1");
        pr->kind = P_POS;
        pr->pos = v;
    } else if (n - i >= 6 && memcmp(s + i, "last()", 6) == 0) {
        pr->kind = P_LAST;
        i += 6;
    } else {
        if (n - i >= 6 && memcmp(s + i, "text()", 6) == 0) {
            pr->kind = P_TEXT;
            i += 6;
        } else {
            pr->kind = P_CHILD;
            if (i < n && s[i] == '@') { pr->kind = P_ATTR; i++; }
            pr->nlen = path_name(s, n, i);
            if (!pr->nlen) return path_fail(q, s, i, "expected a name in the predicate");
            pr->name = s + i;
            i += pr->nlen;
        }
        path_ws(s, n, &i);
        if (i < n && (s[i] == '=' || (s[i] == '!' && i + 1 < n && s[i + 1] == '='))) {
            pr->cmp = true;
            pr->neg = s[i] == '!';
            i += pr->neg ? 2 : 1;
            path_ws(s, n, &i);
            if (i < n && (s[i] == '\'' || s[i] == '"')) {
                char qc = s[i++];
                const char *e = memchr(s + i, qc, n - i);
                if (!e) return path_fail(q, s, i - 1, "unterminated string");
                pr->lit = s + i;
                pr->litn = (size_t)(e - (s + i));
                i = (size_t)(e - s) + 1;
            } else {
                size_t j = i;
                while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.' || s[j] == '-')) j++;
                if (j == i) return path_fail(q, s, i, "expected a quoted string");
                pr->lit = s + i;
                pr->litn = j - i;
                i = j;
            }
        } else if (pr->kind == P_TEXT) {
            return path_fail(q, s, i, "expected '=' or '!=' after text()");
        }
    }
    path_ws(s, n, &i);
    if (i >= n || s[i] != ']') return path_fail(q, s, i, "expected ']'");
    *ip = i + 1;
    return 1;
}

static int path_parse(Path *q, const char *s, size_t n) {
    memset(q, 0, sizeof *q);
    size_t i = 0;
    bool desc = false;
    if (n && s[0] == '/') {
        q->absolute = true;
        i = 1;
        if (i < n && s[i] == '/') { desc = true; i++; }
    }
    if (i >= n) return path_fail(q, s, i, "empty path");
    for (;;) {
        if (q->n == MAX_STEPS) return path_fail(q, s, i, "too many steps");
        Step *st = &q->steps[q->n++];
        memset(st, 0, sizeof *st);
        st->desc = desc;
        if (i + 1 < n && s[i] == '.' && s[i + 1] == '.') {
            return path_fail(q, s, i, "'..' is not supported (nodes have no parent link)");
        } else if (i < n && s[i] == '.') {
            st->kind = S_SELF;
            i++;
        } else if (i < n && s[i] == '@') {
            st->kind = S_ATTR;
            i++;
            if (i < n && s[i] == '*') { st->name = "*"; st->nlen = 1; i++; }
            else {
                st->nlen = path_name(s, n, i);
                if (!st->nlen) return path_fail(q, s, i, "expected an attribute name");
                st->name = s + i;
                i += st->nlen;
            }
        } else if (n - i >= 6 && memcmp(s + i, "text()", 6) == 0) {
            st->kind = S_TEXT;
            i += 6;
        } else if (i < n && s[i] == '*') {
            st->kind = S_NAME;
            st->name = "*";
            st->nlen = 1;
            i++;
        } else {
            st->kind = S_NAME;
            st->nlen = path_name(s, n, i);
            if (!st->nlen) return path_fail(q, s, i, "expected a step");
            st->name = s + i;
            i += st->nlen;
        }
        while (i < n && s[i] == '[') {
            if (st->kind != S_NAME) return path_fail(q, s, i, "predicates only apply to elements");
            if (st->np == MAX_PREDS) return path_fail(q, s, i, "too many predicates");
            i++;
            if (!path_pred(q, s, n, &i, &st->preds[st->np++])) return 0;
        }
        if (i >= n) break;
        if (s[i] != '/') return path_fail(q, s, i, "unexpected character");
        if (st->kind == S_ATTR || st->kind == S_TEXT)
            return path_fail(q, s, i, "@attribute and text() must be the last step");
        i++;
        desc = false;
        if (i < n && s[i] == '/') { desc = true; i++; }
        if (i >= n) return path_fail(q, s, i, "expected a step");
    }
    return 1;
}

/* String value v (string, number or bool) equals s[0..n)? */
static bool value_is(SeaVM *vm, SeaValue v, const char *s, size_t n) {
    if (is_strval(v)) return str_is(v, s, n);
    if (sea_is_int(v) || sea_is_number(v) || sea_is_bool(v)) {
        SeaString *t = sea_vm_tostring(vm, v);
        bool eq = t->len == n && memcmp(t->data, s, n) == 0;
        sea_obj_release(&t->obj);
        return eq;
    }
    return false;
}

static bool attr_get(SeaValue el, const char *name, size_t n, SeaValue *out) {
    SeaValue attrs;
    *out = SEA_NIL_VAL;
    if (!fget(el, K_ATTRS, &attrs)) return false;
    SeaString *k = sea_string_new(name, n);
    bool found = fget(attrs, k, out);
    sea_obj_release(&k->obj);
    return found;
}

static bool pred_ok(SeaVM *vm, SeaValue start, const Pred *pr, SeaValue el) {
    SeaValue v;
    switch (pr->kind) {
        case P_ATTR:
            if (!attr_get(el, pr->name, pr->nlen, &v)) return false;
            return !pr->cmp || value_is(vm, v, pr->lit, pr->litn) != pr->neg;
        case P_TEXT:
            if (!fget(el, K_TEXT, &v)) v = SEA_NIL_VAL;
            return (sea_is_nil(v) ? pr->litn == 0 : value_is(vm, v, pr->lit, pr->litn)) != pr->neg;
        case P_CHILD: {
            Kids k;
            kids_of(el, start, &k);
            for (uint32_t i = 0; i < k.n; i++) {
                SeaValue c = kid(&k, i);
                if (!is_element(c) || !tag_is(c, pr->name, pr->nlen)) continue;
                if (!pr->cmp) return true;
                SeaValue t;
                bool eq = fget(c, K_TEXT, &t) ? value_is(vm, t, pr->lit, pr->litn) : pr->litn == 0;
                if (eq != pr->neg) return true;
            }
            return false;
        }
    }
    return false;
}

/* Apply one step to context node c, appending to out. */
static void step_one(SeaVM *vm, SeaValue start, const Step *st, SeaValue c, VList *out, VList *tmp) {
    if (st->kind == S_SELF) {
        if (c != SEA_TRUE_VAL) vl_push(out, c);
        return;
    }
    if (st->kind == S_ATTR) {
        if (c == SEA_TRUE_VAL || !is_element(c)) return;
        if (st->nlen == 1 && st->name[0] == '*') {
            SeaValue attrs, order, k, v;
            if (!fget(c, K_ATTRS, &attrs)) return;
            /* attr_order first, then any others */
            size_t mark = out->n;
            if (fget(c, K_ATTR_ORDER, &order) && sea_is_array(order)) {
                SeaArray *o = SEA_AS_ARRAY(order);
                for (uint32_t i = 0; i < sea_array_len(o); i++) {
                    SeaValue nm = sea_array_get(o, i);
                    if (!is_strval(nm)) continue;
                    SeaString *f = sea_value_get_flat_string(nm);
                    if (attr_get(c, f->data, f->len, &v)) vl_push(out, v);
                    sea_obj_release(&f->obj);
                }
            }
            if (sea_is_table(attrs) && out->n - mark < sea_table_count(SEA_AS_TABLE(attrs))) {
                out->n = mark;
                uint32_t it = 0;
                while (sea_table_next(SEA_AS_TABLE(attrs), &it, &k, &v))
                    if (!sea_is_nil(v)) vl_push(out, v);
            }
            return;
        }
        SeaValue v;
        if (attr_get(c, st->name, st->nlen, &v)) vl_push(out, v);
        return;
    }
    if (st->kind == S_TEXT) {
        if (c == SEA_TRUE_VAL) return;
        SeaValue nodes, t;
        if (fget(c, K_NODES, &nodes) && sea_is_array(nodes)) {
            SeaArray *a = SEA_AS_ARRAY(nodes);
            for (uint32_t i = 0; i < sea_array_len(a); i++)
                if (item_text(sea_array_get(a, i), &t)) vl_push(out, t);
        } else if (fget(c, K_TEXT, &t) && is_strval(t) && !str_is(t, "", 0)) {
            vl_push(out, t);
        }
        return;
    }
    Kids k;
    kids_of(c, start, &k);
    tmp->n = 0;
    for (uint32_t i = 0; i < k.n; i++) {
        SeaValue x = kid(&k, i);
        if (is_element(x) && tag_is(x, st->name, st->nlen)) vl_push(tmp, x);
    }
    for (int p = 0; p < st->np && tmp->n; p++) {
        const Pred *pr = &st->preds[p];
        size_t m = 0, total = tmp->n;
        for (size_t i = 0; i < total; i++) {
            bool keep;
            if (pr->kind == P_POS) keep = (long)i + 1 == pr->pos;
            else if (pr->kind == P_LAST) keep = i + 1 == total;
            else keep = pred_ok(vm, start, pr, tmp->v[i]);
            if (keep) tmp->v[m++] = tmp->v[i];
        }
        tmp->n = m;
    }
    for (size_t i = 0; i < tmp->n; i++) vl_push(out, tmp->v[i]);
}

/* Pointer set for removing repeated elements after a // step. */
static void dedupe(VList *l) {
    if (l->n < 2) return;
    size_t cap = 32;
    while (cap < l->n * 2) cap *= 2;
    SeaValue *set = calloc(cap, sizeof *set);
    if (!set) { l->oom = 1; return; }
    size_t m = 0;
    for (size_t i = 0; i < l->n; i++) {
        SeaValue x = l->v[i];
        if (!sea_is_obj(x) || is_strval(x)) { l->v[m++] = x; continue; }
        uint64_t h = (uint64_t)x * UINT64_C(0x9E3779B97F4A7C15);
        bool seen = false;
        for (size_t k = (size_t)(h >> 32) & (cap - 1);; k = (k + 1) & (cap - 1)) {
            if (!set[k]) { set[k] = x; break; }
            if (set[k] == x) { seen = true; break; }
        }
        if (!seen) l->v[m++] = x;
    }
    l->n = m;
    free(set);
}

/* Evaluate path on start; matches in out (borrowed). Error message or NULL. */
static const char *query_run(SeaVM *vm, SeaValue start, SeaValue pathv, Path *q, VList *out) {
    SeaString *ps = sea_value_get_flat_string(pathv);
    if (!ps) return "path string expected";
    const char *err = NULL;
    if (!path_parse(q, ps->data, ps->len)) {
        err = q->err;
    } else {
        VList cur = { 0 }, next = { 0 }, ctx = { 0 }, tmp = { 0 };
        vl_push(&cur, q->absolute ? SEA_TRUE_VAL : start);
        for (int s = 0; s < q->n && !err; s++) {
            const Step *st = &q->steps[s];
            next.n = 0;
            for (size_t i = 0; i < cur.n && !err; i++) {
                if (st->desc) {
                    ctx.n = 0;
                    if (!walk(cur.v[i], start, true, &ctx)) err = "the tree is too deep or contains itself";
                    for (size_t j = 0; j < ctx.n && !err; j++) step_one(vm, start, st, ctx.v[j], &next, &tmp);
                } else {
                    step_one(vm, start, st, cur.v[i], &next, &tmp);
                }
            }
            if (st->desc || cur.n > 1) dedupe(&next);
            VList t = cur; cur = next; next = t;
        }
        if (cur.oom || next.oom || ctx.oom || tmp.oom) err = "out of memory";
        if (!err) {
            for (size_t i = 0; i < cur.n; i++)
                if (cur.v[i] != SEA_TRUE_VAL) vl_push(out, cur.v[i]);
        }
        free(cur.v); free(next.v); free(ctx.v); free(tmp.v);
    }
    if (err && err != q->err) snprintf(q->err, sizeof q->err, "%s", err);
    sea_obj_release(&ps->obj);
    return err ? q->err : NULL;
}

static SeaValue do_query(SeaVM *vm, int argc, SeaValue *args, const char *fn, bool one) {
    char msg[220];
    if (argc < 2 || (!is_element(args[0]) && !node_type_is(args[0], T_DOCUMENT))) {
        snprintf(msg, sizeof msg, "xml.%s: an element or a document expected", fn);
        return sea_make_error_str(vm, msg);
    }
    Path *q = malloc(sizeof *q);
    if (!q) return sea_make_error_str(vm, "xml.query: out of memory");
    VList out = { 0 };
    const char *err = query_run(vm, args[0], args[1], q, &out);
    SeaValue r;
    if (err) {
        snprintf(msg, sizeof msg, "xml.%s: %s", fn, err);
        r = sea_make_error_str(vm, msg);
    } else if (one) {
        r = out.n ? sea_val_ref(out.v[0]) : SEA_NIL_VAL;
    } else {
        r = vl_array(&out);
    }
    free(out.v);
    free(q);
    return r;
}

static SeaValue fn_query(SeaVM *vm, int argc, SeaValue *args) { return do_query(vm, argc, args, "query", false); }
static SeaValue fn_query_one(SeaVM *vm, int argc, SeaValue *args) { return do_query(vm, argc, args, "query_one", true); }

/* xml.find(node, tag) → first child element with that tag ("*": any) */
static SeaValue fn_find(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !is_strval(args[1])) return SEA_NIL_VAL;
    SeaString *want = sea_value_get_flat_string(args[1]);
    Kids k;
    kids_of(args[0], args[0], &k);
    SeaValue r = SEA_NIL_VAL;
    for (uint32_t i = 0; i < k.n; i++) {
        SeaValue c = kid(&k, i);
        if (is_element(c) && tag_is(c, want->data, want->len)) { r = sea_val_ref(c); break; }
    }
    sea_obj_release(&want->obj);
    return r;
}

/* xml.findall(node, tag) → child elements with that tag ("*": all) */
static SeaValue fn_findall(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaArray *result = sea_array_new(4);
    if (argc < 2 || !is_strval(args[1])) return sea_obj_val(&result->obj);
    SeaString *want = sea_value_get_flat_string(args[1]);
    Kids k;
    kids_of(args[0], args[0], &k);
    for (uint32_t i = 0; i < k.n; i++) {
        SeaValue c = kid(&k, i);
        if (is_element(c) && tag_is(c, want->data, want->len)) sea_array_push(result, c);
    }
    sea_obj_release(&want->obj);
    return sea_obj_val(&result->obj);
}

/* xml.descendants(node [, tag]) → elements under node, in document order */
static SeaValue fn_descendants(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || (!is_element(args[0]) && !node_type_is(args[0], T_DOCUMENT)))
        return sea_make_error_str(vm, "xml.descendants: an element or a document expected");
    SeaString *want = argc > 1 && is_strval(args[1]) ? sea_value_get_flat_string(args[1]) : NULL;
    VList all = { 0 };
    int ok = walk(args[0], args[0], false, &all);
    SeaValue r;
    if (!ok || all.oom) {
        r = sea_make_error_str(vm, ok ? "xml.descendants: out of memory"
                                      : "xml.descendants: the tree is too deep or contains itself");
    } else {
        size_t m = 0;
        for (size_t i = 0; i < all.n; i++)
            if (!want || tag_is(all.v[i], want->data, want->len)) all.v[m++] = all.v[i];
        all.n = m;
        r = vl_array(&all);
    }
    if (want) sea_obj_release(&want->obj);
    free(all.v);
    return r;
}

/* xml.attr(node, name [, default]) */
static SeaValue fn_attr(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaValue def = argc > 2 ? args[2] : SEA_NIL_VAL;
    if (argc < 2 || !is_strval(args[1])) return sea_val_ref(def);
    SeaString *name = sea_value_get_flat_string(args[1]);
    SeaValue v;
    bool found = attr_get(args[0], name->data, name->len, &v);
    sea_obj_release(&name->obj);
    return sea_val_ref(found ? v : def);
}

/* All text under v, in document order. */
static int deep_text(SeaValue v, Buf *b, int depth) {
    if (depth > XML_DEPTH_CAP) return 0;
    SeaValue nodes, x, s;
    if (fget(v, K_NODES, &nodes) && sea_is_array(nodes)) {
        SeaArray *a = SEA_AS_ARRAY(nodes);
        for (uint32_t i = 0; i < sea_array_len(a); i++) {
            x = sea_array_get(a, i);
            if (item_text(x, &s)) {
                SeaString *f = sea_value_get_flat_string(s);
                buf_push(b, f->data, f->len);
                sea_obj_release(&f->obj);
            } else if (is_element(x) && !deep_text(x, b, depth + 1)) {
                return 0;
            }
        }
        return 1;
    }
    if (fget(v, K_TEXT, &s) && is_strval(s)) {
        SeaString *f = sea_value_get_flat_string(s);
        buf_push(b, f->data, f->len);
        sea_obj_release(&f->obj);
    }
    Kids k;
    kids_of(v, v, &k);
    for (uint32_t i = 0; i < k.n; i++) {
        x = kid(&k, i);
        if (is_element(x) && !deep_text(x, b, depth + 1)) return 0;
    }
    return 1;
}

/* xml.text(node [, deep]) */
static SeaValue fn_text(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return tstr("", 0);
    if (argc > 1 && sea_is_truthy(args[1])) {
        SeaValue root;
        SeaValue v = node_type_is(args[0], T_DOCUMENT) && fget(args[0], K_ROOT, &root) ? root : args[0];
        Buf b = { 0 };
        if (!deep_text(v, &b, 0)) {
            buf_free(&b);
            return sea_make_error_str(vm, "xml.text: the tree is too deep or contains itself");
        }
        SeaValue r = b.oom ? sea_make_error_str(vm, "xml.text: out of memory") : tstr(b.data ? b.data : "", b.len);
        buf_free(&b);
        return r;
    }
    SeaValue tv;
    if (!fget(args[0], K_TEXT, &tv)) return tstr("", 0);
    return sea_val_ref(tv);
}

/* ======================================================================== */
/* Building and editing (tables only)                                       */
/* ======================================================================== */

static SeaTable *need_node(SeaVM *vm, const char *fn, SeaValue v) {
    if (!sea_is_table(v) || !is_element(v))
        sea_vm_error_noreturn(vm, "xml.%s: an element (table with a tag) expected, got %s", fn, sea_type_name(v));
    return SEA_AS_TABLE(v);
}

static SeaArray *arr_field(SeaTable *t, SeaString *k) {
    SeaValue v;
    if (sea_table_get(t, sea_obj_val(&k->obj), &v) && sea_is_array(v)) return SEA_AS_ARRAY(v);
    SeaArray *a = sea_array_new(0);
    tset(t, k, sea_obj_val(&a->obj));
    return a;
}

/* The text of an element is the join of its string nodes (and CDATA). */
static void rebuild_text(SeaTable *node, SeaArray *nodes) {
    Buf b = { 0 };
    SeaValue s;
    for (uint32_t i = 0; i < sea_array_len(nodes); i++) {
        if (!item_text(sea_array_get(nodes, i), &s)) continue;
        SeaString *f = sea_value_get_flat_string(s);
        buf_push(&b, f->data, f->len);
        sea_obj_release(&f->obj);
    }
    tset(node, K_TEXT, tstr(b.data ? b.data : "", b.len));
    buf_free(&b);
}

/* nodes for an element built without one: its text, then its children. */
static SeaArray *nodes_field(SeaTable *node) {
    SeaValue v;
    if (sea_table_get(node, sea_obj_val(&K_NODES->obj), &v) && sea_is_array(v)) return SEA_AS_ARRAY(v);
    SeaArray *a = sea_array_new(0);
    if (sea_table_get(node, sea_obj_val(&K_TEXT->obj), &v) && is_strval(v) && !str_is(v, "", 0))
        sea_array_push(a, v);
    SeaArray *ch = arr_field(node, K_CHILDREN);
    for (uint32_t i = 0; i < sea_array_len(ch); i++) sea_array_push(a, sea_array_get(ch, i));
    tset(node, K_NODES, sea_obj_val(&a->obj));
    return a;
}

static bool appendable(SeaValue x) {
    if (is_strval(x) || sea_is_int(x) || sea_is_number(x)) return true;
    return (sea_is_table(x) || sea_is_map(x) || sea_is_struct(x)) &&
           (is_element(x) || node_type_is(x, T_COMMENT) || node_type_is(x, T_PI) ||
            node_type_is(x, T_CDATA) || node_type_is(x, T_TEXT));
}

static SEA_NORETURN void not_appendable(SeaVM *vm, const char *fn, SeaValue x) {
    sea_vm_error_noreturn(vm, "xml.%s: element, string or comment/cdata/pi/text node expected, got %s",
                          fn, sea_type_name(x));
}

/* x has been checked with appendable(). */
static void append_one(SeaVM *vm, SeaTable *node, SeaValue x) {
    bool text = is_strval(x) || sea_is_int(x) || sea_is_number(x);
    SeaValue s;
    SeaArray *nodes = nodes_field(node);
    SeaArray *children = arr_field(node, K_CHILDREN);
    if (text) {
        SeaString *str = sea_vm_tostring(vm, x);
        sea_array_push(nodes, sea_obj_val(&str->obj));
        sea_obj_release(&str->obj);
        rebuild_text(node, nodes);
        return;
    }
    if (is_element(x)) sea_array_push(children, x);
    sea_array_push(nodes, x);
    if (item_text(x, &s)) rebuild_text(node, nodes);
}

/* xml.append(node, x) → x */
static SeaValue fn_append(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "xml.append: node and child expected");
    SeaTable *node = need_node(vm, "append", args[0]);
    if (!appendable(args[1])) not_appendable(vm, "append", args[1]);
    if (sea_obj_val(&node->obj) == args[1]) sea_vm_error_noreturn(vm, "xml.append: a node cannot contain itself");
    append_one(vm, node, args[1]);
    return sea_val_ref(args[1]);
}

/* xml.element(tag [, attrs [, content...]]) → a new element */
static SeaValue fn_element(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *tag = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!tag || !valid_name(tag->data, tag->len)) {
        if (tag) sea_obj_release(&tag->obj);
        sea_vm_error_noreturn(vm, "xml.element: a valid tag name expected");
    }
    SeaValue attrs_in = argc > 1 ? args[1] : SEA_NIL_VAL;
    if (!sea_is_nil(attrs_in) && !sea_is_table(attrs_in) && !sea_is_map(attrs_in) && !sea_is_struct(attrs_in)) {
        sea_obj_release(&tag->obj);
        sea_vm_error_noreturn(vm, "xml.element: attrs must be a table, got %s", sea_type_name(attrs_in));
    }
    for (int i = 2; i < argc; i++) {          /* check the content before building anything */
        SeaValue x = args[i];
        bool ok = sea_is_nil(x) || appendable(x);
        if (sea_is_array(x)) {
            ok = true;
            for (uint32_t j = 0; ok && j < sea_array_len(SEA_AS_ARRAY(x)); j++) {
                x = sea_array_get(SEA_AS_ARRAY(x), j);
                ok = appendable(x);
            }
        }
        if (!ok) {
            sea_obj_release(&tag->obj);
            not_appendable(vm, "element", x);
        }
    }
    SeaTable *node = sea_table_new(TABLE_MAP);
    tset(node, K_TAG, sea_obj_val(&tag->obj));
    SeaTable *attrs = sea_table_new(TABLE_MAP);
    SeaArray *order = sea_array_new(0);
    if (!sea_is_nil(attrs_in)) {
        size_t n = 0, cap = 8;
        EAttr *list = malloc(cap * sizeof *list);
        uint32_t it = 0;
        SeaValue k, v;
        for (;;) {
            bool more = sea_is_table(attrs_in) ? sea_table_next(SEA_AS_TABLE(attrs_in), &it, &k, &v)
                      : sea_is_map(attrs_in)   ? sea_map_next(SEA_AS_MAP(attrs_in), &it, &k, &v)
                                               : sea_struct_next(SEA_AS_STRUCT(attrs_in), &it, &k, &v);
            if (!more) break;
            if (!sea_is_string(k) || sea_is_nil(v) || !list) continue;
            if (n == cap) {
                cap *= 2;
                EAttr *nl = realloc(list, cap * sizeof *nl);
                if (!nl) continue;
                list = nl;
            }
            list[n++] = (EAttr){ SEA_AS_STRING(k), v, false };
        }
        if (list) {
            qsort(list, n, sizeof *list, eattr_cmp);
            for (size_t i = 0; i < n; i++) {
                sea_table_set(attrs, sea_obj_val(&list[i].name->obj), list[i].val);
                sea_array_push(order, sea_obj_val(&list[i].name->obj));
            }
            free(list);
        }
    }
    tset(node, K_ATTRS, sea_obj_val(&attrs->obj));
    tset(node, K_ATTR_ORDER, sea_obj_val(&order->obj));
    tset(node, K_CHILDREN, sea_obj_val(&sea_array_new(0)->obj));
    tset(node, K_NODES, sea_obj_val(&sea_array_new(0)->obj));
    tset(node, K_TEXT, tstr("", 0));
    for (int i = 2; i < argc; i++) {
        SeaValue x = args[i];
        if (sea_is_array(x)) {
            SeaArray *a = SEA_AS_ARRAY(x);
            for (uint32_t j = 0; j < sea_array_len(a); j++) append_one(vm, node, sea_array_get(a, j));
        } else if (!sea_is_nil(x)) {
            append_one(vm, node, x);
        }
    }
    return sea_obj_val(&node->obj);
}

static void arr_clear(SeaArray *a) {
    while (sea_array_len(a)) val_release(sea_array_pop(a));
}

/* xml.set_text(node, s) → node: the whole content becomes the text s */
static SeaValue fn_set_text(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "xml.set_text: node and text expected");
    SeaTable *node = need_node(vm, "set_text", args[0]);
    SeaString *s = sea_is_nil(args[1]) ? sea_string_new("", 0) : sea_vm_tostring(vm, args[1]);
    SeaArray *children = arr_field(node, K_CHILDREN);
    SeaArray *nodes = arr_field(node, K_NODES);
    arr_clear(children);
    arr_clear(nodes);
    if (s->len) sea_array_push(nodes, sea_obj_val(&s->obj));
    tset(node, K_TEXT, sea_obj_val(&s->obj));
    return sea_val_ref(args[0]);
}

/* xml.set_attr(node, name, value) → node (nil removes the attribute) */
static SeaValue fn_set_attr(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "xml.set_attr: node, name and value expected");
    SeaTable *node = need_node(vm, "set_attr", args[0]);
    if (!sea_is_string(args[1]) || !valid_name(SEA_AS_STRING(args[1])->data, SEA_AS_STRING(args[1])->len))
        sea_vm_error_noreturn(vm, "xml.set_attr: a valid attribute name expected");
    SeaValue val = argc > 2 ? args[2] : SEA_NIL_VAL;
    if (!sea_is_nil(val) && !is_strval(val) && !sea_is_int(val) && !sea_is_number(val) && !sea_is_bool(val))
        sea_vm_error_noreturn(vm, "xml.set_attr: string, number or bool expected, got %s", sea_type_name(val));
    SeaValue av, old;
    SeaTable *attrs;
    if (sea_table_get(node, sea_obj_val(&K_ATTRS->obj), &av) && sea_is_table(av)) {
        attrs = SEA_AS_TABLE(av);
    } else if (sea_is_nil(av)) {
        attrs = sea_table_new(TABLE_MAP);
        tset(node, K_ATTRS, sea_obj_val(&attrs->obj));
    } else {
        sea_vm_error_noreturn(vm, "xml.set_attr: attrs must be a table");
    }
    (void)old;
    SeaArray *order = arr_field(node, K_ATTR_ORDER);
    if (sea_is_nil(val)) {
        sea_table_delete(attrs, args[1]);
        for (uint32_t i = 0; i < sea_array_len(order); i++)
            if (str_eq(sea_array_get(order, i), SEA_AS_STRING(args[1]))) { sea_array_del(order, i); break; }
    } else {
        sea_table_set(attrs, args[1], val);
        bool listed = false;
        for (uint32_t i = 0; i < sea_array_len(order) && !listed; i++)
            listed = str_eq(sea_array_get(order, i), SEA_AS_STRING(args[1]));
        if (!listed) sea_array_push(order, args[1]);
    }
    return sea_val_ref(args[0]);
}

/* xml.remove(node, child) → bool: removes an element or node (by identity)
 * or the first text node equal to a string */
static SeaValue fn_remove(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "xml.remove: node and child expected");
    SeaTable *node = need_node(vm, "remove", args[0]);
    SeaValue x = args[1];
    SeaArray *nodes = nodes_field(node);
    SeaArray *children = arr_field(node, K_CHILDREN);
    bool found = false, text = false;
    SeaValue s;
    SeaString *xs = is_strval(x) ? sea_value_get_flat_string(x) : NULL;
    for (uint32_t i = 0; i < sea_array_len(nodes); i++) {
        SeaValue y = sea_array_get(nodes, i);
        bool hit = xs ? is_strval(y) && str_eq(y, xs) : y == x;
        if (!hit) continue;
        text = item_text(y, &s);
        sea_array_del(nodes, i);
        found = true;
        break;
    }
    for (uint32_t i = 0; !is_strval(x) && i < sea_array_len(children); i++) {
        if (sea_array_get(children, i) == x) {
            sea_array_del(children, i);
            found = true;
            break;
        }
    }
    if (xs) sea_obj_release(&xs->obj);
    if (text) rebuild_text(node, nodes);
    return sea_bool_val(found);
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    pthread_once(&g_keys_once, keys_init);
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "parse",       fn_parse);
    sea_mod_set(mod, "valid",       fn_valid);
    sea_mod_set(mod, "sax",         fn_sax);
    sea_mod_set(mod, "encode",      fn_encode);
    sea_mod_set(mod, "escape",      fn_escape);
    sea_mod_set(mod, "find",        fn_find);
    sea_mod_set(mod, "findall",     fn_findall);
    sea_mod_set(mod, "descendants", fn_descendants);
    sea_mod_set(mod, "query",       fn_query);
    sea_mod_set(mod, "query_one",   fn_query_one);
    sea_mod_set(mod, "attr",        fn_attr);
    sea_mod_set(mod, "text",        fn_text);
    sea_mod_set(mod, "element",     fn_element);
    sea_mod_set(mod, "append",      fn_append);
    sea_mod_set(mod, "set_text",    fn_set_text);
    sea_mod_set(mod, "set_attr",    fn_set_attr);
    sea_mod_set(mod, "remove",      fn_remove);
    return sea_obj_val(&mod->obj);
}
