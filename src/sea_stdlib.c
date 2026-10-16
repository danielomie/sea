#include <errno.h>
#include "sea_bigint.h"
#include "sea_stdlib.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_map.h"
#include "sea_struct.h"
#include "sea_typed_array.h"
#include "sea_channel.h"
#include "sea_memory.h"
#include "sea_parser.h"
#include "sea_compiler.h"
#include "sea_ast.h"
#include "sea_chunk.h"
#include "sea_sched.h"
#include "sea_gc.h"
#include <math.h>
#include <unistd.h>
#include <limits.h>
#include <dlfcn.h>

// ---- Module helpers ----

void mod_set(SeaVM *vm, SeaTable *mod, const char *name,
             SeaValue (*fn)(SeaVM*, int, SeaValue*)) {
    (void)vm;
    sea_mod_set(mod, name, fn);
}

/* Public API: create an empty module table (rc=1). */
SeaTable *sea_mod_new(void) {
    return sea_table_new(TABLE_MAP);
}

/* Public API: register a native function in a module table. */
void sea_mod_set(SeaTable *mod, const char *name,
                 SeaValue (*fn)(SeaVM*, int, SeaValue*)) {
    SeaClosure *cl  = sea_native_closure(name, fn);
    SeaString  *key = sea_string_intern(name, strlen(name));
    sea_table_set(mod, sea_obj_val(&key->obj), sea_obj_val(&cl->obj));
    sea_obj_release(&key->obj);
    sea_obj_release(&cl->obj);
}

// ---- Core library ----

// print(...)
static SeaValue fn_print(SeaVM *vm, int argc, SeaValue *args) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) putchar('\t');
        SeaString *s = sea_vm_tostring(vm, args[i]);
        fputs(s->data, stdout);
        sea_obj_release(&s->obj);
    }
    putchar('\n');
    fflush(stdout);
    return SEA_NIL_VAL;
}

// printf(fmt, ...)
static SeaValue fn_printf(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue s = sea_str_format(vm, argc, args);
    if (sea_is_string(s)) {
        fputs(SEA_AS_STRING(s)->data, stdout);
        fflush(stdout);
        sea_obj_release(sea_as_obj(s));
    }
    return SEA_NIL_VAL;
}

// type(v)
static SeaValue fn_type(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_obj_val(&sea_string_from_cstr("nil")->obj);
    const char *name = sea_type_name(args[0]);
    SeaString *s = sea_string_intern(name, strlen(name));
    return sea_obj_val(&s->obj);   /* intern's reference goes to the caller */
}

// tostring(v)
static SeaValue fn_tostring(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return sea_obj_val(&sea_string_from_cstr("nil")->obj);
    SeaString *s = sea_vm_tostring(vm, args[0]);
    /* Return rc=1; caller owns the reference */
    return sea_obj_val(&s->obj);
}

// tonumber(v)
static SeaValue fn_tonumber(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaValue v = args[0];
    if (sea_is_number(v) || sea_is_int(v)) return v;
    if (sea_is_bigint(v)) return sea_number_val(sea_bigint_to_double(SEA_AS_BIGINT(v)));
    if (sea_is_string(v)) {
        char *end;
        const char *str = SEA_AS_STRING(v)->data;
        // Try integer first (within Sea's 48-bit ints; larger ones become numbers)
        errno = 0;
        long long iv = strtoll(str, &end, 0);
        if (*end == '\0' && end != str && errno == 0 && iv >= -(1LL << 47) && iv < (1LL << 47))
            return sea_int_val((int64_t)iv);
        double n = strtod(str, &end);
        if (*end == '\0' && end != str) return sea_number_val(n);
    }
    return SEA_NIL_VAL;
}

// error(msg)
static SeaValue fn_error(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue msg = (argc > 0) ? args[0] : SEA_NIL_VAL;
    return sea_make_error(vm, msg);
}

// is_error(v)
static SeaValue fn_is_error(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_FALSE_VAL;
    return sea_bool_val(sea_is_error(args[0]));
}

// assert(v, msg?)
static SeaValue fn_assert(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_truthy(args[0])) {
        const char *msg = "assertion failed";
        if (argc > 1) {
            SeaString *s = sea_vm_tostring(vm, args[1]);
            fprintf(stderr, "assertion failed: %s\n", s->data);
            sea_obj_release(&s->obj);
            exit(1);
        }
        fprintf(stderr, "%s\n", msg);
        exit(1);
    }
    return sea_val_ref(args[0]);
}

// len(v)
static SeaValue fn_len(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return sea_int_val(0);
    SeaValue v = args[0];
    if (sea_is_string(v))     return sea_int_val(SEA_AS_STRING(v)->len);
    if (sea_is_rope(v))       return sea_int_val((int64_t)SEA_AS_ROPE(v)->total_len);
    if (sea_is_array(v))      return sea_int_val(SEA_AS_ARRAY(v)->len);
    if (sea_is_table(v))      return sea_int_val(SEA_AS_TABLE(v)->count);
    if (sea_is_map(v))        return sea_int_val(SEA_AS_MAP(v)->count);
    if (sea_is_struct(v))     return sea_int_val(SEA_AS_STRUCT(v)->field_count);
    if (sea_is_int_array(v))  return sea_int_val(SEA_AS_INT_ARRAY(v)->len);
    if (sea_is_float_array(v)) return sea_int_val(SEA_AS_FLOAT_ARRAY(v)->len);
    return sea_int_val(0);
}

// chan(n?)
static SeaValue fn_chan(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    uint32_t cap = 0;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0]))) {
        cap = sea_is_int(args[0]) ? (uint32_t)sea_as_int(args[0]) : (uint32_t)sea_as_number(args[0]);
    }
    SeaChannel *ch = sea_chan_new(cap);
    /* Return rc=1; the VM register is the sole owner */
    return sea_obj_val(&ch->obj);
}

// close(ch)
static SeaValue fn_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc > 0 && sea_is_channel(args[0])) {
        sea_chan_close(SEA_AS_CHANNEL(args[0]));
    }
    return SEA_NIL_VAL;
}

// panic(v)
/* panic(v) called as a function value (the statement form is OP_PANIC):
 * the same unwinding (sea_vm_raise takes an owned value) */
static SeaValue fn_panic(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue v = (argc > 0) ? args[0] : SEA_NIL_VAL;
    sea_vm_raise(vm, sea_val_ref(v));
}

// recover()
static SeaValue fn_recover(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    return sea_vm_recover(vm);
}

/* ---- require() helpers ---- */

/* Return values for require_try_* helpers */
#define REQUIRE_NONE    0
#define REQUIRE_SOURCE  1   /* .sea source file */
#define REQUIRE_NATIVE  2   /* .so / .dylib native library */

/* Try dir/name.sea then dir/name/init.sea; write found path into out.
 * dir NULL: name is a path of its own (an absolute one). */
static int require_try_dir(const char *dir, const char *name,
                            char *out, size_t outsz) {
    const char *sep = dir ? "/" : "";
    if (!dir) dir = "";
    snprintf(out, outsz, "%s%s%s.sea", dir, sep, name);
    if (access(out, R_OK) == 0) return REQUIRE_SOURCE;
    snprintf(out, outsz, "%s%s%s/init.sea", dir, sep, name);
    if (access(out, R_OK) == 0) return REQUIRE_SOURCE;
    return REQUIRE_NONE;
}

/* Try dir/name.so (or .dylib on macOS). */
static int require_try_native(const char *dir, const char *name,
                               char *out, size_t outsz) {
    const char *sep = dir ? "/" : "";
    if (!dir) dir = "";
#if defined(__APPLE__)
    snprintf(out, outsz, "%s%s%s.dylib", dir, sep, name);
    if (access(out, R_OK) == 0) return REQUIRE_NATIVE;
#endif
    snprintf(out, outsz, "%s%s%s.so", dir, sep, name);
    if (access(out, R_OK) == 0) return REQUIRE_NATIVE;
    return REQUIRE_NONE;
}

// require(name)
static SeaValue fn_require(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    const char *name = SEA_AS_STRING(args[0])->data;

    /* 1. Built-in / pre-registered modules (math, string, map, …) */
    SeaValue mod = SEA_NIL_VAL;
    if (sea_globals_get_str(vm, name, &mod))
        return sea_val_ref(mod);

    /* 2. Resolve file path
     *    For each search directory, try .sea source first, then .so native.
     *    Search order:
     *      a) directory of the currently-executing source file
     *      b) current working directory
     *      c) each directory in SEA_PATH (colon-separated)            */
    char path[PATH_MAX];
    int  found = REQUIRE_NONE;

    /* Helper: try both .sea and .so in a given directory */
#define TRY_DIR(dir_) do { \
    found = require_try_dir((dir_), name, path, sizeof(path)); \
    if (!found) found = require_try_native((dir_), name, path, sizeof(path)); \
} while (0)

    /* An absolute path names the file itself: no search */
    bool absolute = name[0] == '/';
    if (absolute) TRY_DIR(NULL);

    if (!found && !absolute && vm->source_name) {
        char dir[PATH_MAX];
        strncpy(dir, vm->source_name, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else strcpy(dir, ".");
        TRY_DIR(dir);
    }

    if (!found && !absolute) TRY_DIR(".");

    if (!found && !absolute) {
        const char *sea_path = getenv("SEA_PATH");
        if (sea_path) {
            char buf[4096];
            strncpy(buf, sea_path, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *tok = strtok(buf, ":");
            while (tok && !found) {
                TRY_DIR(tok);
                tok = strtok(NULL, ":");
            }
        }
    }
#undef TRY_DIR

    if (!found) {
        fprintf(stderr, "sea: module '%s' not found\n", name);
        return SEA_NIL_VAL;
    }

    /* 3. Resolve to canonical absolute path (cache key) */
    char abspath[PATH_MAX];
    if (!realpath(path, abspath))
        strncpy(abspath, path, sizeof(abspath) - 1);

    /* 4. Check module cache */
    SeaString *key     = sea_string_intern(abspath, strlen(abspath));
    SeaValue   key_val = sea_obj_val(&key->obj);
    SeaValue   cached  = SEA_NIL_VAL;
    if (sea_map_get(vm_loaded(vm), key_val, &cached)) {
        sea_obj_release(&key->obj);
        return sea_val_ref(cached);   /* natives return owned values */
    }

    /* 5. Guard against circular require: cache NIL before executing */
    sea_map_set(vm_loaded(vm), key_val, SEA_NIL_VAL);

    /* ---- Native shared library path ---- */
    if (found == REQUIRE_NATIVE) {
        void *handle = dlopen(abspath, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            fprintf(stderr, "sea: dlopen '%s': %s\n", abspath, dlerror());
            sea_obj_release(&key->obj);
            return SEA_NIL_VAL;
        }

        typedef SeaValue (*SeaOpenFn)(SeaVM *vm);
        SeaOpenFn open_fn = (SeaOpenFn)(uintptr_t)dlsym(handle, "sea_open");
        if (!open_fn) {
            fprintf(stderr, "sea: '%s' does not export 'sea_open'\n", abspath);
            dlclose(handle);
            sea_obj_release(&key->obj);
            return SEA_NIL_VAL;
        }

        SeaValue result = open_fn(vm);

        /* Cache the module (sea_map_set retains); sea_open's reference goes
         * to the caller. */
        sea_map_set(vm_loaded(vm), key_val, result);
        sea_obj_release(&key->obj);
        return result;
    }

    /* 6. Read source file */
    FILE *f = fopen(abspath, "rb");
    if (!f) {
        fprintf(stderr, "sea: cannot open '%s'\n", abspath);
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char*)sea_malloc((size_t)sz + 1);
    size_t nr = fread(src, 1, (size_t)sz, f);
    src[nr] = '\0';
    fclose(f);

    /* 7. Parse */
    SeaParser parser;
    sea_parser_init(&parser, src);
    SeaNode *program = sea_parser_parse(&parser);
    sea_free(src, (size_t)sz + 1);

    if (!program) {
        char perr[PATH_MAX + 600];
        sea_parser_error_string(&parser, abspath, perr, sizeof(perr));
        fprintf(stderr, "sea: %s\n", perr);
        sea_parser_free(&parser);
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }
    sea_parser_free(&parser);

    /* 8. Compile */
    SeaChunk *chunk = sea_compile(program, abspath);
    sea_node_free(program);

    if (!chunk) {
        fprintf(stderr, "sea: %s:%d: %s\n", abspath, sea_compile_error_line(),
                sea_compile_error() ? sea_compile_error() : "compile error");
        sea_obj_release(&key->obj);
        return SEA_NIL_VAL;
    }

    /* 9. Execute module in the current VM (re-entrant) */
    const char *saved_source = vm->source_name;
    vm->source_name = abspath;

    SeaValue result = SEA_NIL_VAL;
    sea_vm_exec_mod(vm, chunk, &result);

    /* Transfer chunk ownership to the VM: closures returned from the module
     * hold cl->chunk pointers into this chunk's proto tree.  We must NOT free
     * the chunk here; the VM will free it on sea_vm_free(). */
    chunk->next_owned    = vm->module_chunks;
    vm->module_chunks    = chunk;

    vm->source_name = saved_source;

    /* 10. Update cache with actual result and return: the cache retains;
     * the module's return value (+1) goes to the caller. */
    sea_map_set(vm_loaded(vm), key_val, result);
    sea_obj_release(&key->obj);
    return result;
}

// int_array(cap?) → SeaIntArray
static SeaValue fn_int_array(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    uint32_t cap = 8;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0])))
        cap = (uint32_t)(sea_is_int(args[0]) ? sea_as_int(args[0]) : sea_as_number(args[0]));
    SeaIntArray *a = sea_int_array_new(cap);
    return sea_obj_val(&a->obj);
}

// float_array(cap?) → SeaFloatArray
static SeaValue fn_float_array(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    uint32_t cap = 8;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0])))
        cap = (uint32_t)(sea_is_int(args[0]) ? sea_as_int(args[0]) : sea_as_number(args[0]));
    SeaFloatArray *a = sea_float_array_new(cap);
    return sea_obj_val(&a->obj);
}

void sea_lib_core_register(SeaVM *vm) {
    sea_vm_register_native(vm, "print",    fn_print);
    sea_vm_register_native(vm, "printf",   fn_printf);
    sea_vm_register_native(vm, "type",     fn_type);
    sea_vm_register_native(vm, "tostring", fn_tostring);
    sea_vm_register_native(vm, "tonumber", fn_tonumber);
    sea_vm_register_native(vm, "error",    fn_error);
    sea_vm_register_native(vm, "is_error", fn_is_error);
    sea_vm_register_native(vm, "assert",   fn_assert);
    sea_vm_register_native(vm, "len",      fn_len);
    sea_vm_register_native(vm, "chan",      fn_chan);
    sea_vm_register_native(vm, "close",    fn_close);
    sea_vm_register_native(vm, "panic",    fn_panic);
    sea_vm_register_native(vm, "recover",  fn_recover);
    sea_vm_register_native(vm, "require",  fn_require);
    sea_vm_register_native(vm, "int_array",   fn_int_array);
    sea_vm_register_native(vm, "float_array", fn_float_array);
}

void sea_stdlib_register(SeaVM *vm) {
    sea_lib_core_register(vm);
    sea_lib_math_register(vm);
    sea_lib_string_register(vm);
    sea_lib_map_register(vm);
    sea_lib_io_register(vm);
    sea_lib_os_register(vm);
    sea_lib_sync_register(vm);
    sea_lib_time_register(vm);
    sea_lib_gc_register(vm);
    sea_lib_net_register(vm);
    sea_lib_parser_register(vm);
    sea_lib_sort_register(vm);
    sea_lib_bigint_register(vm);
    sea_lib_utf8_register(vm);
    sea_lib_fs_register(vm);
    sea_lib_re_register(vm);
}
