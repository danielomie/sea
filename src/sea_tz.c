/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Named time zones from the system's tz database (RFC 8536 TZif files).
 *
 * A TZif file lists the instants at which the zone's offset changed, each
 * with the offset and abbreviation in effect from then on, and ends (in
 * version 2 and later) with a POSIX TZ rule such as "<-03>3" or
 * "EST5EDT,M3.2.0,M11.1.0" for the instants after the last listed change.
 * We read the 64-bit (version 2+) part when there is one.
 */

#include "sea_tz.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t utoff;
    bool    isdst;
    char    abbr[8];
} TType;

/* A POSIX TZ rule: std, or std and dst with the dates they start */
typedef struct {
    bool  valid;
    TType std, dst;
    bool  has_dst;
    /* the day a period starts: kind 'J' (1..365, no Feb 29), 'N' (0..365),
     * 'M' (month m, week w 1..5 (5: last), weekday d); at time (seconds
     * of local time, may be negative or past 24h) */
    struct { char kind; int m, w, d, n; int32_t time; } start, end;
} Rule;

struct SeaTZ {
    char    *name;
    int64_t *trans;      /* the instants of change, ascending */
    uint8_t *idx;        /* the type in effect from trans[i] */
    size_t   ntrans;
    TType   *types;
    size_t   ntypes;
    Rule     rule;       /* after the last change */
    SeaTZ   *next;
};

static SeaTZ *g_zones;
static pthread_mutex_t g_zones_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- dates ---- */

/* Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm) */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t floordiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

static int64_t year_of(int64_t t) {
    int64_t z = floordiv(t, 86400) + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    return yoe + era * 400 + (m <= 2);
}

static bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* Seconds since the epoch (as local wall time) at which a rule date starts in year y */
static int64_t rule_day(int64_t y, char kind, int m, int w, int d, int n) {
    int64_t day;
    if (kind == 'J') day = days_from_civil(y, 1, 1) + n - 1 + (leap(y) && n >= 60);
    else if (kind == 'N') day = days_from_civil(y, 1, 1) + n;
    else {
        int64_t first = days_from_civil(y, m, 1);
        int wd = (int)((first % 7 + 7 + 4) % 7);      /* 1970-01-01 was a Thursday */
        int64_t dd = first + (d - wd + 7) % 7 + (int64_t)(w - 1) * 7;
        static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int64_t len = mdays[m - 1] + (m == 2 && leap(y));
        while (dd >= first + len) dd -= 7;              /* week 5: the last one */
        day = dd;
    }
    return day * 86400;
}

/* ---- POSIX TZ rules ---- */

static bool rule_name(const char **p, char *out) {
    const char *s = *p;
    size_t n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>') { if (n < 7) out[n++] = *s; s++; }
        if (*s != '>') return false;
        s++;
    } else {
        while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) { if (n < 7) out[n++] = *s; s++; }
        if (n < 3) return false;
    }
    out[n] = '\0';
    *p = s;
    return true;
}

/* [+-]hh[:mm[:ss]] → seconds */
static bool rule_hms(const char **p, int32_t *out) {
    const char *s = *p;
    int sign = 1;
    if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; s++; }
    if (*s < '0' || *s > '9') return false;
    int32_t v[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) {
        if (k > 0) { if (*s != ':') break; s++; }
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) { v[k] = v[k] * 10 + (*s - '0'); s++; digits++; }
        if (!digits) return false;
    }
    *out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    *p = s;
    return true;
}

static bool rule_date(const char **p, char *kind, int *m, int *w, int *d, int *n, int32_t *time) {
    const char *s = *p;
    *m = *w = *d = *n = 0;
    if (*s == 'M') {
        *kind = 'M';
        s++;
        if (sscanf(s, "%d.%d.%d", m, w, d) != 3 || *m < 1 || *m > 12 || *w < 1 || *w > 5 || *d < 0 || *d > 6) return false;
        while ((*s >= '0' && *s <= '9') || *s == '.') s++;
    } else {
        *kind = 'N';
        if (*s == 'J') { *kind = 'J'; s++; }
        if (*s < '0' || *s > '9') return false;
        while (*s >= '0' && *s <= '9') { *n = *n * 10 + (*s - '0'); s++; }
        if ((*kind == 'J' && (*n < 1 || *n > 365)) || *n > 365) return false;
    }
    *time = 7200;
    if (*s == '/') {
        s++;
        if (!rule_hms(&s, time)) return false;
    }
    *p = s;
    return true;
}

static bool parse_rule(const char *s, Rule *r) {
    memset(r, 0, sizeof *r);
    int32_t off;
    if (!rule_name(&s, r->std.abbr) || !rule_hms(&s, &off)) return false;
    r->std.utoff = -off;                       /* POSIX offsets are west of UTC */
    if (*s) {
        if (!rule_name(&s, r->dst.abbr)) return false;
        r->has_dst = true;
        r->dst.isdst = true;
        r->dst.utoff = r->std.utoff + 3600;
        if (*s && *s != ',') {
            if (!rule_hms(&s, &off)) return false;
            r->dst.utoff = -off;
        }
        const char *dflt = ",M3.2.0,M11.1.0";
        const char *q = *s ? s : dflt;
        if (*q++ != ',' ||
            !rule_date(&q, &r->start.kind, &r->start.m, &r->start.w, &r->start.d, &r->start.n, &r->start.time) ||
            *q++ != ',' ||
            !rule_date(&q, &r->end.kind, &r->end.m, &r->end.w, &r->end.d, &r->end.n, &r->end.time) || *q)
            return false;
    }
    r->valid = true;
    return true;
}

static const TType *rule_type(const Rule *r, int64_t t) {
    if (!r->has_dst) return &r->std;
    int64_t y = year_of(t + r->std.utoff);
    for (int64_t yy = y - 1; yy <= y + 1; yy++) {
        /* the start is in standard time, the end in summer time */
        int64_t s = rule_day(yy, r->start.kind, r->start.m, r->start.w, r->start.d, r->start.n) + r->start.time - r->std.utoff;
        int64_t e = rule_day(yy, r->end.kind, r->end.m, r->end.w, r->end.d, r->end.n) + r->end.time - r->dst.utoff;
        if (s < e) {
            if (t >= s && t < e) return &r->dst;
        } else {
            /* southern hemisphere: summer time spans the new year */
            if (yy == y && (t >= s || t < e)) return &r->dst;
        }
    }
    return &r->std;
}

/* ---- TZif ---- */

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int64_t be64(const unsigned char *p) { return (int64_t)((uint64_t)be32(p) << 32 | be32(p + 4)); }

static bool parse_tzif(const unsigned char *b, size_t len, SeaTZ *z) {
    if (len < 44 || memcmp(b, "TZif", 4) != 0) return false;
    char version = (char)b[4];
    const unsigned char *h = b;
    int tsize = 4;
    for (int pass = 0; pass < 2; pass++) {
        if ((size_t)(h - b) + 44 > len || memcmp(h, "TZif", 4) != 0) return false;
        uint32_t isut = be32(h + 20), isstd = be32(h + 24), leapc = be32(h + 28),
                 timec = be32(h + 32), typec = be32(h + 36), charc = be32(h + 40);
        if (typec == 0 || typec > 256 || timec > 100000 || charc > 10000) return false;
        size_t data = (size_t)timec * tsize + timec + typec * 6 + charc + (size_t)leapc * (tsize + 4) + isstd + isut;
        const unsigned char *d = h + 44;
        if ((size_t)(d - b) + data > len) return false;
        if (pass == 0 && version >= '2') {      /* skip to the 64-bit part */
            h = d + data;
            tsize = 8;
            continue;
        }
        z->ntrans = timec;
        z->trans = malloc((timec ? timec : 1) * sizeof *z->trans);
        z->idx = malloc(timec ? timec : 1);
        for (uint32_t i = 0; i < timec; i++)
            z->trans[i] = tsize == 8 ? be64(d + (size_t)i * 8) : (int32_t)be32(d + (size_t)i * 4);
        d += (size_t)timec * tsize;
        for (uint32_t i = 0; i < timec; i++) {
            if (d[i] >= typec) return false;
            z->idx[i] = d[i];
        }
        d += timec;
        const unsigned char *chars = d + typec * 6;
        z->ntypes = typec;
        z->types = calloc(typec, sizeof *z->types);
        for (uint32_t i = 0; i < typec; i++) {
            const unsigned char *t = d + i * 6;
            z->types[i].utoff = (int32_t)be32(t);
            z->types[i].isdst = t[4] != 0;
            if (t[5] < charc) snprintf(z->types[i].abbr, sizeof z->types[i].abbr, "%.*s", (int)(charc - t[5]), (const char *)chars + t[5]);
        }
        if (tsize == 8) {
            /* the footer: \n rule \n */
            const unsigned char *f = h + 44 + data;
            if (f < b + len && *f == '\n') {
                const unsigned char *e = memchr(f + 1, '\n', (size_t)(b + len - f - 1));
                if (e && e - f - 1 < 128) {
                    char rule[128];
                    memcpy(rule, f + 1, (size_t)(e - f - 1));
                    rule[e - f - 1] = '\0';
                    if (rule[0]) parse_rule(rule, &z->rule);
                }
            }
        }
        return true;
    }
    return false;
}

static bool valid_name(const char *n) {
    if (!*n || *n == '/' || strlen(n) > 200) return false;
    for (const char *p = n; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '_' || c == '-' || c == '+'))
            return false;
        if (c == '/' && p[1] == '/') return false;
    }
    return strstr(n, "..") == NULL;
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    unsigned char *b = malloc(cap);
    size_t r;
    while ((r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            if (cap >= (1u << 20)) break;
            cap *= 2;
            b = realloc(b, cap);
        }
    }
    fclose(f);
    *len = n;
    return b;
}

const SeaTZ *sea_tz_load(const char *name, char *err, size_t errlen) {
    if (!valid_name(name)) {
        snprintf(err, errlen, "unknown time zone '%s'", name);
        return NULL;
    }
    pthread_mutex_lock(&g_zones_mu);
    for (SeaTZ *z = g_zones; z; z = z->next)
        if (strcmp(z->name, name) == 0) {
            pthread_mutex_unlock(&g_zones_mu);
            return z;
        }
    const char *dirs[] = { getenv("TZDIR"), "/usr/share/zoneinfo", "/usr/lib/zoneinfo", "/usr/share/lib/zoneinfo" };
    unsigned char *buf = NULL;
    size_t len = 0;
    bool any_dir = false;
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs && !buf; i++) {
        if (!dirs[i] || !*dirs[i]) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dirs[i], name);
        buf = read_file(path, &len);
        if (!buf) {
            snprintf(path, sizeof path, "%s/UTC", dirs[i]);
            FILE *f = fopen(path, "rb");
            if (f) { any_dir = true; fclose(f); }
        }
    }
    SeaTZ *z = calloc(1, sizeof *z);
    if (!buf || !parse_tzif(buf, len, z)) {
        if (!buf && !any_dir)
            snprintf(err, errlen, "unknown time zone '%s' (no tz database: install tzdata or set TZDIR)", name);
        else
            snprintf(err, errlen, "unknown time zone '%s'", name);
        free(buf);
        free(z->trans);
        free(z->idx);
        free(z->types);
        free(z);
        pthread_mutex_unlock(&g_zones_mu);
        return NULL;
    }
    free(buf);
    z->name = strdup(name);
    z->next = g_zones;
    g_zones = z;
    pthread_mutex_unlock(&g_zones_mu);
    return z;
}

const char *sea_tz_name(const SeaTZ *tz) { return tz->name; }

static const TType *type_at(const SeaTZ *z, int64_t t) {
    if (z->ntrans == 0 || t < z->trans[0]) {
        if (z->ntrans == 0 && z->rule.valid) return rule_type(&z->rule, t);
        /* before the first change: the first standard-time type */
        for (size_t i = 0; i < z->ntypes; i++) if (!z->types[i].isdst) return &z->types[i];
        return &z->types[0];
    }
    if (t >= z->trans[z->ntrans - 1] && z->rule.valid) return rule_type(&z->rule, t);
    size_t lo = 0, hi = z->ntrans;          /* the last change at or before t */
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (z->trans[mid] <= t) lo = mid; else hi = mid;
    }
    return &z->types[z->idx[lo]];
}

int64_t sea_tz_offset(const SeaTZ *tz, int64_t t, const char **abbr) {
    const TType *ty = type_at(tz, t);
    if (abbr) *abbr = ty->abbr;
    return ty->utoff;
}

int64_t sea_tz_from_wall(const SeaTZ *tz, int64_t wall) {
    /* try the offsets in effect a day before and after: the instant whose
     * wall time matches, the earlier if both do */
    int64_t o1 = sea_tz_offset(tz, wall - 86400, NULL), o2 = sea_tz_offset(tz, wall + 86400, NULL);
    int64_t u1 = wall - o1, u2 = wall - o2;
    bool ok1 = sea_tz_offset(tz, u1, NULL) == o1, ok2 = sea_tz_offset(tz, u2, NULL) == o2;
    if (ok1 && ok2) return u1 < u2 ? u1 : u2;
    if (ok1) return u1;
    if (ok2) return u2;
    /* in a gap (clocks moved forward): the wall time read with the offset
     * from before the change, as Go does */
    return wall - o1;
}
