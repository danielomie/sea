#pragma once
#include "sea_common.h"
#include "sea_value.h"
#include "sea_object.h"
#include "sea_chunk.h"
#include "sea_table.h"

/* Forward declare SeaShared (defined in sea_sched.h) */
typedef struct SeaShared SeaShared;

/*
 * The Sea register-based virtual machine.
 */

// Call frame (one per active function call)
typedef struct {
    SeaClosure *closure;       // function being executed
    uint32_t   *ip;            // instruction pointer
    SeaValue   *base;          // base register for this frame
    int         expected_ret;  // expected number of return values (-1 = any)
    SeaValue   *varargs;       // pointer to vararg values (NULL if none)
    int         vararg_count;  // number of vararg values
    int         defer_base;    // index into vm->defers[] where this frame's defers start
    uint32_t    arena_mark;    // frame_arena watermark at frame entry; restored on return
} SeaCallFrame;

// Open upvalue - captures a local variable by reference
struct SeaUpvalue {
    SeaObject   obj;       // must be first
    SeaValue   *location;  // points into register file while open
    SeaValue    closed;    // value stored here when closed
    SeaUpvalue *next;      // linked list of all open upvalues
};

// Closure = chunk + captured upvalues
struct SeaClosure {
    SeaObject    obj;        // must be first
    SeaChunk    *chunk;      // bytecode (not owned; owned by proto pool)
    bool         is_native;
    SeaValue   (*native)(SeaVM *vm, int argc, SeaValue *args);
    const char  *native_name;
    SeaUpvalue **upvalues;
    int          upval_count;
};

// Native function wrapper
struct SeaFunction {
    SeaObject   obj;
    const char *name;
    SeaValue  (*fn)(SeaVM *vm, int argc, SeaValue *args);
};

// Error type
struct SeaError {
    SeaObject  obj;
    SeaValue   message;
};

// Deferred call entry
typedef struct {
    SeaValue fn;
    SeaValue args[SEA_DEFER_MAX_ARGS];   // deferred args (compiler enforces the limit)
    int      argc;
} SeaDeferEntry;

struct SeaVM {
    // Register file (large flat array)
    SeaValue    regs[SEA_MAX_REGISTERS * SEA_MAX_CALL_DEPTH];

    // Call stack
    SeaCallFrame frames[SEA_MAX_CALL_DEPTH];
    int          frame_count;

    // Open upvalue chain (sorted by register address, highest first)
    SeaUpvalue  *open_upvalues;

    // Shared state (globals, loaded modules) — owned by main VM, borrowed by children
    SeaShared   *shared;
    bool         owns_shared;   // true only for the root VM

    // Defer stack (per-vm, cleared on function return)
    SeaDeferEntry defers[256];
    int           defer_count;

    // Panic state
    bool      panicking;
    SeaValue  panic_val;

    // Error message
    char err_msg[512];

    // Source name for error reporting
    const char *source_name;

    // Linked list of chunks owned by this VM (module chunks that must outlive
    // the closures they produced; freed in sea_vm_free)
    SeaChunk *module_chunks;

    // Captures the top-level return value when sea_vm_exec_mod is running
    SeaValue exec_result;
    bool     has_exec_result;

    /* Frame arena: bump allocator for non-escaping objects (no RC, O(1) free).
     * Each frame saves/restores frame_arena_used as a watermark.
     * _Alignas(8) ensures the base address is 8-byte aligned for SeaStruct etc. */
#define SEA_FRAME_ARENA_SIZE 65536
    _Alignas(8) uint8_t frame_arena[SEA_FRAME_ARENA_SIZE];
    uint32_t frame_arena_used;

    // M:N scheduler: the goroutine currently running in this VM (NULL = main thread)
    SeaGoroutine *goroutine;

    // JIT: per-VM cache of the program-wide SeaShared.jit_enabled, copied at VM
    // init. The hot path (OP_CALL) reads THIS (thread-local) and not the shared
    // copy, to avoid cache contention/false-sharing across goroutine threads.
    bool jit_enabled;
};

/* Convenience accessors — keep existing code working with minimal churn */
#define vm_globals(vm)  ((vm)->shared->globals)
#define vm_loaded(vm)   ((vm)->shared->loaded)

// Create/destroy VM
SeaVM*  sea_vm_new(void);
// Create a child VM that shares the parent's SeaShared state.
// Use for goroutines so they see the same globals and modules as the parent.
SeaVM*  sea_vm_new_child(SeaShared *shared);
void    sea_vm_free(SeaVM *vm);
void    sea_vm_reset_child(SeaVM *vm);  /* clean up + zero for pool reuse */

// Execute a compiled chunk in the VM
int     sea_vm_exec(SeaVM *vm, SeaChunk *chunk);

// Execute a chunk and capture its top-level return value (for module loading)
int     sea_vm_exec_mod(SeaVM *vm, SeaChunk *chunk, SeaValue *result);

// Call a Sea function value from C
SeaValue sea_vm_call(SeaVM *vm, SeaValue fn, SeaValue *args, int argc);

/* M:N scheduler helpers (Fase 4):
 *   sea_vm_goroutine_init — push the initial call frame for a goroutine
 *                           without running it yet.
 *   sea_vm_resume         — run vm_run(vm, 0); returns SEA_OK when the
 *                           goroutine's top-level function returns, or
 *                           SEA_SUSPENDED when it parked on a channel. */
/* Returns true when a call frame was pushed (resume with sea_vm_resume), false
 * when there is nothing to resume: a native function, which it has already run
 * to completion. Callers guarantee `fn` is a closure (OP_GO checks). */
bool sea_vm_goroutine_init(SeaVM *vm, SeaValue fn, SeaValue *args, int argc);
int  sea_vm_resume(SeaVM *vm);

// Create a native closure
SeaClosure* sea_native_closure(const char *name, SeaValue (*fn)(SeaVM*, int, SeaValue*));

// Register a native function as a global
void sea_vm_register_native(SeaVM *vm, const char *name,
                             SeaValue (*fn)(SeaVM*, int, SeaValue*));

// Register a module table as a global
void sea_vm_register_module(SeaVM *vm, const char *name, SeaTable *mod);

// Runtime error (longjmp out of VM)
void sea_vm_error(SeaVM *vm, const char *fmt, ...);
void sea_vm_error_noreturn(SeaVM *vm, const char *fmt, ...) SEA_NORETURN;

// Value-to-string (for print/tostring)
SeaString* sea_vm_tostring(SeaVM *vm, SeaValue v);

// Create an error value
SeaValue sea_make_error(SeaVM *vm, SeaValue msg);
SeaValue sea_make_error_str(SeaVM *vm, const char *msg);
