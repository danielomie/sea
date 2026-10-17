/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "sea_ast.h"
#include "sea_chunk.h"

/*
 * AST-to-bytecode compiler.
 * Single-pass, register-based.
 */

/* Per-register metadata tracked during compilation.
 * Replaces the former separate reg_is_int[] and reg_owned[] arrays and
 * provides hooks for future constant-folding and copy-propagation passes. */
typedef struct {
    uint8_t is_int;    /* 1 = register provably holds int64 (enables fast-path opcodes) */
    uint8_t is_scalar; /* 1 = value needs no refcounting: nil, bool, number, or a
                          pinned string constant (is_int implies it) */
    uint8_t is_owned;  /* temporaries only: 1 = holds a reference (+1) the code
                          must consume — move into a local, pass, return or
                          release (docs/memory.md). Named locals always own. */
    /* Etapa 2 (constant folding): */
    uint8_t is_const;  /* 1 = integer value is statically known (valid when is_int=1) */
    int64_t const_int; /* known integer value (valid when is_int && is_const) */
    /* Etapa 3 (copy propagation): */
    uint8_t has_copy;  /* 1 = this reg is a transparent alias of copy_of */
    uint8_t copy_of;   /* source register index (valid when has_copy=1) */
    /* Etapa 4 (dead write elimination): */
    int     dwe_write_pos; /* chunk position of last pure write (-1 = none) */
    uint8_t dwe_read;      /* 1 = register was read since last pure write */
} SeaRegMeta;

/* Escape analysis: identifies local struct literals safe for arena allocation.
 * A candidate is a NODE_TABLE with all-string keys that is the sole initializer
 * of a `local x = {...}` declaration and never escapes the function. */
#define SEA_ESCAPE_MAX_CANDS 64

typedef struct {
    SeaNode *table_node;   /* the NODE_TABLE AST node */
    char    *local_name;   /* name of the owning local */
    bool     escaped;      /* true = must use heap */
} SeaEscapeCand;

typedef struct {
    SeaEscapeCand cands[SEA_ESCAPE_MAX_CANDS];
    int           count;
    /* collect mode (sea_collect_captured): instead of marking candidates,
     * gather every identifier referenced inside a closure body. Names point
     * into the AST (valid while compiling). */
    bool          collect;
    const char  **names;
    int           name_count, name_cap;
} SeaEscapeInfo;

// Local variable entry
typedef struct {
    char   *name;
    int     depth;      // scope depth
    bool    captured;   // is it captured by a closure?
    bool    is_const;   // declared with `const` — assignment is a compile error
    int     reg;        // register number
} SeaLocal;

// Upvalue entry (for the current function)
typedef struct {
    bool    is_local;   // true = refers to local in direct enclosing scope
    bool    is_const;   // source local was declared with `const`
    uint8_t idx;        // register or upvalue index
    char   *name;
} SeaUpvalEntry;

// Compiler state (one per function scope)
typedef struct SeaCompiler {
    struct SeaCompiler *parent;    // enclosing function compiler

    SeaChunk *chunk;

    SeaLocal locals[SEA_MAX_REGISTERS];
    int      local_count;
    int      scope_depth;

    // Next available register (stack top)
    int      reg_top;

    SeaUpvalEntry upvalues[SEA_MAX_UPVALUES];
    int           upval_count;

    // Break/continue patch lists (indices into chunk->code)
    int *break_list;
    int  break_count;
    int  break_cap;
    int *continue_list;
    int  continue_count;
    int  continue_cap;

    // Defer count for current function
    int defer_count;

    /* Ownership cleanup for break/continue: locals from this index on were
     * declared inside the innermost loop's body (-1 outside loops). */
    int loop_body_local;

    /* The function body being compiled (for whole-function scans) and, for
     * `local function name`, the name: references to it in the body load the
     * running closure (OP_LOAD_SELF / OP_CALL_SELF) instead of capturing the
     * local — which would make the closure own itself (a cycle). self_ok is
     * false when the enclosing function ever assigns the name. */
    SeaNode    *body;
    const char *self_name;
    bool        self_ok;
    const char *pending_self;   /* set by `local function name` for its NODE_FUNC */

    bool had_error;
    char err_msg[512];

    /* Per-register metadata: type hints, ownership, and future optimizations.
     * Replaces the former reg_owned[] and reg_is_int[] arrays. */
    SeaRegMeta reg_meta[SEA_MAX_REGISTERS];


    /* Escape analysis results for this function's body. */
    SeaEscapeInfo escape_info;
    SeaEscapeInfo captured;     /* names referenced by closures (collect mode) */
} SeaCompiler;

// Compile a program block into a chunk
SeaChunk* sea_compile(SeaNode *program, const char *source_name);
// Compile the program's main script (see sea_compile_main in sea_compiler.c)
SeaChunk* sea_compile_main(SeaNode *program, const char *source_name);

/* Emit OP_COVER at each statement (coverage; set from SEA_COVER by main) */
extern bool sea_cover_enabled;

// The first error of the last failed compilation on this thread (message
// without position; NULL if none) and its line
const char* sea_compile_error(void);
int         sea_compile_error_line(void);
