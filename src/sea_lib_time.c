/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_netpoll.h"
#include "sea_string.h"
#include "sea_table.h"
#include "sea_memory.h"
#include "sea_tz.h"
#include <time.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define TIME_NS  1LL
#define TIME_US  1000LL
#define TIME_MS  1000000LL
#define TIME_SEC 1000000000LL
#define TIME_MIN 60000000000LL
#define TIME_HR  3600000000000LL

static int64_t get_int_field(SeaValue v, const char *name, bool *found) {
    *found = false;
    if (!sea_is_table(v)) return 0;
    SeaValue w = SEA_NIL_VAL;
    if (!sea_table_get_str(SEA_AS_TABLE(v), name, &w) || !sea_is_int(w)) return 0;
    *found = true;
    return sea_as_int(w);
}

static int64_t get_sec(SeaValue v) { bool f; return get_int_field(v, "__sec", &f); }
static int64_t get_ns(SeaValue v)  { bool f; return get_int_field(v, "__ns", &f); }

/* A time's zone: the local one (TZ / /etc/localtime), a fixed offset east
 * of UTC in seconds (field __off), or a named zone of the tz database
 * (field __tz: "America/Sao_Paulo", src/sea_tz.c) */
enum { Z_LOCAL, Z_FIXED, Z_NAMED };
typedef struct {
    int          kind;
    int64_t      off;      /* Z_FIXED */
    const SeaTZ *tz;       /* Z_NAMED */
} Zone;

static const Zone ZONE_LOCAL = { Z_LOCAL, 0, NULL };
static const Zone ZONE_UTC = { Z_FIXED, 0, NULL };

static void get_zone(SeaValue v, Zone *z) {
    bool has_off;
    int64_t off = get_int_field(v, "__off", &has_off);
    *z = ZONE_LOCAL;
    if (has_off) {
        z->kind = Z_FIXED;
        z->off = off;
        return;
    }
    SeaValue name = SEA_NIL_VAL;
    if (sea_is_table(v) && sea_table_get_str(SEA_AS_TABLE(v), "__tz", &name) && sea_is_string(name)) {
        char err[160];
        const SeaTZ *tz = sea_tz_load(SEA_AS_STRING(name)->data, err, sizeof err);
        if (tz) { z->kind = Z_NAMED; z->tz = tz; }
    }
}

static int64_t arg_ns(SeaValue v) {
    if (sea_is_int(v))    return sea_as_int(v);
    if (sea_is_number(v)) return (int64_t)sea_as_number(v);
    return 0;
}

static SeaValue make_time_z(SeaVM *vm, int64_t sec, int64_t ns, const Zone *z);
static SeaValue make_time(SeaVM *vm, int64_t sec, int64_t ns) { return make_time_z(vm, sec, ns, &ZONE_LOCAL); }

/* "+03:00" / "-05:30" / "Z" for an offset in seconds */
static void offset_text(int64_t off, bool colon, bool z, char *out, size_t n) {
    if (off == 0 && z) { snprintf(out, n, "Z"); return; }
    char sign = off < 0 ? '-' : '+';
    int64_t a = off < 0 ? -off : off;
    snprintf(out, n, colon ? "%c%02d:%02d" : "%c%02d%02d", sign, (int)(a / 3600), (int)(a / 60 % 60));
}

/* The broken-down time of an instant in a zone */
static void broken_in(const Zone *z, int64_t sec, struct tm *tm) {
    static _Thread_local char abbr[16];
    if (z->kind == Z_LOCAL) {
        time_t t = (time_t)sec;
        localtime_r(&t, tm);
        return;
    }
    const char *name = NULL;
    int64_t off = z->kind == Z_FIXED ? z->off : sea_tz_offset(z->tz, sec, &name);
    time_t t = (time_t)(sec + off);
    gmtime_r(&t, tm);
    tm->tm_gmtoff = (long)off;
    if (name && *name) snprintf(abbr, sizeof abbr, "%s", name);
    else if (off == 0) snprintf(abbr, sizeof abbr, "UTC");
    else offset_text(off, false, false, abbr, sizeof abbr);
    tm->tm_zone = abbr;
}

/* The broken-down time of a Time value in its zone */
static void broken(SeaValue v, struct tm *tm) {
    Zone z;
    get_zone(v, &z);
    broken_in(&z, get_sec(v), tm);
}

/* A zone: "local", "UTC", "Z", "+03:00", "-0300", "+3", or a name of the tz
 * database ("America/Sao_Paulo"). false with a message in err. */
static bool parse_zone(const char *s, Zone *zone, char *err, size_t errlen) {
    *zone = ZONE_LOCAL;
    const char *z = s;
    if (!z || !*z || strcmp(z, "local") == 0 || strcmp(z, "Local") == 0) return true;
    if (strcmp(z, "UTC") == 0 || strcmp(z, "utc") == 0 || strcmp(z, "Z") == 0 || strcmp(z, "GMT") == 0) {
        *zone = ZONE_UTC;
        return true;
    }
    if (z[0] != '+' && z[0] != '-') {
        const SeaTZ *tz = sea_tz_load(z, err, errlen);
        if (!tz) return false;
        zone->kind = Z_NAMED;
        zone->tz = tz;
        return true;
    }
    snprintf(err, errlen, "unknown time zone '%.60s' (use \"UTC\", \"local\", an offset like \"-03:00\" or a name like \"America/Sao_Paulo\")", s);
    int sign = z[0] == '-' ? -1 : 1;
    const char *p = z + 1;
    int h = 0, m = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 2) { h = h * 10 + (*p - '0'); p++; digits++; }
    if (!digits) return false;
    if (*p == ':') p++;
    digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 2) { m = m * 10 + (*p - '0'); p++; digits++; }
    if (*p || h > 14 || m > 59) return false;
    zone->kind = Z_FIXED;
    zone->off = sign * (h * 3600 + m * 60);
    return true;
}

/* Seconds since the epoch of a broken-down time in a zone (fields may be out
 * of range: month 13, day 0, ... are normalized) */
static int64_t epoch_of(struct tm *tm, const Zone *z) {
    if (z->kind == Z_FIXED) return (int64_t)timegm(tm) - z->off;
    if (z->kind == Z_NAMED) return sea_tz_from_wall(z->tz, (int64_t)timegm(tm));
    tm->tm_isdst = -1;
    return (int64_t)mktime(tm);
}

// ---- Time methods -----------------------------------------------------------

#define TM_FIELD(fname, expr)                                              \
    static SeaValue fname(SeaVM *vm, int argc, SeaValue *args) {           \
        (void)vm;                                                          \
        if (argc < 1) return sea_int_val(0);                               \
        struct tm tm;                                                      \
        broken(args[0], &tm);                                              \
        return sea_int_val(expr);                                          \
    }

TM_FIELD(tm_year,    tm.tm_year + 1900)
TM_FIELD(tm_month,   tm.tm_mon + 1)
TM_FIELD(tm_day,     tm.tm_mday)
TM_FIELD(tm_hour,    tm.tm_hour)
TM_FIELD(tm_minute,  tm.tm_min)
TM_FIELD(tm_second,  tm.tm_sec)
TM_FIELD(tm_weekday, tm.tm_wday)
TM_FIELD(tm_yearday, tm.tm_yday + 1)
TM_FIELD(tm_offset,  tm.tm_gmtoff)

static SeaValue tm_unix(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    return sea_int_val(get_sec(args[0]));
}

static SeaValue tm_unix_nano(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_number_val(0.0);
    // Return as float: nanoseconds since epoch exceed Sea's 48-bit int range
    return sea_number_val((double)get_sec(args[0]) * 1e9 + (double)get_ns(args[0]));
}

static SeaValue tm_nanosecond(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    return sea_int_val(get_ns(args[0]));
}

static SeaValue same_zone(SeaVM *vm, SeaValue t, int64_t sec, int64_t ns) {
    Zone z;
    get_zone(t, &z);
    return make_time_z(vm, sec, ns, &z);
}

/* t:add(ns): a duration in nanoseconds (an int, or a number for long ones) */
static SeaValue tm_add(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) return SEA_NIL_VAL;
    int64_t s = get_sec(args[0]), n = get_ns(args[0]);
    if (sea_is_number(args[1])) {
        double d = sea_as_number(args[1]);
        double ws = (double)(int64_t)(d / 1e9);
        s += (int64_t)ws;
        n += (int64_t)(d - ws * 1e9);
    } else {
        int64_t d = arg_ns(args[1]);
        s += d / TIME_SEC;
        n += d % TIME_SEC;
    }
    while (n < 0)         { s--; n += TIME_SEC; }
    while (n >= TIME_SEC) { s++; n -= TIME_SEC; }
    return same_zone(vm, args[0], s, n);
}

/* t:add_seconds(s): s may be large (weeks, years) or fractional */
static SeaValue tm_add_seconds(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) return SEA_NIL_VAL;
    int64_t s = get_sec(args[0]), n = get_ns(args[0]);
    if (sea_is_int(args[1])) {
        s += sea_as_int(args[1]);
    } else if (sea_is_number(args[1])) {
        double d = sea_as_number(args[1]);
        double whole = (double)(int64_t)d;
        s += (int64_t)whole;
        n += (int64_t)((d - whole) * 1e9);
        while (n < 0)         { s--; n += TIME_SEC; }
        while (n >= TIME_SEC) { s++; n -= TIME_SEC; }
    } else {
        sea_vm_error_noreturn(vm, "time.add_seconds: expected a number of seconds");
    }
    return same_zone(vm, args[0], s, n);
}

/* t:add_date(years, months, days): calendar arithmetic in the time's zone,
 * keeping the time of day (Jan 31 + 1 month is Mar 3 or 2, as in Go) */
static SeaValue tm_add_date(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return SEA_NIL_VAL;
    for (int i = 1; i < argc && i < 4; i++)
        if (!sea_is_int(args[i])) sea_vm_error_noreturn(vm, "time.add_date: expected integers (years, months, days)");
    struct tm tm;
    broken(args[0], &tm);
    tm.tm_year += argc > 1 ? (int)sea_as_int(args[1]) : 0;
    tm.tm_mon  += argc > 2 ? (int)sea_as_int(args[2]) : 0;
    tm.tm_mday += argc > 3 ? (int)sea_as_int(args[3]) : 0;
    Zone z;
    get_zone(args[0], &z);
    return make_time_z(vm, epoch_of(&tm, &z), get_ns(args[0]), &z);
}

// Returns duration as float (nanoseconds) to handle ranges beyond 48-bit int
static SeaValue tm_sub(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return sea_number_val(0.0);
    double d = (double)(get_sec(args[0]) - get_sec(args[1])) * 1e9
             + (double)(get_ns(args[0])  - get_ns(args[1]));
    return sea_number_val(d);
}

static int tm_cmp(SeaValue a, SeaValue b) {
    int64_t s1 = get_sec(a), n1 = get_ns(a), s2 = get_sec(b), n2 = get_ns(b);
    if (s1 != s2) return s1 < s2 ? -1 : 1;
    return n1 < n2 ? -1 : n1 > n2;
}

static SeaValue tm_before(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    return sea_bool_val(argc >= 2 && tm_cmp(args[0], args[1]) < 0);
}

static SeaValue tm_after(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    return sea_bool_val(argc >= 2 && tm_cmp(args[0], args[1]) > 0);
}

static SeaValue tm_equal(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    return sea_bool_val(argc >= 2 && tm_cmp(args[0], args[1]) == 0);
}

/* The same instant shown in another zone */
static SeaValue tm_utc(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return SEA_NIL_VAL;
    return make_time_z(vm, get_sec(args[0]), get_ns(args[0]), &ZONE_UTC);
}

static SeaValue tm_local(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return SEA_NIL_VAL;
    return make_time_z(vm, get_sec(args[0]), get_ns(args[0]), &ZONE_LOCAL);
}

static SeaValue tm_in_zone(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_string(args[1])) sea_vm_error_noreturn(vm, "time.in_zone: expected a zone (\"UTC\", \"local\", \"+03:00\", \"Europe/Lisbon\")");
    Zone z;
    char err[200];
    if (!parse_zone(SEA_AS_STRING(args[1])->data, &z, err, sizeof err))
        sea_vm_error_noreturn(vm, "time.in_zone: %s", err);
    return make_time_z(vm, get_sec(args[0]), get_ns(args[0]), &z);
}

/* Go layout → output, for one broken-down time. Tokens: 2006 06 01 1 Jan
 * January 02 2 _2 15 03 3 04 4 05 5 PM pm Mon Monday MST -0700 -07:00
 * Z0700 Z07:00 .000 .000000 .000000000 (fractions, also with 9s) */
static void format_layout(const char *f, const struct tm *tm, int64_t ns, char *out, size_t n) {
    static const char *const MON[] = {"January", "February", "March", "April", "May", "June", "July",
                                      "August", "September", "October", "November", "December"};
    static const char *const DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    size_t pos = 0;
#define PUT(...) do { if (pos < n) pos += (size_t)snprintf(out + pos, n - pos, __VA_ARGS__); } while (0)
    while (*f && pos + 1 < n) {
        int h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        if (!strncmp(f, "2006", 4))         { PUT("%04d", tm->tm_year + 1900); f += 4; }
        else if (!strncmp(f, "January", 7)) { PUT("%s", MON[tm->tm_mon]); f += 7; }
        else if (!strncmp(f, "Jan", 3))     { PUT("%.3s", MON[tm->tm_mon]); f += 3; }
        else if (!strncmp(f, "Monday", 6))  { PUT("%s", DAY[tm->tm_wday]); f += 6; }
        else if (!strncmp(f, "Mon", 3))     { PUT("%.3s", DAY[tm->tm_wday]); f += 3; }
        else if (!strncmp(f, "MST", 3))     { PUT("%s", tm->tm_zone ? tm->tm_zone : ""); f += 3; }
        else if (!strncmp(f, "Z07:00", 6) || !strncmp(f, "-07:00", 6) || !strncmp(f, "Z0700", 5) || !strncmp(f, "-0700", 5)) {
            bool z = *f == 'Z', colon = f[3] == ':';
            char o[16];
            offset_text(tm->tm_gmtoff, colon, z, o, sizeof o);
            PUT("%s", o);
            f += colon ? 6 : 5;
        }
        else if (!strncmp(f, "15", 2))      { PUT("%02d", tm->tm_hour); f += 2; }
        else if (!strncmp(f, "06", 2))      { PUT("%02d", tm->tm_year % 100); f += 2; }
        else if (!strncmp(f, "01", 2))      { PUT("%02d", tm->tm_mon + 1); f += 2; }
        else if (!strncmp(f, "02", 2))      { PUT("%02d", tm->tm_mday); f += 2; }
        else if (!strncmp(f, "_2", 2))      { PUT("%2d", tm->tm_mday); f += 2; }
        else if (!strncmp(f, "03", 2))      { PUT("%02d", h12); f += 2; }
        else if (!strncmp(f, "04", 2))      { PUT("%02d", tm->tm_min); f += 2; }
        else if (!strncmp(f, "05", 2))      { PUT("%02d", tm->tm_sec); f += 2; }
        else if (!strncmp(f, "PM", 2))      { PUT("%s", tm->tm_hour < 12 ? "AM" : "PM"); f += 2; }
        else if (!strncmp(f, "pm", 2))      { PUT("%s", tm->tm_hour < 12 ? "am" : "pm"); f += 2; }
        else if (f[0] == '.' && (f[1] == '0' || f[1] == '9')) {
            int digits = 0;
            char d = f[1];
            while (f[1 + digits] == d) digits++;
            char frac[16];
            snprintf(frac, sizeof frac, "%09lld", (long long)ns);
            frac[digits > 9 ? 9 : digits] = '\0';
            if (d == '9') {   /* trailing zeros dropped */
                int k = (int)strlen(frac);
                while (k > 0 && frac[k - 1] == '0') frac[--k] = '\0';
                if (k) PUT(".%s", frac);
            } else {
                PUT(".%s", frac);
            }
            f += 1 + digits;
        }
        else if (f[0] == '1' && !(f[1] >= '0' && f[1] <= '9')) { PUT("%d", tm->tm_mon + 1); f++; }
        else if (f[0] == '2' && !(f[1] >= '0' && f[1] <= '9')) { PUT("%d", tm->tm_mday); f++; }
        else if (f[0] == '3' && !(f[1] >= '0' && f[1] <= '9')) { PUT("%d", h12); f++; }
        else if (f[0] == '4' && !(f[1] >= '0' && f[1] <= '9')) { PUT("%d", tm->tm_min); f++; }
        else if (f[0] == '5' && !(f[1] >= '0' && f[1] <= '9')) { PUT("%d", tm->tm_sec); f++; }
        else { out[pos++] = *f++; }
    }
#undef PUT
    if (pos >= n) pos = n - 1;
    out[pos] = '\0';
}

static SeaValue tm_format(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_string(args[1])) return SEA_NIL_VAL;
    struct tm tm;
    broken(args[0], &tm);
    char buf[512];
    format_layout(SEA_AS_STRING(args[1])->data, &tm, get_ns(args[0]), buf, sizeof buf);
    return sea_obj_val(&sea_string_from_cstr(buf)->obj);
}

static SeaValue tm_string(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    struct tm tm;
    broken(args[0], &tm);
    char buf[64];
    format_layout("2006-01-02 15:04:05", &tm, 0, buf, sizeof buf);
    return sea_obj_val(&sea_string_from_cstr(buf)->obj);
}

/* t:iso([nanos]) → RFC 3339: 2026-10-18T14:30:00-03:00 (Z in UTC); with a true
 * argument, the fraction of a second too */
static SeaValue tm_iso(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    struct tm tm;
    broken(args[0], &tm);
    char buf[64];
    bool frac = argc > 1 && sea_is_truthy(args[1]);
    format_layout(frac ? "2006-01-02T15:04:05.999999999Z07:00" : "2006-01-02T15:04:05Z07:00",
                  &tm, get_ns(args[0]), buf, sizeof buf);
    return sea_obj_val(&sea_string_from_cstr(buf)->obj);
}

/* t:zone() → "local", "UTC", the offset ("+03:00") or the zone's name */
static SeaValue tm_zone(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    Zone z;
    get_zone(args[0], &z);
    char buf[16];
    if (z.kind == Z_NAMED) return sea_obj_val(&sea_string_from_cstr(sea_tz_name(z.tz))->obj);
    if (z.kind == Z_LOCAL) snprintf(buf, sizeof buf, "local");
    else if (z.off == 0) snprintf(buf, sizeof buf, "UTC");
    else offset_text(z.off, true, false, buf, sizeof buf);
    return sea_obj_val(&sea_string_from_cstr(buf)->obj);
}

static SeaValue make_time_z(SeaVM *vm, int64_t sec, int64_t ns, const Zone *z) {
    SeaTable *t = sea_table_new(TABLE_STRUCT);
    sea_table_set_str(t, "__sec", sea_int_val(sec));
    sea_table_set_str(t, "__ns",  sea_int_val(ns));
    if (z->kind == Z_FIXED) sea_table_set_str(t, "__off", sea_int_val(z->off));
    if (z->kind == Z_NAMED) {
        SeaString *name = sea_string_from_cstr(sea_tz_name(z->tz));
        sea_table_set_str(t, "__tz", sea_obj_val(&name->obj));
        sea_obj_release(&name->obj);
    }
    mod_set(vm, t, "unix",        tm_unix);
    mod_set(vm, t, "unix_nano",   tm_unix_nano);
    mod_set(vm, t, "year",        tm_year);
    mod_set(vm, t, "month",       tm_month);
    mod_set(vm, t, "day",         tm_day);
    mod_set(vm, t, "hour",        tm_hour);
    mod_set(vm, t, "minute",      tm_minute);
    mod_set(vm, t, "second",      tm_second);
    mod_set(vm, t, "nanosecond",  tm_nanosecond);
    mod_set(vm, t, "weekday",     tm_weekday);
    mod_set(vm, t, "yearday",     tm_yearday);
    mod_set(vm, t, "offset",      tm_offset);
    mod_set(vm, t, "add",         tm_add);
    mod_set(vm, t, "add_seconds", tm_add_seconds);
    mod_set(vm, t, "add_date",    tm_add_date);
    mod_set(vm, t, "sub",         tm_sub);
    mod_set(vm, t, "before",      tm_before);
    mod_set(vm, t, "after",       tm_after);
    mod_set(vm, t, "equal",       tm_equal);
    mod_set(vm, t, "utc",         tm_utc);
    mod_set(vm, t, "local_time",  tm_local);
    mod_set(vm, t, "in_zone",     tm_in_zone);
    mod_set(vm, t, "zone",        tm_zone);
    mod_set(vm, t, "format",      tm_format);
    mod_set(vm, t, "string",      tm_string);
    mod_set(vm, t, "iso",         tm_iso);
    return sea_obj_val(&t->obj);
}

/* For print / tostring (src/sea_show.c): a Time as text, or NULL when v is
 * not one */
SeaString *sea_time_show(SeaValue v) {
    bool a, b;
    get_int_field(v, "__sec", &a);
    get_int_field(v, "__ns", &b);
    if (!a || !b) return NULL;
    struct tm tm;
    broken(v, &tm);
    char buf[80], out[96];
    format_layout(get_ns(v) ? "2006-01-02T15:04:05.999999999Z07:00" : "2006-01-02T15:04:05Z07:00",
                  &tm, get_ns(v), buf, sizeof buf);
    snprintf(out, sizeof out, "time(%s)", buf);
    return sea_string_from_cstr(out);
}

// ---- Parsing ----------------------------------------------------------------

typedef struct {
    int year, mon, day, hour, min, sec;
    int64_t ns;
    bool has_off;
    int64_t off;
    int pm;          /* -1: no AM/PM, 0 AM, 1 PM */
} Fields;

static bool num(const char **s, int min_digits, int max_digits, int *out) {
    int v = 0, d = 0;
    while (d < max_digits && **s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; d++; }
    if (d < min_digits) return false;
    *out = v;
    return true;
}

static bool frac(const char **s, int64_t *ns) {
    if (**s != '.' && **s != ',') return true;
    (*s)++;
    int64_t v = 0;
    int d = 0;
    while (**s >= '0' && **s <= '9') {
        if (d < 9) { v = v * 10 + (**s - '0'); d++; }
        (*s)++;
    }
    if (!d) return false;
    while (d < 9) { v *= 10; d++; }
    *ns = v;
    return true;
}

static bool zone_suffix(const char **s, Fields *f) {
    const char *p = *s;
    if (*p == 'Z' || *p == 'z') { f->has_off = true; f->off = 0; *s = p + 1; return true; }
    if (*p != '+' && *p != '-') return false;
    int sign = *p == '-' ? -1 : 1;
    p++;
    int h, m = 0;
    if (!num(&p, 2, 2, &h)) return false;
    if (*p == ':') p++;
    if (*p >= '0' && *p <= '9' && !num(&p, 2, 2, &m)) return false;
    f->has_off = true;
    f->off = sign * (h * 3600 + m * 60);
    *s = p;
    return true;
}

/* ISO 8601 / RFC 3339: 2026-10-18, 2026-10-18T14:30, 2026-10-18 14:30:05.25,
 * ...Z / ...+03:00 / ...-0300 */
static bool parse_iso(const char *s, Fields *f) {
    if (!num(&s, 4, 4, &f->year) || *s++ != '-' || !num(&s, 2, 2, &f->mon) || *s++ != '-' || !num(&s, 2, 2, &f->day))
        return false;
    if (*s == 'T' || *s == 't' || *s == ' ') {
        s++;
        if (!num(&s, 2, 2, &f->hour) || *s++ != ':' || !num(&s, 2, 2, &f->min)) return false;
        if (*s == ':') {
            s++;
            if (!num(&s, 2, 2, &f->sec) || !frac(&s, &f->ns)) return false;
        }
        if (*s == ' ' && (s[1] == 'Z' || s[1] == '+' || s[1] == '-')) s++;
        if (*s && !zone_suffix(&s, f)) return false;
    }
    return *s == '\0';
}

static bool match_word(const char **s, const char *const *words, int count, int abbrev, int *index) {
    for (int i = 0; i < count; i++) {
        size_t n = abbrev ? (size_t)abbrev : strlen(words[i]);
        if (strncasecmp(*s, words[i], n) == 0) { *s += n; *index = i; return true; }
    }
    return false;
}

/* A Go layout: the tokens of format_layout */
static bool parse_layout(const char *l, const char *s, Fields *f) {
    static const char *const MON[] = {"January", "February", "March", "April", "May", "June", "July",
                                      "August", "September", "October", "November", "December"};
    static const char *const DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    int ignore;
    while (*l) {
        if (!strncmp(l, "2006", 4))         { if (!num(&s, 4, 4, &f->year)) return false; l += 4; }
        else if (!strncmp(l, "January", 7)) { if (!match_word(&s, MON, 12, 0, &f->mon)) return false; f->mon++; l += 7; }
        else if (!strncmp(l, "Jan", 3))     { if (!match_word(&s, MON, 12, 3, &f->mon)) return false; f->mon++; l += 3; }
        else if (!strncmp(l, "Monday", 6))  { if (!match_word(&s, DAY, 7, 0, &ignore)) return false; l += 6; }
        else if (!strncmp(l, "Mon", 3))     { if (!match_word(&s, DAY, 7, 3, &ignore)) return false; l += 3; }
        else if (!strncmp(l, "MST", 3))     {
            if (!strncmp(s, "UTC", 3) || !strncmp(s, "GMT", 3)) { f->has_off = true; f->off = 0; }
            while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++;
            l += 3;
        }
        else if (!strncmp(l, "Z07:00", 6) || !strncmp(l, "-07:00", 6) || !strncmp(l, "Z0700", 5) || !strncmp(l, "-0700", 5)) {
            if (!zone_suffix(&s, f)) return false;
            l += l[3] == ':' ? 6 : 5;
        }
        else if (!strncmp(l, "15", 2))      { if (!num(&s, 1, 2, &f->hour)) return false; l += 2; }
        else if (!strncmp(l, "06", 2))      { int y; if (!num(&s, 2, 2, &y)) return false; f->year = y < 69 ? 2000 + y : 1900 + y; l += 2; }
        else if (!strncmp(l, "01", 2))      { if (!num(&s, 2, 2, &f->mon)) return false; l += 2; }
        else if (!strncmp(l, "02", 2))      { if (!num(&s, 2, 2, &f->day)) return false; l += 2; }
        else if (!strncmp(l, "_2", 2))      { if (*s == ' ') s++; if (!num(&s, 1, 2, &f->day)) return false; l += 2; }
        else if (!strncmp(l, "03", 2))      { if (!num(&s, 2, 2, &f->hour)) return false; l += 2; }
        else if (!strncmp(l, "04", 2))      { if (!num(&s, 2, 2, &f->min)) return false; l += 2; }
        else if (!strncmp(l, "05", 2))      { if (!num(&s, 2, 2, &f->sec)) return false; l += 2; }
        else if (!strncmp(l, "PM", 2) || !strncmp(l, "pm", 2)) {
            if (!strncasecmp(s, "AM", 2)) f->pm = 0;
            else if (!strncasecmp(s, "PM", 2)) f->pm = 1;
            else return false;
            s += 2;
            l += 2;
        }
        else if (l[0] == '.' && (l[1] == '0' || l[1] == '9')) {
            while (l[1] == '0' || l[1] == '9') l++;
            l++;
            if (!frac(&s, &f->ns)) return false;
        }
        else if (l[0] == '1' && !(l[1] >= '0' && l[1] <= '9')) { if (!num(&s, 1, 2, &f->mon)) return false; l++; }
        else if (l[0] == '2' && !(l[1] >= '0' && l[1] <= '9')) { if (!num(&s, 1, 2, &f->day)) return false; l++; }
        else if (l[0] == '3' && !(l[1] >= '0' && l[1] <= '9')) { if (!num(&s, 1, 2, &f->hour)) return false; l++; }
        else if (l[0] == '4' && !(l[1] >= '0' && l[1] <= '9')) { if (!num(&s, 1, 2, &f->min)) return false; l++; }
        else if (l[0] == '5' && !(l[1] >= '0' && l[1] <= '9')) { if (!num(&s, 1, 2, &f->sec)) return false; l++; }
        else {
            if (*s != *l) return false;
            s++;
            l++;
        }
    }
    if (f->pm == 1 && f->hour < 12) f->hour += 12;
    if (f->pm == 0 && f->hour == 12) f->hour = 0;
    return *s == '\0';
}

// ---- Module-level functions -------------------------------------------------

/* time.date(year, month, day [, hour, min, sec [, zone]]) → Time */
static SeaValue time_date_fn(SeaVM *vm, int argc, SeaValue *args) {
    int v[6] = {1970, 1, 1, 0, 0, 0};
    int n = argc < 6 ? argc : 6;
    for (int i = 0; i < n; i++) {
        if (i >= 3 && sea_is_string(args[i])) { n = i; break; }
        if (!sea_is_int(args[i])) sea_vm_error_noreturn(vm, "time.date: expected integers (year, month, day, hour, min, sec)");
        v[i] = (int)sea_as_int(args[i]);
    }
    if (argc < 3) sea_vm_error_noreturn(vm, "time.date: expected year, month and day");
    Zone zone = ZONE_LOCAL;
    SeaValue z = argc > n && sea_is_string(args[n]) ? args[n] : SEA_NIL_VAL;
    char err[200];
    if (sea_is_string(z) && !parse_zone(SEA_AS_STRING(z)->data, &zone, err, sizeof err))
        sea_vm_error_noreturn(vm, "time.date: %s", err);
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = v[0] - 1900;
    tm.tm_mon = v[1] - 1;
    tm.tm_mday = v[2];
    tm.tm_hour = v[3];
    tm.tm_min = v[4];
    tm.tm_sec = v[5];
    return make_time_z(vm, epoch_of(&tm, &zone), 0, &zone);
}

/* time.parse(s [, layout [, zone]]) → Time | error. Without a layout: ISO
 * 8601 (2026-10-18, 2026-10-18 14:30, 2026-10-18T14:30:05.5Z, ...-03:00).
 * A text without a zone is in `zone` (default local). */
static SeaValue time_parse_fn(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_string(args[0])) return sea_make_error_str(vm, "time.parse: expected a string");
    const char *s = SEA_AS_STRING(args[0])->data;
    Fields f;
    memset(&f, 0, sizeof f);
    f.year = 1970; f.mon = 1; f.day = 1; f.pm = -1;
    bool ok;
    const char *layout = argc > 1 && sea_is_string(args[1]) ? SEA_AS_STRING(args[1])->data : NULL;
    if (layout && !*layout) layout = NULL;
    ok = layout ? parse_layout(layout, s, &f) : parse_iso(s, &f);
    char msg[256];
    if (!ok) {
        snprintf(msg, sizeof msg, "time.parse: '%.80s' does not match %s%.60s", s,
                 layout ? "the layout " : "ISO 8601 (2006-01-02, 2006-01-02 15:04:05, 2006-01-02T15:04:05Z07:00)",
                 layout ? layout : "");
        return sea_make_error_str(vm, msg);
    }
    if (f.mon < 1 || f.mon > 12 || f.day < 1 || f.day > 31 || f.hour > 23 || f.min > 59 || f.sec > 60) {
        snprintf(msg, sizeof msg, "time.parse: '%.80s': a field is out of range", s);
        return sea_make_error_str(vm, msg);
    }
    Zone zone = ZONE_LOCAL;
    if (f.has_off) {
        zone.kind = Z_FIXED;
        zone.off = f.off;
    } else if (argc > 2 && sea_is_string(args[2])) {
        char err[200];
        if (!parse_zone(SEA_AS_STRING(args[2])->data, &zone, err, sizeof err)) {
            snprintf(msg, sizeof msg, "time.parse: %s", err);
            return sea_make_error_str(vm, msg);
        }
    }
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = f.year - 1900;
    tm.tm_mon = f.mon - 1;
    tm.tm_mday = f.day;
    tm.tm_hour = f.hour;
    tm.tm_min = f.min;
    tm.tm_sec = f.sec;
    int64_t sec = epoch_of(&tm, &zone);
    /* the date must exist (2026-02-30 is not Mar 2) */
    struct tm chk;
    broken_in(&zone, sec, &chk);
    if (chk.tm_mday != f.day || chk.tm_mon != f.mon - 1) {
        snprintf(msg, sizeof msg, "time.parse: '%.80s' is not a valid date", s);
        return sea_make_error_str(vm, msg);
    }
    return make_time_z(vm, sec, f.ns, &zone);
}

/* time.days(ns) → days as a number */
static SeaValue time_days_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_number_val(0.0);
    double d = sea_is_number(args[0]) ? sea_as_number(args[0]) : (double)arg_ns(args[0]);
    return sea_number_val(d / (double)(24 * TIME_HR));
}

static SeaValue time_now(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return make_time(vm, (int64_t)ts.tv_sec, (int64_t)ts.tv_nsec);
}

static SeaValue time_unix_fn(SeaVM *vm, int argc, SeaValue *args) {
    int64_t sec = 0, ns = 0;
    if (argc >= 1) sec = sea_is_int(args[0]) ? sea_as_int(args[0]) : (int64_t)sea_as_number(args[0]);
    if (argc >= 2) ns  = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
    return make_time(vm, sec, ns);
}

static SeaValue time_sleep_fn(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return SEA_NIL_VAL;
    int64_t ns = arg_ns(args[0]);
    /* In a goroutine, a sleep of 1 ms or more parks it (the worker thread
     * runs other goroutines meanwhile) instead of blocking the thread. The
     * deadline is kept in the VM's I/O call, so the re-run after waking
     * finishes the same sleep (sea_netpoll.h). */
    if (vm->goroutine && vm->native_depth == 0 && (ns >= TIME_MS || vm->io_call.active)) {
        sea_io_begin(vm, ns / TIME_MS);
        if (sea_io_wait(vm, -1, 0) == SEA_IO_PARK) return SEA_PARK_VAL;
        sea_io_end(vm);
        return SEA_NIL_VAL;
    }
    if (ns > 0) {
        struct timespec ts;
        ts.tv_sec  = (time_t)(ns / TIME_SEC);
        ts.tv_nsec = (long)(ns % TIME_SEC);
        nanosleep(&ts, NULL);
    }
    return SEA_NIL_VAL;
}

static SeaValue time_since_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_number_val(0.0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double d = (double)((int64_t)ts.tv_sec - get_sec(args[0])) * 1e9
             + (double)((int64_t)ts.tv_nsec - get_ns(args[0]));
    return sea_number_val(d);
}

static SeaValue time_until_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_number_val(0.0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double d = (double)(get_sec(args[0]) - (int64_t)ts.tv_sec) * 1e9
             + (double)(get_ns(args[0])  - (int64_t)ts.tv_nsec);
    return sea_number_val(d);
}

static SeaValue time_hours_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_number_val(0.0);
    return sea_number_val((double)arg_ns(args[0]) / (double)TIME_HR);
}

static SeaValue time_minutes_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_number_val(0.0);
    return sea_number_val((double)arg_ns(args[0]) / (double)TIME_MIN);
}

static SeaValue time_seconds_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_number_val(0.0);
    return sea_number_val((double)arg_ns(args[0]) / (double)TIME_SEC);
}

static SeaValue time_milliseconds_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    return sea_int_val(arg_ns(args[0]) / TIME_MS);
}

static SeaValue time_microseconds_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    return sea_int_val(arg_ns(args[0]) / TIME_US);
}

static SeaValue time_nanoseconds_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    return sea_int_val(arg_ns(args[0]));
}

static SeaValue time_clock_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    return sea_number_val((double)clock() / (double)CLOCKS_PER_SEC);
}

static SeaValue time_time_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    return sea_int_val((int64_t)time(NULL));
}

void sea_lib_time_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);

    mod_set(vm, mod, "now",          time_now);
    mod_set(vm, mod, "unix",         time_unix_fn);
    mod_set(vm, mod, "time",         time_time_fn);
    mod_set(vm, mod, "sleep",        time_sleep_fn);
    mod_set(vm, mod, "clock",        time_clock_fn);
    mod_set(vm, mod, "since",        time_since_fn);
    mod_set(vm, mod, "until",        time_until_fn);
    mod_set(vm, mod, "hours",        time_hours_fn);
    mod_set(vm, mod, "minutes",      time_minutes_fn);
    mod_set(vm, mod, "seconds",      time_seconds_fn);
    mod_set(vm, mod, "milliseconds", time_milliseconds_fn);
    mod_set(vm, mod, "microseconds", time_microseconds_fn);
    mod_set(vm, mod, "nanoseconds",  time_nanoseconds_fn);
    mod_set(vm, mod, "days",         time_days_fn);
    mod_set(vm, mod, "date",         time_date_fn);
    mod_set(vm, mod, "parse",        time_parse_fn);

    sea_table_set_str(mod, "Nanosecond",  sea_int_val(TIME_NS));
    sea_table_set_str(mod, "Microsecond", sea_int_val(TIME_US));
    sea_table_set_str(mod, "Millisecond", sea_int_val(TIME_MS));
    sea_table_set_str(mod, "Second",      sea_int_val(TIME_SEC));
    sea_table_set_str(mod, "Minute",      sea_int_val(TIME_MIN));
    sea_table_set_str(mod, "Hour",        sea_int_val(TIME_HR));
    sea_table_set_str(mod, "Day",         sea_int_val(24 * TIME_HR));   /* fits the 48-bit int; 2 days do not: add_date */

    sea_vm_register_module(vm, "time", mod);
    sea_obj_release(&mod->obj);
}
