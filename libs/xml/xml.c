/*
 * libs/xml/xml.c — XML parser/encoder for Sea.
 *
 * Build:  cd libs/xml && make
 * Usage:  local xml = require("./libs/xml/xml")
 *
 * Exports:
 *   xml.parse(str)          → element table or nil on error
 *   xml.encode(node)        → XML string or nil
 *   xml.find(node, tag)     → first child element with that tag, or nil
 *   xml.findall(node, tag)  → SeaArray of child elements with that tag
 *   xml.attr(node, name)    → attribute string value, or nil
 *   xml.text(node)          → text content string (empty string if none)
 *
 * Node table format:
 *   { tag="name", attrs={key=val,...}, children=[child,...], text="..." }
 *
 * text is the concatenation of all text-node content within the element
 * (CDATA sections count as text; entities are decoded).
 * children contains only element-type children, not text nodes.
 */

#include "sea.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Dynamic write buffer ------------------------------------------------- */

typedef struct { char *data; size_t len; size_t cap; } Buf;

static void buf_init(Buf *b)         { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf *b)         { free(b->data); buf_init(b); }
static void buf_clear(Buf *b)        { b->len = 0; if (b->data) b->data[0] = '\0'; }

static int buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
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

static int buf_push_c(Buf *b, char c) { return buf_push(b, &c, 1); }

/* Steal contents of b into a new SeaString; resets b to empty. */
static SeaValue buf_to_str(Buf *b) {
    SeaValue v = sea_obj_val(&sea_string_new(b->data ? b->data : "", b->len)->obj);
    buf_free(b);
    return v;
}

/* ---- XML parser state ------------------------------------------------------ */

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    int         failed;
} XP;

static int xp_consume(XP *p, const char *s) {
    size_t n = strlen(s);
    if (p->pos + n > p->len || memcmp(p->src + p->pos, s, n) != 0) return 0;
    p->pos += n;
    return 1;
}

static void xp_skip_ws(XP *p) {
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static int is_name_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c == '_' || c == ':' || c >= 0x80;
}
static int is_name_char(unsigned char c) {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static int xp_read_name(XP *p, Buf *b) {
    buf_clear(b);
    if (p->pos >= p->len || !is_name_start((unsigned char)p->src[p->pos]))
        return 0;
    while (p->pos < p->len && is_name_char((unsigned char)p->src[p->pos]))
        buf_push_c(b, p->src[p->pos++]);
    return 1;
}

/* Decode XML entities from raw[0..len) into b. */
static void decode_entities(const char *raw, size_t len, Buf *b) {
    for (size_t i = 0; i < len; ) {
        if (raw[i] != '&') { buf_push_c(b, raw[i++]); continue; }
        /* find ';', max 16 chars ahead */
        size_t j = i + 1;
        while (j < len && j - i < 16 && raw[j] != ';') j++;
        if (j >= len || raw[j] != ';') { buf_push_c(b, raw[i++]); continue; }
        const char *e = raw + i + 1;
        size_t el    = j - i - 1;
        if      (el == 3 && memcmp(e, "amp",  3) == 0) buf_push_c(b, '&');
        else if (el == 2 && memcmp(e, "lt",   2) == 0) buf_push_c(b, '<');
        else if (el == 2 && memcmp(e, "gt",   2) == 0) buf_push_c(b, '>');
        else if (el == 4 && memcmp(e, "apos", 4) == 0) buf_push_c(b, '\'');
        else if (el == 4 && memcmp(e, "quot", 4) == 0) buf_push_c(b, '"');
        else if (el == 4 && memcmp(e, "nbsp", 4) == 0) buf_push(b, "\xC2\xA0", 2);
        else if (el > 1 && e[0] == '#') {
            unsigned int cp = (el > 2 && e[1] == 'x')
                ? (unsigned int)strtoul(e + 2, NULL, 16)
                : (unsigned int)strtoul(e + 1, NULL, 10);
            if (cp < 0x80) {
                buf_push_c(b, (char)cp);
            } else if (cp < 0x800) {
                buf_push_c(b, (char)(0xC0 | (cp >> 6)));
                buf_push_c(b, (char)(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                buf_push_c(b, (char)(0xE0 | (cp >> 12)));
                buf_push_c(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                buf_push_c(b, (char)(0x80 | (cp & 0x3F)));
            } else {
                buf_push_c(b, (char)(0xF0 | (cp >> 18)));
                buf_push_c(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
                buf_push_c(b, (char)(0x80 | ((cp >>  6) & 0x3F)));
                buf_push_c(b, (char)(0x80 | (cp & 0x3F)));
            }
        } else {
            buf_push(b, raw + i, j - i + 1);  /* unknown entity: pass through */
        }
        i = j + 1;
    }
}

/* Read a quoted attribute value; returns a SeaString object (RC=1). */
static SeaValue xp_attr_val(XP *p) {
    if (p->pos >= p->len) { p->failed = 1; return SEA_NIL_VAL; }
    char q = p->src[p->pos];
    if (q != '"' && q != '\'') { p->failed = 1; return SEA_NIL_VAL; }
    p->pos++;
    size_t start = p->pos;
    while (p->pos < p->len && p->src[p->pos] != q) p->pos++;
    if (p->pos >= p->len) { p->failed = 1; return SEA_NIL_VAL; }
    Buf b; buf_init(&b);
    decode_entities(p->src + start, p->pos - start, &b);
    p->pos++;  /* skip closing quote */
    return buf_to_str(&b);
}

/* Allocate a node table {tag, attrs, children, text=""}.  RC=1. */
static SeaTable *make_node(const char *tag, size_t tlen) {
    SeaTable *node = sea_table_new(TABLE_MAP);

    SeaValue tv = sea_obj_val(&sea_string_new(tag, tlen)->obj);
    sea_table_set_str(node, "tag", tv);
    sea_obj_release(sea_as_obj(tv));

    SeaTable *attrs = sea_table_new(TABLE_MAP);
    SeaValue av = sea_obj_val(&attrs->obj);
    sea_table_set_str(node, "attrs", av);
    sea_obj_release(&attrs->obj);

    SeaArray *ch = sea_array_new(4);
    SeaValue cv = sea_obj_val(&ch->obj);
    sea_table_set_str(node, "children", cv);
    sea_obj_release(&ch->obj);

    SeaValue empty = sea_obj_val(&sea_string_new("", 0)->obj);
    sea_table_set_str(node, "text", empty);
    sea_obj_release(sea_as_obj(empty));

    return node;  /* RC=1, caller owns */
}

/* Forward declaration */
static SeaValue xp_element(XP *p);

/* Skip <!-- ... --> (already past '<!--') */
static void xp_skip_comment(XP *p) {
    while (p->pos < p->len) {
        if (xp_consume(p, "-->")) return;
        p->pos++;
    }
}

/* Skip <? ... ?> (already past '<?') */
static void xp_skip_pi(XP *p) {
    while (p->pos < p->len) {
        if (xp_consume(p, "?>")) return;
        p->pos++;
    }
}

/* Read <![CDATA[...]]> content into text buf (already past '<![CDATA['). */
static void xp_read_cdata(XP *p, Buf *text) {
    while (p->pos < p->len) {
        if (xp_consume(p, "]]>")) return;
        buf_push_c(text, p->src[p->pos++]);
    }
}

/* Skip <!...> (comment or DOCTYPE).  Already past '<!'. */
static void xp_skip_bang(XP *p) {
    if (xp_consume(p, "--")) { xp_skip_comment(p); return; }
    /* DOCTYPE or similar: skip to matching '>' with bracket nesting */
    int depth = 1;
    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if      (c == '[') depth++;
        else if (c == ']') depth--;
        else if (c == '>' && depth <= 1) return;
    }
}

/*
 * Parse one element.  Position is just after '<'.
 * Returns node table (RC=1) or SEA_NIL_VAL on error (p->failed set).
 */
static SeaValue xp_element(XP *p) {
    Buf name; buf_init(&name);
    if (!xp_read_name(p, &name)) {
        buf_free(&name);
        p->failed = 1;
        return SEA_NIL_VAL;
    }

    SeaTable *node = make_node(name.data, name.len);
    buf_free(&name);

    /* Retrieve attrs table and children array by pointer (node holds RC). */
    SeaValue av, cv;
    sea_table_get_str(node, "attrs",    &av);
    sea_table_get_str(node, "children", &cv);
    SeaTable *attrs    = SEA_AS_TABLE(av);
    SeaArray *children = SEA_AS_ARRAY(cv);

    /* ---- Parse attributes ---- */
    Buf aname; buf_init(&aname);
    while (!p->failed) {
        xp_skip_ws(p);
        if (p->pos >= p->len) { p->failed = 1; break; }
        char c = p->src[p->pos];
        if (c == '>' || c == '/') break;

        if (!xp_read_name(p, &aname)) { p->failed = 1; break; }
        xp_skip_ws(p);

        SeaValue val;
        if (xp_consume(p, "=")) {
            xp_skip_ws(p);
            val = xp_attr_val(p);
            if (p->failed) break;
        } else {
            /* Value-less attribute (HTML-lenient): treat as empty string */
            val = sea_obj_val(&sea_string_new("", 0)->obj);
        }

        sea_table_set_str(attrs, aname.data, val);
        sea_obj_release(sea_as_obj(val));
        buf_clear(&aname);
    }
    buf_free(&aname);

    if (p->failed) { sea_obj_release(&node->obj); return SEA_NIL_VAL; }

    /* Self-closing */
    if (xp_consume(p, "/>")) return sea_obj_val(&node->obj);

    if (!xp_consume(p, ">")) {
        p->failed = 1;
        sea_obj_release(&node->obj);
        return SEA_NIL_VAL;
    }

    /* ---- Parse content ---- */
    Buf text; buf_init(&text);
    int saw_close = 0;

    while (!p->failed && p->pos < p->len) {
        if (p->src[p->pos] != '<') {
            /* Text content: collect up to next '<' */
            size_t start = p->pos;
            while (p->pos < p->len && p->src[p->pos] != '<') p->pos++;
            decode_entities(p->src + start, p->pos - start, &text);
            continue;
        }

        p->pos++;  /* consume '<' */
        if (p->pos >= p->len) { p->failed = 1; break; }

        char next = p->src[p->pos];

        if (next == '/') {
            /* Closing tag: skip name and '>' */
            p->pos++;
            while (p->pos < p->len && p->src[p->pos] != '>') p->pos++;
            if (p->pos < p->len) p->pos++;  /* consume '>' */
            saw_close = 1;
            break;
        }

        if (next == '!') {
            p->pos++;
            if (xp_consume(p, "[CDATA[")) xp_read_cdata(p, &text);
            else                          xp_skip_bang(p);
            continue;
        }

        if (next == '?') {
            p->pos++;
            xp_skip_pi(p);
            continue;
        }

        /* Child element */
        SeaValue child = xp_element(p);
        if (p->failed || sea_is_nil(child)) {
            p->failed = 1;
            break;
        }
        sea_array_push(children, child);
        sea_obj_release(sea_as_obj(child));
    }

    if (!p->failed && !saw_close) p->failed = 1;

    if (p->failed) {
        buf_free(&text);
        sea_obj_release(&node->obj);
        return SEA_NIL_VAL;
    }

    /* Store combined text content */
    SeaValue tv = buf_to_str(&text);  /* buf_to_str frees text */
    sea_table_set_str(node, "text", tv);
    sea_obj_release(sea_as_obj(tv));

    return sea_obj_val(&node->obj);
}

/* Skip prolog (XML declaration, DOCTYPE, comments) and parse root element. */
static SeaValue xp_document(XP *p) {
    xp_skip_ws(p);
    while (p->pos < p->len) {
        if (!xp_consume(p, "<")) break;
        if (xp_consume(p, "?")) {
            xp_skip_pi(p);
        } else if (xp_consume(p, "!")) {
            xp_skip_bang(p);
        } else {
            return xp_element(p);
        }
        xp_skip_ws(p);
    }
    p->failed = 1;
    return SEA_NIL_VAL;
}

/* ---- fn_parse -------------------------------------------------------------- */

static SeaValue fn_parse(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    SeaString *s = SEA_AS_STRING(args[0]);
    XP p = { s->data, s->len, 0, 0 };
    SeaValue result = xp_document(&p);
    if (p.failed) {
        if (!sea_is_nil(result)) sea_obj_release(sea_as_obj(result));
        return SEA_NIL_VAL;
    }
    return result;
}

/* ---- Encoder -------------------------------------------------------------- */

static void xml_escape_text(Buf *b, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '&') buf_push(b, "&amp;", 5);
        else if (c == '<') buf_push(b, "&lt;",  4);
        else if (c == '>') buf_push(b, "&gt;",  4);
        else               buf_push_c(b, (char)c);
    }
}

static void xml_escape_attr(Buf *b, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '&') buf_push(b, "&amp;",  5);
        else if (c == '<') buf_push(b, "&lt;",   4);
        else if (c == '"') buf_push(b, "&quot;", 6);
        else               buf_push_c(b, (char)c);
    }
}

static int encode_node(Buf *b, SeaValue v, int depth) {
    if (depth > 256 || !sea_is_table(v)) return 0;
    SeaTable *node = SEA_AS_TABLE(v);

    SeaValue tag_v;
    if (!sea_table_get_str(node, "tag", &tag_v) || !sea_is_string(tag_v)) return 0;
    SeaString *tag = SEA_AS_STRING(tag_v);

    buf_push_c(b, '<');
    buf_push(b, tag->data, tag->len);

    SeaValue attrs_v;
    if (sea_table_get_str(node, "attrs", &attrs_v) && sea_is_table(attrs_v)) {
        SeaTable *attrs = SEA_AS_TABLE(attrs_v);
        uint32_t iter = 0; SeaValue k, av;
        while (sea_table_next(attrs, &iter, &k, &av)) {
            if (!sea_is_string(k)) continue;
            SeaString *ks = SEA_AS_STRING(k);
            buf_push_c(b, ' ');
            buf_push(b, ks->data, ks->len);
            buf_push(b, "=\"", 2);
            if (sea_is_string(av)) {
                SeaString *vs = SEA_AS_STRING(av);
                xml_escape_attr(b, vs->data, vs->len);
            }
            buf_push_c(b, '"');
        }
    }

    SeaValue cv, tv;
    int has_ch = sea_table_get_str(node, "children", &cv)
              && sea_is_array(cv)
              && sea_array_len(SEA_AS_ARRAY(cv)) > 0;
    int has_tx = sea_table_get_str(node, "text", &tv)
              && sea_is_string(tv)
              && SEA_AS_STRING(tv)->len > 0;

    if (!has_ch && !has_tx) {
        buf_push(b, "/>", 2);
        return 1;
    }

    buf_push_c(b, '>');

    if (has_tx) {
        SeaString *ts = SEA_AS_STRING(tv);
        xml_escape_text(b, ts->data, ts->len);
    }

    if (has_ch) {
        SeaArray *ch = SEA_AS_ARRAY(cv);
        uint32_t n = sea_array_len(ch);
        for (uint32_t i = 0; i < n; i++) {
            if (!encode_node(b, sea_array_get(ch, (int64_t)i), depth + 1))
                return 0;
        }
    }

    buf_push(b, "</", 2);
    buf_push(b, tag->data, tag->len);
    buf_push_c(b, '>');
    return 1;
}

static SeaValue fn_encode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    Buf b; buf_init(&b);
    if (!encode_node(&b, args[0], 0)) { buf_free(&b); return SEA_NIL_VAL; }
    SeaValue r = sea_obj_val(&sea_string_new(b.data ? b.data : "", b.len)->obj);
    buf_free(&b);
    return r;
}

/* ---- Helper functions ----------------------------------------------------- */

static SeaValue fn_find(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_table(args[0]) || !sea_is_string(args[1]))
        return SEA_NIL_VAL;
    SeaTable *node = SEA_AS_TABLE(args[0]);
    SeaString *want = SEA_AS_STRING(args[1]);

    SeaValue cv;
    if (!sea_table_get_str(node, "children", &cv) || !sea_is_array(cv))
        return SEA_NIL_VAL;
    SeaArray *ch = SEA_AS_ARRAY(cv);
    uint32_t n = sea_array_len(ch);
    for (uint32_t i = 0; i < n; i++) {
        SeaValue child = sea_array_get(ch, (int64_t)i);
        if (!sea_is_table(child)) continue;
        SeaValue ctag;
        if (!sea_table_get_str(SEA_AS_TABLE(child), "tag", &ctag)) continue;
        if (sea_is_string(ctag) && sea_string_equal(SEA_AS_STRING(ctag), want))
            return sea_val_ref(child);   /* natives return owned values */
    }
    return SEA_NIL_VAL;
}

static SeaValue fn_findall(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaArray *result = sea_array_new(4);
    if (argc < 2 || !sea_is_table(args[0]) || !sea_is_string(args[1]))
        return sea_obj_val(&result->obj);

    SeaTable *node = SEA_AS_TABLE(args[0]);
    SeaString *want = SEA_AS_STRING(args[1]);

    SeaValue cv;
    if (!sea_table_get_str(node, "children", &cv) || !sea_is_array(cv))
        return sea_obj_val(&result->obj);

    SeaArray *ch = SEA_AS_ARRAY(cv);
    uint32_t n = sea_array_len(ch);
    for (uint32_t i = 0; i < n; i++) {
        SeaValue child = sea_array_get(ch, (int64_t)i);
        if (!sea_is_table(child)) continue;
        SeaValue ctag;
        if (!sea_table_get_str(SEA_AS_TABLE(child), "tag", &ctag)) continue;
        if (sea_is_string(ctag) && sea_string_equal(SEA_AS_STRING(ctag), want))
            sea_array_push(result, child);
    }
    return sea_obj_val(&result->obj);
}

static SeaValue fn_attr(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_table(args[0]) || !sea_is_string(args[1]))
        return SEA_NIL_VAL;
    SeaTable *node = SEA_AS_TABLE(args[0]);
    SeaValue av;
    if (!sea_table_get_str(node, "attrs", &av) || !sea_is_table(av))
        return SEA_NIL_VAL;
    SeaValue result;
    if (!sea_table_get(SEA_AS_TABLE(av), args[1], &result)) return SEA_NIL_VAL;
    return sea_val_ref(result);
}

static SeaValue fn_text(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_table(args[0])) return sea_obj_val(&sea_string_new("", 0)->obj);
    SeaValue tv;
    if (!sea_table_get_str(SEA_AS_TABLE(args[0]), "text", &tv))
        return sea_obj_val(&sea_string_new("", 0)->obj);
    return sea_val_ref(tv);
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "parse",   fn_parse);
    sea_mod_set(mod, "encode",  fn_encode);
    sea_mod_set(mod, "find",    fn_find);
    sea_mod_set(mod, "findall", fn_findall);
    sea_mod_set(mod, "attr",    fn_attr);
    sea_mod_set(mod, "text",    fn_text);
    return sea_obj_val(&mod->obj);
}
