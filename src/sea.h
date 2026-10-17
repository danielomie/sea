/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: MIT
 */

/*
 * sea.h — Public C API for the Sea language.
 *
 * Include this single header when writing an external Sea module (native
 * shared library loaded by require()).
 *
 * -----------------------------------------------------------------------
 * External module convention
 * -----------------------------------------------------------------------
 * Compile your module as a shared library and export a function named
 * `sea_open`:
 *
 *   SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
 *       SeaTable *mod = sea_mod_new();
 *       sea_mod_set(mod, "hello", fn_hello);
 *       // ... more functions ...
 *       return sea_obj_val(&mod->obj);   // owned (+1): require takes it
 *   }
 *
 * Ownership (docs/memory.md): native functions BORROW their arguments (never
 * release them) and RETURN AN OWNED value: a fresh object (rc=1) as is, an
 * argument or a container element via sea_val_ref(v). Containers retain what
 * you store; sea_vm_call() borrows its args and returns an owned result.
 *
 * Build command (Linux):
 *   gcc -shared -fPIC -o mymod.so mymod.c -I/path/to/sea/src
 *
 * Usage in Sea code:
 *   local m = require("./mymod")
 *   m.hello()
 * -----------------------------------------------------------------------
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---- core types ---- */
#include "sea_common.h"
#include "sea_value.h"
#include "sea_object.h"
#include "sea_string.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_map.h"
#include "sea_struct.h"
#include "sea_typed_array.h"
#include "sea_bigint.h"
#include "sea_utf8.h"
#include "sea_vm.h"

/* ---- visibility ---- */
#if defined(_WIN32)
#  define SEA_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define SEA_EXPORT __attribute__((visibility("default")))
#else
#  define SEA_EXPORT
#endif

/* ---- native function type ---- */
typedef SeaValue (*SeaNativeFn)(SeaVM *vm, int argc, SeaValue *args);

/* ---- module helpers ---- */

/*
 * Create an empty module table (rc=1). Return it from sea_open() as
 * sea_obj_val(&mod->obj): that reference goes to require().
 */
SeaTable *sea_mod_new(void);

/*
 * Register a native function `fn` under `name` in module table `mod`.
 * Call this before returning the module from sea_open().
 */
void sea_mod_set(SeaTable *mod, const char *name, SeaNativeFn fn);

#ifdef __cplusplus
}
#endif
