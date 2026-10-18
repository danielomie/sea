/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * libs/yaml/yaml.c — YAML 1.2 for Sea: decode, encode, events and emit.
 *
 * Build:  make -C libs/yaml
 * Usage:  local yaml = require("../libs/yaml/yaml")   -- relative to your file
 *
 *   yaml.decode(s [, opts])       → value | error     (one document)
 *   yaml.decode_all(s [, opts])   → array | error     (every document)
 *   yaml.valid(s)                 → bool
 *   yaml.encode(v [, opts])       → string | error
 *   yaml.encode_all(docs [, opts])→ string | error
 *   yaml.events(s)                → array of event tables | error
 *   yaml.emit(events [, opts])    → string | error
 *
 * The reader follows libyaml's design: a scanner turns characters into
 * tokens (with the "simple key" lookahead that finds `key:` after the
 * fact), a recursive-descent parser turns tokens into events, and a composer
 * builds Sea values from the events. The writer builds a node tree (from
 * values or from events) and emits it in block or flow style.
 *
 * Failures are error values (is_error(e), e.message), e.g.
 * "yaml.decode: mapping values are not allowed here at line 3, column 9".
 */

#include "sea.h"
#include "sea_rope.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YAML_MAX_DEPTH  512
#define YAML_DEPTH_CAP  10000      /* bounded by the C stack (recursive parser and emitter) */
#define SIMPLE_KEY_MAX  1024       /* an implicit key spans at most this many bytes */

/* ---- Dynamic write buffer ------------------------------------------------- */

typedef struct { char *data; size_t len, cap; int oom; } Buf;

static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static int buf_grow(Buf *b, size_t need) {
    size_t nc = b->cap ? b->cap : 64;
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
    b->data[b->len] = '\0';
}

static inline void buf_putc(Buf *b, char c) {
    if (!buf_reserve(b, 1)) return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

static inline void buf_puts(Buf *b, const char *s) { buf_push(b, s, strlen(s)); }

static inline void buf_spaces(Buf *b, int n) { while (n-- > 0) buf_putc(b, ' '); }

static inline void buf_clear(Buf *b) { b->len = 0; if (b->data) b->data[0] = '\0'; }

/* Take the buffer's bytes as a malloc'd NUL-terminated string (never NULL
 * unless out of memory); the buffer is left empty. */
static char *buf_take(Buf *b, size_t *len) {
    char *s = b->data ? b->data : malloc(1);
    if (s && !b->data) s[0] = '\0';
    if (len) *len = b->len;
    b->data = NULL;
    b->len = b->cap = 0;
    return s;
}

static char *xstrndup(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* ---- Options (table, map or struct) --------------------------------------- */

static bool opt_get(SeaValue opts, const char *field, SeaValue *out) {
    *out = SEA_NIL_VAL;
    if (sea_is_table(opts)) return sea_table_get_str(SEA_AS_TABLE(opts), field, out) && !sea_is_nil(*out);
    if (sea_is_map(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        bool found = sea_map_get(SEA_AS_MAP(opts), sea_obj_val(&k->obj), out);
        sea_obj_release(&k->obj);
        return found && !sea_is_nil(*out);
    }
    if (sea_is_struct(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        *out = sea_struct_get(SEA_AS_STRUCT(opts), k);
        sea_obj_release(&k->obj);
        return !sea_is_nil(*out);
    }
    return false;
}

static int opt_flag(SeaValue opts, const char *field, int def) {
    SeaValue v;
    if (!opt_get(opts, field, &v) || !sea_is_bool(v)) return def;
    return sea_as_bool(v);
}

static int opt_int(SeaValue opts, const char *field, int def) {
    SeaValue v;
    if (!opt_get(opts, field, &v)) return def;
    if (sea_is_int(v)) return (int)sea_as_int(v);
    if (sea_is_number(v)) return (int)sea_as_number(v);
    return def;
}

/* A string option compared to s. */
static bool opt_is(SeaValue opts, const char *field, const char *s) {
    SeaValue v;
    if (!opt_get(opts, field, &v)) return false;
    SeaString *str = sea_value_get_flat_string(v);
    if (!str) return false;
    bool eq = strcmp(str->data, s) == 0;
    sea_obj_release(&str->obj);
    return eq;
}

static void val_release(SeaValue v) { if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v)); }

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

static void tset_str(SeaTable *t, const char *k, SeaValue v) {   /* takes v */
    sea_table_set_str(t, k, v);
    val_release(v);
}

/* ---- UTF-8 ------------------------------------------------------------------ */

#define utf8_seq(s, n, cp) sea_utf8_decode((s), (n), (cp))   /* sea_utf8.h */

static void push_utf8(Buf *b, uint32_t cp) {
    char t[4];
    buf_push(b, t, (size_t)sea_utf8_encode(cp, t));
}

/* YAML's c-printable set (tab, line breaks and the printable code points). */
static bool printable_cp(uint32_t c) {
    return c == 0x09 || c == 0x0A || c == 0x0D || (c >= 0x20 && c <= 0x7E) || c == 0x85 ||
           (c >= 0xA0 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF);
}

/* ======================================================================== */
/* Scanner: characters → tokens                                             */
/* ======================================================================== */

typedef struct { size_t index; int line, column; } Mark;

enum {
    T_NONE, T_STREAM_START, T_STREAM_END, T_VERSION, T_TAG_DIRECTIVE, T_DOC_START, T_DOC_END,
    T_BSEQ_START, T_BMAP_START, T_BLOCK_END, T_FSEQ_START, T_FSEQ_END, T_FMAP_START, T_FMAP_END,
    T_BLOCK_ENTRY, T_FLOW_ENTRY, T_KEY, T_VALUE, T_ALIAS, T_ANCHOR, T_TAG, T_SCALAR
};

enum { S_ANY, S_PLAIN, S_SINGLE, S_DOUBLE, S_LITERAL, S_FOLDED };

typedef struct {
    int    type;
    Mark   start, end;
    char  *a;  size_t alen;     /* scalar value, anchor/alias name, tag handle, directive handle */
    char  *b;  size_t blen;     /* tag suffix, directive prefix */
    int    style;
    int    major, minor;
} Token;

typedef struct { bool possible, required, flow_map; size_t token_number; Mark mark; } SimpleKey;

typedef struct { char *handle, *prefix; } TagDir;

typedef struct Y Y;

/* Events (parser → handler). Strings are borrowed for the call. */
enum { E_STREAM_START, E_STREAM_END, E_DOC_START, E_DOC_END, E_ALIAS, E_SCALAR,
       E_SEQ_START, E_SEQ_END, E_MAP_START, E_MAP_END };

typedef struct {
    int         type;
    Mark        start, end;
    const char *anchor;          /* NULL: none (alias: the name) */
    const char *tag;             /* NULL: none; resolved (tag:yaml.org,2002:str, !local, ...) */
    const char *value;           /* scalar */
    size_t      len;
    int         style;           /* scalar: S_*; collections: 0 block, 1 flow */
    bool        implicit;        /* document start/end without --- / ... */
    int         major, minor;    /* document start: %YAML version (0: none) */
    TagDir     *tagdirs;         /* document start: %TAG directives */
    size_t      ntagdirs;
} Event;

typedef int (*EventFn)(Y *y, Event *e);

struct Y {
    const unsigned char *src;
    size_t   len;
    Mark     mark;

    Token   *tok;                /* queue: tok[head..len) */
    size_t   tok_head, tok_len, tok_cap;
    size_t   tokens_parsed;      /* tokens taken by the parser */
    bool     token_available;
    bool     stream_start_produced, stream_end_produced;

    int      flow_level;
    int      indent;             /* current block indentation column (-1 at top) */
    int     *indents;
    size_t   n_indents, cap_indents;
    SimpleKey *skeys;
    size_t   n_skeys, cap_skeys;
    bool     simple_key_allowed;
    bool     json_like;          /* the last token was a quoted scalar or a flow end: ':' after it is a value */

    /* parser */
    TagDir  *tagdirs;
    size_t   ntagdirs, cap_tagdirs;
    int      depth, max_depth;
    EventFn  on_event;
    void    *ctx;

    /* first error wins */
    bool     failed;
    char     err[192];
    Mark     err_mark;
};

static int y_error(Y *y, const char *msg, Mark m) {
    if (!y->failed) {
        y->failed = true;
        snprintf(y->err, sizeof y->err, "%s", msg);
        y->err_mark = m;
    }
    return 0;
}

static int y_oom(Y *y) { return y_error(y, "out of memory", y->mark); }

/* ---- Character tests ----------------------------------------------------- */

static inline int CH(Y *y, long k) {
    long i = (long)y->mark.index + k;
    return i >= 0 && (size_t)i < y->len ? y->src[i] : -1;     /* -1: end of input */
}
#define IS_Z(y, k)       (CH(y, k) < 0)
#define IS_BREAK(y, k)   (CH(y, k) == '\n' || CH(y, k) == '\r')
#define IS_BLANK(y, k)   (CH(y, k) == ' ' || CH(y, k) == '\t')
#define IS_BREAKZ(y, k)  (IS_BREAK(y, k) || IS_Z(y, k))
#define IS_BLANKZ(y, k)  (IS_BLANK(y, k) || IS_BREAKZ(y, k))
#define IS_FLOW_IND(c)   ((c) == ',' || (c) == '[' || (c) == ']' || (c) == '{' || (c) == '}')

static inline bool is_alnum_ch(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
}

/* Move past one (non-break) character. */
static inline void skip(Y *y) {
    unsigned char c = y->src[y->mark.index];
    size_t n = 1;
    if (c >= 0x80) {
        while (y->mark.index + n < y->len && (y->src[y->mark.index + n] & 0xC0) == 0x80) n++;
    }
    y->mark.index += n;
    y->mark.column++;
}

/* Move past a line break (\n, \r\n or \r). */
static inline void skip_break(Y *y) {
    if (CH(y, 0) == '\r' && CH(y, 1) == '\n') y->mark.index += 2;
    else y->mark.index++;
    y->mark.line++;
    y->mark.column = 0;
}

/* Copy one character into b and move past it. */
static inline void copy_ch(Y *y, Buf *b) {
    size_t at = y->mark.index;
    skip(y);
    buf_push(b, (const char *)y->src + at, y->mark.index - at);
}

/* A line break normalized to '\n'. */
static inline void copy_break(Y *y, Buf *b) {
    skip_break(y);
    buf_putc(b, '\n');
}

static bool at_doc_marker(Y *y) {
    if (y->mark.column != 0) return false;
    int c = CH(y, 0);
    return (c == '-' || c == '.') && CH(y, 1) == c && CH(y, 2) == c && IS_BLANKZ(y, 3);
}

/* ---- Token queue ------------------------------------------------------------ */

static void token_free(Token *t) {
    free(t->a);
    free(t->b);
    t->a = t->b = NULL;
}

static Token *tok_insert(Y *y, size_t pos, int type, Mark start, Mark end) {
    if (y->tok_len == y->tok_cap) {
        if (y->tok_head > 0) {                       /* reclaim taken slots first */
            memmove(y->tok, y->tok + y->tok_head, (y->tok_len - y->tok_head) * sizeof *y->tok);
            y->tok_len -= y->tok_head;
            y->tok_head = 0;
        }
        if (y->tok_len == y->tok_cap) {
            size_t nc = y->tok_cap ? y->tok_cap * 2 : 16;
            Token *nt = realloc(y->tok, nc * sizeof *nt);
            if (!nt) { y_oom(y); return NULL; }
            y->tok = nt;
            y->tok_cap = nc;
        }
    }
    size_t at = y->tok_head + pos;
    memmove(y->tok + at + 1, y->tok + at, (y->tok_len - at) * sizeof *y->tok);
    y->tok_len++;
    Token *t = &y->tok[at];
    memset(t, 0, sizeof *t);
    t->type = type;
    t->start = start;
    t->end = end;
    return t;
}

static Token *tok_append(Y *y, int type, Mark start, Mark end) {
    return tok_insert(y, y->tok_len - y->tok_head, type, start, end);
}

/* ---- Indentation and simple keys ----------------------------------------- */

static int push_int(Y *y, int **arr, size_t *n, size_t *cap, int v) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        int *na = realloc(*arr, nc * sizeof *na);
        if (!na) return y_oom(y);
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = v;
    return 1;
}

/* Is the run of blanks just before index (on its line) holding a tab? */
static bool tab_before(Y *y, size_t index) {
    while (index > 0) {
        unsigned char c = y->src[index - 1];
        if (c == '\t') return true;
        if (c != ' ') return false;
        index--;
    }
    return false;
}

/* Spaces at the start of the current line. */
static int line_spaces(Y *y) {
    size_t i = y->mark.index;
    while (i > 0 && y->src[i - 1] != '\n' && y->src[i - 1] != '\r') i--;
    int n = 0;
    while (i + (size_t)n < y->len && y->src[i + (size_t)n] == ' ') n++;
    return n;
}

static int roll_indent(Y *y, int column, long number, int type, Mark m) {
    if (y->flow_level) return 1;
    if (y->indent < column) {
        if (tab_before(y, m.index))
            return y_error(y, "found a tab character used to indent a block collection (YAML indents with spaces)", m);
        if (!push_int(y, &y->indents, &y->n_indents, &y->cap_indents, y->indent)) return 0;
        if ((int)y->n_indents > y->max_depth) return y_error(y, "nesting too deep", m);
        y->indent = column;
        Token *t = number < 0 ? tok_append(y, type, m, m)
                              : tok_insert(y, (size_t)number - y->tokens_parsed, type, m, m);
        if (!t) return 0;
    }
    return 1;
}

static int unroll_indent(Y *y, int column) {
    if (y->flow_level) return 1;
    while (y->indent > column) {
        if (!tok_append(y, T_BLOCK_END, y->mark, y->mark)) return 0;
        y->indent = y->indents[--y->n_indents];
    }
    return 1;
}

static SimpleKey *top_key(Y *y) { return &y->skeys[y->n_skeys - 1]; }

static int remove_simple_key(Y *y) {
    SimpleKey *k = top_key(y);
    if (k->possible && k->required)
        return y_error(y, "could not find expected ':' (an implicit key must be followed by ':')", k->mark);
    k->possible = false;
    return 1;
}

static int save_simple_key(Y *y) {
    bool required = !y->flow_level && y->indent == y->mark.column;
    if (y->simple_key_allowed) {
        if (!remove_simple_key(y)) return 0;
        SimpleKey *k = top_key(y);
        k->possible = true;
        k->required = required;
        k->token_number = y->tokens_parsed + (y->tok_len - y->tok_head);
        k->mark = y->mark;
    }
    return 1;
}

/* A possible simple key stops being one past its line or SIMPLE_KEY_MAX bytes. */
static int stale_simple_keys(Y *y) {
    for (size_t i = 0; i < y->n_skeys; i++) {
        SimpleKey *k = &y->skeys[i];
        bool multiline_ok = i > 0 && k->flow_map;     /* a flow mapping's keys may span lines */
        if (k->possible && ((!multiline_ok && k->mark.line < y->mark.line) ||
                            k->mark.index + SIMPLE_KEY_MAX < y->mark.index)) {
            if (k->required)
                return y_error(y, "could not find expected ':' (an implicit key must be followed by ':')", k->mark);
            k->possible = false;
        }
    }
    return 1;
}

static int push_simple_key(Y *y) {
    if (y->n_skeys == y->cap_skeys) {
        size_t nc = y->cap_skeys ? y->cap_skeys * 2 : 16;
        SimpleKey *nk = realloc(y->skeys, nc * sizeof *nk);
        if (!nk) return y_oom(y);
        y->skeys = nk;
        y->cap_skeys = nc;
    }
    memset(&y->skeys[y->n_skeys++], 0, sizeof(SimpleKey));
    return 1;
}

static int increase_flow_level(Y *y, bool map) {
    if (!push_simple_key(y)) return 0;
    top_key(y)->flow_map = map;
    if (++y->flow_level > y->max_depth) return y_error(y, "nesting too deep", y->mark);
    return 1;
}

static void decrease_flow_level(Y *y) {
    if (y->flow_level) {
        y->flow_level--;
        y->n_skeys--;
    }
}

/* ---- Whitespace and comments --------------------------------------------- */

/* True when everything from the start of the line to the mark is blank. */
static bool only_blanks_before(Y *y) {
    size_t i = y->mark.index;
    while (i > 0) {
        unsigned char c = y->src[i - 1];
        if (c == '\n' || c == '\r') return true;
        if (c != ' ' && c != '\t') return false;
        i--;
    }
    return true;
}

static int scan_to_next_token(Y *y) {
    for (;;) {
        if (y->mark.index == 0 && y->len >= 3 && memcmp(y->src, "\xEF\xBB\xBF", 3) == 0) {
            y->mark.index = 3;                       /* byte-order mark */
        }
        while (CH(y, 0) == ' ' || CH(y, 0) == '\t') skip(y);
        if (CH(y, 0) == '#') {
            if (y->mark.column > 0 && !IS_BLANK(y, -1))
                return y_error(y, "a comment must be separated from the preceding token by whitespace", y->mark);
            while (!IS_BREAKZ(y, 0)) skip(y);
        }
        if (IS_BREAK(y, 0)) {
            skip_break(y);
            if (!y->flow_level) y->simple_key_allowed = true;
        } else {
            break;
        }
    }
    if (IS_Z(y, 0) || !only_blanks_before(y)) return 1;
    /* the first token on a line: indentation is spaces only */
    int spaces = line_spaces(y);
    if (!y->flow_level) {
        if (tab_before(y, y->mark.index) && spaces <= y->indent)
            return y_error(y, "found a tab character used for indentation (YAML indents with spaces)", y->mark);
    } else if (spaces <= y->indent && CH(y, 0) != ']' && CH(y, 0) != '}' && !at_doc_marker(y)) {
        return y_error(y, "a flow collection's continuation line is not indented enough", y->mark);
    }
    return 1;
}

/* ---- Directives ------------------------------------------------------------ */

static int scan_tag_uri(Y *y, bool verbatim, bool directive, const char *head, size_t headlen, Mark start,
                        char **out, size_t *outlen);

static int scan_tag_handle(Y *y, bool directive, Mark start, char **out, size_t *outlen) {
    if (CH(y, 0) != '!')
        return y_error(y, directive ? "did not find expected '!' in a %TAG directive" : "did not find expected '!'", start);
    Buf b = { 0 };
    copy_ch(y, &b);
    while (is_alnum_ch(CH(y, 0))) copy_ch(y, &b);
    if (CH(y, 0) == '!') {
        copy_ch(y, &b);
    } else if (directive && !(b.len == 1)) {
        buf_free(&b);
        return y_error(y, "did not find expected '!' to end a %TAG handle", start);
    }
    if (b.oom) { buf_free(&b); return y_oom(y); }
    *out = buf_take(&b, outlen);
    return 1;
}

static int scan_directive(Y *y) {
    Mark start = y->mark;
    skip(y);                                         /* '%' */
    size_t ns = y->mark.index;
    while (is_alnum_ch(CH(y, 0))) skip(y);
    size_t nlen = y->mark.index - ns;
    if (nlen == 0) return y_error(y, "did not find expected directive name", start);
    if (!IS_BLANKZ(y, 0)) return y_error(y, "found unexpected character in a directive name", y->mark);
    const char *name = (const char *)y->src + ns;
    Token *t = NULL;

    if (nlen == 4 && memcmp(name, "YAML", 4) == 0) {
        while (IS_BLANK(y, 0)) skip(y);
        int v[2] = { 0, 0 };
        for (int part = 0; part < 2; part++) {
            int digits = 0;
            while (CH(y, 0) >= '0' && CH(y, 0) <= '9') {
                if (++digits > 9) return y_error(y, "version number too long in %YAML directive", start);
                v[part] = v[part] * 10 + (CH(y, 0) - '0');
                skip(y);
            }
            if (!digits) return y_error(y, "did not find expected version number in %YAML directive", y->mark);
            if (part == 0) {
                if (CH(y, 0) != '.') return y_error(y, "did not find expected '.' in %YAML directive", y->mark);
                skip(y);
            }
        }
        t = tok_append(y, T_VERSION, start, y->mark);
        if (!t) return 0;
        t->major = v[0];
        t->minor = v[1];
    } else if (nlen == 3 && memcmp(name, "TAG", 3) == 0) {
        while (IS_BLANK(y, 0)) skip(y);
        char *handle = NULL, *prefix = NULL;
        size_t hlen = 0, plen = 0;
        if (!scan_tag_handle(y, true, start, &handle, &hlen)) return 0;
        if (!IS_BLANK(y, 0)) { free(handle); return y_error(y, "did not find expected whitespace in %TAG directive", y->mark); }
        while (IS_BLANK(y, 0)) skip(y);
        if (!scan_tag_uri(y, true, true, NULL, 0, start, &prefix, &plen)) { free(handle); return 0; }
        if (!IS_BLANKZ(y, 0)) {
            free(handle);
            free(prefix);
            return y_error(y, "did not find expected whitespace or line break in %TAG directive", y->mark);
        }
        t = tok_append(y, T_TAG_DIRECTIVE, start, y->mark);
        if (!t) { free(handle); free(prefix); return 0; }
        t->a = handle; t->alen = hlen;
        t->b = prefix; t->blen = plen;
    } else {
        /* reserved directive: ignored (YAML 1.2, 6.8) */
        while (!IS_BREAKZ(y, 0)) skip(y);
    }
    while (IS_BLANK(y, 0)) skip(y);
    if (CH(y, 0) == '#' && IS_BLANK(y, -1)) while (!IS_BREAKZ(y, 0)) skip(y);
    if (!IS_BREAKZ(y, 0)) return y_error(y, "did not find expected comment or line break after a directive", y->mark);
    return 1;
}

/* ---- Anchors, aliases and tags ------------------------------------------- */

static int scan_anchor(Y *y, int type) {
    Mark start = y->mark;
    skip(y);                                         /* '&' or '*' */
    size_t s = y->mark.index;
    for (;;) {
        int c = CH(y, 0);
        if (c < 0 || IS_BLANKZ(y, 0) || IS_FLOW_IND(c)) break;
        skip(y);
    }
    size_t n = y->mark.index - s;
    if (n == 0)
        return y_error(y, type == T_ANCHOR ? "did not find expected anchor name" : "did not find expected alias name", start);
    Token *t = tok_append(y, type, start, y->mark);
    if (!t) return 0;
    t->a = xstrndup((const char *)y->src + s, n);
    t->alen = n;
    return t->a ? 1 : y_oom(y);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool uri_char(int c, bool verbatim) {
    if (c < 0) return false;
    if (is_alnum_ch(c)) return true;
    if (strchr(";/?:@&=+$.!~*'()%#", c) && c != 0) return true;
    return verbatim && (c == ',' || c == '[' || c == ']');
}

static int scan_tag_uri(Y *y, bool verbatim, bool directive, const char *head, size_t headlen, Mark start,
                        char **out, size_t *outlen) {
    Buf b = { 0 };
    if (headlen > 1) buf_push(&b, head + 1, headlen - 1);     /* the handle's word part, without the '!' */
    while (uri_char(CH(y, 0), verbatim || directive)) {
        if (CH(y, 0) == '!' && !verbatim && !directive) {
            /* '!' in a shorthand suffix only as a character, never ends it */
        }
        if (CH(y, 0) == '%') {
            int h1 = hexval(CH(y, 1)), h2 = hexval(CH(y, 2));
            if (h1 < 0 || h2 < 0) { buf_free(&b); return y_error(y, "found an invalid %-escape in a tag", y->mark); }
            buf_putc(&b, (char)(h1 * 16 + h2));
            skip(y); skip(y); skip(y);
            continue;
        }
        copy_ch(y, &b);
    }
    if (b.oom) { buf_free(&b); return y_oom(y); }
    if (b.len == 0 && (verbatim || directive)) {
        buf_free(&b);
        return y_error(y, "did not find expected tag URI", start);
    }
    *out = buf_take(&b, outlen);
    return *out ? 1 : y_oom(y);
}

static int scan_tag(Y *y) {
    Mark start = y->mark;
    char *handle = NULL, *suffix = NULL;
    size_t hlen = 0, slen = 0;
    if (CH(y, 1) == '<') {                           /* verbatim: !<tag:...> */
        skip(y); skip(y);
        if (!scan_tag_uri(y, true, false, NULL, 0, start, &suffix, &slen)) return 0;
        if (CH(y, 0) != '>') { free(suffix); return y_error(y, "did not find the expected '>' of a verbatim tag", y->mark); }
        skip(y);
        handle = xstrndup("", 0);
    } else {
        if (!scan_tag_handle(y, false, start, &handle, &hlen)) return 0;
        if (hlen > 1 && handle[0] == '!' && handle[hlen - 1] == '!') {
            if (!scan_tag_uri(y, false, false, NULL, 0, start, &suffix, &slen)) { free(handle); return 0; }
            if (slen == 0) {
                free(handle);
                free(suffix);
                return y_error(y, "did not find the expected tag suffix", start);
            }
        } else {
            /* "!foo": handle "!" and suffix "foo"; a lone "!" is the non-specific tag */
            if (!scan_tag_uri(y, false, false, handle, hlen, start, &suffix, &slen)) { free(handle); return 0; }
            free(handle);
            if (slen == 0) {
                handle = xstrndup("", 0);
                free(suffix);
                suffix = xstrndup("!", 1);
                slen = 1;
            } else {
                handle = xstrndup("!", 1);
            }
            hlen = handle ? strlen(handle) : 0;
        }
    }
    if (!handle || !suffix) { free(handle); free(suffix); return y_oom(y); }
    if (!IS_BLANKZ(y, 0) && !(y->flow_level && IS_FLOW_IND(CH(y, 0)))) {
        free(handle);
        free(suffix);
        return y_error(y, "did not find expected whitespace or line break after a tag", y->mark);
    }
    Token *t = tok_append(y, T_TAG, start, y->mark);
    if (!t) { free(handle); free(suffix); return 0; }
    t->a = handle; t->alen = strlen(handle);
    t->b = suffix; t->blen = slen;
    return 1;
}

/* ---- Block scalars ---------------------------------------------------------- */

/* Empty lines before (and between) the lines of a block scalar. *indent < 0:
 * not known yet, detected from the first content line. */
static int block_scalar_breaks(Y *y, int *indent, Buf *breaks) {
    int max_empty = 0;
    for (;;) {
        while ((*indent < 0 || y->mark.column < *indent) && CH(y, 0) == ' ') skip(y);
        if (CH(y, 0) == '\t' && (*indent < 0 ? y->mark.column <= y->indent : y->mark.column < *indent))
            return y_error(y, "found a tab character where an indentation space is expected", y->mark);
        if (IS_Z(y, 0) && y->mark.column > 0 && (*indent < 0 || y->mark.column <= *indent)) {
            buf_putc(breaks, '\n');                 /* a last line of spaces without a line break */
            break;
        }
        if (!IS_BREAK(y, 0)) break;
        if (*indent < 0 && y->mark.column > max_empty) max_empty = y->mark.column;
        copy_break(y, breaks);
    }
    if (*indent < 0) {
        int col = y->mark.column;
        if (!IS_Z(y, 0) && col > y->indent && !at_doc_marker(y)) {
            if (col < max_empty)
                return y_error(y, "a leading empty line of a block scalar has more spaces than its first content line", y->mark);
            *indent = col;
        } else {
            *indent = max_empty;
        }
        if (*indent < y->indent + 1) *indent = y->indent + 1;
    }
    return 1;
}

static int scan_block_scalar(Y *y, bool literal) {
    Mark start = y->mark;
    skip(y);                                         /* '|' or '>' */
    int chomping = 0, increment = 0;
    if (CH(y, 0) == '+' || CH(y, 0) == '-') {
        chomping = CH(y, 0) == '+' ? 1 : -1;
        skip(y);
        if (CH(y, 0) >= '0' && CH(y, 0) <= '9') {
            if (CH(y, 0) == '0') return y_error(y, "found an indentation indicator equal to 0", y->mark);
            increment = CH(y, 0) - '0';
            skip(y);
        }
    } else if (CH(y, 0) >= '0' && CH(y, 0) <= '9') {
        if (CH(y, 0) == '0') return y_error(y, "found an indentation indicator equal to 0", y->mark);
        increment = CH(y, 0) - '0';
        skip(y);
        if (CH(y, 0) == '+' || CH(y, 0) == '-') {
            chomping = CH(y, 0) == '+' ? 1 : -1;
            skip(y);
        }
    }
    if (!IS_BLANKZ(y, 0) && CH(y, 0) != '#')
        return y_error(y, "did not find expected comment or line break after a block scalar header", y->mark);
    while (IS_BLANK(y, 0)) skip(y);
    if (CH(y, 0) == '#') {
        if (y->src[y->mark.index - 1] != ' ' && y->src[y->mark.index - 1] != '\t')
            return y_error(y, "a comment must be separated from the block scalar header by a space", y->mark);
        while (!IS_BREAKZ(y, 0)) skip(y);
    }
    if (!IS_BREAKZ(y, 0))
        return y_error(y, "did not find expected comment or line break after a block scalar header", y->mark);
    if (IS_BREAK(y, 0)) skip_break(y);
    Mark end = y->mark;

    /* at the top level the indicator is the column itself, as libyaml (and
     * the emitters built on it) read it */
    int indent = increment ? (y->indent >= 0 ? y->indent + increment : increment) : -1;

    Buf str = { 0 }, leading = { 0 }, trailing = { 0 };
    int ok = block_scalar_breaks(y, &indent, &trailing);
    bool leading_blank = false;
    while (ok && y->mark.column == indent && !IS_Z(y, 0) && !at_doc_marker(y)) {
        bool trailing_blank = IS_BLANK(y, 0);
        if (!literal && leading.len && leading.data[0] == '\n' && !leading_blank && !trailing_blank) {
            if (trailing.len == 0) buf_putc(&str, ' ');
            buf_clear(&leading);
        } else {
            buf_push(&str, leading.data, leading.len);
            buf_clear(&leading);
        }
        buf_push(&str, trailing.data, trailing.len);
        buf_clear(&trailing);
        leading_blank = IS_BLANK(y, 0);
        while (!IS_BREAKZ(y, 0)) copy_ch(y, &str);
        end = y->mark;
        if (IS_Z(y, 0)) {
            buf_putc(&leading, '\n');               /* the last line ends at the end of input */
            break;
        }
        copy_break(y, &leading);
        ok = block_scalar_breaks(y, &indent, &trailing);
    }
    if (ok && chomping != -1) buf_push(&str, leading.data, leading.len);
    if (ok && chomping == 1) buf_push(&str, trailing.data, trailing.len);
    buf_free(&leading);
    buf_free(&trailing);
    if (!ok) { buf_free(&str); return 0; }
    if (str.oom) { buf_free(&str); return y_oom(y); }
    Token *t = tok_append(y, T_SCALAR, start, end);
    if (!t) { buf_free(&str); return 0; }
    t->a = buf_take(&str, &t->alen);
    t->style = literal ? S_LITERAL : S_FOLDED;
    return 1;
}

/* ---- Quoted scalars --------------------------------------------------------- */

static int scan_flow_scalar(Y *y, bool single) {
    Mark start = y->mark;
    skip(y);
    Buf str = { 0 }, ws = { 0 }, leading = { 0 }, trailing = { 0 };
    int ok = 1;
    for (;;) {
        if (at_doc_marker(y)) { ok = y_error(y, "found a document marker inside a quoted scalar", y->mark); break; }
        if (IS_Z(y, 0)) { ok = y_error(y, "found unexpected end of input in a quoted scalar", start); break; }
        bool leading_blanks = false;
        while (!IS_BLANKZ(y, 0)) {
            int c = CH(y, 0);
            if (single && c == '\'' && CH(y, 1) == '\'') {
                buf_putc(&str, '\'');
                skip(y); skip(y);
            } else if (c == (single ? '\'' : '"')) {
                break;
            } else if (!single && c == '\\' && IS_BREAK(y, 1)) {
                skip(y);
                skip_break(y);
                leading_blanks = true;
                break;
            } else if (!single && c == '\\') {
                int code_len = 0;
                Mark esc = y->mark;
                switch (CH(y, 1)) {
                    case '0':  buf_putc(&str, '\0'); break;
                    case 'a':  buf_putc(&str, '\a'); break;
                    case 'b':  buf_putc(&str, '\b'); break;
                    case 't': case '\t': buf_putc(&str, '\t'); break;
                    case 'n':  buf_putc(&str, '\n'); break;
                    case 'v':  buf_putc(&str, '\v'); break;
                    case 'f':  buf_putc(&str, '\f'); break;
                    case 'r':  buf_putc(&str, '\r'); break;
                    case 'e':  buf_putc(&str, '\x1B'); break;
                    case ' ':  buf_putc(&str, ' '); break;
                    case '"':  buf_putc(&str, '"'); break;
                    case '/':  buf_putc(&str, '/'); break;
                    case '\\': buf_putc(&str, '\\'); break;
                    case 'N':  push_utf8(&str, 0x85); break;
                    case '_':  push_utf8(&str, 0xA0); break;
                    case 'L':  push_utf8(&str, 0x2028); break;
                    case 'P':  push_utf8(&str, 0x2029); break;
                    case 'x':  code_len = 2; break;
                    case 'u':  code_len = 4; break;
                    case 'U':  code_len = 8; break;
                    default:
                        ok = y_error(y, "found unknown escape character in a double-quoted scalar", esc);
                        break;
                }
                if (!ok) break;
                skip(y); skip(y);
                if (code_len) {
                    uint32_t v = 0;
                    for (int k = 0; k < code_len; k++) {
                        int h = hexval(CH(y, k));
                        if (h < 0) { ok = y_error(y, "did not find expected hexadecimal digits in an escape", esc); break; }
                        v = v * 16 + (uint32_t)h;
                    }
                    if (!ok) break;
                    if ((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
                        ok = y_error(y, "found an invalid Unicode character escape", esc);
                        break;
                    }
                    push_utf8(&str, v);
                    for (int k = 0; k < code_len; k++) skip(y);
                }
            } else {
                copy_ch(y, &str);
            }
        }
        if (!ok) break;
        if (CH(y, 0) == (single ? '\'' : '"')) break;
        /* blanks and line breaks: folded */
        while (IS_BLANK(y, 0) || IS_BREAK(y, 0)) {
            if (IS_BLANK(y, 0)) {
                if (!leading_blanks) copy_ch(y, &ws);
                else skip(y);
            } else {
                if (!leading_blanks) {
                    buf_clear(&ws);
                    copy_break(y, &leading);
                    leading_blanks = true;
                } else {
                    copy_break(y, &trailing);
                }
            }
        }
        if (leading_blanks) {
            if (leading.len && leading.data[0] == '\n') {
                if (trailing.len == 0) buf_putc(&str, ' ');
                else buf_push(&str, trailing.data, trailing.len);
            } else {
                buf_push(&str, leading.data, leading.len);
                buf_push(&str, trailing.data, trailing.len);
            }
            buf_clear(&leading);
            buf_clear(&trailing);
        } else {
            buf_push(&str, ws.data, ws.len);
            buf_clear(&ws);
        }
        if (leading_blanks && y->flow_level == 0 && line_spaces(y) <= y->indent && !IS_Z(y, 0)) {
            /* a continuation line of a multi-line quoted scalar must be indented */
            ok = y_error(y, "a quoted scalar's continuation line is not indented enough", y->mark);
            break;
        }
    }
    buf_free(&ws);
    buf_free(&leading);
    buf_free(&trailing);
    if (!ok) { buf_free(&str); return 0; }
    skip(y);                                         /* closing quote */
    if (str.oom) { buf_free(&str); return y_oom(y); }
    Token *t = tok_append(y, T_SCALAR, start, y->mark);
    if (!t) { buf_free(&str); return 0; }
    t->a = buf_take(&str, &t->alen);
    t->style = single ? S_SINGLE : S_DOUBLE;
    return 1;
}

/* ---- Plain scalars ---------------------------------------------------------- */

static int scan_plain_scalar(Y *y) {
    Mark start = y->mark, end = y->mark;
    Buf str = { 0 }, ws = { 0 }, leading = { 0 }, trailing = { 0 };
    int indent = y->indent + 1;
    bool leading_blanks = false;
    int ok = 1;
    for (;;) {
        if (at_doc_marker(y)) break;
        if (CH(y, 0) == '#') break;
        while (!IS_BLANKZ(y, 0)) {
            int c = CH(y, 0);
            if (c == ':' && (IS_BLANKZ(y, 1) || (y->flow_level && IS_FLOW_IND(CH(y, 1))))) break;
            if (y->flow_level && IS_FLOW_IND(c)) break;
            if (leading_blanks || ws.len) {
                if (leading_blanks) {
                    if (leading.len && leading.data[0] == '\n') {
                        if (trailing.len == 0) buf_putc(&str, ' ');
                        else buf_push(&str, trailing.data, trailing.len);
                    } else {
                        buf_push(&str, leading.data, leading.len);
                        buf_push(&str, trailing.data, trailing.len);
                    }
                    buf_clear(&leading);
                    buf_clear(&trailing);
                    leading_blanks = false;
                } else {
                    buf_push(&str, ws.data, ws.len);
                    buf_clear(&ws);
                }
            }
            copy_ch(y, &str);
            end = y->mark;
        }
        if (!(IS_BLANK(y, 0) || IS_BREAK(y, 0))) break;
        while (IS_BLANK(y, 0) || IS_BREAK(y, 0)) {
            if (IS_BLANK(y, 0)) {
                if (leading_blanks && y->mark.column < indent && CH(y, 0) == '\t' && !y->flow_level) {
                    size_t i = 0;
                    while (IS_BLANK(y, i)) i++;
                    if (!(CH(y, i) == '\n' || CH(y, i) == '\r' || CH(y, i) < 0 || CH(y, i) == '#')) {
                        ok = y_error(y, "found a tab character that violates indentation", y->mark);
                        break;
                    }
                }
                if (!leading_blanks) copy_ch(y, &ws);
                else skip(y);
            } else {
                if (!leading_blanks) {
                    buf_clear(&ws);
                    copy_break(y, &leading);
                    leading_blanks = true;
                } else {
                    copy_break(y, &trailing);
                }
            }
        }
        if (!ok) break;
        if (!y->flow_level && y->mark.column < indent) break;
    }
    buf_free(&ws);
    buf_free(&leading);
    buf_free(&trailing);
    if (!ok) { buf_free(&str); return 0; }
    if (str.oom) { buf_free(&str); return y_oom(y); }
    Token *t = tok_append(y, T_SCALAR, start, end);
    if (!t) { buf_free(&str); return 0; }
    t->a = buf_take(&str, &t->alen);
    t->style = S_PLAIN;
    if (leading_blanks) y->simple_key_allowed = true;
    return 1;
}

/* ---- Token dispatch ---------------------------------------------------------- */

static int fetch_simple(Y *y, int type) {
    Mark start = y->mark;
    if (type == T_DOC_START || type == T_DOC_END) { skip(y); skip(y); }
    skip(y);
    return tok_append(y, type, start, y->mark) != NULL;
}

static int fetch_next_token(Y *y) {
    if (!y->stream_start_produced) {
        y->indent = -1;
        y->simple_key_allowed = true;
        y->stream_start_produced = true;
        if (!push_simple_key(y)) return 0;
        return tok_append(y, T_STREAM_START, y->mark, y->mark) != NULL;
    }
    if (!scan_to_next_token(y)) return 0;
    if (!stale_simple_keys(y)) return 0;
    if (!unroll_indent(y, y->mark.column)) return 0;

    if (IS_Z(y, 0)) {
        if (y->flow_level) return y_error(y, "found unexpected end of input inside a flow collection", y->mark);
        if (!unroll_indent(y, -1)) return 0;
        if (!remove_simple_key(y)) return 0;
        y->simple_key_allowed = false;
        y->stream_end_produced = true;
        return tok_append(y, T_STREAM_END, y->mark, y->mark) != NULL;
    }
    int c = CH(y, 0);
    bool json_like = y->json_like;
    y->json_like = false;
    if (y->mark.column == 0 && c == '%' && !y->flow_level) {
        if (!unroll_indent(y, -1) || !remove_simple_key(y)) return 0;
        y->simple_key_allowed = false;
        return scan_directive(y);
    }
    if (at_doc_marker(y)) {
        if (!unroll_indent(y, -1) || !remove_simple_key(y)) return 0;
        y->simple_key_allowed = false;
        if (y->flow_level) return y_error(y, "found a document marker inside a flow collection", y->mark);
        if (c == '-') return fetch_simple(y, T_DOC_START);
        if (!fetch_simple(y, T_DOC_END)) return 0;
        /* only a comment may follow "..." on its line */
        while (IS_BLANK(y, 0)) skip(y);
        if (CH(y, 0) == '#' && IS_BLANK(y, -1)) while (!IS_BREAKZ(y, 0)) skip(y);
        if (!IS_BREAKZ(y, 0)) return y_error(y, "found content after a document end marker ('...')", y->mark);
        return 1;
    }
    switch (c) {
        case '[': case '{':
            if (!save_simple_key(y) || !increase_flow_level(y, c == '{')) return 0;
            y->simple_key_allowed = true;
            return fetch_simple(y, c == '[' ? T_FSEQ_START : T_FMAP_START);
        case ']': case '}':
            if (!remove_simple_key(y)) return 0;
            decrease_flow_level(y);
            y->simple_key_allowed = false;
            y->json_like = true;
            return fetch_simple(y, c == ']' ? T_FSEQ_END : T_FMAP_END);
        case ',':
            if (!remove_simple_key(y)) return 0;
            y->simple_key_allowed = true;
            return fetch_simple(y, T_FLOW_ENTRY);
        default: break;
    }
    if (c == '-' && IS_BLANKZ(y, 1)) {
        if (!y->flow_level) {
            if (!y->simple_key_allowed) return y_error(y, "block sequence entries are not allowed here", y->mark);
            if (!roll_indent(y, y->mark.column, -1, T_BSEQ_START, y->mark)) return 0;
        }
        if (!remove_simple_key(y)) return 0;
        y->simple_key_allowed = true;
        return fetch_simple(y, T_BLOCK_ENTRY);
    }
    if (c == '?' && IS_BLANKZ(y, 1)) {
        if (!y->flow_level) {
            if (!y->simple_key_allowed) return y_error(y, "mapping keys are not allowed here", y->mark);
            if (!roll_indent(y, y->mark.column, -1, T_BMAP_START, y->mark)) return 0;
        }
        if (!remove_simple_key(y)) return 0;
        y->simple_key_allowed = !y->flow_level;
        return fetch_simple(y, T_KEY);
    }
    if (c == ':' && (IS_BLANKZ(y, 1) || (y->flow_level && (IS_FLOW_IND(CH(y, 1)) || json_like)))) {
        SimpleKey *k = top_key(y);
        if (k->possible) {
            if (!tok_insert(y, k->token_number - y->tokens_parsed, T_KEY, k->mark, k->mark)) return 0;
            if (!roll_indent(y, k->mark.column, (long)k->token_number, T_BMAP_START, k->mark)) return 0;
            k->possible = false;
            y->simple_key_allowed = false;
        } else {
            if (!y->flow_level) {
                if (!y->simple_key_allowed) return y_error(y, "mapping values are not allowed here", y->mark);
                if (!roll_indent(y, y->mark.column, -1, T_BMAP_START, y->mark)) return 0;
            }
            y->simple_key_allowed = !y->flow_level;
        }
        return fetch_simple(y, T_VALUE);
    }
    if (c == '*' || c == '&') {
        if (!save_simple_key(y)) return 0;
        y->simple_key_allowed = false;
        return scan_anchor(y, c == '*' ? T_ALIAS : T_ANCHOR);
    }
    if (c == '!') {
        if (!save_simple_key(y)) return 0;
        y->simple_key_allowed = false;
        return scan_tag(y);
    }
    if ((c == '|' || c == '>') && !y->flow_level) {
        if (!remove_simple_key(y)) return 0;
        y->simple_key_allowed = true;
        return scan_block_scalar(y, c == '|');
    }
    if (c == '\'' || c == '"') {
        if (!save_simple_key(y)) return 0;
        y->simple_key_allowed = false;
        y->json_like = true;
        return scan_flow_scalar(y, c == '\'');
    }
    bool indicator = IS_BLANKZ(y, 0) || strchr("-?:,[]{}#&*!|>'\"%@`", c);
    if (!indicator || (c == '-' && !IS_BLANK(y, 1) && !(y->flow_level && IS_FLOW_IND(CH(y, 1)))) ||
        ((c == '?' || c == ':') && !IS_BLANKZ(y, 1) && !(y->flow_level && IS_FLOW_IND(CH(y, 1))))) {
        if (!save_simple_key(y)) return 0;
        y->simple_key_allowed = false;
        return scan_plain_scalar(y);
    }
    if (c == '\t') return y_error(y, "found a tab character where a token is expected", y->mark);
    if (c == '@' || c == '`') return y_error(y, "found a reserved indicator ('@' or '`') that cannot start a plain scalar", y->mark);
    char msg[80];
    snprintf(msg, sizeof msg, "found character '%c' that cannot start any token", c);
    return y_error(y, msg, y->mark);
}

static int fetch_more_tokens(Y *y) {
    for (;;) {
        bool need = false;
        if (y->tok_head == y->tok_len) {
            need = true;
        } else {
            if (!stale_simple_keys(y)) return 0;
            for (size_t i = 0; i < y->n_skeys; i++) {
                if (y->skeys[i].possible && y->skeys[i].token_number == y->tokens_parsed) { need = true; break; }
            }
        }
        if (!need) break;
        if (y->stream_end_produced) break;
        if (!fetch_next_token(y)) return 0;
    }
    y->token_available = true;
    return 1;
}

/* The next token (NULL after an error). */
static Token *peek(Y *y) {
    if (y->failed) return NULL;
    if (!y->token_available && !fetch_more_tokens(y)) return NULL;
    if (y->tok_head == y->tok_len) { y_error(y, "unexpected end of input", y->mark); return NULL; }
    return &y->tok[y->tok_head];
}

/* Drop the next token (freeing its strings unless the caller took them). */
static void skip_token(Y *y) {
    token_free(&y->tok[y->tok_head]);
    y->tok_head++;
    y->tokens_parsed++;
    y->token_available = false;
    if (y->tok_head == y->tok_len) y->tok_head = y->tok_len = 0;
}

/* ======================================================================== */
/* Parser: tokens → events (recursive descent)                              */
/* ======================================================================== */

#define TAG_PREFIX "tag:yaml.org,2002:"

static int emit_event(Y *y, Event *e) {
    if (y->failed) return 0;
    return y->on_event(y, e);
}

static int simple_event(Y *y, int type, Mark m) {
    Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.start = e.end = m;
    return emit_event(y, &e);
}

static void tagdirs_clear(Y *y) {
    for (size_t i = 0; i < y->ntagdirs; i++) {
        free(y->tagdirs[i].handle);
        free(y->tagdirs[i].prefix);
    }
    y->ntagdirs = 0;
}

/* The full tag for a handle + suffix (malloc'd), using the %TAG directives. */
static char *resolve_tag(Y *y, Token *t) {
    if (t->alen == 0) return xstrndup(t->b, t->blen);          /* verbatim, or "!" */
    const char *prefix = NULL;
    for (size_t i = 0; i < y->ntagdirs; i++)
        if (strcmp(y->tagdirs[i].handle, t->a) == 0) { prefix = y->tagdirs[i].prefix; break; }
    if (!prefix) {
        if (strcmp(t->a, "!") == 0) prefix = "!";
        else if (strcmp(t->a, "!!") == 0) prefix = TAG_PREFIX;
        else {
            char msg[128];
            snprintf(msg, sizeof msg, "found undefined tag handle '%.60s'", t->a);
            y_error(y, msg, t->start);
            return NULL;
        }
    }
    size_t pl = strlen(prefix);
    char *r = malloc(pl + t->blen + 1);
    if (!r) { y_oom(y); return NULL; }
    memcpy(r, prefix, pl);
    memcpy(r + pl, t->b, t->blen);
    r[pl + t->blen] = '\0';
    return r;
}

static int parse_node(Y *y, bool block, bool indentless);

static int empty_scalar(Y *y, Mark m) {
    Event e;
    memset(&e, 0, sizeof e);
    e.type = E_SCALAR;
    e.start = e.end = m;
    e.value = "";
    e.style = S_PLAIN;
    return emit_event(y, &e);
}

static int enter(Y *y, Mark m) {
    if (++y->depth > y->max_depth) return y_error(y, "nesting too deep", m);
    return 1;
}

static int collection_event(Y *y, int type, Mark m, char *anchor, char *tag, int flow) {
    Event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.start = e.end = m;
    e.anchor = anchor;
    e.tag = tag;
    e.style = flow;
    int ok = emit_event(y, &e);
    free(anchor);
    free(tag);
    return ok;
}

static int parse_block_sequence(Y *y) {
    for (;;) {
        Token *t = peek(y);
        if (!t) return 0;
        if (t->type == T_BLOCK_ENTRY) {
            Mark m = t->end;
            skip_token(y);
            t = peek(y);
            if (!t) return 0;
            if (t->type == T_BLOCK_ENTRY || t->type == T_BLOCK_END) {
                if (!empty_scalar(y, m)) return 0;
            } else if (!parse_node(y, true, false)) {
                return 0;
            }
        } else if (t->type == T_BLOCK_END) {
            Mark m = t->start;
            skip_token(y);
            y->depth--;
            return simple_event(y, E_SEQ_END, m);
        } else {
            return y_error(y, "did not find expected '-' indicator (while parsing a block sequence)", t->start);
        }
    }
}

static int parse_indentless_sequence(Y *y) {
    for (;;) {
        Token *t = peek(y);
        if (!t) return 0;
        if (t->type != T_BLOCK_ENTRY) {
            y->depth--;
            return simple_event(y, E_SEQ_END, t->start);
        }
        Mark m = t->end;
        skip_token(y);
        t = peek(y);
        if (!t) return 0;
        if (t->type == T_BLOCK_ENTRY || t->type == T_KEY || t->type == T_VALUE || t->type == T_BLOCK_END) {
            if (!empty_scalar(y, m)) return 0;
        } else if (!parse_node(y, true, false)) {
            return 0;
        }
    }
}

static int parse_block_mapping(Y *y) {
    for (;;) {
        Token *t = peek(y);
        if (!t) return 0;
        if (t->type == T_KEY) {
            Mark m = t->end;
            skip_token(y);
            t = peek(y);
            if (!t) return 0;
            if (t->type != T_KEY && t->type != T_VALUE && t->type != T_BLOCK_END) {
                if (!parse_node(y, true, true)) return 0;
            } else if (!empty_scalar(y, m)) {
                return 0;
            }
        } else if (t->type == T_VALUE) {
            if (!empty_scalar(y, t->start)) return 0;     /* ": v" without a key */
        } else if (t->type == T_BLOCK_END) {
            Mark m = t->start;
            skip_token(y);
            y->depth--;
            return simple_event(y, E_MAP_END, m);
        } else {
            return y_error(y, "did not find expected key (while parsing a block mapping)", t->start);
        }
        /* the value */
        t = peek(y);
        if (!t) return 0;
        if (t->type == T_VALUE) {
            Mark m = t->end;
            skip_token(y);
            t = peek(y);
            if (!t) return 0;
            if (t->type != T_KEY && t->type != T_VALUE && t->type != T_BLOCK_END) {
                if (!parse_node(y, true, true)) return 0;
            } else if (!empty_scalar(y, m)) {
                return 0;
            }
        } else if (!empty_scalar(y, t->start)) {
            return 0;
        }
    }
}

/* One "key: value" pair inside a flow sequence or mapping, after its KEY token
 * (or at a VALUE token when the key is empty). */
static int parse_flow_pair(Y *y, int end_type, Mark m) {
    Token *t = peek(y);
    if (!t) return 0;
    if (t->type != T_VALUE && t->type != T_FLOW_ENTRY && t->type != end_type) {
        if (!parse_node(y, false, false)) return 0;
    } else if (!empty_scalar(y, m)) {
        return 0;
    }
    t = peek(y);
    if (!t) return 0;
    if (t->type == T_VALUE) {
        Mark vm = t->end;
        skip_token(y);
        t = peek(y);
        if (!t) return 0;
        if (t->type != T_FLOW_ENTRY && t->type != end_type) return parse_node(y, false, false);
        return empty_scalar(y, vm);
    }
    return empty_scalar(y, t->start);
}

static int parse_flow_sequence(Y *y) {
    bool first = true;
    for (;;) {
        Token *t = peek(y);
        if (!t) return 0;
        if (t->type != T_FSEQ_END) {
            if (!first) {
                if (t->type != T_FLOW_ENTRY)
                    return y_error(y, "did not find expected ',' or ']' (while parsing a flow sequence)", t->start);
                skip_token(y);
                t = peek(y);
                if (!t) return 0;
            }
        }
        if (t->type == T_FSEQ_END) {
            Mark m = t->start;
            skip_token(y);
            y->depth--;
            return simple_event(y, E_SEQ_END, m);
        }
        if (t->type == T_FLOW_ENTRY) return y_error(y, "found an empty entry in a flow sequence", t->start);
        first = false;
        if (t->type == T_KEY || t->type == T_VALUE) {
            Mark m = t->start;
            if (!enter(y, m)) return 0;
            if (!collection_event(y, E_MAP_START, m, NULL, NULL, 1)) return 0;
            if (t->type == T_KEY) skip_token(y);
            if (!parse_flow_pair(y, T_FSEQ_END, m)) return 0;
            y->depth--;
            t = peek(y);
            if (!t) return 0;
            if (!simple_event(y, E_MAP_END, t->start)) return 0;
        } else if (!parse_node(y, false, false)) {
            return 0;
        }
    }
}

static int parse_flow_mapping(Y *y) {
    bool first = true;
    for (;;) {
        Token *t = peek(y);
        if (!t) return 0;
        if (t->type != T_FMAP_END) {
            if (!first) {
                if (t->type != T_FLOW_ENTRY)
                    return y_error(y, "did not find expected ',' or '}' (while parsing a flow mapping)", t->start);
                skip_token(y);
                t = peek(y);
                if (!t) return 0;
            }
        }
        if (t->type == T_FMAP_END) {
            Mark m = t->start;
            skip_token(y);
            y->depth--;
            return simple_event(y, E_MAP_END, m);
        }
        if (t->type == T_FLOW_ENTRY) return y_error(y, "found an empty entry in a flow mapping", t->start);
        first = false;
        if (t->type == T_KEY) {
            Mark m = t->end;
            skip_token(y);
            if (!parse_flow_pair(y, T_FMAP_END, m)) return 0;
        } else if (t->type == T_VALUE) {
            if (!parse_flow_pair(y, T_FMAP_END, t->start)) return 0;
        } else {
            /* "{a, b}": a key without a value */
            if (!parse_node(y, false, false)) return 0;
            t = peek(y);
            if (!t) return 0;
            if (!empty_scalar(y, t->start)) return 0;
        }
    }
}

static int parse_node(Y *y, bool block, bool indentless) {
    Token *t = peek(y);
    if (!t) return 0;
    if (t->type == T_ALIAS) {
        Event e;
        memset(&e, 0, sizeof e);
        e.type = E_ALIAS;
        e.start = t->start;
        e.end = t->end;
        e.anchor = t->a;
        int ok = emit_event(y, &e);
        skip_token(y);
        return ok;
    }
    char *anchor = NULL, *tag = NULL;
    Mark start = t->start, tag_mark = t->start;
    bool have_props = false;
    for (int i = 0; i < 2 && t; i++) {
        if (t->type == T_ANCHOR && !anchor) {
            anchor = t->a;
            t->a = NULL;
        } else if (t->type == T_TAG && !tag) {
            tag_mark = t->start;
            tag = resolve_tag(y, t);
            if (!tag) { free(anchor); return 0; }
        } else {
            break;
        }
        have_props = true;
        skip_token(y);
        t = peek(y);
    }
    if (!t) { free(anchor); free(tag); return 0; }
    if (t->type == T_ANCHOR || t->type == T_TAG) {
        free(anchor);
        free(tag);
        return y_error(y, t->type == T_ANCHOR ? "found a second anchor on the same node" : "found a second tag on the same node", t->start);
    }
    (void)tag_mark;

    if (indentless && t->type == T_BLOCK_ENTRY) {
        if (!enter(y, t->start)) { free(anchor); free(tag); return 0; }
        if (!collection_event(y, E_SEQ_START, start, anchor, tag, 0)) return 0;
        return parse_indentless_sequence(y);
    }
    if (t->type == T_SCALAR) {
        Event e;
        memset(&e, 0, sizeof e);
        e.type = E_SCALAR;
        e.start = have_props ? start : t->start;
        e.end = t->end;
        e.anchor = anchor;
        e.tag = tag;
        e.value = t->a;
        e.len = t->alen;
        e.style = t->style;
        int ok = emit_event(y, &e);
        free(anchor);
        free(tag);
        skip_token(y);
        return ok;
    }
    if (t->type == T_FSEQ_START || t->type == T_FMAP_START) {
        int type = t->type == T_FSEQ_START ? E_SEQ_START : E_MAP_START;
        if (!enter(y, t->start)) { free(anchor); free(tag); return 0; }
        skip_token(y);
        if (!collection_event(y, type, start, anchor, tag, 1)) return 0;
        return type == E_SEQ_START ? parse_flow_sequence(y) : parse_flow_mapping(y);
    }
    if (block && (t->type == T_BSEQ_START || t->type == T_BMAP_START)) {
        int type = t->type == T_BSEQ_START ? E_SEQ_START : E_MAP_START;
        if (!enter(y, t->start)) { free(anchor); free(tag); return 0; }
        skip_token(y);
        if (!collection_event(y, type, start, anchor, tag, 0)) return 0;
        return type == E_SEQ_START ? parse_block_sequence(y) : parse_block_mapping(y);
    }
    if (have_props) {
        Event e;
        memset(&e, 0, sizeof e);
        e.type = E_SCALAR;
        e.start = start;
        e.end = t->start;
        e.anchor = anchor;
        e.tag = tag;
        e.value = "";
        e.style = S_PLAIN;
        int ok = emit_event(y, &e);
        free(anchor);
        free(tag);
        return ok;
    }
    if (t->type == T_BLOCK_ENTRY && !block)
        return y_error(y, "block sequence entries ('- ') are not allowed in a flow collection", t->start);
    return y_error(y, block ? "did not find expected node content (while parsing a block node)"
                            : "did not find expected node content (while parsing a flow node)", t->start);
}

/* Directives before a document; true if any were found. */
static int parse_directives(Y *y, Event *doc) {
    tagdirs_clear(y);
    bool have_version = false;
    for (;;) {
        Token *t = peek(y);
        if (!t) return 0;
        if (t->type == T_VERSION) {
            if (have_version) return y_error(y, "found a duplicate %YAML directive", t->start);
            if (t->major != 1) return y_error(y, "found an incompatible YAML version (only 1.x is supported)", t->start);
            have_version = true;
            doc->major = t->major;
            doc->minor = t->minor;
        } else if (t->type == T_TAG_DIRECTIVE) {
            for (size_t i = 0; i < y->ntagdirs; i++)
                if (strcmp(y->tagdirs[i].handle, t->a) == 0)
                    return y_error(y, "found a duplicate %TAG directive", t->start);
            if (y->ntagdirs == y->cap_tagdirs) {
                size_t nc = y->cap_tagdirs ? y->cap_tagdirs * 2 : 4;
                TagDir *nd = realloc(y->tagdirs, nc * sizeof *nd);
                if (!nd) return y_oom(y);
                y->tagdirs = nd;
                y->cap_tagdirs = nc;
            }
            y->tagdirs[y->ntagdirs].handle = t->a;
            y->tagdirs[y->ntagdirs].prefix = t->b;
            y->ntagdirs++;
            t->a = t->b = NULL;
        } else {
            break;
        }
        skip_token(y);
    }
    doc->tagdirs = y->tagdirs;
    doc->ntagdirs = y->ntagdirs;
    return 1;
}

static int parse_stream(Y *y) {
    Token *t = peek(y);
    if (!t) return 0;
    if (!simple_event(y, E_STREAM_START, t->start)) return 0;
    skip_token(y);
    bool open_ended = true;          /* the previous document ended with "..." (or none yet) */
    for (;;) {
        t = peek(y);
        if (!t) return 0;
        while (t->type == T_DOC_END) {         /* stray "..." */
            open_ended = true;
            skip_token(y);
            t = peek(y);
            if (!t) return 0;
        }
        if (t->type == T_STREAM_END) {
            int ok = simple_event(y, E_STREAM_END, t->start);
            skip_token(y);
            return ok;
        }
        Event doc;
        memset(&doc, 0, sizeof doc);
        doc.type = E_DOC_START;
        doc.start = t->start;
        bool explicit_start = false;
        if (t->type == T_VERSION || t->type == T_TAG_DIRECTIVE) {
            if (!open_ended) return y_error(y, "directives must follow a document end marker ('...')", t->start);
            if (!parse_directives(y, &doc)) return 0;
            t = peek(y);
            if (!t) return 0;
            if (t->type != T_DOC_START) return y_error(y, "did not find expected '---' after directives", t->start);
        } else {
            tagdirs_clear(y);
        }
        if (t->type == T_DOC_START) {
            explicit_start = true;
            doc.end = t->end;
            skip_token(y);
        } else if (!open_ended) {
            return y_error(y, "did not find expected '---' before the next document", t->start);
        }
        doc.implicit = !explicit_start;
        doc.tagdirs = y->tagdirs;
        doc.ntagdirs = y->ntagdirs;
        if (!emit_event(y, &doc)) return 0;

        t = peek(y);
        if (!t) return 0;
        if (explicit_start && (t->type == T_VERSION || t->type == T_TAG_DIRECTIVE || t->type == T_DOC_START ||
                               t->type == T_DOC_END || t->type == T_STREAM_END)) {
            if (!empty_scalar(y, t->start)) return 0;
        } else if (!parse_node(y, true, false)) {
            return 0;
        }
        t = peek(y);
        if (!t) return 0;
        Event end;
        memset(&end, 0, sizeof end);
        end.type = E_DOC_END;
        end.start = end.end = t->start;
        end.implicit = true;
        open_ended = false;
        if (t->type == T_DOC_END) {
            end.implicit = false;
            end.end = t->end;
            open_ended = true;
            skip_token(y);
        } else if (t->type != T_DOC_START && t->type != T_STREAM_END) {
            return y_error(y, "did not find expected end of document (more content after the root node)", t->start);
        }
        if (!emit_event(y, &end)) return 0;
    }
}

/* ---- Input checks and the run ---------------------------------------------- */

/* Reject invalid UTF-8 and non-printable characters up front. */
static int check_input(Y *y) {
    const unsigned char *s = y->src;
    size_t i = 0;
    int line = 0, col = 0;
    while (i < y->len) {
        uint32_t cp;
        int n = utf8_seq(s + i, y->len - i, &cp);
        if (!n || !printable_cp(cp)) {
            if (cp == 0xFEFF && n) { i += (size_t)n; col++; continue; }     /* BOM */
            Mark m = { i, line, col };
            char msg[96];
            if (!n) snprintf(msg, sizeof msg, "invalid UTF-8");
            else snprintf(msg, sizeof msg, "found a non-printable character (U+%04X)", (unsigned)cp);
            return y_error(y, msg, m);
        }
        if (cp == '\n' || (cp == '\r' && !(i + 1 < y->len && s[i + 1] == '\n'))) { line++; col = 0; }
        else if (cp != '\r') col++;
        i += (size_t)n;
    }
    return 1;
}

static void y_init(Y *y, const char *src, size_t len, int max_depth, EventFn fn, void *ctx) {
    memset(y, 0, sizeof *y);
    y->src = (const unsigned char *)src;
    y->len = len;
    y->max_depth = max_depth;
    y->on_event = fn;
    y->ctx = ctx;
}

static void y_free(Y *y) {
    for (size_t i = y->tok_head; i < y->tok_len; i++) token_free(&y->tok[i]);
    free(y->tok);
    free(y->indents);
    free(y->skeys);
    tagdirs_clear(y);
    free(y->tagdirs);
}

static int y_run(Y *y) {
    if (!check_input(y)) return 0;
    return parse_stream(y) && !y->failed;
}

/* "<fn>: <message> at line L, column C" */
static SeaValue y_error_value(SeaVM *vm, Y *y, const char *fn) {
    char msg[320];
    snprintf(msg, sizeof msg, "%s: %s at line %d, column %d", fn, y->err, y->err_mark.line + 1, y->err_mark.column + 1);
    return sea_make_error_str(vm, msg);
}

/* ======================================================================== */
/* Scalar resolution (schemas)                                              */
/* ======================================================================== */

enum { SCHEMA_CORE, SCHEMA_JSON, SCHEMA_FAILSAFE, SCHEMA_11 };

#define SEA_INT_LIM (UINT64_C(1) << 47)

static bool eq_any(const char *s, size_t n, const char *const *words) {
    for (; *words; words++)
        if (strlen(*words) == n && memcmp(*words, s, n) == 0) return true;
    return false;
}

static const char *const NULLS_CORE[]  = { "null", "Null", "NULL", "~", NULL };
static const char *const TRUES_CORE[]  = { "true", "True", "TRUE", NULL };
static const char *const FALSES_CORE[] = { "false", "False", "FALSE", NULL };
static const char *const TRUES_11[]    = { "true", "True", "TRUE", "yes", "Yes", "YES", "on", "On", "ON", NULL };
static const char *const FALSES_11[]   = { "false", "False", "FALSE", "no", "No", "NO", "off", "Off", "OFF", NULL };

/* Integer value of digits in base (with '_' skipped when allow_us): an int,
 * or a bigint / number past Sea's int range. false if not all digits. */
static bool make_int(const char *s, size_t n, int base, bool neg, bool allow_us, bool bigints, SeaValue *out) {
    if (n == 0) return false;
    uint64_t acc = 0;
    bool big = false, any = false;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '_' && allow_us) continue;
        int d = hexval(c);
        if (d < 0 || d >= base) return false;
        any = true;
        if (acc > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) big = true;
        else acc = acc * (uint64_t)base + (uint64_t)d;
    }
    if (!any) return false;
    if (!big) {
        if (!neg && acc < SEA_INT_LIM) { *out = sea_int_val((int64_t)acc); return true; }
        if (neg && acc <= SEA_INT_LIM) { *out = sea_int_val(-(int64_t)acc); return true; }
    }
    /* wide: exact through bigint */
    char *tmp = malloc(n + 2);
    if (!tmp) return false;
    size_t k = 0;
    if (neg) tmp[k++] = '-';
    for (size_t i = 0; i < n; i++) if (s[i] != '_') tmp[k++] = s[i];
    SeaBigInt *b = sea_bigint_parse(tmp, k, base);
    free(tmp);
    if (!b) return false;
    if (bigints) { *out = sea_obj_val(&b->obj); return true; }
    double d = sea_bigint_to_double(b);
    sea_obj_release(&b->obj);
    *out = sea_number_val(d);
    return true;
}

static bool make_float(const char *s, size_t n, bool allow_us, SeaValue *out) {
    char small[64];
    char *tmp = n < sizeof small ? small : malloc(n + 1);
    if (!tmp) return false;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) if (!(allow_us && s[i] == '_')) tmp[k++] = s[i];
    tmp[k] = '\0';
    char *end;
    double d = strtod(tmp, &end);
    bool ok = end == tmp + k && k > 0;
    if (tmp != small) free(tmp);
    if (ok) *out = sea_number_val(d);
    return ok;
}

static bool is_digits(const char *s, size_t n, bool allow_us) {
    if (!n) return false;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '9') any = true;
        else if (!(allow_us && s[i] == '_')) return false;
    }
    return any;
}

/* [-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?  (1.1: '_' allowed, '.' required) */
static bool float_syntax(const char *s, size_t n, bool yaml11) {
    size_t i = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) i++;
    size_t int_start = i;
    while (i < n && ((s[i] >= '0' && s[i] <= '9') || (yaml11 && s[i] == '_'))) i++;
    bool int_digits = i > int_start;
    bool dot = false, frac_digits = false;
    if (i < n && s[i] == '.') {
        dot = true;
        i++;
        size_t fs = i;
        while (i < n && ((s[i] >= '0' && s[i] <= '9') || (yaml11 && s[i] == '_'))) i++;
        frac_digits = i > fs;
    }
    if (!int_digits && !frac_digits) return false;
    if (yaml11 && !dot) return false;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '-' || s[i] == '+')) i++;
        else if (yaml11) return false;
        size_t es = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == es) return false;
    }
    return i == n;
}

static bool special_float(const char *s, size_t n, SeaValue *out) {
    size_t i = 0;
    bool neg = false;
    if (n && (s[0] == '-' || s[0] == '+')) { neg = s[0] == '-'; i = 1; }
    static const char *const infs[] = { ".inf", ".Inf", ".INF", NULL };
    static const char *const nans[] = { ".nan", ".NaN", ".NAN", NULL };
    if (eq_any(s + i, n - i, infs)) { *out = sea_number_val(neg ? -INFINITY : INFINITY); return true; }
    if (i == 0 && eq_any(s, n, nans)) { *out = sea_number_val(NAN); return true; }
    return false;
}

/* 1.1 sexagesimal: [-+]?[0-9][0-9_]*(:[0-5]?[0-9])+(\.[0-9_]*)? */
static bool sexagesimal(const char *s, size_t n, SeaValue *out) {
    size_t i = 0;
    bool neg = false;
    if (n && (s[0] == '-' || s[0] == '+')) { neg = s[0] == '-'; i = 1; }
    if (i >= n || s[i] < '0' || s[i] > '9') return false;
    double v = 0;
    bool any_colon = false, is_float = false;
    double part = 0;
    while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
        if (s[i] != '_') part = part * 10 + (s[i] - '0');
        i++;
    }
    v = part;
    while (i < n && s[i] == ':') {
        any_colon = true;
        i++;
        size_t ds = i;
        part = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { part = part * 10 + (s[i] - '0'); i++; }
        if (i - ds < 1 || i - ds > 2 || part > 59) return false;
        v = v * 60 + part;
    }
    if (!any_colon) return false;
    if (i < n && s[i] == '.') {
        is_float = true;
        i++;
        double scale = 0.1;
        while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            if (s[i] != '_') { v += (s[i] - '0') * scale; scale /= 10; }
            i++;
        }
    }
    if (i != n) return false;
    if (neg) v = -v;
    if (!is_float && fabs(v) < (double)SEA_INT_LIM) *out = sea_int_val((int64_t)v);
    else *out = sea_number_val(v);
    return true;
}

/* An integer in the schema's forms. */
static bool resolve_int(const char *s, size_t n, int schema, bool bigints, SeaValue *out) {
    if (n == 0) return false;
    if (schema == SCHEMA_JSON) {
        size_t i = s[0] == '-' ? 1 : 0;
        if (i >= n || !is_digits(s + i, n - i, false)) return false;
        if (s[i] == '0' && n - i > 1) return false;
        return make_int(s + i, n - i, 10, i == 1, false, bigints, out);
    }
    bool y11 = schema == SCHEMA_11;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    if (i >= n) return false;
    const char *d = s + i;
    size_t dn = n - i;
    if (dn > 2 && d[0] == '0' && d[1] == 'x') {
        if (!y11 && i) return false;                      /* core: 0x/0o unsigned */
        return make_int(d + 2, dn - 2, 16, neg, y11, bigints, out);
    }
    if (!y11 && dn > 2 && d[0] == '0' && d[1] == 'o') {
        if (i) return false;
        return make_int(d + 2, dn - 2, 8, neg, false, bigints, out);
    }
    if (y11 && dn > 2 && d[0] == '0' && d[1] == 'b') return make_int(d + 2, dn - 2, 2, neg, true, bigints, out);
    if (y11 && dn > 1 && d[0] == '0') return make_int(d + 1, dn - 1, 8, neg, true, bigints, out);
    if (!is_digits(d, dn, y11)) return false;
    if (y11 && d[0] == '_') return false;
    return make_int(d, dn, 10, neg, y11, bigints, out);
}

static bool resolve_float(const char *s, size_t n, int schema, SeaValue *out) {
    if (schema == SCHEMA_JSON) {
        size_t i = s[0] == '-' ? 1 : 0;
        if (i >= n || s[i] < '0' || s[i] > '9') return false;
        if (s[i] == '0' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9') return false;
        if (!float_syntax(s, n, false) || s[0] == '+') return false;
        return make_float(s, n, false, out);
    }
    if (special_float(s, n, out)) return true;
    if (!float_syntax(s, n, schema == SCHEMA_11)) return false;
    return make_float(s, n, schema == SCHEMA_11, out);
}

/* The value of a plain scalar without a tag. */
static SeaValue resolve_plain(const char *s, size_t n, int schema, bool bigints, bool *is_str) {
    SeaValue v;
    *is_str = false;
    if (schema == SCHEMA_FAILSAFE) { *is_str = true; return SEA_NIL_VAL; }
    if (schema == SCHEMA_JSON) {
        if (n == 0 || (n == 4 && memcmp(s, "null", 4) == 0)) return SEA_NIL_VAL;
        if (n == 4 && memcmp(s, "true", 4) == 0) return SEA_TRUE_VAL;
        if (n == 5 && memcmp(s, "false", 5) == 0) return SEA_FALSE_VAL;
    } else {
        if (n == 0 || eq_any(s, n, NULLS_CORE)) return SEA_NIL_VAL;
        if (eq_any(s, n, schema == SCHEMA_11 ? TRUES_11 : TRUES_CORE)) return SEA_TRUE_VAL;
        if (eq_any(s, n, schema == SCHEMA_11 ? FALSES_11 : FALSES_CORE)) return SEA_FALSE_VAL;
    }
    unsigned char c = (unsigned char)s[0];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
        if (resolve_int(s, n, schema, bigints, &v)) return v;
        if (resolve_float(s, n, schema, &v)) return v;
        if (schema == SCHEMA_11 && sexagesimal(s, n, &v)) return v;
    }
    *is_str = true;
    return SEA_NIL_VAL;
}

/* ======================================================================== */
/* Composer: events → Sea values                                            */
/* ======================================================================== */

typedef struct { char *name; SeaValue val; } Anchor;

enum { F_SEQ, F_MAP };

typedef struct {
    int       kind;
    SeaValue  container;          /* owned */
    SeaValue  key;                /* owned, when have_key */
    bool      have_key, key_merge;
    Mark      key_mark;
    char     *tag;                /* owned: applied when the collection ends */
    Mark      start;
    SeaValue *merges;             /* owned: maps to merge in at the end */
    size_t    nmerge, capmerge;
} Frame;

#define KEYCACHE_SIZE  512
#define KEYCACHE_PROBE 8

typedef struct {
    SeaVM    *vm;
    int       schema, doc_schema;
    bool      schema_given;
    bool      bigints, string_keys, dup_ok, merge, tagged, build, single;
    SeaValue  tags;               /* borrowed: tag → handler */
    Frame    *fr;
    size_t    nfr, capfr;
    Anchor   *anchors;
    size_t    nanch, capanch;
    SeaValue  doc;                /* the document being built (owned) */
    bool      have_doc;
    SeaArray *docs;               /* decode_all */
    int       ndocs;
    SeaString **keys;             /* key cache */
    bool      panicked;
} Comp;

static void anchors_clear(Comp *c) {
    for (size_t i = 0; i < c->nanch; i++) {
        free(c->anchors[i].name);
        val_release(c->anchors[i].val);
    }
    c->nanch = 0;
}

/* Define (or redefine: a later anchor of the same name wins) an anchor. */
static int anchor_set(Y *y, Comp *c, const char *name, SeaValue v) {
    for (size_t i = c->nanch; i-- > 0;) {
        if (strcmp(c->anchors[i].name, name) == 0) {
            if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
            val_release(c->anchors[i].val);
            c->anchors[i].val = v;
            return 1;
        }
    }
    if (c->nanch == c->capanch) {
        size_t nc = c->capanch ? c->capanch * 2 : 8;
        Anchor *na = realloc(c->anchors, nc * sizeof *na);
        if (!na) return y_oom(y);
        c->anchors = na;
        c->capanch = nc;
    }
    char *nm = xstrndup(name, strlen(name));
    if (!nm) return y_oom(y);
    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
    c->anchors[c->nanch++] = (Anchor){ nm, v };
    return 1;
}

static SeaString *comp_key(Comp *c, const char *data, size_t len) {
    if (!c->keys) c->keys = calloc(KEYCACHE_SIZE, sizeof *c->keys);
    uint32_t h = sea_string_hash(data, len);
    if (c->keys) {
        for (int i = 0; i < KEYCACHE_PROBE; i++) {
            SeaString **slot = &c->keys[(h + (uint32_t)i) & (KEYCACHE_SIZE - 1)];
            SeaString *s = *slot;
            if (s && s->len == len && memcmp(s->data, data, len) == 0) {
                sea_obj_retain(&s->obj);
                return s;
            }
            if (!s) {
                s = sea_string_find_intern(data, len);
                if (!s) s = sea_string_new(data, len);
                *slot = s;
                sea_obj_retain(&s->obj);
                return s;
            }
        }
    }
    SeaString *s = sea_string_find_intern(data, len);
    return s ? s : sea_string_new(data, len);
}

static bool key_is_scalar(SeaValue v) {
    return sea_is_string(v) || sea_is_int(v) || sea_is_number(v) || sea_is_bool(v) || sea_is_bigint(v);
}

static int describe_key(SeaValue k, char *out, size_t n) {
    if (sea_is_string(k)) return snprintf(out, n, "'%.60s'", SEA_AS_STRING(k)->data);
    if (sea_is_int(k)) return snprintf(out, n, "%lld", (long long)sea_as_int(k));
    if (sea_is_bool(k)) return snprintf(out, n, "%s", sea_as_bool(k) ? "true" : "false");
    if (sea_is_number(k)) return snprintf(out, n, "%g", sea_as_number(k));
    return snprintf(out, n, "(%s)", sea_type_name(k));
}

static void merge_into(SeaTable *dst, SeaTable *src) {
    uint32_t it = 0;
    SeaValue k, v, old;
    while (sea_table_next(src, &it, &k, &v))
        if (!sea_table_get(dst, k, &old)) sea_table_set(dst, k, v);
}

/* Add a finished value (owned) to the innermost open collection or the document. */
static int comp_add(Y *y, Comp *c, SeaValue v, Mark m) {
    if (c->nfr == 0) {
        val_release(c->doc);
        c->doc = v;
        c->have_doc = true;
        return 1;
    }
    Frame *f = &c->fr[c->nfr - 1];
    if (!c->build) { val_release(v); return 1; }
    if (f->kind == F_SEQ) {
        sea_array_push(SEA_AS_ARRAY(f->container), v);
        val_release(v);
        return 1;
    }
    if (!f->have_key) {
        if (!key_is_scalar(v) && !f->key_merge) {
            const char *what = sea_is_nil(v) ? "a null" : sea_is_array(v) ? "a sequence" : sea_is_table(v) ? "a mapping" : "this";
            char msg[160];
            snprintf(msg, sizeof msg, "%s mapping key is not supported (keys are strings, numbers or bools%s)",
                     what, sea_is_nil(v) ? "; quote it or use string_keys" : "");
            val_release(v);
            return y_error(y, msg, m);
        }
        if (sea_is_number(v) && isnan(sea_as_number(v))) {
            return y_error(y, "a NaN mapping key is not supported", m);
        }
        f->key = v;
        f->have_key = true;
        f->key_mark = m;
        return 1;
    }
    f->have_key = false;
    SeaTable *t = SEA_AS_TABLE(f->container);
    if (f->key_merge) {
        f->key_merge = false;
        val_release(f->key);
        f->key = SEA_NIL_VAL;
        /* "<<: *base" or "<<: [*a, *b]": merged at the end, explicit keys win */
        size_t add = 1;
        SeaArray *list = NULL;
        if (sea_is_array(v)) { list = SEA_AS_ARRAY(v); add = sea_array_len(list); }
        else if (!sea_is_table(v)) {
            val_release(v);
            return y_error(y, "a merge key ('<<') needs a mapping or a sequence of mappings", m);
        }
        if (f->nmerge + add > f->capmerge) {
            size_t nc = f->capmerge ? f->capmerge : 4;
            while (nc < f->nmerge + add) nc *= 2;
            SeaValue *nm = realloc(f->merges, nc * sizeof *nm);
            if (!nm) { val_release(v); return y_oom(y); }
            f->merges = nm;
            f->capmerge = nc;
        }
        if (list) {
            for (uint32_t i = 0; i < sea_array_len(list); i++) {
                SeaValue item = sea_array_get(list, (int64_t)i);
                if (!sea_is_table(item)) {
                    val_release(v);
                    return y_error(y, "a merge key ('<<') needs a mapping or a sequence of mappings", m);
                }
                sea_obj_retain(sea_as_obj(item));
                f->merges[f->nmerge++] = item;
            }
            val_release(v);
        } else {
            f->merges[f->nmerge++] = v;
        }
        return 1;
    }
    SeaValue old;
    if (!c->dup_ok && sea_table_get(t, f->key, &old)) {
        char k[96], msg[160];
        describe_key(f->key, k, sizeof k);
        snprintf(msg, sizeof msg, "duplicate mapping key %s", k);
        val_release(v);
        val_release(f->key);
        f->key = SEA_NIL_VAL;
        return y_error(y, msg, f->key_mark);
    }
    sea_table_set(t, f->key, v);
    val_release(f->key);
    val_release(v);
    f->key = SEA_NIL_VAL;
    return 1;
}

static int push_frame(Y *y, Comp *c, int kind, SeaValue container, const char *tag, Mark m) {
    if (c->nfr == c->capfr) {
        size_t nc = c->capfr ? c->capfr * 2 : 16;
        Frame *nf = realloc(c->fr, nc * sizeof *nf);
        if (!nf) { val_release(container); return y_oom(y); }
        c->fr = nf;
        c->capfr = nc;
    }
    Frame *f = &c->fr[c->nfr++];
    memset(f, 0, sizeof *f);
    f->kind = kind;
    f->container = container;
    f->key = SEA_NIL_VAL;
    f->start = m;
    if (tag) {
        f->tag = xstrndup(tag, strlen(tag));
        if (!f->tag) return y_oom(y);
    }
    return 1;
}

static void frame_free(Frame *f) {
    val_release(f->container);
    if (f->have_key || f->key_merge) val_release(f->key);
    for (size_t i = 0; i < f->nmerge; i++) val_release(f->merges[i]);
    free(f->merges);
    free(f->tag);
}

static int base64_decode(const char *s, size_t n, Buf *out) {
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else if (ch == '=') { pad++; continue; }
        else if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') continue;
        else return 0;
        if (pad) return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf_putc(out, (char)((acc >> bits) & 0xFF));
        }
    }
    return pad <= 2;
}

/* Call the handler for a custom tag, or wrap the value with tagged = true.
 * Takes v; *out gets the result (owned). 0 on a panic in the handler. */
static int apply_custom_tag(Y *y, Comp *c, const char *tag, SeaValue v, SeaValue *out, Mark m) {
    SeaValue fn = SEA_NIL_VAL;
    if (!sea_is_nil(c->tags)) {
        if (!opt_get(c->tags, tag, &fn)) opt_get(c->tags, "*", &fn);
    }
    if (!sea_is_nil(fn)) {
        SeaValue args[2] = { v, new_str(tag, strlen(tag)) };
        SeaValue r = sea_vm_call(c->vm, fn, args, 2);
        val_release(args[0]);
        val_release(args[1]);
        if (c->vm->panic_propagating) {
            val_release(r);
            c->panicked = true;
            return y_error(y, "a tag handler panicked", m);
        }
        *out = r;
        return 1;
    }
    if (c->tagged) {
        SeaTable *w = sea_table_new(TABLE_MAP);
        tset_str(w, "tag", new_str(tag, strlen(tag)));
        sea_table_set_str(w, "value", v);
        val_release(v);
        *out = sea_obj_val(&w->obj);
        return 1;
    }
    *out = v;
    return 1;
}

/* A scalar's value for its tag (or its resolution when untagged). */
static int scalar_value(Y *y, Comp *c, Event *e, SeaValue *out) {
    const char *s = e->value;
    size_t n = e->len;
    const char *tag = e->tag;
    int schema = c->doc_schema;
    if (!tag && e->style == S_PLAIN) {
        bool is_str;
        SeaValue v = resolve_plain(s, n, schema, c->bigints, &is_str);
        if (!is_str) { *out = v; return 1; }
        if (schema == SCHEMA_FAILSAFE && n == 0) { *out = sea_obj_val(&sea_string_new("", 0)->obj); return 1; }
        *out = new_str(s, n);
        return 1;
    }
    if (!tag || strcmp(tag, "!") == 0) { *out = new_str(s, n); return 1; }
    if (strncmp(tag, TAG_PREFIX, sizeof TAG_PREFIX - 1) == 0) {
        const char *t = tag + sizeof TAG_PREFIX - 1;
        int sch = schema == SCHEMA_11 ? SCHEMA_11 : SCHEMA_CORE;
        char msg[128];
        if (strcmp(t, "str") == 0) { *out = new_str(s, n); return 1; }
        if (strcmp(t, "null") == 0) {
            if (n == 0 || eq_any(s, n, NULLS_CORE)) { *out = SEA_NIL_VAL; return 1; }
        } else if (strcmp(t, "bool") == 0) {
            if (eq_any(s, n, sch == SCHEMA_11 ? TRUES_11 : TRUES_CORE)) { *out = SEA_TRUE_VAL; return 1; }
            if (eq_any(s, n, sch == SCHEMA_11 ? FALSES_11 : FALSES_CORE)) { *out = SEA_FALSE_VAL; return 1; }
        } else if (strcmp(t, "int") == 0) {
            if (resolve_int(s, n, sch, c->bigints, out)) return 1;
            if (sch == SCHEMA_11 && sexagesimal(s, n, out)) return 1;
        } else if (strcmp(t, "float") == 0) {
            if (resolve_float(s, n, sch, out)) return 1;
            SeaValue iv;
            if (resolve_int(s, n, sch, false, &iv)) {
                *out = sea_number_val(sea_is_int(iv) ? (double)sea_as_int(iv) : sea_as_number(iv));
                return 1;
            }
        } else if (strcmp(t, "binary") == 0) {
            Buf b = { 0 };
            if (base64_decode(s, n, &b) && !b.oom) {
                *out = new_str(b.data ? b.data : "", b.len);
                buf_free(&b);
                return 1;
            }
            buf_free(&b);
        } else if (strcmp(t, "timestamp") == 0 || strcmp(t, "merge") == 0 || strcmp(t, "value") == 0) {
            *out = new_str(s, n);
            return 1;
        } else if (strcmp(t, "seq") == 0 || strcmp(t, "map") == 0 || strcmp(t, "set") == 0 ||
                   strcmp(t, "omap") == 0 || strcmp(t, "pairs") == 0) {
            snprintf(msg, sizeof msg, "the tag !!%s cannot be used on a scalar", t);
            return y_error(y, msg, e->start);
        } else {
            return apply_custom_tag(y, c, tag, new_str(s, n), out, e->start);
        }
        snprintf(msg, sizeof msg, "invalid value for the tag !!%s: '%.40s'", t, s);
        return y_error(y, msg, e->start);
    }
    return apply_custom_tag(y, c, tag, new_str(s, n), out, e->start);
}

/* Is the mapping key at this point a merge key ("<<")? */
static bool merge_key(Comp *c, Event *e) {
    if (!c->merge || e->len != 2 || memcmp(e->value, "<<", 2) != 0) return false;
    if (e->tag) return strcmp(e->tag, TAG_PREFIX "merge") == 0;
    return e->style == S_PLAIN;
}

static int comp_event(Y *y, Event *e) {
    Comp *c = y->ctx;
    switch (e->type) {
        case E_STREAM_START:
        case E_STREAM_END:
            return 1;
        case E_DOC_START:
            if (c->single && c->ndocs == 1)
                return y_error(y, "the input has more than one document (use yaml.decode_all)", e->start);
            c->ndocs++;
            anchors_clear(c);
            c->doc_schema = c->schema;
            if (!c->schema_given && e->major == 1 && e->minor == 1) c->doc_schema = SCHEMA_11;
            return 1;
        case E_DOC_END:
            if (c->build && c->docs) {
                sea_array_push(c->docs, c->doc);
                val_release(c->doc);
                c->doc = SEA_NIL_VAL;
            }
            return 1;
        case E_ALIAS: {
            for (size_t i = c->nanch; i-- > 0;) {
                if (strcmp(c->anchors[i].name, e->anchor) == 0) {
                    SeaValue v = c->anchors[i].val;
                    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
                    if (c->nfr && c->fr[c->nfr - 1].kind == F_MAP && !c->fr[c->nfr - 1].have_key &&
                        c->string_keys && !sea_is_string(v) && key_is_scalar(v)) {
                        /* string_keys: an aliased scalar key as text */
                        SeaString *s = sea_vm_tostring(c->vm, v);
                        val_release(v);
                        v = sea_obj_val(&s->obj);
                    }
                    return comp_add(y, c, v, e->start);
                }
            }
            char msg[128];
            snprintf(msg, sizeof msg, "found undefined alias '%.60s'", e->anchor);
            return y_error(y, msg, e->start);
        }
        case E_SCALAR: {
            SeaValue v;
            Frame *f = c->nfr ? &c->fr[c->nfr - 1] : NULL;
            bool key_pos = f && f->kind == F_MAP && !f->have_key;
            if (key_pos && merge_key(c, e)) {
                f->key_merge = true;
                f->key = SEA_NIL_VAL;
                f->have_key = true;
                f->key_mark = e->start;
                if (e->anchor && !anchor_set(y, c, e->anchor, SEA_NIL_VAL)) return 0;
                return 1;
            }
            if (!c->build) {
                v = SEA_NIL_VAL;
            } else if (key_pos && c->string_keys && (!e->tag || strcmp(e->tag, TAG_PREFIX "str") == 0 ||
                                                      (e->style != S_PLAIN))) {
                v = sea_obj_val(&comp_key(c, e->value, e->len)->obj);
            } else {
                if (!scalar_value(y, c, e, &v)) return 0;
                if (key_pos && sea_is_string(v) && !e->tag) {
                    /* share key strings across the document */
                    SeaString *k = comp_key(c, e->value, e->len);
                    val_release(v);
                    v = sea_obj_val(&k->obj);
                } else if (key_pos && c->string_keys && !sea_is_string(v) && key_is_scalar(v)) {
                    SeaString *s = sea_vm_tostring(c->vm, v);
                    val_release(v);
                    v = sea_obj_val(&s->obj);
                }
            }
            if (e->anchor && !anchor_set(y, c, e->anchor, v)) { val_release(v); return 0; }
            return comp_add(y, c, v, e->start);
        }
        case E_SEQ_START:
        case E_MAP_START: {
            bool map = e->type == E_MAP_START;
            const char *tag = e->tag;
            if (tag && strncmp(tag, TAG_PREFIX, sizeof TAG_PREFIX - 1) == 0) {
                const char *t = tag + sizeof TAG_PREFIX - 1;
                bool ok_seq = !strcmp(t, "seq") || !strcmp(t, "omap") || !strcmp(t, "pairs");
                bool ok_map = !strcmp(t, "map") || !strcmp(t, "set");
                bool known = ok_seq || ok_map || !strcmp(t, "str") || !strcmp(t, "int") || !strcmp(t, "float") ||
                             !strcmp(t, "bool") || !strcmp(t, "null") || !strcmp(t, "binary") || !strcmp(t, "timestamp");
                if (known && (map ? !ok_map : !ok_seq)) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "the tag !!%s cannot be used on a %s", t, map ? "mapping" : "sequence");
                    return y_error(y, msg, e->start);
                }
            }
            SeaValue container = SEA_NIL_VAL;
            if (c->build) {
                container = map ? sea_obj_val(&sea_table_new(TABLE_MAP)->obj) : sea_obj_val(&sea_array_new(4)->obj);
                /* the anchor names the collection while it is built: "&a [*a]" is a cycle */
                if (e->anchor && !anchor_set(y, c, e->anchor, container)) { val_release(container); return 0; }
            }
            return push_frame(y, c, map ? F_MAP : F_SEQ, container, tag, e->start);
        }
        case E_SEQ_END:
        case E_MAP_END: {
            Frame f = c->fr[--c->nfr];
            SeaValue v = f.container;
            f.container = SEA_NIL_VAL;
            if (c->build && f.kind == F_MAP) {
                SeaTable *t = SEA_AS_TABLE(v);
                for (size_t i = 0; i < f.nmerge; i++)
                    if (sea_as_obj(f.merges[i]) != &t->obj) merge_into(t, SEA_AS_TABLE(f.merges[i]));
            }
            int ok = 1;
            if (c->build && f.tag) {
                if (strcmp(f.tag, TAG_PREFIX "set") == 0) {
                    /* !!set: {a, b} / ? a → keys with true */
                    SeaTable *t = SEA_AS_TABLE(v);
                    SeaTable *set = sea_table_new(TABLE_MAP);
                    uint32_t it = 0;
                    SeaValue k, val;
                    while (sea_table_next(t, &it, &k, &val)) sea_table_set(set, k, SEA_TRUE_VAL);
                    val_release(v);
                    v = sea_obj_val(&set->obj);
                } else if (strcmp(f.tag, "!") != 0 && strncmp(f.tag, TAG_PREFIX, sizeof TAG_PREFIX - 1) != 0) {
                    ok = apply_custom_tag(y, c, f.tag, v, &v, f.start);
                    if (!ok) v = SEA_NIL_VAL;
                }
            }
            frame_free(&f);
            if (!ok) return 0;
            return comp_add(y, c, v, f.start);
        }
    }
    return 1;
}

static void comp_free(Comp *c) {
    while (c->nfr) frame_free(&c->fr[--c->nfr]);
    free(c->fr);
    anchors_clear(c);
    free(c->anchors);
    val_release(c->doc);
    if (c->docs) sea_obj_release(&c->docs->obj);
    if (c->keys) {
        for (int i = 0; i < KEYCACHE_SIZE; i++)
            if (c->keys[i]) sea_obj_release(&c->keys[i]->obj);
        free(c->keys);
    }
}

static int max_depth_opt(SeaValue opts) {
    int d = opt_int(opts, "max_depth", YAML_MAX_DEPTH);
    if (d < 1) d = YAML_MAX_DEPTH;
    if (d > YAML_DEPTH_CAP) d = YAML_DEPTH_CAP;
    return d;
}

static SeaValue decode_impl(SeaVM *vm, int argc, SeaValue *args, bool all) {
    const char *fn = all ? "yaml.decode_all" : "yaml.decode";
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s: string expected", fn);
        return sea_make_error_str(vm, msg);
    }
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    Comp c;
    memset(&c, 0, sizeof c);
    c.vm = vm;
    c.build = true;
    c.single = !all;
    c.doc = SEA_NIL_VAL;
    c.schema = SCHEMA_CORE;
    SeaValue sv;
    if (opt_get(opts, "schema", &sv)) {
        c.schema_given = true;
        if (opt_is(opts, "schema", "core")) c.schema = SCHEMA_CORE;
        else if (opt_is(opts, "schema", "json")) c.schema = SCHEMA_JSON;
        else if (opt_is(opts, "schema", "failsafe")) c.schema = SCHEMA_FAILSAFE;
        else if (opt_is(opts, "schema", "1.1") || opt_is(opts, "schema", "yaml11")) c.schema = SCHEMA_11;
        else {
            sea_obj_release(&s->obj);
            char msg[128];
            snprintf(msg, sizeof msg, "%s: unknown schema (use \"core\", \"json\", \"failsafe\" or \"1.1\")", fn);
            return sea_make_error_str(vm, msg);
        }
    }
    c.bigints = opt_flag(opts, "bigint", 0);
    c.string_keys = opt_flag(opts, "string_keys", 0);
    c.dup_ok = opt_flag(opts, "duplicate_keys", 0);
    c.merge = opt_flag(opts, "merge", 1);
    c.tagged = opt_flag(opts, "tagged", 0);
    SeaValue tags;
    c.tags = opt_get(opts, "tags", &tags) ? tags : SEA_NIL_VAL;
    if (all) c.docs = sea_array_new(4);

    Y y;
    y_init(&y, s->data, s->len, max_depth_opt(opts), comp_event, &c);
    int ok = y_run(&y);
    SeaValue r;
    if (!ok) {
        r = c.panicked ? SEA_NIL_VAL : y_error_value(vm, &y, fn);
    } else if (all) {
        r = sea_obj_val(&c.docs->obj);
        c.docs = NULL;
    } else {
        r = c.doc;
        c.doc = SEA_NIL_VAL;
    }
    y_free(&y);
    comp_free(&c);
    sea_obj_release(&s->obj);
    return r;
}

static SeaValue fn_decode(SeaVM *vm, int argc, SeaValue *args) { return decode_impl(vm, argc, args, false); }
static SeaValue fn_decode_all(SeaVM *vm, int argc, SeaValue *args) { return decode_impl(vm, argc, args, true); }

static int null_event(Y *y, Event *e) { (void)y; (void)e; return 1; }

static SeaValue fn_valid(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return SEA_FALSE_VAL;
    /* anchors and aliases are checked too: compose without building values */
    Comp c;
    memset(&c, 0, sizeof c);
    c.vm = vm;
    c.doc = SEA_NIL_VAL;
    c.merge = true;
    Y y;
    y_init(&y, s->data, s->len, YAML_MAX_DEPTH, comp_event, &c);
    int ok = y_run(&y);
    y_free(&y);
    comp_free(&c);
    sea_obj_release(&s->obj);
    (void)null_event;
    return sea_bool_val(ok);
}

/* ---- yaml.events -------------------------------------------------------------- */

static const char *const EVENT_NAMES[] = {
    "stream_start", "stream_end", "document_start", "document_end", "alias", "scalar",
    "sequence_start", "sequence_end", "mapping_start", "mapping_end"
};
static const char *const STYLE_NAMES[] = { "any", "plain", "single", "double", "literal", "folded" };

typedef struct { SeaArray *out; } EvOut;

static int ev_event(Y *y, Event *e) {
    EvOut *o = y->ctx;
    SeaTable *t = sea_table_new(TABLE_MAP);
    tset_str(t, "type", new_str(EVENT_NAMES[e->type], strlen(EVENT_NAMES[e->type])));
    tset_str(t, "line", sea_int_val(e->start.line + 1));
    tset_str(t, "column", sea_int_val(e->start.column + 1));
    if (e->anchor) tset_str(t, e->type == E_ALIAS ? "alias" : "anchor", new_str(e->anchor, strlen(e->anchor)));
    if (e->tag) tset_str(t, "tag", new_str(e->tag, strlen(e->tag)));
    switch (e->type) {
        case E_SCALAR:
            tset_str(t, "value", new_str(e->value, e->len));
            tset_str(t, "style", new_str(STYLE_NAMES[e->style], strlen(STYLE_NAMES[e->style])));
            break;
        case E_SEQ_START:
        case E_MAP_START:
            tset_str(t, "style", new_str(e->style ? "flow" : "block", e->style ? 4 : 5));
            break;
        case E_DOC_START:
            tset_str(t, "implicit", sea_bool_val(e->implicit));
            if (e->major) {
                char v[32];
                int n = snprintf(v, sizeof v, "%d.%d", e->major, e->minor);
                tset_str(t, "version", new_str(v, (size_t)n));
            }
            if (e->ntagdirs) {
                SeaTable *td = sea_table_new(TABLE_MAP);
                for (size_t i = 0; i < e->ntagdirs; i++)
                    tset_str(td, e->tagdirs[i].handle, new_str(e->tagdirs[i].prefix, strlen(e->tagdirs[i].prefix)));
                tset_str(t, "tags", sea_obj_val(&td->obj));
            }
            break;
        case E_DOC_END:
            tset_str(t, "implicit", sea_bool_val(e->implicit));
            break;
        default:
            break;
    }
    sea_array_push(o->out, sea_obj_val(&t->obj));
    sea_obj_release(&t->obj);
    return 1;
}

static SeaValue fn_events(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return sea_make_error_str(vm, "yaml.events: string expected");
    EvOut o = { sea_array_new(16) };
    Y y;
    y_init(&y, s->data, s->len, max_depth_opt(argc > 1 ? args[1] : SEA_NIL_VAL), ev_event, &o);
    int ok = y_run(&y);
    SeaValue r;
    if (ok) {
        r = sea_obj_val(&o.out->obj);
    } else {
        r = y_error_value(vm, &y, "yaml.events");
        sea_obj_release(&o.out->obj);
    }
    y_free(&y);
    sea_obj_release(&s->obj);
    return r;
}

/* ======================================================================== */
/* Writer: node tree → text                                                 */
/* ======================================================================== */

enum { N_SCALAR, N_SEQ, N_MAP, N_ALIAS };
enum { C_ANY = -1, C_BLOCK = 0, C_FLOW = 1 };

typedef struct YN YN;
struct YN {
    int    kind;
    int    style;            /* scalar: S_*; collection: C_* */
    char  *tag, *anchor;     /* alias: anchor is the name */
    char  *val;
    size_t len;
    bool   str_value;        /* encode: a Sea string (plain only if it reads back as a string) */
    YN   **kids;             /* map: key, value, key, value, ... */
    size_t n, cap;
};

static void yn_free(YN *n) {
    if (!n) return;
    for (size_t i = 0; i < n->n; i++) yn_free(n->kids[i]);
    free(n->kids);
    free(n->tag);
    free(n->anchor);
    free(n->val);
    free(n);
}

static YN *yn_new(int kind, int style) {
    YN *n = calloc(1, sizeof *n);
    if (n) { n->kind = kind; n->style = style; }
    return n;
}

static YN *yn_scalar(const char *s, size_t len, int style, bool str_value) {
    YN *n = yn_new(N_SCALAR, style);
    if (!n) return NULL;
    n->val = xstrndup(s, len);
    if (!n->val) { free(n); return NULL; }
    n->len = len;
    n->str_value = str_value;
    return n;
}

static int yn_add(YN *p, YN *k) {
    if (!k) return 0;
    if (p->n == p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 4;
        YN **nk = realloc(p->kids, nc * sizeof *nk);
        if (!nk) { yn_free(k); return 0; }
        p->kids = nk;
        p->cap = nc;
    }
    p->kids[p->n++] = k;
    return 1;
}

typedef struct {
    YN     *root;
    bool    explicit_start, explicit_end;
    int     major, minor;
    TagDir *tagdirs;          /* owned */
    size_t  ntagdirs;
} Doc;

typedef struct {
    Buf     b;
    int     step;             /* indentation */
    bool    flow_all;
    TagDir *tagdirs;          /* the current document's %TAG directives (borrowed) */
    size_t  ntagdirs;
} Em;

/* ---- Scalar analysis ---------------------------------------------------------- */

typedef struct {
    bool valid;               /* UTF-8, all printable (tab and \n allowed) */
    bool multiline;           /* contains \n */
    bool has_cr;              /* \r or other line-break characters (U+0085, U+2028, U+2029) */
    bool has_tab;
    bool lead_space, trail_space;
    bool only_breaks;         /* non-empty, nothing but \n */
} Scan;

static Scan analyze(const char *s, size_t n) {
    Scan a = { true, false, false, false, false, false, n > 0 };
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_seq(u + i, n - i, &cp);
        if (!k || !printable_cp(cp) || cp == 0xFEFF) { a.valid = false; k = k ? k : 1; cp = 0; }
        if (cp == '\n') a.multiline = true;
        else a.only_breaks = false;
        if (cp == '\r' || cp == 0x85 || cp == 0x2028 || cp == 0x2029) a.has_cr = true;
        if (cp == '\t') a.has_tab = true;
        i += (size_t)k;
    }
    if (n) {
        a.lead_space = s[0] == ' ' || s[0] == '\t';
        a.trail_space = s[n - 1] == ' ' || s[n - 1] == '\t';
    }
    return a;
}

/* Can s be written as a plain scalar here (syntax only)? */
static bool plain_syntax_ok(const char *s, size_t n, bool flow) {
    if (n == 0) return false;
    unsigned char c0 = (unsigned char)s[0];
    if (c0 == ' ' || c0 == '\t' || s[n - 1] == ' ' || s[n - 1] == '\t') return false;
    if (strchr(",[]{}#&*!|>'\"%@`", c0)) return false;
    if (c0 == '-' || c0 == '?' || c0 == ':') {
        if (n == 1) return false;
        unsigned char c1 = (unsigned char)s[1];
        if (c1 == ' ' || c1 == '\t' || (flow && IS_FLOW_IND(c1))) return false;
    }
    if (n >= 3 && (memcmp(s, "---", 3) == 0 || memcmp(s, "...", 3) == 0)) return false;
    if (s[n - 1] == ':') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r' || c == '\t') return false;
        if (c == ':' && i + 1 < n && (s[i + 1] == ' ' || (flow && IS_FLOW_IND((unsigned char)s[i + 1])))) return false;
        if (c == '#' && i > 0 && s[i - 1] == ' ') return false;
        if (flow && IS_FLOW_IND(c)) return false;
    }
    return true;
}

/* Would this plain text read back as something other than a string (under
 * the core schema or YAML 1.1, which many readers still use)? */
static bool plain_is_ambiguous(const char *s, size_t n) {
    static const char *const extra[] = { "y", "Y", "n", "N", "=", "<<", NULL };
    if (eq_any(s, n, extra)) return true;
    /* YAML 1.1 timestamps: 2001-12-14, 2001-12-14 21:59:43.10 -5 */
    if (n >= 8 && s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9' &&
        s[3] >= '0' && s[3] <= '9' && s[4] == '-' && s[5] >= '0' && s[5] <= '9')
        return true;
    for (int sch = 0; sch < 2; sch++) {
        bool is_str;
        SeaValue v = resolve_plain(s, n, sch == 0 ? SCHEMA_CORE : SCHEMA_11, false, &is_str);
        val_release(v);
        if (!is_str) return true;
    }
    return false;
}

static int choose_style(YN *n, bool flow, bool key) {
    Scan a = analyze(n->val, n->len);
    int want = n->style;
    if (n->len == 0 && want == S_PLAIN && !n->str_value) return S_PLAIN;      /* empty (null) */
    if (want == S_ANY || want == S_PLAIN) {
        if (a.valid && plain_syntax_ok(n->val, n->len, flow) && !(n->str_value && plain_is_ambiguous(n->val, n->len)))
            return S_PLAIN;
    }
    bool block_ok = !flow && !key && a.valid && !a.has_cr && n->len > 0;
    if ((want == S_LITERAL || want == S_FOLDED) && block_ok) return want;
    if (want == S_ANY && a.multiline && block_ok) return S_LITERAL;
    if ((want == S_ANY || want == S_PLAIN || want == S_SINGLE || want == S_LITERAL || want == S_FOLDED) &&
        a.valid && !a.multiline && !a.has_cr)
        return S_SINGLE;
    return S_DOUBLE;
}

/* ---- Writing scalars ------------------------------------------------------------ */

static void em_double(Em *e, const char *s, size_t n) {
    Buf *b = &e->b;
    const unsigned char *u = (const unsigned char *)s;
    buf_putc(b, '"');
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_seq(u + i, n - i, &cp);
        if (!k) { buf_puts(b, "\\uFFFD"); i++; continue; }
        char t[16];
        switch (cp) {
            case '"':  buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\0': buf_puts(b, "\\0"); break;
            case '\a': buf_puts(b, "\\a"); break;
            case '\b': buf_puts(b, "\\b"); break;
            case '\t': buf_puts(b, "\\t"); break;
            case '\n': buf_puts(b, "\\n"); break;
            case '\v': buf_puts(b, "\\v"); break;
            case '\f': buf_puts(b, "\\f"); break;
            case '\r': buf_puts(b, "\\r"); break;
            case 0x1B: buf_puts(b, "\\e"); break;
            case 0x85: buf_puts(b, "\\N"); break;
            case 0xA0: buf_puts(b, "\\_"); break;
            case 0x2028: buf_puts(b, "\\L"); break;
            case 0x2029: buf_puts(b, "\\P"); break;
            default:
                if (!printable_cp(cp) || cp == 0xFEFF) {
                    if (cp <= 0xFF) snprintf(t, sizeof t, "\\x%02X", (unsigned)cp);
                    else if (cp <= 0xFFFF) snprintf(t, sizeof t, "\\u%04X", (unsigned)cp);
                    else snprintf(t, sizeof t, "\\U%08X", (unsigned)cp);
                    buf_puts(b, t);
                } else {
                    buf_push(b, s + i, (size_t)k);
                }
        }
        i += (size_t)k;
    }
    buf_putc(b, '"');
}

static void em_single(Em *e, const char *s, size_t n) {
    buf_putc(&e->b, '\'');
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') buf_putc(&e->b, '\'');
        buf_putc(&e->b, s[i]);
    }
    buf_putc(&e->b, '\'');
}

/* A literal or folded block scalar: the header at the cursor, then the
 * content lines at column ci; parent is the enclosing block's column (-1 at
 * the top) for the indentation indicator. */
static void em_block_scalar(Em *e, const char *s, size_t n, int style, int ci, int parent) {
    Buf *b = &e->b;
    size_t trail = 0;
    while (trail < n && s[n - 1 - trail] == '\n') trail++;
    size_t body = n - trail;
    size_t first = 0;
    while (first < body && s[first] == '\n') first++;
    buf_putc(b, style == S_LITERAL ? '|' : '>');
    if (first < body && s[first] == ' ') buf_putc(b, (char)('0' + (parent < 0 ? ci : ci - parent)));
    if (trail == 0) buf_putc(b, '-');
    else if (trail > 1 || body == 0) buf_putc(b, '+');
    buf_putc(b, '\n');
    if (body == 0) {
        for (size_t m = 0; m < trail; m++) buf_putc(b, '\n');
        return;
    }
    /* one output line per line of the value; folded style doubles a break
     * between two text lines that would otherwise fold into a space */
    size_t i = 0;
    int pending = 0;
    bool have_prev = false, prev_blank = false;
    for (;;) {
        size_t j = i;
        while (j < body && s[j] != '\n') j++;
        if (j > i) {
            bool blank = s[i] == ' ' || s[i] == '\t';
            if (style == S_FOLDED && have_prev && !prev_blank && !blank) pending++;
            while (pending > 0) { buf_putc(b, '\n'); pending--; }
            buf_spaces(b, ci);
            buf_push(b, s + i, j - i);
            buf_putc(b, '\n');
            have_prev = true;
            prev_blank = blank;
        } else {
            pending++;
        }
        if (j >= body) break;
        i = j + 1;
    }
    for (size_t m = 1; m < trail; m++) buf_putc(b, '\n');
}

static void em_scalar(Em *e, YN *n, bool flow, bool key, int ci, int parent) {
    int st = choose_style(n, flow, key);
    switch (st) {
        case S_PLAIN:   buf_push(&e->b, n->val, n->len); break;
        case S_SINGLE:  em_single(e, n->val, n->len); break;
        case S_DOUBLE:  em_double(e, n->val, n->len); break;
        default:        em_block_scalar(e, n->val, n->len, st, ci, parent); break;
    }
}

static bool is_block_scalar(YN *n, bool flow, bool key) {
    if (n->kind != N_SCALAR) return false;
    int st = choose_style(n, flow, key);
    return st == S_LITERAL || st == S_FOLDED;
}

/* ---- Tags and properties --------------------------------------------------------- */

static bool tag_char_ok(unsigned char c) {
    return is_alnum_ch(c) || (c && strchr(";/?:@&=+$.~*'()#", c));
}

static void em_tag_suffix(Buf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (tag_char_ok(c)) buf_putc(b, (char)c);
        else {
            char t[4];
            snprintf(t, sizeof t, "%%%02X", c);
            buf_puts(b, t);
        }
    }
}

static void em_tag(Em *e, const char *tag) {
    Buf *b = &e->b;
    if (strcmp(tag, "!") == 0) { buf_putc(b, '!'); return; }
    size_t best = 0;
    const char *handle = NULL;
    for (size_t i = 0; i < e->ntagdirs; i++) {
        size_t pl = strlen(e->tagdirs[i].prefix);
        if (pl > best && strncmp(tag, e->tagdirs[i].prefix, pl) == 0 && tag[pl]) {
            best = pl;
            handle = e->tagdirs[i].handle;
        }
    }
    if (handle) {
        buf_puts(b, handle);
        em_tag_suffix(b, tag + best);
    } else if (strncmp(tag, TAG_PREFIX, sizeof TAG_PREFIX - 1) == 0 && tag[sizeof TAG_PREFIX - 1]) {
        buf_puts(b, "!!");
        em_tag_suffix(b, tag + sizeof TAG_PREFIX - 1);
    } else if (tag[0] == '!' && tag[1]) {
        buf_putc(b, '!');
        em_tag_suffix(b, tag + 1);
    } else {
        buf_puts(b, "!<");
        for (const char *s = tag; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (tag_char_ok(c) || c == '!' || c == ',' || c == '[' || c == ']') buf_putc(b, (char)c);
            else {
                char t[4];
                snprintf(t, sizeof t, "%%%02X", c);
                buf_puts(b, t);
            }
        }
        buf_putc(b, '>');
    }
}

/* "&anchor !tag" (whichever exist); true if anything was written. */
static bool em_props(Em *e, YN *n) {
    bool any = false;
    if (n->anchor && n->kind != N_ALIAS) {
        buf_putc(&e->b, '&');
        buf_puts(&e->b, n->anchor);
        any = true;
    }
    if (n->tag) {
        if (any) buf_putc(&e->b, ' ');
        em_tag(e, n->tag);
        any = true;
    }
    return any;
}

/* ---- Flow style -------------------------------------------------------------------- */

static bool is_empty_plain(YN *n) {
    return n->kind == N_SCALAR && n->len == 0 && n->style == S_PLAIN && !n->str_value;
}

static void em_flow(Em *e, YN *n, bool key, bool in_seq) {
    Buf *b = &e->b;
    if (n->kind == N_ALIAS) {
        buf_putc(b, '*');
        buf_puts(b, n->anchor);
        return;
    }
    bool props = em_props(e, n);
    if (n->kind == N_SCALAR) {
        if (is_empty_plain(n)) {
            if (!props && in_seq) buf_puts(b, "null");      /* "[a, , b]" is not YAML; "{a: }" is */
            else if (props) buf_putc(b, ' ');              /* "[!!str ]": libyaml wants a space after a tag */
            return;
        }
        if (props) buf_putc(b, ' ');
        em_scalar(e, n, true, key, 0, 0);
        return;
    }
    if (props) buf_putc(b, ' ');
    if (n->kind == N_SEQ) {
        buf_putc(b, '[');
        for (size_t i = 0; i < n->n; i++) {
            if (i) buf_puts(b, ", ");
            em_flow(e, n->kids[i], false, true);
        }
        buf_putc(b, ']');
        return;
    }
    buf_putc(b, '{');
    for (size_t i = 0; i + 1 < n->n; i += 2) {
        if (i) buf_puts(b, ", ");
        YN *k = n->kids[i], *v = n->kids[i + 1];
        em_flow(e, k, true, false);
        if (k->kind == N_ALIAS) buf_putc(b, ' ');
        buf_putc(b, ':');
        if (!(is_empty_plain(v) && !v->anchor && !v->tag)) buf_putc(b, ' ');
        em_flow(e, v, false, false);
    }
    buf_putc(b, '}');
}

/* ---- Block style ------------------------------------------------------------------- */

enum { X_ROOT, X_COMPACT, X_VALUE };

static bool is_block_collection(Em *e, YN *n) {
    return (n->kind == N_SEQ || n->kind == N_MAP) && n->n > 0 && n->style != C_FLOW && !e->flow_all;
}

/* Is the buffer at the start of a line? */
static bool at_line_start(Em *e) { return e->b.len == 0 || e->b.data[e->b.len - 1] == '\n'; }

static void em_block(Em *e, YN *n, int ctx, int col);

/* A mapping key fits on one line as an implicit key. */
static bool simple_key(Em *e, YN *k) {
    if (k->kind == N_ALIAS) return true;
    if (k->kind == N_SCALAR) {
        int st = choose_style(k, false, true);
        return (st == S_PLAIN || st == S_SINGLE || st == S_DOUBLE) && k->len <= 512 && !is_empty_plain(k);
    }
    if (is_block_collection(e, k)) return false;
    Em t = { { 0 }, e->step, true, e->tagdirs, e->ntagdirs };
    em_flow(&t, k, true, false);
    bool ok = t.b.len <= 512 && !t.b.oom;
    buf_free(&t.b);
    return ok;
}

/* Write node n at the cursor. ctx says where the cursor is:
 *   X_ROOT:    the start of a line (the document root); entries at column 0
 *   X_COMPACT: after "- ", "? " or ": " whose indicator is at col - 2
 *   X_VALUE:   right after "key:" of a block mapping whose entries are at col */
static void em_block(Em *e, YN *n, int ctx, int col) {
    Buf *b = &e->b;
    int cc = ctx == X_ROOT ? 0 : ctx == X_COMPACT ? col : col + e->step;      /* where nested entries go */
    int parent = ctx == X_ROOT ? -1 : ctx == X_COMPACT ? col - 2 : col;

    if (n->kind == N_ALIAS || !is_block_collection(e, n)) {
        bool block_sc = n->kind == N_SCALAR && is_block_scalar(n, e->flow_all, false);
        if (ctx == X_VALUE) {
            if (is_empty_plain(n) && !n->anchor && !n->tag) return;          /* "key:" */
            buf_putc(b, ' ');
        }
        if (n->kind == N_ALIAS) { buf_putc(b, '*'); buf_puts(b, n->anchor); return; }
        if (n->kind != N_SCALAR) { em_flow(e, n, false, false); return; }
        bool props = em_props(e, n);
        if (is_empty_plain(n)) return;
        if (props) buf_putc(b, ' ');
        if (block_sc) {
            int ci = ctx == X_ROOT ? (e->step > 8 ? 8 : e->step) : ctx == X_COMPACT ? col : col + e->step;
            em_block_scalar(e, n->val, n->len, choose_style(n, false, false), ci, parent);
            /* the block scalar ends with its own line break: the next write must not add one */
            return;
        }
        em_scalar(e, n, e->flow_all, false, 0, parent);
        return;
    }

    /* a non-empty block collection */
    bool props = false;
    if (n->anchor || n->tag) {
        if (ctx == X_VALUE) buf_putc(b, ' ');
        props = em_props(e, n);
    }
    bool inline_first = ctx == X_COMPACT && !props;
    if (ctx == X_ROOT && !props && at_line_start(e)) inline_first = true;

    if (n->kind == N_SEQ) {
        for (size_t i = 0; i < n->n; i++) {
            if (!(i == 0 && inline_first)) {
                if (!at_line_start(e)) buf_putc(b, '\n');
                buf_spaces(b, cc);
            }
            YN *item = n->kids[i];
            if (is_empty_plain(item) && !item->anchor && !item->tag) { buf_putc(b, '-'); continue; }
            buf_puts(b, "- ");
            em_block(e, item, X_COMPACT, cc + 2);
        }
        return;
    }
    for (size_t i = 0; i + 1 < n->n; i += 2) {
        if (!(i == 0 && inline_first)) {
            if (!at_line_start(e)) buf_putc(b, '\n');
            buf_spaces(b, cc);
        }
        YN *k = n->kids[i], *v = n->kids[i + 1];
        if (simple_key(e, k)) {
            if (k->kind == N_SCALAR) {
                bool kp = em_props(e, k);
                if (kp) buf_putc(b, ' ');
                em_scalar(e, k, false, true, 0, cc);
            } else {
                em_flow(e, k, true, false);
            }
            if (k->kind == N_ALIAS) buf_putc(b, ' ');
            buf_putc(b, ':');
            em_block(e, v, X_VALUE, cc);
        } else {
            buf_puts(b, "? ");
            em_block(e, k, X_COMPACT, cc + 2);
            if (!at_line_start(e)) buf_putc(b, '\n');
            buf_spaces(b, cc);
            buf_putc(b, ':');
            if (is_empty_plain(v) && !v->anchor && !v->tag) continue;
            buf_putc(b, ' ');
            em_block(e, v, X_COMPACT, cc + 2);
        }
    }
}

static void em_document(Em *e, Doc *d, bool first, bool prev_open) {
    Buf *b = &e->b;
    e->tagdirs = d->tagdirs;
    e->ntagdirs = d->ntagdirs;
    bool directives = d->major || d->ntagdirs;
    if (directives && !first && !prev_open) buf_puts(b, "...\n");
    if (d->major) {
        char v[40];
        snprintf(v, sizeof v, "%%YAML %d.%d\n", d->major, d->minor);
        buf_puts(b, v);
    }
    for (size_t i = 0; i < d->ntagdirs; i++) {
        buf_puts(b, "%TAG ");
        buf_puts(b, d->tagdirs[i].handle);
        buf_putc(b, ' ');
        buf_puts(b, d->tagdirs[i].prefix);
        buf_putc(b, '\n');
    }
    YN *r = d->root;
    bool empty = !r || (is_empty_plain(r) && !r->anchor && !r->tag);
    bool explicit_start = d->explicit_start || !first || directives || empty;
    if (explicit_start) {
        buf_puts(b, "---");
        if (!empty) {
            if (is_block_collection(e, r) && !r->anchor && !r->tag) buf_putc(b, '\n');
            else buf_putc(b, ' ');
        }
    }
    if (!empty) em_block(e, r, X_ROOT, 0);
    if (!at_line_start(e)) buf_putc(b, '\n');
    if (d->explicit_end) buf_puts(b, "...\n");
}

static void em_stream(Em *e, Doc *docs, size_t n) {
    bool prev_open = false;
    for (size_t i = 0; i < n; i++) {
        em_document(e, &docs[i], i == 0, prev_open);
        prev_open = docs[i].explicit_end;
    }
}

static void docs_free(Doc *docs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        yn_free(docs[i].root);
        for (size_t j = 0; j < docs[i].ntagdirs; j++) {
            free(docs[i].tagdirs[j].handle);
            free(docs[i].tagdirs[j].prefix);
        }
        free(docs[i].tagdirs);
    }
    free(docs);
}

static int em_step_opt(SeaValue opts) {
    int s = opt_int(opts, "indent", 2);
    if (s < 1) s = 1;
    if (s > 9) s = 9;
    return s;
}

/* ======================================================================== */
/* Encoder: Sea values → node tree                                          */
/* ======================================================================== */

typedef struct { SeaObject *obj; int count, id; bool emitted; } Seen;

typedef struct {
    SeaVM      *vm;
    int         max_depth, depth;
    bool        sort_keys, anchors;
    SeaObject **open;          /* containers on the path (cycle check without anchors) */
    Seen       *seen;          /* anchors: pointer → count (open addressing) */
    size_t      seen_cap, seen_n;
    int         next_id;
    const char *err;
    char        errbuf[128];
} Enc;

static int fmt_int(char *out, int64_t v) { return snprintf(out, 24, "%lld", (long long)v); }

/* Shortest round-trip form; an integral value keeps ".0" so it reads back as a float. */
static int fmt_double(char *out, double d) {
    if (isnan(d)) return snprintf(out, 8, ".nan");
    if (isinf(d)) return snprintf(out, 8, d < 0 ? "-.inf" : ".inf");
    int n = snprintf(out, 32, "%.15g", d);
    if (strtod(out, NULL) != d) {
        n = snprintf(out, 32, "%.16g", d);
        if (strtod(out, NULL) != d) n = snprintf(out, 32, "%.17g", d);
    }
    char *e = strpbrk(out, "eE");
    if (!strpbrk(out, ".n")) {
        if (e) {                                      /* "1e+20" → "1.0e+20": YAML 1.1 readers need the dot */
            memmove(e + 2, e, (size_t)(out + n - e) + 1);
            e[0] = '.';
            e[1] = '0';
            n += 2;
        } else {
            out[n++] = '.'; out[n++] = '0'; out[n] = '\0';
        }
    }
    return n;
}

static bool encodable(SeaValue v) {
    if (sea_is_nil(v) || sea_is_bool(v) || sea_is_int(v) || sea_is_number(v) || sea_is_bigint(v)) return true;
    return sea_is_string(v) || sea_is_rope(v) || sea_is_array(v) || sea_is_table(v) ||
           sea_is_map(v) || sea_is_struct(v) || sea_is_int_array(v) || sea_is_float_array(v);
}

static bool is_container(SeaValue v) {
    return sea_is_array(v) || sea_is_table(v) || sea_is_map(v) || sea_is_struct(v) ||
           sea_is_int_array(v) || sea_is_float_array(v);
}

/* A table whose keys are exactly 0..n-1 ({a, b, c}) is a sequence. */
static bool is_array_table(SeaTable *t) {
    uint32_t count = sea_table_count(t);
    if (count == 0) return false;
    uint32_t iter = 0;
    SeaValue k, v;
    while (sea_table_next(t, &iter, &k, &v)) {
        if (!sea_is_int(k)) return false;
        int64_t i = sea_as_int(k);
        if (i < 0 || (uint64_t)i >= count) return false;
    }
    return true;
}

static Seen *seen_find(Enc *e, SeaObject *o, bool insert) {
    if (insert && (e->seen_n + 1) * 2 > e->seen_cap) {
        size_t nc = e->seen_cap ? e->seen_cap * 2 : 64;
        Seen *ns = calloc(nc, sizeof *ns);
        if (!ns) return NULL;
        for (size_t i = 0; i < e->seen_cap; i++) {
            if (!e->seen[i].obj) continue;
            size_t h = ((uintptr_t)e->seen[i].obj >> 4) & (nc - 1);
            while (ns[h].obj) h = (h + 1) & (nc - 1);
            ns[h] = e->seen[i];
        }
        free(e->seen);
        e->seen = ns;
        e->seen_cap = nc;
    }
    if (!e->seen_cap) return NULL;
    size_t h = ((uintptr_t)o >> 4) & (e->seen_cap - 1);
    while (e->seen[h].obj) {
        if (e->seen[h].obj == o) return &e->seen[h];
        h = (h + 1) & (e->seen_cap - 1);
    }
    if (!insert) return NULL;
    e->seen[h].obj = o;
    e->seen_n++;
    return &e->seen[h];
}

/* One mapping entry for the encoder: the key's node and the value. */
typedef struct { YN *key; SeaValue val; } Member;

static int member_cmp(const void *a, const void *b) {
    const YN *x = ((const Member *)a)->key, *y = ((const Member *)b)->key;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->val, y->val, n);
    if (c) return c;
    return (x->len > y->len) - (x->len < y->len);
}

/* Iterate the elements of an array-like value (array, typed array, 0..n-1 table). */
static uint32_t seq_len(SeaValue v) {
    if (sea_is_array(v)) return sea_array_len(SEA_AS_ARRAY(v));
    if (sea_is_int_array(v)) return SEA_AS_INT_ARRAY(v)->len;
    if (sea_is_float_array(v)) return SEA_AS_FLOAT_ARRAY(v)->len;
    return sea_table_count(SEA_AS_TABLE(v));
}

static bool is_seq_value(SeaValue v) {
    return sea_is_array(v) || sea_is_int_array(v) || sea_is_float_array(v) ||
           (sea_is_table(v) && is_array_table(SEA_AS_TABLE(v)));
}

/* Pass 1 (anchors): count how often each container is reached. */
static int enc_count(Enc *e, SeaValue v) {
    if (!is_container(v) || sea_is_int_array(v) || sea_is_float_array(v)) {
        if (sea_is_int_array(v) || sea_is_float_array(v)) {
            Seen *s = seen_find(e, sea_as_obj(v), true);
            if (!s) return 0;
            s->count++;
        }
        return 1;
    }
    Seen *s = seen_find(e, sea_as_obj(v), true);
    if (!s) { e->err = "out of memory"; return 0; }
    if (s->count++) return 1;
    if (++e->depth > e->max_depth) { e->err = "nesting too deep"; return 0; }
    uint32_t it = 0;
    SeaValue k, val;
    int ok = 1;
    if (sea_is_array(v)) {
        for (uint32_t i = 0; ok && i < sea_array_len(SEA_AS_ARRAY(v)); i++) ok = enc_count(e, sea_array_get(SEA_AS_ARRAY(v), (int64_t)i));
    } else if (sea_is_table(v)) {
        while (ok && sea_table_next(SEA_AS_TABLE(v), &it, &k, &val)) ok = enc_count(e, val);
    } else if (sea_is_map(v)) {
        while (ok && sea_map_next(SEA_AS_MAP(v), &it, &k, &val)) ok = enc_count(e, val);
    } else {
        while (ok && sea_struct_next(SEA_AS_STRUCT(v), &it, &k, &val)) ok = enc_count(e, val);
    }
    e->depth--;
    return ok;
}

static YN *enc_scalar_key(SeaValue k) {
    char t[40];
    if (sea_is_string(k)) return yn_scalar(SEA_AS_STRING(k)->data, SEA_AS_STRING(k)->len, S_ANY, true);
    if (sea_is_rope(k)) {
        SeaString *s = sea_rope_flatten(SEA_AS_ROPE(k));
        YN *n = yn_scalar(s->data, s->len, S_ANY, true);
        sea_obj_release(&s->obj);
        return n;
    }
    if (sea_is_int(k)) return yn_scalar(t, (size_t)fmt_int(t, sea_as_int(k)), S_PLAIN, false);
    if (sea_is_bool(k)) return yn_scalar(sea_as_bool(k) ? "true" : "false", sea_as_bool(k) ? 4 : 5, S_PLAIN, false);
    if (sea_is_number(k)) {
        double d = sea_as_number(k);
        if (isnan(d)) return NULL;
        return yn_scalar(t, (size_t)fmt_double(t, d), S_PLAIN, false);
    }
    if (sea_is_bigint(k)) {
        SeaString *s = sea_bigint_to_string(SEA_AS_BIGINT(k), 10);
        YN *n = yn_scalar(s->data, s->len, S_PLAIN, false);
        sea_obj_release(&s->obj);
        return n;
    }
    return NULL;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* A string that is not valid UTF-8 becomes !!binary (base64). */
static YN *enc_string(const char *s, size_t n) {
    bool utf8 = true;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_seq((const unsigned char *)s + i, n - i, &cp);
        if (!k) { utf8 = false; break; }
        i += (size_t)k;
    }
    if (utf8) return yn_scalar(s, n, S_ANY, true);
    Buf b = { 0 };
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)u[i] << 16;
        if (i + 1 < n) v |= (uint32_t)u[i + 1] << 8;
        if (i + 2 < n) v |= u[i + 2];
        char q[4] = { B64[(v >> 18) & 63], B64[(v >> 12) & 63],
                      i + 1 < n ? B64[(v >> 6) & 63] : '=', i + 2 < n ? B64[v & 63] : '=' };
        buf_push(&b, q, 4);
    }
    YN *node = yn_scalar(b.data ? b.data : "", b.len, S_PLAIN, false);
    buf_free(&b);
    if (node) node->tag = xstrndup(TAG_PREFIX "binary", sizeof TAG_PREFIX - 1 + 6);
    return node;
}

static YN *enc_node(Enc *e, SeaValue v);

static YN *enc_seq(Enc *e, SeaValue v, YN *node) {
    uint32_t n = seq_len(v);
    for (uint32_t i = 0; i < n; i++) {
        YN *item;
        char t[40];
        if (sea_is_int_array(v)) {
            item = yn_scalar(t, (size_t)fmt_int(t, SEA_AS_INT_ARRAY(v)->data[i]), S_PLAIN, false);
        } else if (sea_is_float_array(v)) {
            item = yn_scalar(t, (size_t)fmt_double(t, SEA_AS_FLOAT_ARRAY(v)->data[i]), S_PLAIN, false);
        } else {
            SeaValue x;
            if (sea_is_array(v)) x = sea_array_get(SEA_AS_ARRAY(v), (int64_t)i);
            else if (!sea_table_get(SEA_AS_TABLE(v), sea_int_val((int64_t)i), &x)) x = SEA_NIL_VAL;
            if (!encodable(x)) x = SEA_NIL_VAL;
            item = enc_node(e, x);
            if (!item) { yn_free(node); return NULL; }
        }
        if (!yn_add(node, item)) { yn_free(node); if (!e->err) e->err = "out of memory"; return NULL; }
    }
    return node;
}

static YN *enc_map(Enc *e, SeaValue v, YN *node) {
    Member *ms = NULL;
    size_t n = 0, cap = 0;
    uint32_t it = 0;
    SeaValue k, val;
    for (;;) {
        bool more;
        if (sea_is_table(v)) more = sea_table_next(SEA_AS_TABLE(v), &it, &k, &val);
        else if (sea_is_map(v)) more = sea_map_next(SEA_AS_MAP(v), &it, &k, &val);
        else more = sea_struct_next(SEA_AS_STRUCT(v), &it, &k, &val);
        if (!more) break;
        if (!encodable(val)) continue;
        YN *kn = enc_scalar_key(k);
        if (!kn) continue;                           /* keys without a YAML form are left out */
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            Member *nm = realloc(ms, nc * sizeof *nm);
            if (!nm) { yn_free(kn); e->err = "out of memory"; break; }
            ms = nm;
            cap = nc;
        }
        ms[n++] = (Member){ kn, val };
    }
    if (e->sort_keys && n > 1) qsort(ms, n, sizeof *ms, member_cmp);
    size_t i = 0;
    for (; i < n && !e->err; i++) {
        YN *vn = enc_node(e, ms[i].val);
        if (!vn) break;
        if (!yn_add(node, ms[i].key)) { yn_free(vn); ms[i].key = NULL; e->err = "out of memory"; i++; break; }
        ms[i].key = NULL;
        if (!yn_add(node, vn)) { e->err = "out of memory"; i++; break; }
    }
    for (size_t j = 0; j < n; j++) if (ms[j].key) yn_free(ms[j].key);
    free(ms);
    if (e->err) { yn_free(node); return NULL; }
    return node;
}

static YN *enc_node(Enc *e, SeaValue v) {
    char t[40];
    if (sea_is_nil(v)) return yn_scalar("null", 4, S_PLAIN, false);
    if (sea_is_bool(v)) return yn_scalar(sea_as_bool(v) ? "true" : "false", sea_as_bool(v) ? 4 : 5, S_PLAIN, false);
    if (sea_is_int(v)) return yn_scalar(t, (size_t)fmt_int(t, sea_as_int(v)), S_PLAIN, false);
    if (sea_is_number(v)) return yn_scalar(t, (size_t)fmt_double(t, sea_as_number(v)), S_PLAIN, false);
    if (sea_is_bigint(v)) return enc_scalar_key(v);
    if (sea_is_string(v)) return enc_string(SEA_AS_STRING(v)->data, SEA_AS_STRING(v)->len);
    if (sea_is_rope(v)) {
        SeaString *s = sea_rope_flatten(SEA_AS_ROPE(v));
        YN *n = enc_string(s->data, s->len);
        sea_obj_release(&s->obj);
        return n;
    }
    if (!encodable(v)) {
        snprintf(e->errbuf, sizeof e->errbuf, "cannot encode a value of type %s", sea_type_name(v));
        e->err = e->errbuf;
        return NULL;
    }
    SeaObject *o = sea_as_obj(v);
    char *anchor = NULL;
    if (e->anchors) {
        Seen *s = seen_find(e, o, false);
        if (s && s->count > 1) {
            char name[24];
            if (s->emitted) {
                YN *a = yn_new(N_ALIAS, 0);
                snprintf(name, sizeof name, "id%03d", s->id);
                if (a) a->anchor = xstrndup(name, strlen(name));
                if (!a || !a->anchor) { yn_free(a); e->err = "out of memory"; return NULL; }
                return a;
            }
            s->emitted = true;
            s->id = ++e->next_id;
            snprintf(name, sizeof name, "id%03d", s->id);
            anchor = xstrndup(name, strlen(name));
        }
    } else {
        for (int i = 0; i < e->depth; i++)
            if (e->open[i] == o) { e->err = "cycle detected (a value contains itself; use anchors = true)"; return NULL; }
    }
    if (e->depth >= e->max_depth) { free(anchor); e->err = "nesting too deep"; return NULL; }
    e->open[e->depth++] = o;
    bool seq = is_seq_value(v);
    YN *node = yn_new(seq ? N_SEQ : N_MAP, C_ANY);
    if (!node) { free(anchor); e->depth--; e->err = "out of memory"; return NULL; }
    node->anchor = anchor;
    node = seq ? enc_seq(e, v, node) : enc_map(e, v, node);
    e->depth--;
    return node;
}

static void enc_free(Enc *e) {
    free(e->open);
    free(e->seen);
}

static int enc_init(Enc *e, SeaVM *vm, SeaValue opts) {
    memset(e, 0, sizeof *e);
    e->vm = vm;
    e->max_depth = max_depth_opt(opts);
    e->sort_keys = opt_flag(opts, "sort_keys", 0);
    e->anchors = opt_flag(opts, "anchors", 0);
    e->open = malloc((size_t)(e->max_depth + 1) * sizeof *e->open);
    return e->open != NULL;
}

/* Build the document for one value. */
static YN *enc_document(Enc *e, SeaValue v) {
    if (e->anchors) {
        e->depth = 0;
        if (!enc_count(e, v)) return NULL;
        e->depth = 0;
    }
    if (!encodable(v)) {
        snprintf(e->errbuf, sizeof e->errbuf, "cannot encode a value of type %s", sea_type_name(v));
        e->err = e->errbuf;
        return NULL;
    }
    return enc_node(e, v);
}

static SeaValue encode_impl(SeaVM *vm, int argc, SeaValue *args, bool all) {
    const char *fn = all ? "yaml.encode_all" : "yaml.encode";
    char msg[200];
    if (argc < 1) { snprintf(msg, sizeof msg, "%s: value expected", fn); return sea_make_error_str(vm, msg); }
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    SeaValue src = args[0];
    uint32_t ndocs = 1;
    if (all) {
        if (!is_seq_value(src) || sea_is_int_array(src) || sea_is_float_array(src)) {
            if (!(sea_is_table(src) && sea_table_count(SEA_AS_TABLE(src)) == 0) &&
                !(sea_is_array(src))) {
                snprintf(msg, sizeof msg, "%s: an array of documents expected", fn);
                return sea_make_error_str(vm, msg);
            }
        }
        ndocs = sea_is_table(src) ? sea_table_count(SEA_AS_TABLE(src)) : seq_len(src);
    }
    Enc e;
    if (!enc_init(&e, vm, opts)) { snprintf(msg, sizeof msg, "%s: out of memory", fn); return sea_make_error_str(vm, msg); }
    Doc *docs = calloc(ndocs ? ndocs : 1, sizeof *docs);
    bool explicit_start = opt_flag(opts, "explicit_start", 0);
    bool explicit_end = opt_flag(opts, "explicit_end", 0);
    bool ok = docs != NULL;
    if (!ok) e.err = "out of memory";
    for (uint32_t i = 0; ok && i < ndocs; i++) {
        SeaValue v = src;
        if (all) {
            if (sea_is_array(src)) v = sea_array_get(SEA_AS_ARRAY(src), (int64_t)i);
            else if (!sea_table_get(SEA_AS_TABLE(src), sea_int_val((int64_t)i), &v)) v = SEA_NIL_VAL;
            /* anchors are per document */
            free(e.seen);
            e.seen = NULL;
            e.seen_cap = e.seen_n = 0;
            e.next_id = 0;
        }
        docs[i].root = enc_document(&e, v);
        docs[i].explicit_start = explicit_start;
        docs[i].explicit_end = explicit_end;
        if (!docs[i].root) ok = false;
    }
    SeaValue r;
    if (!ok) {
        snprintf(msg, sizeof msg, "%s: %s", fn, e.err ? e.err : "out of memory");
        r = sea_make_error_str(vm, msg);
    } else {
        Em em;
        memset(&em, 0, sizeof em);
        em.step = em_step_opt(opts);
        em.flow_all = opt_flag(opts, "flow", 0);
        em_stream(&em, docs, ndocs);
        if (em.b.oom) {
            snprintf(msg, sizeof msg, "%s: out of memory", fn);
            r = sea_make_error_str(vm, msg);
        } else {
            r = new_str(em.b.data ? em.b.data : "", em.b.len);
        }
        buf_free(&em.b);
    }
    if (docs) docs_free(docs, ndocs);
    enc_free(&e);
    return r;
}

static SeaValue fn_encode(SeaVM *vm, int argc, SeaValue *args) { return encode_impl(vm, argc, args, false); }
static SeaValue fn_encode_all(SeaVM *vm, int argc, SeaValue *args) { return encode_impl(vm, argc, args, true); }

/* ======================================================================== */
/* yaml.emit: event tables → node tree → text                               */
/* ======================================================================== */

static char *opt_cstr(SeaValue t, const char *field) {
    SeaValue v;
    if (!opt_get(t, field, &v)) return NULL;
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return NULL;
    char *r = xstrndup(s->data, s->len);
    sea_obj_release(&s->obj);
    return r;
}

static int style_of(SeaValue t) {
    char *s = opt_cstr(t, "style");
    int st = S_ANY;
    if (s) {
        for (int i = 0; i < 6; i++) if (strcmp(s, STYLE_NAMES[i]) == 0) st = i;
        if (strcmp(s, "flow") == 0) st = C_FLOW;
        else if (strcmp(s, "block") == 0) st = C_BLOCK;
        free(s);
    }
    return st;
}

static SeaValue fn_emit(SeaVM *vm, int argc, SeaValue *args) {
    char msg[200];
    if (argc < 1 || !(sea_is_array(args[0]) || (sea_is_table(args[0]) &&
                                                (is_array_table(SEA_AS_TABLE(args[0])) || !sea_table_count(SEA_AS_TABLE(args[0]))))))
        return sea_make_error_str(vm, "yaml.emit: an array of events expected");
    SeaValue evs = args[0];
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    uint32_t n = seq_len(evs);
    Doc *docs = NULL;
    size_t ndocs = 0, capdocs = 0;
    YN **stack = NULL;
    size_t depth = 0, capstack = 0;
    bool in_doc = false;
    const char *err = NULL;
    uint32_t i = 0;
    for (; i < n && !err; i++) {
        SeaValue ev;
        if (sea_is_array(evs)) ev = sea_array_get(SEA_AS_ARRAY(evs), (int64_t)i);
        else if (!sea_table_get(SEA_AS_TABLE(evs), sea_int_val((int64_t)i), &ev)) ev = SEA_NIL_VAL;
        char *type = opt_cstr(ev, "type");
        if (!type) { err = "an event without a type"; break; }
        YN *node = NULL;
        bool opens = false, closes = false;
        if (!strcmp(type, "stream_start") || !strcmp(type, "stream_end")) {
            if (in_doc) err = "a stream event inside a document";
        } else if (!strcmp(type, "document_start")) {
            if (in_doc) err = "document_start inside a document";
            else {
                if (ndocs == capdocs) {
                    size_t nc = capdocs ? capdocs * 2 : 4;
                    Doc *nd = realloc(docs, nc * sizeof *nd);
                    if (!nd) { err = "out of memory"; free(type); break; }
                    docs = nd;
                    capdocs = nc;
                }
                Doc *d = &docs[ndocs++];
                memset(d, 0, sizeof *d);
                SeaValue imp;
                d->explicit_start = opt_get(ev, "implicit", &imp) && sea_is_bool(imp) && !sea_as_bool(imp);
                char *ver = opt_cstr(ev, "version");
                if (ver) {
                    if (sscanf(ver, "%d.%d", &d->major, &d->minor) != 2 || d->major != 1) err = "unsupported version";
                    free(ver);
                }
                SeaValue td;
                if (opt_get(ev, "tags", &td) && (sea_is_table(td) || sea_is_map(td))) {
                    uint32_t it = 0;
                    SeaValue k, v;
                    size_t cnt = sea_is_table(td) ? sea_table_count(SEA_AS_TABLE(td)) : 64;
                    d->tagdirs = calloc(cnt ? cnt : 1, sizeof *d->tagdirs);
                    while (d->tagdirs && d->ntagdirs < cnt &&
                           (sea_is_table(td) ? sea_table_next(SEA_AS_TABLE(td), &it, &k, &v)
                                             : sea_map_next(SEA_AS_MAP(td), &it, &k, &v))) {
                        SeaString *ks = sea_value_get_flat_string(k), *vs = sea_value_get_flat_string(v);
                        if (ks && vs) {
                            d->tagdirs[d->ntagdirs].handle = xstrndup(ks->data, ks->len);
                            d->tagdirs[d->ntagdirs].prefix = xstrndup(vs->data, vs->len);
                            d->ntagdirs++;
                        }
                        if (ks) sea_obj_release(&ks->obj);
                        if (vs) sea_obj_release(&vs->obj);
                    }
                }
                in_doc = true;
            }
        } else if (!strcmp(type, "document_end")) {
            if (!in_doc || depth) err = "document_end outside a document or inside a collection";
            else {
                SeaValue imp;
                docs[ndocs - 1].explicit_end = opt_get(ev, "implicit", &imp) && sea_is_bool(imp) && !sea_as_bool(imp);
                in_doc = false;
            }
        } else if (!strcmp(type, "scalar")) {
            char *val = opt_cstr(ev, "value");
            node = yn_scalar(val ? val : "", val ? strlen(val) : 0, style_of(ev), false);
            SeaValue raw;
            if (node && opt_get(ev, "value", &raw) && sea_is_string(raw)) {
                /* keep embedded NUL bytes */
                free(node->val);
                node->val = xstrndup(SEA_AS_STRING(raw)->data, SEA_AS_STRING(raw)->len);
                node->len = SEA_AS_STRING(raw)->len;
            }
            if (node && node->style == S_ANY) node->str_value = true;
            free(val);
        } else if (!strcmp(type, "alias")) {
            node = yn_new(N_ALIAS, 0);
            if (node) {
                node->anchor = opt_cstr(ev, "alias");
                if (!node->anchor) node->anchor = opt_cstr(ev, "anchor");
                if (!node->anchor) err = "an alias event without a name";
            }
        } else if (!strcmp(type, "sequence_start") || !strcmp(type, "mapping_start")) {
            int st = style_of(ev);
            node = yn_new(type[0] == 's' ? N_SEQ : N_MAP, st == C_FLOW ? C_FLOW : st == C_BLOCK ? C_BLOCK : C_ANY);
            opens = true;
        } else if (!strcmp(type, "sequence_end") || !strcmp(type, "mapping_end")) {
            closes = true;
            int kind = type[0] == 's' ? N_SEQ : N_MAP;
            if (!depth || stack[depth - 1]->kind != kind) err = "an end event that does not match the open collection";
            else if (kind == N_MAP && stack[depth - 1]->n % 2) err = "a mapping with a key but no value";
        } else {
            err = "unknown event type";
        }
        free(type);
        if (err) { yn_free(node); break; }
        if (closes) {
            node = stack[--depth];
        } else if (!node) {
            continue;
        } else {
            if (!node->anchor || node->kind != N_ALIAS) {
                char *anchor = opt_cstr(ev, "anchor");
                if (anchor && node->kind != N_ALIAS) node->anchor = anchor;
                else free(anchor);
                node->tag = opt_cstr(ev, "tag");
            }
            if (!in_doc) { yn_free(node); err = "a node outside a document"; break; }
            if (opens) {
                if (depth >= YAML_DEPTH_CAP) { yn_free(node); err = "nesting too deep"; break; }
                if (depth == capstack) {
                    size_t nc = capstack ? capstack * 2 : 16;
                    YN **ns = realloc(stack, nc * sizeof *ns);
                    if (!ns) { yn_free(node); err = "out of memory"; break; }
                    stack = ns;
                    capstack = nc;
                }
                stack[depth++] = node;
                continue;
            }
        }
        /* a complete node: into the open collection, or the document root */
        if (depth) {
            if (!yn_add(stack[depth - 1], node)) err = "out of memory";
        } else {
            Doc *d = &docs[ndocs - 1];
            if (d->root) { yn_free(node); err = "a second root node in a document"; }
            else d->root = node;
        }
    }
    if (!err && (depth || in_doc)) err = "the events end inside a document";
    SeaValue r;
    if (err) {
        snprintf(msg, sizeof msg, "yaml.emit: %s (event %u)", err, (unsigned)(i < n ? i + 1 : n));
        r = sea_make_error_str(vm, msg);
    } else {
        Em em;
        memset(&em, 0, sizeof em);
        em.step = em_step_opt(opts);
        em.flow_all = opt_flag(opts, "flow", 0);
        em_stream(&em, docs, ndocs);
        r = em.b.oom ? sea_make_error_str(vm, "yaml.emit: out of memory") : new_str(em.b.data ? em.b.data : "", em.b.len);
        buf_free(&em.b);
    }
    while (depth) yn_free(stack[--depth]);
    free(stack);
    if (docs) docs_free(docs, ndocs);
    return r;
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "decode",     fn_decode);
    sea_mod_set(mod, "decode_all", fn_decode_all);
    sea_mod_set(mod, "valid",      fn_valid);
    sea_mod_set(mod, "encode",     fn_encode);
    sea_mod_set(mod, "encode_all", fn_encode_all);
    sea_mod_set(mod, "events",     fn_events);
    sea_mod_set(mod, "emit",       fn_emit);
    return sea_obj_val(&mod->obj);
}
