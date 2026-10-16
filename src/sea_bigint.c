/*
 * sea_bigint.c — arbitrary-precision integers (see sea_bigint.h).
 *
 * Magnitudes are arrays of 32-bit limbs, least significant first. Schoolbook
 * multiplication, Knuth's algorithm D for division (Hacker's Delight
 * divmnu), decimal conversion by repeated division by 10^9. Division and
 * modulo round toward minus infinity, like Sea's // and % on ints.
 */

#include "sea_bigint.h"
#include "sea_memory.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIMBS (1u << 22)   /* 128 Mbit (16 MB): the largest result allowed */

/* ---- magnitudes ------------------------------------------------------------ */

/* A read-only integer: limbs, count, sign. Small values use buf. */
typedef struct {
    const uint32_t *d;
    uint32_t        n;
    bool            neg;
    uint32_t        buf[34];        /* any finite double fits in 33 limbs */
} View;

static uint32_t trim(const uint32_t *d, uint32_t n) {
    while (n && !d[n - 1]) n--;
    return n;
}

static int mag_cmp(const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn) {
    if (an != bn) return an < bn ? -1 : 1;
    for (uint32_t i = an; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* out (max(an, bn) + 1 limbs) = a + b; returns the length */
static uint32_t mag_add(const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn, uint32_t *out) {
    if (an < bn) { const uint32_t *t = a; a = b; b = t; uint32_t tn = an; an = bn; bn = tn; }
    uint64_t carry = 0;
    uint32_t i = 0;
    for (; i < bn; i++) { carry += (uint64_t)a[i] + b[i]; out[i] = (uint32_t)carry; carry >>= 32; }
    for (; i < an; i++) { carry += a[i]; out[i] = (uint32_t)carry; carry >>= 32; }
    out[i] = (uint32_t)carry;
    return trim(out, an + 1);
}

/* out (an limbs) = a - b, with a >= b; returns the length */
static uint32_t mag_sub(const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn, uint32_t *out) {
    int64_t borrow = 0;
    uint32_t i = 0;
    for (; i < bn; i++) {
        int64_t t = (int64_t)a[i] - b[i] - borrow;
        borrow = t < 0;
        out[i] = (uint32_t)(t + (borrow << 32));
    }
    for (; i < an; i++) {
        int64_t t = (int64_t)a[i] - borrow;
        borrow = t < 0;
        out[i] = (uint32_t)(t + (borrow << 32));
    }
    return trim(out, an);
}

/* out (an + bn limbs, not overlapping a or b) = a * b; returns the length */
static uint32_t mag_mul(const uint32_t *a, uint32_t an, const uint32_t *b, uint32_t bn, uint32_t *out) {
    memset(out, 0, (size_t)(an + bn) * sizeof *out);
    for (uint32_t i = 0; i < an; i++) {
        uint64_t carry = 0, ai = a[i];
        if (!ai) continue;
        for (uint32_t j = 0; j < bn; j++) {
            carry += ai * b[j] + out[i + j];
            out[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        out[i + bn] = (uint32_t)carry;
    }
    return trim(out, an + bn);
}

/* q = u / d (in place allowed); returns the remainder */
static uint32_t mag_divsmall(const uint32_t *u, uint32_t n, uint32_t d, uint32_t *q) {
    uint64_t r = 0;
    for (uint32_t i = n; i-- > 0;) {
        uint64_t cur = (r << 32) | u[i];
        q[i] = (uint32_t)(cur / d);
        r = cur % d;
    }
    return (uint32_t)r;
}

/* u (m limbs) / v (n limbs, n >= 2, v[n-1] != 0, m >= n): q gets m - n + 1
 * limbs, r gets n limbs (either may be NULL). Knuth D, Hacker's Delight. */
static bool mag_divmod_long(const uint32_t *u, uint32_t m, const uint32_t *v, uint32_t n,
                            uint32_t *q, uint32_t *r) {
    uint32_t *vn = malloc((size_t)n * sizeof *vn), *un = malloc((size_t)(m + 1) * sizeof *un);
    if (!vn || !un) { free(vn); free(un); return false; }
    int s = __builtin_clz(v[n - 1]);
    for (uint32_t i = n - 1; i > 0; i--)
        vn[i] = (v[i] << s) | (s ? (uint32_t)((uint64_t)v[i - 1] >> (32 - s)) : 0);
    vn[0] = v[0] << s;
    un[m] = s ? (uint32_t)((uint64_t)u[m - 1] >> (32 - s)) : 0;
    for (uint32_t i = m - 1; i > 0; i--)
        un[i] = (u[i] << s) | (s ? (uint32_t)((uint64_t)u[i - 1] >> (32 - s)) : 0);
    un[0] = u[0] << s;
    const uint64_t B = UINT64_C(1) << 32;
    for (int64_t j = (int64_t)(m - n); j >= 0; j--) {
        uint64_t num = ((uint64_t)un[j + n] << 32) | un[j + n - 1];
        uint64_t qhat = num / vn[n - 1], rhat = num % vn[n - 1];
        while (qhat >= B || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
            qhat--;
            rhat += vn[n - 1];
            if (rhat >= B) break;
        }
        int64_t k = 0, t;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t p = qhat * vn[i];
            t = (int64_t)un[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);
            un[i + j] = (uint32_t)t;
            k = (int64_t)(p >> 32) - (t >> 32);
        }
        t = (int64_t)un[j + n] - k;
        un[j + n] = (uint32_t)t;
        if (t < 0) {                     /* qhat was one too large: add back */
            qhat--;
            uint64_t c = 0;
            for (uint32_t i = 0; i < n; i++) {
                c += (uint64_t)un[i + j] + vn[i];
                un[i + j] = (uint32_t)c;
                c >>= 32;
            }
            un[j + n] += (uint32_t)c;
        }
        if (q) q[j] = (uint32_t)qhat;
    }
    if (r) {
        for (uint32_t i = 0; i + 1 < n; i++)
            r[i] = (un[i] >> s) | (s ? (uint32_t)((uint64_t)un[i + 1] << (32 - s)) : 0);
        r[n - 1] = un[n - 1] >> s;
    }
    free(vn);
    free(un);
    return true;
}

/* ---- objects ------------------------------------------------------------- */

static SeaBigInt *bi_alloc(uint32_t cap) {
    SeaBigInt *b = sea_malloc(sizeof(SeaBigInt) + (size_t)cap * sizeof(uint32_t));
    sea_obj_init(&b->obj, OBJ_BIGINT, 1);
    b->n = 0;
    b->cap = cap;
    b->neg = 0;
    return b;
}

size_t sea_bigint_size(const SeaBigInt *b) {
    return sizeof(SeaBigInt) + (size_t)b->cap * sizeof(uint32_t);
}

/* A new bigint holding d[0..n) (trimmed) with sign neg. */
static SeaBigInt *bi_make(const uint32_t *d, uint32_t n, bool neg) {
    n = trim(d, n);
    SeaBigInt *b = bi_alloc(n);
    if (n) memcpy(b->d, d, (size_t)n * sizeof *d);
    b->n = n;
    b->neg = n && neg;
    return b;
}

SeaBigInt *sea_bigint_from_u64(uint64_t v) {
    uint32_t d[2] = { (uint32_t)v, (uint32_t)(v >> 32) };
    return bi_make(d, 2, false);
}

SeaBigInt *sea_bigint_from_i64(int64_t v) {
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    uint32_t d[2] = { (uint32_t)m, (uint32_t)(m >> 32) };
    return bi_make(d, 2, v < 0);
}

/* Limbs of trunc(d) (finite) into out[34]; returns the count. */
static uint32_t double_limbs(double d, uint32_t *out, bool *neg) {
    *neg = d < 0;
    d = trunc(fabs(d));
    memset(out, 0, 34 * sizeof *out);
    if (d < 1) return 0;
    int e;
    double m = frexp(d, &e);                         /* d = m * 2^e, m in [0.5, 1) */
    uint64_t mant = (uint64_t)ldexp(m, 53);           /* 53 bits */
    int shift = e - 53;
    if (shift <= 0) {
        mant >>= -shift;
        out[0] = (uint32_t)mant;
        out[1] = (uint32_t)(mant >> 32);
        return trim(out, 2);
    }
    uint32_t w = (uint32_t)shift / 32, bit = (uint32_t)shift % 32;
    unsigned __int128 x = (unsigned __int128)mant << bit;
    out[w] = (uint32_t)x;
    out[w + 1] = (uint32_t)(x >> 32);
    out[w + 2] = (uint32_t)(x >> 64);
    return trim(out, w + 3);
}

SeaBigInt *sea_bigint_from_double(double d) {
    uint32_t buf[34];
    bool neg;
    uint32_t n = double_limbs(d, buf, &neg);
    return bi_make(buf, n, neg);
}

SeaBigInt *sea_bigint_from_bytes(const uint8_t *be, size_t len, bool neg) {
    uint32_t n = (uint32_t)((len + 3) / 4);
    SeaBigInt *b = bi_alloc(n);
    memset(b->d, 0, (size_t)n * sizeof(uint32_t));
    for (size_t i = 0; i < len; i++) {
        size_t k = len - 1 - i;                       /* byte k from the least significant end */
        b->d[k / 4] |= (uint32_t)be[i] << (8 * (k % 4));
    }
    b->n = trim(b->d, n);
    b->neg = b->n && neg;
    return b;
}

size_t sea_bigint_byte_len(const SeaBigInt *b) {
    if (!b->n) return 0;
    uint32_t top = b->d[b->n - 1];
    return (size_t)(b->n - 1) * 4 + (size_t)((32 - __builtin_clz(top) + 7) / 8);
}

void sea_bigint_to_bytes(const SeaBigInt *b, uint8_t *out, size_t len) {
    memset(out, 0, len);
    for (size_t k = 0; k < len && k / 4 < b->n; k++)
        out[len - 1 - k] = (uint8_t)(b->d[k / 4] >> (8 * (k % 4)));
}

bool sea_bigint_to_i64(const SeaBigInt *b, int64_t *out) {
    if (b->n > 2) return false;
    uint64_t m = b->n == 0 ? 0 : b->n == 1 ? b->d[0] : ((uint64_t)b->d[1] << 32) | b->d[0];
    if (b->neg) {
        if (m > (uint64_t)INT64_MAX + 1) return false;
        *out = (int64_t)(0 - m);
    } else {
        if (m > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)m;
    }
    return true;
}

double sea_bigint_to_double(const SeaBigInt *b) {
    if (b->n <= 2) {
        uint64_t m = b->n == 0 ? 0 : b->n == 1 ? b->d[0] : ((uint64_t)b->d[1] << 32) | b->d[0];
        double d = (double)m;                         /* correctly rounded */
        return b->neg ? -d : d;
    }
    SeaString *s = sea_bigint_to_string(b, 10);       /* strtod rounds correctly */
    double d = strtod(s->data, NULL);
    sea_obj_release(&s->obj);
    return d;
}

static const char DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";

SeaString *sea_bigint_to_string(const SeaBigInt *b, int base) {
    if (base < 2 || base > 36) base = 10;
    if (!b->n) return sea_string_new("0", 1);
    /* chunk = base^k, the largest power that fits in 32 bits */
    uint32_t chunk = (uint32_t)base, k = 1;
    while ((uint64_t)chunk * (uint32_t)base <= 0xFFFFFFFFu) { chunk *= (uint32_t)base; k++; }
    uint32_t n = b->n;
    uint32_t *t = malloc((size_t)n * sizeof *t);
    size_t cap = (size_t)n * 32 + 2;                  /* enough digits for base 2 */
    char *out = malloc(cap);
    if (!t || !out) { free(t); free(out); return sea_string_new("?", 1); }
    memcpy(t, b->d, (size_t)n * sizeof *t);
    size_t pos = cap;
    while (n) {
        uint32_t r = mag_divsmall(t, n, chunk, t);
        n = trim(t, n);
        for (uint32_t i = 0; i < k && (n || r); i++) {
            out[--pos] = DIGITS[r % (uint32_t)base];
            r /= (uint32_t)base;
        }
    }
    if (b->neg) out[--pos] = '-';
    SeaString *s = sea_string_new(out + pos, cap - pos);
    free(t);
    free(out);
    return s;
}

static int digit_of(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

SeaBigInt *sea_bigint_parse(const char *s, size_t len, int base) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    while (len > i && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    bool neg = false;
    if (i < len && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (i + 1 < len && s[i] == '0') {
        char p = (char)(s[i + 1] | 0x20);
        int pb = p == 'x' ? 16 : p == 'o' ? 8 : p == 'b' ? 2 : 0;
        if (pb && (base == 0 || base == pb)) { base = pb; i += 2; }
    }
    if (base == 0) base = 10;
    if (base < 2 || base > 36 || i >= len) return NULL;
    uint32_t chunk = (uint32_t)base, k = 1;
    while ((uint64_t)chunk * (uint32_t)base <= 0xFFFFFFFFu) { chunk *= (uint32_t)base; k++; }
    size_t ndig = 0;
    for (size_t j = i; j < len; j++) if (s[j] != '_') ndig++;
    uint32_t cap = (uint32_t)(ndig * 6 / 32 + 2);     /* log2(36) < 6 bits per digit */
    if (cap > MAX_LIMBS) return NULL;
    uint32_t *d = calloc(cap, sizeof *d), n = 0;
    if (!d) return NULL;
    uint32_t acc = 0, cnt = 0, mul = 1;
    bool last_us = true;                              /* no leading '_' */
    for (size_t j = i; j <= len; j++) {
        if (j < len) {
            if (s[j] == '_') {
                if (last_us) { free(d); return NULL; }
                last_us = true;
                continue;
            }
            int v = digit_of(s[j]);
            if (v >= base) { free(d); return NULL; }
            acc = acc * (uint32_t)base + (uint32_t)v;
            mul *= (uint32_t)base;
            cnt++;
            last_us = false;
            if (cnt < k) continue;
        } else if (last_us || !cnt) {
            if (last_us) { free(d); return NULL; }    /* trailing '_' or no digits */
            break;
        }
        /* d = d * mul + acc */
        uint64_t carry = acc;
        for (uint32_t t = 0; t < n; t++) {
            carry += (uint64_t)d[t] * mul;
            d[t] = (uint32_t)carry;
            carry >>= 32;
        }
        if (carry) d[n++] = (uint32_t)carry;
        acc = 0;
        cnt = 0;
        mul = 1;
        if (j >= len) break;
    }
    SeaBigInt *b = bi_make(d, n, neg);
    free(d);
    return b;
}

int sea_bigint_cmp(const SeaBigInt *a, const SeaBigInt *b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c = mag_cmp(a->d, a->n, b->d, b->n);
    return a->neg ? -c : c;
}

uint32_t sea_bigint_hash(const SeaBigInt *b) {
    uint64_t h = UINT64_C(0xcbf29ce484222325) ^ b->neg;
    for (uint32_t i = 0; i < b->n; i++) {
        h ^= b->d[i];
        h *= UINT64_C(0x100000001b3);
    }
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    return (uint32_t)h;
}

/* ---- values -------------------------------------------------------------- */

/* v as an integer view: bigint, int, or a finite integral number. */
static bool view_of(SeaValue v, View *w) {
    if (sea_is_bigint(v)) {
        SeaBigInt *b = SEA_AS_BIGINT(v);
        w->d = b->d;
        w->n = b->n;
        w->neg = b->neg;
        return true;
    }
    if (sea_is_int(v)) {
        int64_t x = sea_as_int(v);
        uint64_t m = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
        w->buf[0] = (uint32_t)m;
        w->buf[1] = (uint32_t)(m >> 32);
        w->d = w->buf;
        w->n = trim(w->buf, 2);
        w->neg = x < 0;
        return true;
    }
    if (sea_is_number(v)) {
        double d = sea_as_number(v);
        if (!isfinite(d) || d != trunc(d)) return false;
        w->n = double_limbs(d, w->buf, &w->neg);
        w->d = w->buf;
        return true;
    }
    return false;
}

bool sea_bigint_of(SeaValue v, SeaBigInt **out) {
    if (sea_is_bigint(v)) {
        sea_obj_retain(sea_as_obj(v));
        *out = SEA_AS_BIGINT(v);
        return true;
    }
    View w;
    if (!view_of(v, &w)) return false;
    *out = bi_make(w.d, w.n, w.neg);
    return true;
}

int sea_bigint_compare(SeaValue a, SeaValue b, int *out) {
    bool an = sea_is_bigint(a) || sea_is_int(a) || sea_is_number(a);
    bool bn = sea_is_bigint(b) || sea_is_int(b) || sea_is_number(b);
    if (!an || !bn) return -1;
    if ((sea_is_number(a) && isnan(sea_as_number(a))) || (sea_is_number(b) && isnan(sea_as_number(b))))
        return 0;
    /* an infinity is beyond every bigint */
    if (sea_is_number(a) && isinf(sea_as_number(a))) { *out = sea_as_number(a) < 0 ? -1 : 1; return 1; }
    if (sea_is_number(b) && isinf(sea_as_number(b))) { *out = sea_as_number(b) < 0 ? 1 : -1; return 1; }
    /* a fractional number: compare with its floor, then the fraction decides */
    double frac_a = 0, frac_b = 0;
    View va, vb;
    if (sea_is_number(a)) {
        double d = sea_as_number(a), f = floor(d);
        frac_a = d - f;
        va.n = double_limbs(f, va.buf, &va.neg);
        va.d = va.buf;
    } else {
        view_of(a, &va);
    }
    if (sea_is_number(b)) {
        double d = sea_as_number(b), f = floor(d);
        frac_b = d - f;
        vb.n = double_limbs(f, vb.buf, &vb.neg);
        vb.d = vb.buf;
    } else {
        view_of(b, &vb);
    }
    int c;
    if (va.neg != vb.neg) c = va.neg ? -1 : 1;
    else {
        c = mag_cmp(va.d, va.n, vb.d, vb.n);
        if (va.neg) c = -c;
    }
    if (c == 0) c = frac_a > frac_b ? 1 : frac_a < frac_b ? -1 : 0;
    *out = c;
    return 1;
}

bool sea_bigint_equal(SeaValue a, SeaValue b) {
    if (a == b) return true;
    if (!sea_is_bigint(a) && !sea_is_bigint(b)) return false;
    int c;
    return sea_bigint_compare(a, b, &c) == 1 && c == 0;
}

SeaBigInt *sea_bigint_negate(const SeaBigInt *b, bool abs_only) {
    return bi_make(b->d, b->n, abs_only ? false : !b->neg);
}

/* r = a + b (signed); NULL on allocation failure */
static SeaBigInt *add_views(const View *a, const View *b) {
    uint32_t cap = (a->n > b->n ? a->n : b->n) + 1;
    uint32_t *t = malloc((size_t)cap * sizeof *t);
    if (!t) return NULL;
    uint32_t n;
    bool neg;
    if (a->neg == b->neg) {
        n = mag_add(a->d, a->n, b->d, b->n, t);
        neg = a->neg;
    } else if (mag_cmp(a->d, a->n, b->d, b->n) >= 0) {
        n = mag_sub(a->d, a->n, b->d, b->n, t);
        neg = a->neg;
    } else {
        n = mag_sub(b->d, b->n, a->d, a->n, t);
        neg = b->neg;
    }
    SeaBigInt *r = bi_make(t, n, neg);
    free(t);
    return r;
}

/* Floored division of views: quotient and/or remainder (new bigints). */
static bool divmod_views(const View *a, const View *b, SeaBigInt **qo, SeaBigInt **ro) {
    uint32_t qn = a->n >= b->n ? a->n - b->n + 1 : 1;
    uint32_t *q = calloc(qn + 1, sizeof *q), *r = calloc(b->n + 1, sizeof *r);
    if (!q || !r) { free(q); free(r); return false; }
    uint32_t rn;
    if (mag_cmp(a->d, a->n, b->d, b->n) < 0) {
        memcpy(r, a->d, (size_t)a->n * sizeof *r);
        rn = a->n;
    } else if (b->n == 1) {
        r[0] = mag_divsmall(a->d, a->n, b->d[0], q);
        rn = trim(r, 1);
    } else {
        if (!mag_divmod_long(a->d, a->n, b->d, b->n, q, r)) { free(q); free(r); return false; }
        rn = trim(r, b->n);
    }
    uint32_t qlen = trim(q, qn);
    bool qneg = a->neg != b->neg;
    if (qneg && rn) {
        /* floor: q = -(q + 1), r = |b| - r, with the sign of b */
        uint32_t *q1 = calloc(qlen + 2, sizeof *q1), *r1 = calloc(b->n + 1, sizeof *r1);
        if (!q1 || !r1) { free(q); free(r); free(q1); free(r1); return false; }
        uint32_t one = 1;
        qlen = mag_add(q, qlen, &one, 1, q1);
        memcpy(q, q1, (size_t)qlen * sizeof *q);
        rn = mag_sub(b->d, b->n, r, rn, r1);
        memcpy(r, r1, (size_t)(b->n) * sizeof *r);
        free(q1);
        free(r1);
    }
    if (qo) *qo = bi_make(q, qlen, qneg);
    if (ro) *ro = bi_make(r, rn, b->neg);
    free(q);
    free(r);
    return true;
}

static uint64_t bit_len(const View *v) {
    if (!v->n) return 0;
    return (uint64_t)(v->n - 1) * 32 + (uint64_t)(32 - __builtin_clz(v->d[v->n - 1]));
}

SeaBigInt *sea_bigint_arith(char op, SeaValue av, SeaValue bv, const char **err) {
    View a, b;
    *err = NULL;
    if (!view_of(av, &a) || !view_of(bv, &b)) { *err = "integer expected"; return NULL; }
    SeaBigInt *r = NULL;
    switch (op) {
        case '+':
            r = add_views(&a, &b);
            break;
        case '-':
            b.neg = b.n && !b.neg;
            r = add_views(&a, &b);
            break;
        case '*': {
            if ((uint64_t)a.n + b.n > MAX_LIMBS) { *err = "result too large"; return NULL; }
            uint32_t *t = malloc(((size_t)a.n + b.n + 1) * sizeof *t);
            if (!t) break;
            uint32_t n = mag_mul(a.d, a.n, b.d, b.n, t);
            r = bi_make(t, n, a.neg != b.neg);
            free(t);
            break;
        }
        case '/':
        case '%':
            if (!b.n) { *err = "division by zero"; return NULL; }
            if (!divmod_views(&a, &b, op == '/' ? &r : NULL, op == '%' ? &r : NULL)) r = NULL;
            break;
        case '^': {
            if (b.neg) { *err = "negative exponent"; return NULL; }
            bool odd = b.n && (b.d[0] & 1);
            /* 0, 1 and -1 stay small whatever the exponent */
            if (a.n == 0) { uint32_t v = b.n == 0; r = bi_make(&v, 1, false); break; }
            if (a.n == 1 && a.d[0] == 1) { uint32_t v = 1; r = bi_make(&v, 1, a.neg && odd); break; }
            uint64_t bl = bit_len(&a), limit = (uint64_t)MAX_LIMBS * 32;
            if (b.n > 2 || (b.n == 2 && b.d[1] >= 0x80000000u)) { *err = "result too large"; return NULL; }
            int64_t e = b.n == 0 ? 0 : b.n == 1 ? b.d[0] : (int64_t)(((uint64_t)b.d[1] << 32) | b.d[0]);
            if ((uint64_t)e > limit || bl * (uint64_t)e > limit) { *err = "result too large"; return NULL; }
            bool neg = a.neg && odd;
            /* square and multiply on plain limb arrays */
            uint32_t rn = 1, bn = a.n, cap = (uint32_t)((bit_len(&a) * (uint64_t)e) / 32 + 2);
            if (cap < a.n + 2) cap = a.n + 2;
            uint32_t *res = calloc(cap, sizeof *res), *base = calloc(cap, sizeof *base),
                     *tmpb = calloc(2 * (size_t)cap, sizeof *tmpb);
            if (!res || !base || !tmpb) { free(res); free(base); free(tmpb); break; }
            res[0] = 1;
            memcpy(base, a.d, (size_t)a.n * sizeof *base);
            while (e > 0) {
                if (e & 1) {
                    uint32_t n = mag_mul(res, rn, base, bn, tmpb);
                    memcpy(res, tmpb, (size_t)n * sizeof *res);
                    rn = n ? n : 1;
                    if (!n) res[0] = 0;
                }
                e >>= 1;
                if (e) {
                    uint32_t n = mag_mul(base, bn, base, bn, tmpb);
                    memcpy(base, tmpb, (size_t)n * sizeof *base);
                    bn = n;
                }
            }
            r = bi_make(res, rn, neg);
            free(res);
            free(base);
            free(tmpb);
            break;
        }
    }
    if (!r && !*err) *err = "out of memory";
    return r;
}
