#pragma once
#include "sea_common.h"
#include "sea_value.h"
#include "sea_opcode.h"

/*
 * SeaChunk - compiled function body:
 *   - bytecode instructions
 *   - constant pool
 *   - nested function prototypes
 *   - metadata (name, arity, upvalue descriptors)
 */

// Upvalue descriptor (how to find the upvalue at closure creation time)
typedef struct {
    bool    is_local;  // true = it's a local in direct enclosing scope
    uint8_t idx;       // register index (if local) or upvalue index (if not)
    char   *name;      // debug name
} SeaUpvalDesc;

struct SeaChunk {
    char     *name;          // function/source name

    uint32_t *code;          // instruction array
    int       code_len;
    int       code_cap;

    int      *lines;         // source line for each instruction
    int       lines_len;     // same as code_len

    SeaValue *constants;     // constant pool
    int       const_len;
    int       const_cap;

    SeaChunk **protos;       // nested function prototypes
    int        proto_len;
    int        proto_cap;

    uint8_t   arity;         // number of fixed parameters
    bool      is_vararg;     // accepts ...
    uint8_t   max_stack;     // max register count needed

    SeaUpvalDesc *upvals;    // upvalue descriptors
    int           upval_count;

    SeaChunk     *next_owned; // intrusive link for VM's module-chunk ownership list

    /* JIT (see docs/feature_jit.md). Runtime-only, not serialized.
     *   jit_fn:     NULL = not compiled, SEA_JIT_FAILED = refused, else native fn.
     *   jit_result: owning gcc_jit_result* for jit_fn (released in sea_chunk_free).
     *   exec_count: call counter feeding the hotness threshold.
     *   jit_hot:    back-edge countdown; the interpreter takes the OSR slow path
     *               when it drops below zero (see JIT_BACKEDGE in sea_vm.c).
     *   jit_armed:  jit_hot has been loaded with the loop threshold.
     *   jit_next:   link in the background compile queue (sea_jit.c). */
    void    *jit_fn;
    void    *jit_result;
    uint32_t exec_count;
    int32_t  jit_hot;
    uint8_t  jit_armed;
    SeaChunk *jit_next;

    /* Inline caches for table lookups, one slot per instruction (used by
     * LOAD_GLOBAL / GET_FIELD); allocated on first use, see sea_chunk_ics. */
    struct SeaTableICSlot *ics;
};

/* One inline-cache slot; layout-compatible with SeaTableIC (sea_table.h).
 * GET_TABLE / SET_TABLE instructions use `idx` instead as type feedback for
 * the JIT: which non-generic-array containers the site has seen. */
struct SeaTableICSlot { void *entries; uint32_t idx; };
#define SEA_FB_TYPED_ARRAY 1u   /* int_array / float_array */
#define SEA_FB_OTHER       2u   /* table, map, struct, string, out of bounds */


/* The chunk's inline-cache array (zeroed), allocated on first use; safe to
 * call from several threads. */
struct SeaTableICSlot *sea_chunk_ics(SeaChunk *c);

// Create a new empty chunk
SeaChunk* sea_chunk_new(const char *name);

// Free chunk and all nested protos
void sea_chunk_free(SeaChunk *chunk);

// Add a constant to the pool (returns index)
int sea_chunk_add_const(SeaChunk *c, SeaValue val);
int sea_chunk_add_const_str(SeaChunk *c, const char *s);

// Emit an instruction
void sea_chunk_emit(SeaChunk *c, uint32_t instr, int line);

// Emit a jump instruction (returns position for patching)
int sea_chunk_emit_jmp(SeaChunk *c, SeaOpcode op, int a, int line);

// Patch a jump instruction at pos to target position
void sea_chunk_patch_jmp(SeaChunk *c, int pos, int target);

// Current code position
int sea_chunk_pos(SeaChunk *c);

// Serialization
int       sea_chunk_write(SeaChunk *c, FILE *f);
SeaChunk* sea_chunk_read(FILE *f);

// Disassembly (pretty print)
void sea_chunk_disasm(SeaChunk *c, FILE *f);
void sea_chunk_disasm_instr(SeaChunk *c, int idx, FILE *f);
