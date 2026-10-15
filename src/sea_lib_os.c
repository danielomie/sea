#include "sea_stdlib.h"
#include "sea_string.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_memory.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// os.exit(code?)
static SeaValue os_exit(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int code = 0;
    if (argc > 0 && (sea_is_int(args[0]) || sea_is_number(args[0]))) {
        code = (int)(sea_is_int(args[0]) ? sea_as_int(args[0]) : (int64_t)sea_as_number(args[0]));
    }
    exit(code);
}

// os.getenv(name)
static SeaValue os_getenv(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    const char *val = getenv(SEA_AS_STRING(args[0])->data);
    if (!val) return SEA_NIL_VAL;
    return sea_obj_val(&sea_string_from_cstr(val)->obj);
}

/* os.exec(cmd) → {code=N, output="..."}
 * Runs cmd via /bin/sh, captures stdout+stderr, returns exit code. */
static SeaValue os_exec(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    const char *cmd = SEA_AS_STRING(args[0])->data;

    /* Redirect stderr to stdout so we capture both. */
    char full[4096];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);

    FILE *fp = popen(full, "r");
    if (!fp) return SEA_NIL_VAL;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        if (len + n + 1 > cap) {
            size_t nc = cap ? cap : 4096;
            while (nc < len + n + 1) nc *= 2;
            buf = realloc(buf, nc);
            cap = nc;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    if (buf) buf[len] = '\0';

    int status = pclose(fp);
    int code   = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    SeaTable  *t   = sea_table_new(TABLE_MAP);
    SeaString *out = sea_string_new(buf ? buf : "", len);
    free(buf);

    sea_table_set_str(t, "code",   sea_int_val(code));
    sea_table_set_str(t, "output", sea_obj_val(&out->obj));
    sea_obj_release(&out->obj);
    return sea_obj_val(&t->obj);
}

/* os.readfile(path) → string or nil */
static SeaValue os_readfile(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    FILE *f = fopen(SEA_AS_STRING(args[0])->data, "rb");
    if (!f) return SEA_NIL_VAL;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        if (len + n + 1 > cap) {
            size_t nc = cap ? cap : 4096;
            while (nc < len + n + 1) nc *= 2;
            buf = realloc(buf, nc);
            cap = nc;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    fclose(f);
    if (buf) buf[len] = '\0';

    SeaValue r = sea_obj_val(&sea_string_new(buf ? buf : "", len)->obj);
    free(buf);
    return r;
}

/* os.filesize(path) → integer bytes, or nil if file doesn't exist */
static SeaValue os_filesize(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    struct stat st;
    if (stat(SEA_AS_STRING(args[0])->data, &st) != 0) return SEA_NIL_VAL;
    return sea_int_val((int64_t)st.st_size);
}

/* os.glob(dir, suffix) → sorted array of full paths matching dir/ entries ending in suffix */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static SeaValue os_glob(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_string(args[0]) || !sea_is_string(args[1])) return SEA_NIL_VAL;
    const char *dir    = SEA_AS_STRING(args[0])->data;
    const char *suffix = SEA_AS_STRING(args[1])->data;
    size_t suflen      = strlen(suffix);

    DIR *d = opendir(dir);
    if (!d) return sea_obj_val(&sea_array_new(0)->obj);

    /* Collect matching names first, then sort. */
    char **names = NULL;
    size_t count = 0, ncap = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < suflen) continue;
        if (strcmp(name + nlen - suflen, suffix) != 0) continue;

        /* Build full path */
        size_t flen = strlen(dir) + 1 + nlen + 1;
        char *path  = malloc(flen);
        snprintf(path, flen, "%s/%s", dir, name);

        if (count >= ncap) {
            ncap = ncap ? ncap * 2 : 16;
            names = realloc(names, ncap * sizeof(char *));
        }
        names[count++] = path;
    }
    closedir(d);

    if (count > 0) qsort(names, count, sizeof(char *), cmp_str);

    SeaArray *arr = sea_array_new((uint32_t)count);
    for (size_t i = 0; i < count; i++) {
        SeaValue v = sea_obj_val(&sea_string_from_cstr(names[i])->obj);
        sea_array_push(arr, v);
        sea_obj_release(sea_as_obj(v));
        free(names[i]);
    }
    free(names);

    return sea_obj_val(&arr->obj);
}

/* Script-level args set before register; os.args[1] = first script arg. */
static int    g_os_argc = 0;
static char **g_os_argv = NULL;

void sea_lib_os_set_args(SeaVM *vm, int argc, char **argv) {
    (void)vm;
    g_os_argc = argc;
    g_os_argv = argv;
}

void sea_lib_os_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);

    mod_set(vm, mod, "exit",     os_exit);
    mod_set(vm, mod, "getenv",   os_getenv);
    mod_set(vm, mod, "exec",     os_exec);
    mod_set(vm, mod, "readfile", os_readfile);
    mod_set(vm, mod, "filesize", os_filesize);
    mod_set(vm, mod, "glob",     os_glob);

    /* os.args: index 0 = nil, [1..n] = script arguments */
    SeaArray *arr = sea_array_new((uint32_t)(g_os_argc + 1));
    sea_array_push(arr, SEA_NIL_VAL);
    for (int i = 0; i < g_os_argc; i++) {
        SeaString *s = sea_string_from_cstr(g_os_argv[i]);
        sea_array_push(arr, sea_obj_val(&s->obj));
        sea_obj_release(&s->obj);
    }
    sea_table_set_str(mod, "args", sea_obj_val(&arr->obj));
    sea_obj_release(&arr->obj);

    sea_vm_register_module(vm, "os", mod);
    sea_obj_release(&mod->obj);
}
