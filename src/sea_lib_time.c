/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_netpoll.h"
#include "sea_string.h"
#include "sea_table.h"
#include "sea_memory.h"
#include <time.h>
#include <string.h>

#define TIME_NS  1LL
#define TIME_US  1000LL
#define TIME_MS  1000000LL
#define TIME_SEC 1000000000LL
#define TIME_MIN 60000000000LL
#define TIME_HR  3600000000000LL

static int64_t get_sec(SeaValue v) {
    if (!sea_is_table(v)) return 0;
    SeaValue w = SEA_NIL_VAL;
    sea_table_get_str(SEA_AS_TABLE(v), "__sec", &w);
    return sea_is_int(w) ? sea_as_int(w) : 0;
}

static int64_t get_ns(SeaValue v) {
    if (!sea_is_table(v)) return 0;
    SeaValue w = SEA_NIL_VAL;
    sea_table_get_str(SEA_AS_TABLE(v), "__ns", &w);
    return sea_is_int(w) ? sea_as_int(w) : 0;
}

static int64_t arg_ns(SeaValue v) {
    if (sea_is_int(v))    return sea_as_int(v);
    if (sea_is_number(v)) return (int64_t)sea_as_number(v);
    return 0;
}

static SeaValue make_time(SeaVM *vm, int64_t sec, int64_t ns);

// ---- Time methods -----------------------------------------------------------

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

static SeaValue tm_year(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    return sea_int_val(tm.tm_year + 1900);
}

static SeaValue tm_month(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    return sea_int_val(tm.tm_mon + 1);
}

static SeaValue tm_day(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    return sea_int_val(tm.tm_mday);
}

static SeaValue tm_hour(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    return sea_int_val(tm.tm_hour);
}

static SeaValue tm_minute(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    return sea_int_val(tm.tm_min);
}

static SeaValue tm_second(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    return sea_int_val(tm.tm_sec);
}

static SeaValue tm_nanosecond(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    return sea_int_val(get_ns(args[0]));
}

static SeaValue tm_weekday(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    return sea_int_val(tm.tm_wday);
}

static SeaValue tm_yearday(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    return sea_int_val(tm.tm_yday + 1);
}

static SeaValue tm_add(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) return SEA_NIL_VAL;
    int64_t s  = get_sec(args[0]);
    int64_t n  = get_ns(args[0]);
    int64_t d  = arg_ns(args[1]);
    s += d / TIME_SEC;
    n += d % TIME_SEC;
    if (n < 0)        { s--; n += TIME_SEC; }
    if (n >= TIME_SEC){ s++; n -= TIME_SEC; }
    return make_time(vm, s, n);
}

// Returns duration as float (nanoseconds) to handle ranges beyond 48-bit int
static SeaValue tm_sub(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return sea_number_val(0.0);
    double d = (double)(get_sec(args[0]) - get_sec(args[1])) * 1e9
             + (double)(get_ns(args[0])  - get_ns(args[1]));
    return sea_number_val(d);
}

static SeaValue tm_before(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_FALSE_VAL;
    int64_t s1 = get_sec(args[0]), n1 = get_ns(args[0]);
    int64_t s2 = get_sec(args[1]), n2 = get_ns(args[1]);
    return sea_bool_val(s1 < s2 || (s1 == s2 && n1 < n2));
}

static SeaValue tm_after(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_FALSE_VAL;
    int64_t s1 = get_sec(args[0]), n1 = get_ns(args[0]);
    int64_t s2 = get_sec(args[1]), n2 = get_ns(args[1]);
    return sea_bool_val(s1 > s2 || (s1 == s2 && n1 > n2));
}

static SeaValue tm_equal(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2) return SEA_FALSE_VAL;
    return sea_bool_val(get_sec(args[0]) == get_sec(args[1]) &&
                        get_ns(args[0])  == get_ns(args[1]));
}

// Translate a subset of Go reference-time tokens to strftime format specifiers.
// Go reference: Mon Jan 2 15:04:05 MST 2006
static void go_to_strftime(const char *fmt, char *out, size_t outsz) {
    size_t pos = 0, i = 0, flen = strlen(fmt);
    while (i < flen && pos + 8 < outsz) {
        if      (strncmp(fmt+i,"2006",4)==0) { pos+=snprintf(out+pos,outsz-pos,"%%Y"); i+=4; }
        else if (strncmp(fmt+i,"January",7)==0) { pos+=snprintf(out+pos,outsz-pos,"%%B"); i+=7; }
        else if (strncmp(fmt+i,"Monday",6)==0) { pos+=snprintf(out+pos,outsz-pos,"%%A"); i+=6; }
        else if (strncmp(fmt+i,"Jan",3)==0) { pos+=snprintf(out+pos,outsz-pos,"%%b"); i+=3; }
        else if (strncmp(fmt+i,"Mon",3)==0) { pos+=snprintf(out+pos,outsz-pos,"%%a"); i+=3; }
        else if (strncmp(fmt+i,"15",2)==0) { pos+=snprintf(out+pos,outsz-pos,"%%H"); i+=2; }
        else if (strncmp(fmt+i,"06",2)==0) { pos+=snprintf(out+pos,outsz-pos,"%%y"); i+=2; }
        else if (strncmp(fmt+i,"01",2)==0) { pos+=snprintf(out+pos,outsz-pos,"%%m"); i+=2; }
        else if (strncmp(fmt+i,"02",2)==0) { pos+=snprintf(out+pos,outsz-pos,"%%d"); i+=2; }
        else if (strncmp(fmt+i,"04",2)==0) { pos+=snprintf(out+pos,outsz-pos,"%%M"); i+=2; }
        else if (strncmp(fmt+i,"05",2)==0) { pos+=snprintf(out+pos,outsz-pos,"%%S"); i+=2; }
        else if (strncmp(fmt+i,"-0700",5)==0) { pos+=snprintf(out+pos,outsz-pos,"%%z"); i+=5; }
        else if (strncmp(fmt+i,"MST",3)==0) { pos+=snprintf(out+pos,outsz-pos,"%%Z"); i+=3; }
        else if (strncmp(fmt+i,"PM",2)==0) { pos+=snprintf(out+pos,outsz-pos,"%%p"); i+=2; }
        else if (fmt[i]=='%') { if (pos+2<outsz){out[pos++]='%'; out[pos++]='%';} i++; }
        else { out[pos++] = fmt[i++]; }
    }
    out[pos] = '\0';
}

static SeaValue tm_format(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_string(args[1])) return SEA_NIL_VAL;
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    char strfmt[512], buf[256];
    go_to_strftime(SEA_AS_STRING(args[1])->data, strfmt, sizeof(strfmt));
    strftime(buf, sizeof(buf), strfmt, &tm);
    return sea_obj_val(&sea_string_from_cstr(buf)->obj);
}

static SeaValue tm_string(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    time_t sec = (time_t)get_sec(args[0]);
    struct tm tm;
    localtime_r(&sec, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return sea_obj_val(&sea_string_from_cstr(buf)->obj);
}

static SeaValue make_time(SeaVM *vm, int64_t sec, int64_t ns) {
    SeaTable *t = sea_table_new(TABLE_STRUCT);
    sea_table_set_str(t, "__sec", sea_int_val(sec));
    sea_table_set_str(t, "__ns",  sea_int_val(ns));
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
    mod_set(vm, t, "add",         tm_add);
    mod_set(vm, t, "sub",         tm_sub);
    mod_set(vm, t, "before",      tm_before);
    mod_set(vm, t, "after",       tm_after);
    mod_set(vm, t, "equal",       tm_equal);
    mod_set(vm, t, "format",      tm_format);
    mod_set(vm, t, "string",      tm_string);
    return sea_obj_val(&t->obj);
}

// ---- Module-level functions -------------------------------------------------

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

    sea_table_set_str(mod, "Nanosecond",  sea_int_val(TIME_NS));
    sea_table_set_str(mod, "Microsecond", sea_int_val(TIME_US));
    sea_table_set_str(mod, "Millisecond", sea_int_val(TIME_MS));
    sea_table_set_str(mod, "Second",      sea_int_val(TIME_SEC));
    sea_table_set_str(mod, "Minute",      sea_int_val(TIME_MIN));
    sea_table_set_str(mod, "Hour",        sea_int_val(TIME_HR));

    sea_vm_register_module(vm, "time", mod);
    sea_obj_release(&mod->obj);
}
