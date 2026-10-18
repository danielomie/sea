/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_string.h"
#include "sea_table.h"
#include "sea_memory.h"
#include "sea_rope.h"
#include "sea_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- stdin helpers (original io.read / io.write) ------------------------- */

// io.read(fmt?)
static SeaValue io_read(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    const char *fmt = "*l"; // default: read line
    if (argc > 0 && sea_is_string(args[0])) {
        fmt = SEA_AS_STRING(args[0])->data;
    }

    if (strcmp(fmt, "*l") == 0 || strcmp(fmt, "l") == 0) {
        char buf[4096];
        if (fgets(buf, sizeof(buf), stdin) == NULL) return SEA_NIL_VAL;
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') { buf[--len] = '\0'; }
        return sea_obj_val(&sea_string_new(buf, len)->obj);
    }
    if (strcmp(fmt, "*n") == 0 || strcmp(fmt, "n") == 0) {
        double n;
        if (scanf("%lf", &n) != 1) return SEA_NIL_VAL;
        return sea_number_val(n);
    }
    if (strcmp(fmt, "*a") == 0 || strcmp(fmt, "a") == 0) {
        char *buf = NULL;
        int len = 0, cap = 0;
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            SEA_DYNARRAY_PUSH(buf, len, cap, char, (char)c);
        }
        SEA_DYNARRAY_PUSH(buf, len, cap, char, '\0');
        SeaString *s = sea_string_new(buf, (size_t)(len - 1));
        sea_free(buf, (size_t)cap);
        return sea_obj_val(&s->obj);
    }
    return SEA_NIL_VAL;
}

/* ---- Buffered file handles ----------------------------------------------- */
/*
 * Two heap-allocated handle types, packed into Sea int values as pointers.
 * A tag field at offset 0 lets io.close() dispatch correctly.
 *
 *   IoFile    — wraps a FILE* (reader or writer, C stdio buffered)
 *   IoScanner — wraps FILE* + a growable line buffer (bufio.Scanner style)
 */

#define TAG_FILE    0xF11E0001u
#define TAG_SCANNER 0xF11E0002u

typedef struct {
    unsigned int tag;   /* TAG_FILE */
    FILE        *fp;
} IoFile;

typedef struct {
    unsigned int  tag;  /* TAG_SCANNER */
    FILE         *fp;
    char         *buf;  /* current scanned line (without trailing newline) */
    size_t        cap;  /* allocated capacity of buf */
    size_t        len;  /* length of current line in buf */
} IoScanner;

#define PACK(p)          sea_int_val((int64_t)(uintptr_t)(p))
#define UNPACK_RAW(v)    ((void*)(uintptr_t)(uint64_t)sea_as_int(v))

static IoFile    *as_file(SeaValue v) { return (IoFile*)UNPACK_RAW(v); }
static IoScanner *as_scanner(SeaValue v) { return (IoScanner*)UNPACK_RAW(v); }

/* ---- io.open(path [, mode]) → handle | nil ------------------------------- */
/*
 * mode: "r" (default), "w", "a", "rb", "wb", "ab"
 * Returns a buffered file handle. C stdio uses 8 KB buffers by default.
 */
static SeaValue io_open(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    const char *path = SEA_AS_STRING(args[0])->data;
    const char *mode = "r";
    if (argc >= 2 && sea_is_string(args[1]))
        mode = SEA_AS_STRING(args[1])->data;

    FILE *fp = fopen(path, mode);
    if (!fp) return SEA_NIL_VAL;

    IoFile *h = SEA_ALLOC(IoFile);
    h->tag = TAG_FILE;
    h->fp  = fp;
    return PACK(h);
}

/* ---- io.stdin() → handle | nil ------------------------------------------- */
/*
 * A handle on standard input, for io.read_line / io.read_bytes / io.read_all.
 * It reads a dup of fd 0, so it works whatever stdin is (a pipe, a terminal, a
 * file, or a socket, which opening /dev/stdin refuses with ENXIO: Node hands
 * child processes socketpairs), and io.close() leaves fd 0 open.
 */
static SeaValue io_stdin(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    int fd = dup(0);
    if (fd < 0) return SEA_NIL_VAL;
    FILE *fp = fdopen(fd, "rb");
    if (!fp) {
        close(fd);
        return SEA_NIL_VAL;
    }
    IoFile *h = SEA_ALLOC(IoFile);
    h->tag = TAG_FILE;
    h->fp  = fp;
    return PACK(h);
}

/* ---- io.close(handle) → nil --------------------------------------------- */
static SeaValue io_close(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    void *p = UNPACK_RAW(args[0]);
    if (!p) return SEA_NIL_VAL;
    unsigned int tag = *(unsigned int*)p;
    if (tag == TAG_FILE) {
        IoFile *h = (IoFile*)p;
        fclose(h->fp);
        SEA_FREE(h, IoFile);
    } else if (tag == TAG_SCANNER) {
        IoScanner *sc = (IoScanner*)p;
        fclose(sc->fp);
        if (sc->buf) sea_free(sc->buf, sc->cap);
        SEA_FREE(sc, IoScanner);
    }
    return SEA_NIL_VAL;
}

/* ---- io.read_line(handle) → string | nil --------------------------------- */
/*
 * Reads one line, strips the trailing newline.
 * Returns nil on EOF.
 */
static SeaValue io_read_line(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    IoFile *h = as_file(args[0]);
    if (!h || h->tag != TAG_FILE) return SEA_NIL_VAL;

    /* grow-until-newline loop so lines of any length work */
    char  *line = NULL;
    size_t cap  = 0, len = 0;

    while (1) {
        if (len + 1 >= cap) {
            size_t nc = cap ? cap * 2 : 256;
            line = (char*)sea_realloc(line, cap, nc);
            cap  = nc;
        }
        int c = fgetc(h->fp);
        if (c == EOF) {
            if (len == 0) { sea_free(line, cap); return SEA_NIL_VAL; }
            break;
        }
        if (c == '\n') break;
        line[len++] = (char)c;
    }
    line[len] = '\0';
    SeaValue r = sea_obj_val(&sea_string_new(line, len)->obj);
    sea_free(line, cap);
    return r;
}

/* ---- io.read_bytes(handle, n) → string | nil ----------------------------- */
/*
 * Reads up to n bytes. Returns nil on immediate EOF.
 */
static SeaValue io_read_bytes(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    IoFile *h = as_file(args[0]);
    if (!h || h->tag != TAG_FILE) return SEA_NIL_VAL;
    size_t n = sea_is_int(args[1]) ? (size_t)sea_as_int(args[1])
                                   : (size_t)sea_as_number(args[1]);
    if (n == 0) return sea_obj_val(&sea_string_new("", 0)->obj);

    char *buf = (char*)sea_malloc(n);
    size_t got = fread(buf, 1, n, h->fp);
    if (got == 0) { sea_free(buf, n); return SEA_NIL_VAL; }
    SeaValue r = sea_obj_val(&sea_string_new(buf, got)->obj);
    sea_free(buf, n);
    return r;
}

/* ---- io.read_all(handle) → string --------------------------------------- */
static SeaValue io_read_all(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    IoFile *h = as_file(args[0]);
    if (!h || h->tag != TAG_FILE) return SEA_NIL_VAL;

    char   *buf = NULL;
    size_t  len = 0, cap = 0;
    char    tmp[8192];
    size_t  got;
    while ((got = fread(tmp, 1, sizeof(tmp), h->fp)) > 0) {
        if (len + got + 1 > cap) {
            size_t nc = cap ? cap : 8192;
            while (nc < len + got + 1) nc *= 2;
            buf = (char*)sea_realloc(buf, cap, nc);
            cap = nc;
        }
        memcpy(buf + len, tmp, got);
        len += got;
    }
    SeaValue r = sea_obj_val(&sea_string_new(buf ? buf : "", len)->obj);
    if (buf) sea_free(buf, cap);
    return r;
}

/* ---- io.write_string(handle, s) → nil ----------------------------------- */
static SeaValue io_write_string(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0]) || !sea_is_string(args[1]))
        return SEA_NIL_VAL;
    IoFile *h = as_file(args[0]);
    if (!h || h->tag != TAG_FILE) return SEA_NIL_VAL;
    SeaString *s = SEA_AS_STRING(args[1]);
    fwrite(s->data, 1, s->len, h->fp);
    return SEA_NIL_VAL;
}

/* ---- io.write_line(handle, s) → nil ------------------------------------- */
static SeaValue io_write_line(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_int(args[0]) || !sea_is_string(args[1]))
        return SEA_NIL_VAL;
    IoFile *h = as_file(args[0]);
    if (!h || h->tag != TAG_FILE) return SEA_NIL_VAL;
    SeaString *s = SEA_AS_STRING(args[1]);
    fwrite(s->data, 1, s->len, h->fp);
    fputc('\n', h->fp);
    return SEA_NIL_VAL;
}

/* ---- io.flush(handle) → nil --------------------------------------------- */
static SeaValue io_flush(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    IoFile *h = as_file(args[0]);
    if (!h || h->tag != TAG_FILE) return SEA_NIL_VAL;
    fflush(h->fp);
    return SEA_NIL_VAL;
}

/* ---- io.scanner(path) → scanner | nil ----------------------------------- */
/*
 * Opens a file for line-by-line scanning (bufio.Scanner style).
 *
 *   local sc = io.scanner("file.txt")
 *   while io.scan(sc) do
 *       print(io.scan_text(sc))
 *   end
 *   io.close(sc)
 */
static SeaValue io_scanner(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_string(args[0])) return SEA_NIL_VAL;
    FILE *fp = fopen(SEA_AS_STRING(args[0])->data, "r");
    if (!fp) return SEA_NIL_VAL;

    IoScanner *sc = SEA_ALLOC(IoScanner);
    sc->tag = TAG_SCANNER;
    sc->fp  = fp;
    sc->buf = NULL;
    sc->cap = 0;
    sc->len = 0;
    return PACK(sc);
}

/* ---- io.scan(scanner) → bool ------------------------------------------- */
/*
 * Advances the scanner to the next line.
 * Returns true if a line was read, false on EOF.
 */
static SeaValue io_scan(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return sea_bool_val(false);
    IoScanner *sc = as_scanner(args[0]);
    if (!sc || sc->tag != TAG_SCANNER) return sea_bool_val(false);

    sc->len = 0;
    while (1) {
        if (sc->len + 1 >= sc->cap) {
            size_t nc = sc->cap ? sc->cap * 2 : 256;
            sc->buf = (char*)sea_realloc(sc->buf, sc->cap, nc);
            sc->cap = nc;
        }
        int c = fgetc(sc->fp);
        if (c == EOF) {
            if (sc->len == 0) return sea_bool_val(false);
            break;
        }
        if (c == '\n') break;
        sc->buf[sc->len++] = (char)c;
    }
    sc->buf[sc->len] = '\0';
    return sea_bool_val(true);
}

/* ---- io.scan_text(scanner) → string ------------------------------------ */
static SeaValue io_scan_text(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_int(args[0])) return SEA_NIL_VAL;
    IoScanner *sc = as_scanner(args[0]);
    if (!sc || sc->tag != TAG_SCANNER || !sc->buf) return SEA_NIL_VAL;
    return sea_obj_val(&sea_string_new(sc->buf, sc->len)->obj);
}

/* ---- io.write(...) / io.write_err(...) → nil ------------------------------
 * The values (strings as they are, bytes and all; others as tostring shows
 * them) on stdout / stderr, with no separator and no line break. */
static void write_values(SeaVM *vm, FILE *fp, int argc, SeaValue *args) {
    for (int i = 0; i < argc; i++) {
        SeaString *s = sea_is_string(args[i]) || sea_is_rope(args[i])
                       ? sea_value_get_flat_string(args[i]) : sea_vm_tostring(vm, args[i]);
        fwrite(s->data, 1, s->len, fp);
        sea_obj_release(&s->obj);
    }
    fflush(fp);
}

static SeaValue io_write(SeaVM *vm, int argc, SeaValue *args) {
    write_values(vm, stdout, argc, args);
    return SEA_NIL_VAL;
}

static SeaValue io_write_err(SeaVM *vm, int argc, SeaValue *args) {
    write_values(vm, stderr, argc, args);
    return SEA_NIL_VAL;
}

/* ---- Registration -------------------------------------------------------- */

void sea_lib_io_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);

    /* stdin helpers */
    mod_set(vm, mod, "read",         io_read);
    mod_set(vm, mod, "write",        io_write);
    mod_set(vm, mod, "write_err",    io_write_err);

    /* buffered file I/O */
    mod_set(vm, mod, "open",         io_open);
    mod_set(vm, mod, "stdin",        io_stdin);
    mod_set(vm, mod, "close",        io_close);
    mod_set(vm, mod, "read_line",    io_read_line);
    mod_set(vm, mod, "read_bytes",   io_read_bytes);
    mod_set(vm, mod, "read_all",     io_read_all);
    mod_set(vm, mod, "write_string", io_write_string);
    mod_set(vm, mod, "write_line",   io_write_line);
    mod_set(vm, mod, "flush",        io_flush);
    mod_set(vm, mod, "scanner",      io_scanner);
    mod_set(vm, mod, "scan",         io_scan);
    mod_set(vm, mod, "scan_text",    io_scan_text);

    sea_vm_register_module(vm, "io", mod);
    sea_obj_release(&mod->obj);
}
