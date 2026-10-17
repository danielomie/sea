/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: MIT
 */

/*
 * hello.c — minimal example of an external Sea native library.
 *
 * Build:
 *   make lib SRC=examples/hello.c OUT=examples/hello.so
 *
 * Use in Sea:
 *   local h = require("./hello")
 *   h.greet("world")    -- prints "Hello, world!"
 *   print(h.add(3, 4))  -- prints 7
 */

#include "sea.h"
#include <stdio.h>

/* hello.greet(name) — prints "Hello, <name>!" */
static SeaValue fn_greet(SeaVM *vm, int argc, SeaValue *args) {
    const char *who = "world";
    if (argc >= 1 && sea_is_string(args[0]))
        who = SEA_AS_STRING(args[0])->data;
    printf("Hello, %s!\n", who);
    return SEA_NIL_VAL;
}

/* hello.add(a, b) — returns a + b (integers) */
static SeaValue fn_add(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) return sea_int_val(0);
    int64_t a = sea_is_int(args[0]) ? sea_as_int(args[0])
                                     : (int64_t)sea_as_number(args[0]);
    int64_t b = sea_is_int(args[1]) ? sea_as_int(args[1])
                                     : (int64_t)sea_as_number(args[1]);
    return sea_int_val(a + b);
}

/* hello.version() — returns a version string */
static SeaValue fn_version(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    SeaString *s = sea_string_intern("1.0.0", 5);
    return sea_obj_val(&s->obj);
}

/* Module entry point — must be named sea_open */
SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "greet",   fn_greet);
    sea_mod_set(mod, "add",     fn_add);
    sea_mod_set(mod, "version", fn_version);

    /* Return rc=1 — fn_require takes ownership and balances after caching. */
    return sea_obj_val(&mod->obj);
}
