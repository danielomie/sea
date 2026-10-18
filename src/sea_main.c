/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_common.h"
#include "sea_lexer.h"
#include "sea_parser.h"
#include "sea_compiler.h"
#include "sea_vm.h"
#include "sea_stdlib.h"
#include "sea_chunk.h"
#include "sea_string.h"
#include "sea_memory.h"
#include "sea_sched.h"
#include "sea_tools.h"     /* gen/: the scripts in tools/, embedded */
#include "sea_bundle.h"
#include "sea_prof.h"
#include "sea_cover.h"
#include <unistd.h>

#ifndef SEA_NO_READLINE
#  ifdef __has_include
#    if __has_include(<readline/readline.h>)
#      include <readline/readline.h>
#      include <readline/history.h>
#      define HAVE_READLINE 1
#    endif
#  endif
#endif

// ---- Source file reading ----

static char* read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "sea: cannot open file '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *buf = (char*)sea_malloc((size_t)size + 1);
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

// ---- Execution helpers ----

static int run_source(SeaVM *vm, const char *source, const char *name) {
    SeaParser parser;
    sea_parser_init(&parser, source);
    SeaNode *program = sea_parser_parse(&parser);

    if (!program) {
        char perr[640];
        sea_parser_error_string(&parser, name, perr, sizeof(perr));
        fprintf(stderr, "%s\n", perr);
        sea_parser_free(&parser);
        return SEA_ERR;
    }
    sea_parser_free(&parser);

    SeaChunk *chunk = sea_compile_main(program, name);
    sea_node_free(program);

    if (!chunk) {
        fprintf(stderr, "%s:%d: %s\n", name, sea_compile_error_line(),
                sea_compile_error() ? sea_compile_error() : "compile error");
        return SEA_ERR;
    }

    if (sea_prof_enabled) sea_prof_set_root(vm, chunk);
    sea_cover_set_root(vm, chunk);
    int result = sea_vm_exec(vm, chunk);
    /* Goroutines spawned by this chunk run its bytecode: wait for them before
     * freeing it (freeing first was a use-after-free on worker threads). */
    if (sea_global_sched) sea_sched_drain(sea_global_sched);
    sea_prof_finish();
    sea_cover_finish();
    sea_chunk_free(chunk);
    return result;
}

/* Load a compiled .seac file and run it (same contract as run_source). */
static int run_compiled(SeaVM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "sea: cannot open '%s'\n", path);
        return SEA_ERR;
    }
    SeaChunk *chunk = sea_chunk_read(f);
    fclose(f);
    sea_chunk_set_source(chunk);
    if (!chunk) {
        fprintf(stderr, "sea: invalid or outdated bytecode file '%s' "
                        "(recompile it with `sea compile`)\n", path);
        return SEA_ERR;
    }
    if (sea_prof_enabled) sea_prof_set_root(vm, chunk);
    sea_cover_set_root(vm, chunk);
    int result = sea_vm_exec(vm, chunk);
    /* as in run_source: goroutines run this chunk's bytecode */
    if (sea_global_sched) sea_sched_drain(sea_global_sched);
    sea_prof_finish();
    sea_cover_finish();
    sea_chunk_free(chunk);
    return result;
}

static bool is_seac_path(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && strcmp(dot, ".seac") == 0;
}

// ---- Commands ----

static int   g_script_argc = 0;
static char **g_script_argv = NULL;

/* Set by the global `--jit` flag (see docs/feature_jit.md). Off by default. */
static bool  g_jit_enabled = false;

/* Run a .sea source file or a compiled .seac file. Both get the full runtime
 * (string caches, goroutine scheduler, os.args, stdlib). With embedded set,
 * path is only a name and embedded is the source (the built-in tools). */
static void cmd_run_ex(const char *path, const char *embedded) {
    bool compiled = !embedded && is_seac_path(path);
    char *source = NULL;
    if (embedded) {
        size_t n = strlen(embedded);
        source = sea_malloc(n + 1);
        memcpy(source, embedded, n + 1);
    } else if (!compiled) {
        source = read_file(path);
        if (!source) { exit(1); }
    }

    sea_string_intern_init();
    sea_string_cache_init();
    sea_string_int_cache_init();

    /* Init global scheduler with N = nproc worker threads */
    int nproc = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) nproc = 4;
    sea_global_sched = sea_sched_new(nproc);
    sea_sched_start(sea_global_sched);

    SeaVM *vm = sea_vm_new();
    vm->shared->jit_enabled = g_jit_enabled; vm->jit_enabled = g_jit_enabled;
    sea_lib_os_set_args(vm, g_script_argc, g_script_argv);
    sea_stdlib_register(vm);
    vm->source_name = path;

    int result = compiled ? run_compiled(vm, path) : run_source(vm, source, path);

    /* Wait for any spawned goroutines to finish, then stop workers */
    sea_sched_drain(sea_global_sched);
    sea_sched_stop(sea_global_sched);

    if (source) sea_free(source, strlen(source) + 1);
    sea_vm_free(vm);
    sea_sched_free(sea_global_sched);
    sea_global_sched = NULL;
    sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();

    if (result != SEA_OK) exit(1);
}

static void cmd_run(const char *path) { cmd_run_ex(path, NULL); }

static void cmd_compile(const char *path, const char *outpath) {
    char *source = read_file(path);
    if (!source) exit(1);

    sea_string_intern_init();
    sea_string_cache_init();
    sea_string_int_cache_init();

    SeaParser parser;
    sea_parser_init(&parser, source);
    SeaNode *program = sea_parser_parse(&parser);

    if (!program) {
        char perr[640];
        sea_parser_error_string(&parser, path, perr, sizeof(perr));
        fprintf(stderr, "%s\n", perr);
        sea_parser_free(&parser);
        sea_free(source, strlen(source)+1);
        sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();
        exit(1);
    }
    sea_parser_free(&parser);

    SeaChunk *chunk = sea_compile_main(program, path);
    sea_node_free(program);
    sea_free(source, strlen(source)+1);

    if (!chunk) {
        fprintf(stderr, "%s:%d: %s\n", path, sea_compile_error_line(),
                sea_compile_error() ? sea_compile_error() : "compile error");
        sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();
        exit(1);
    }

    const char *out = outpath ? outpath : "out.seac";
    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "sea: cannot write to '%s'\n", out);
        sea_chunk_free(chunk);
        sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();
        exit(1);
    }
    int werr = sea_chunk_write(chunk, f);
    if (fclose(f) != 0) werr = -1;
    sea_chunk_free(chunk);
    if (werr != 0) {
        fprintf(stderr, "sea: failed to write '%s'\n", out);
        remove(out);
        exit(1);
    }
    sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();
    printf("compiled to %s\n", out);
}

static void cmd_bytecode(const char *path) {
    char *source = read_file(path);
    if (!source) exit(1);

    sea_string_intern_init();
    sea_string_cache_init();
    sea_string_int_cache_init();

    SeaParser parser;
    sea_parser_init(&parser, source);
    SeaNode *program = sea_parser_parse(&parser);

    if (!program) {
        char perr[640];
        sea_parser_error_string(&parser, path, perr, sizeof(perr));
        fprintf(stderr, "%s\n", perr);
        sea_parser_free(&parser);
        sea_free(source, strlen(source)+1);
        sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();
        exit(1);
    }
    sea_parser_free(&parser);

    SeaChunk *chunk = sea_compile_main(program, path);
    sea_node_free(program);
    sea_free(source, strlen(source)+1);

    if (!chunk) {
        fprintf(stderr, "%s:%d: %s\n", path, sea_compile_error_line(),
                sea_compile_error() ? sea_compile_error() : "compile error");
        sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();
        exit(1);
    }

    sea_chunk_disasm(chunk, stdout);
    sea_chunk_free(chunk);
    sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();
}

static void cmd_repl(void) {
    sea_string_intern_init();
    sea_string_cache_init();
    sea_string_int_cache_init();

    int nproc = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) nproc = 4;
    sea_global_sched = sea_sched_new(nproc);
    sea_sched_start(sea_global_sched);

    SeaVM *vm = sea_vm_new();
    vm->shared->jit_enabled = g_jit_enabled; vm->jit_enabled = g_jit_enabled;
    sea_stdlib_register(vm);
    vm->source_name = "<repl>";

    printf("Sea " SEA_VERSION " REPL (type 'exit' to quit)\n");

#ifdef HAVE_READLINE
    char *line;
    while ((line = readline(">>> ")) != NULL) {
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            free(line);
            break;
        }
        if (*line != '\0') {
            add_history(line);
            run_source(vm, line, "<repl>");
            sea_sched_drain(sea_global_sched);
        }
        free(line);
    }
#else
    char buf[4096];
    for (;;) {
        printf(">>> ");
        fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
        if (strcmp(buf, "exit") == 0 || strcmp(buf, "quit") == 0) break;
        if (len == 0) continue;
        run_source(vm, buf, "<repl>");
        sea_sched_drain(sea_global_sched);
    }
#endif

    sea_sched_stop(sea_global_sched);
    sea_vm_free(vm);
    sea_sched_free(sea_global_sched);
    sea_global_sched = NULL;
    sea_string_intern_free();
    sea_string_int_cache_free();
    sea_string_cache_free();
}

// ---- Main ----

static void usage(void) {
    fprintf(stderr,
        "Usage: sea [command] [args...]\n"
        "\n"
        "Commands:\n"
        "  sea                          Start REPL\n"
        "  sea run <file.sea>           Execute source file\n"
        "  sea compile <file> [-o out]  Compile to bytecode\n"
        "  sea bytecode <file.sea>      Print disassembly\n"
        "  sea fmt [flags] [path...]    Format Sea source (sea fmt -h)\n"
        "  sea doc [flags] [path...]    Documentation from comments (sea doc -h)\n"
        "  sea test [flags] [path...]   Run the *_test.sea files (sea test -h)\n"
        "  sea check [flags] [path...]  Find likely mistakes (sea check -h)\n"
        "  sea build [-o out] <file>    A standalone executable (sea build -h)\n"
        "  sea prof [flags] <file> ...  Run with the CPU profiler (sea prof -h)\n"
        "  sea new [--lib] <name>       Start a project (sea new -h)\n"
        "  sea get [URL[@ref]]          Install / add dependencies (sea get -h)\n"
        "  sea lsp                      Language server for editors (stdin/stdout)\n"
        "  sea version                  Print version\n"
    );
}

int main(int argc, char *argv[]) {
    const char *cover = getenv("SEA_COVER");
    if (cover && *cover) sea_cover_start(cover);

    /* A program bundled by `sea build`: every argument is the program's */
    if (sea_bundle_open()) {
        const SeaBundleEntry *m = sea_bundle_main();
        g_script_argc = argc - 1;
        g_script_argv = argv + 1;
        cmd_run_ex(m->name, m->data);
        return 0;
    }

    /* Global flags consumed before the subcommand. Only leading `--`-flags are
     * scanned (so flags after a script name stay in os.args). Currently: --jit.
     * Accepts e.g. `sea --jit run x.sea` and `sea --jit x.sea`. */
    {
        int r = 1;
        for (; r < argc; r++) {
            const char *arg = argv[r];
            if (arg[0] != '-') break;                 /* first positional: stop */
            if (strcmp(arg, "--jit") == 0) { g_jit_enabled = true; continue; }
            break;  /* leave other flags/commands (-v, --version) for the dispatch */
        }
        /* Compact: drop the consumed --jit token(s) by shifting the rest down. */
        int w = 1;
        for (; r < argc; r++) argv[w++] = argv[r];
        argc = w;
        argv[argc] = NULL;
    }
    if (g_jit_enabled) {
#ifndef SEA_JIT
        fprintf(stderr, "sea: --jit ignored (JIT not built in; rebuild with `make jit`)\n");
        g_jit_enabled = false;
#else
        sea_jit_init();
#endif
    }

    if (argc < 2) {
        cmd_repl();
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "-v") == 0 || strcmp(cmd, "--version") == 0) {
        printf("sea version " SEA_VERSION "\n");
        return 0;
    }

    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "sea run: expected filename\n");
            return 1;
        }
        cmd_run(argv[2]);
        return 0;
    }

    if (strcmp(cmd, "prof") == 0) {
        SeaProfOptions po = { 100, NULL, false, 20 };
        int i = 2;
        for (; i < argc && argv[i][0] == '-'; i++) {
            const char *a = argv[i];
            bool has_val = i + 1 < argc;
            if (strcmp(a, "-o") == 0 && has_val) po.out = argv[++i];
            else if (strcmp(a, "-hz") == 0 && has_val) po.hz = atoi(argv[++i]);
            else if (strcmp(a, "-top") == 0 && has_val) po.top = atoi(argv[++i]);
            else if (strcmp(a, "-folded") == 0) po.folded = true;
            else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { i = argc; break; }
            else { fprintf(stderr, "sea prof: unknown flag %s\n", a); i = argc; break; }
        }
        if (i >= argc || po.hz <= 0 || po.hz > 10000) {
            fprintf(stderr,
                "usage: sea prof [-o file] [-folded] [-hz N] [-top N] <file.sea|file.seac> [args...]\n"
                "\n"
                "Runs the program and reports where its CPU time goes: functions by their own\n"
                "time and by the time of what they call, and the hottest lines (on stderr, or\n"
                "in -o file). -folded writes folded stacks for flame graphs (also when the -o\n"
                "file ends in .folded). -hz: samples per second of CPU (default 100).\n");
            return 2;
        }
        if (po.out) {
            size_t n = strlen(po.out);
            if (n > 7 && strcmp(po.out + n - 7, ".folded") == 0) po.folded = true;
        }
        if (g_jit_enabled) fprintf(stderr, "sea prof: JIT-compiled code is not sampled precisely; run without --jit\n");
        g_script_argc = argc - i - 1;
        g_script_argv = argv + i + 1;
        sea_prof_start(&po);
        cmd_run(argv[i]);
        return 0;
    }

    if (strcmp(cmd, "compile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "sea compile: expected filename\n");
            return 1;
        }
        const char *outpath = NULL;
        for (int i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "-o") == 0) {
                outpath = argv[i+1];
                break;
            }
        }
        cmd_compile(argv[2], outpath);
        return 0;
    }

    if (strcmp(cmd, "bytecode") == 0) {
        if (argc < 3) {
            fprintf(stderr, "sea bytecode: expected filename\n");
            return 1;
        }
        cmd_bytecode(argv[2]);
        return 0;
    }

    /* Built-in tools written in Sea (the scripts in tools/, embedded at build time) */
    for (int i = 0; SEA_TOOLS[i].name; i++) {
        if (strcmp(cmd, SEA_TOOLS[i].name) == 0) {
            char name[64];
            snprintf(name, sizeof name, "sea %s", SEA_TOOLS[i].name);
            g_script_argc = argc - 2;
            g_script_argv = argv + 2;
            cmd_run_ex(name, SEA_TOOLS[i].src);
            return 0;
        }
    }

    // Try to run as file directly (sea file.sea [arg1 arg2 ...])
    if (argc >= 2) {
        // Check if it ends in .sea
        const char *dot = strrchr(cmd, '.');
        if (dot && strcmp(dot, ".sea") == 0) {
            /* args after the filename become os.args[1..n] */
            g_script_argc = argc - 2;
            g_script_argv = argv + 2;
            cmd_run(cmd);
            return 0;
        }
        // Check if it ends in .seac (compiled)
        if (dot && strcmp(dot, ".seac") == 0) {
            g_script_argc = argc - 2;
            g_script_argv = argv + 2;
            cmd_run(cmd);
            return 0;
        }
    }

    usage();
    return 1;
}
