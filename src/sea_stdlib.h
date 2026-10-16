#pragma once
#include "sea_vm.h"
#include "sea_table.h"
#include "sea_string.h"

/*
 * Standard library registration.
 * Registers all built-in functions and modules into a VM.
 */

void sea_stdlib_register(SeaVM *vm);

// Individual library registrars
void sea_lib_core_register(SeaVM *vm);
void sea_lib_math_register(SeaVM *vm);
void sea_lib_string_register(SeaVM *vm);
void sea_lib_io_register(SeaVM *vm);
void sea_lib_os_register(SeaVM *vm);
void sea_lib_os_set_args(SeaVM *vm, int argc, char **argv);
void sea_lib_sync_register(SeaVM *vm);
void sea_lib_map_register(SeaVM *vm);
void sea_lib_time_register(SeaVM *vm);
void sea_lib_net_register(SeaVM *vm);
void sea_lib_parser_register(SeaVM *vm);   /* require("sea/parser") */
void sea_lib_sort_register(SeaVM *vm);

// Internal helper used by stdlib lib files
void mod_set(SeaVM *vm, SeaTable *mod, const char *name,
             SeaValue (*fn)(SeaVM*, int, SeaValue*));

// Public API (used by external .so modules via sea.h)
SeaTable *sea_mod_new(void);

// Exposed for printf
SeaValue sea_str_format(SeaVM *vm, int argc, SeaValue *args);
void      sea_mod_set(SeaTable *mod, const char *name,
                      SeaValue (*fn)(SeaVM*, int, SeaValue*));
