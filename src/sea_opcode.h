/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_common.h"

/*
 * Sea bytecode instruction set.
 *
 * Instruction encoding: 32-bit fixed width.
 *
 * Format A:   [opcode:8][A:8][B:8][C:8]
 * Format Bx:  [opcode:8][A:8][Bx:16]    (unsigned 16-bit)
 * Format sBx: [opcode:8][A:8][sBx:16]   (signed, biased by 32767)
 */

/* Max arguments of a deferred call: the VM copies them into a fixed-size
 * SeaDeferEntry; the compiler rejects `defer` calls with more. */
#define SEA_DEFER_MAX_ARGS 8

typedef enum {
    // Load / move
    OP_LOAD_NIL,      // R[A..A+B-1] = nil
    OP_LOAD_TRUE,     // R[A] = true
    OP_LOAD_FALSE,    // R[A] = false
    OP_LOAD_INT,      // R[A] = sBx (signed immediate)
    OP_LOAD_K,        // R[A] = K[Bx]
    OP_MOVE,          // R[A] = R[B]
    OP_LOAD_GLOBAL,   // R[A] = G[K[Bx]]
    OP_STORE_GLOBAL,  // G[K[Bx]] = R[A]
    OP_LOAD_UP,       // R[A] = UpV[B]
    OP_STORE_UP,      // UpV[B] = R[A]
    OP_CLOSE_UP,      // close upvalues from R[A] upward

    // Arithmetic
    OP_ADD,           // R[A] = R[B] + R[C]
    OP_SUB,           // R[A] = R[B] - R[C]
    OP_MUL,           // R[A] = R[B] * R[C]
    OP_DIV,           // R[A] = R[B] / R[C]
    OP_IDIV,          // R[A] = R[B] // R[C]
    OP_MOD,           // R[A] = R[B] % R[C]
    OP_POW,           // R[A] = R[B] ^ R[C]
    OP_NEG,           // R[A] = -R[B]
    OP_UNM,           // alias for NEG

    // Integer fast paths
    OP_ADD_INT,
    OP_SUB_INT,
    OP_MUL_INT,
    OP_IDIV_INT,
    OP_MOD_INT,
    OP_IIADD,           // R[A] = R[B] + R[C], no type checks (compiler-proven int)
    OP_IISUB,           // R[A] = R[B] - R[C], no type checks (compiler-proven int)
    OP_ADDI,            // R[A] = R[B] + (int8_t)C, with runtime int check
    OP_SUBI,            // R[A] = R[B] - (int8_t)C, with runtime int check

    // Bitwise
    OP_BAND,          // R[A] = R[B] & R[C]
    OP_BOR,           // R[A] = R[B] | R[C]
    OP_BXOR,          // R[A] = R[B] ~ R[C]
    OP_BNOT,          // R[A] = ~R[B]
    OP_SHL,           // R[A] = R[B] << R[C]
    OP_SHR,           // R[A] = R[B] >> R[C]

    // String
    OP_CONCAT,        // R[A] = R[B] .. R[B+1] .. ... (C = count)

    // Comparison (result is bool in R[A])
    OP_EQ,            // R[A] = R[B] == R[C]
    OP_NEQ,           // R[A] = R[B] ~= R[C]
    OP_LT,            // R[A] = R[B] < R[C]
    OP_LE,            // R[A] = R[B] <= R[C]
    OP_GT,            // R[A] = R[B] > R[C]
    OP_GE,            // R[A] = R[B] >= R[C]
    OP_NOT,           // R[A] = !R[B]

    // Comparison with integer immediate (result is bool in R[A]; C = int8_t immediate)
    OP_LTI,           // R[A] = R[B] < (int8_t)C
    OP_LEI,           // R[A] = R[B] <= (int8_t)C
    OP_GTI,           // R[A] = R[B] > (int8_t)C
    OP_GEI,           // R[A] = R[B] >= (int8_t)C

    // Jumps (sBx = signed offset from next instruction)
    OP_JMP,           // PC += sBx
    OP_JMP_TRUE,      // if R[A] then PC += sBx
    OP_JMP_FALSE,     // if !R[A] then PC += sBx
    OP_JMP_NIL,       // if R[A] == nil then PC += sBx

    // Tables
    OP_NEW_TABLE,     // R[A] = new table (B: 0=map, 1=struct)
    OP_GET_TABLE,     // R[A] = R[B][R[C]]
    OP_GET_TABLE2,    // R[A] = R[B][R[C]]; R[A+1] = ok (bool) — two-value lookup
    OP_SET_TABLE,     // R[A][R[B]] = R[C]
    OP_GET_FIELD,     // R[A] = R[B].K[C]
    OP_SET_FIELD,     // R[A].K[B] = R[C]
    OP_NEW_STRUCT,    // R[A] = new struct(field_count=B)

    // Arrays
    OP_NEW_ARRAY,     // R[A] = new array(cap=B)
    OP_GET_ARRAY,     // R[A] = R[B][R[C]]
    OP_SET_ARRAY,     // R[A][R[B]] = R[C]
    OP_ARRAY_PUSH,    // R[A].push(R[B])

    // Typed arrays (no per-element RC)
    OP_GET_IARRAY,    // R[A] = int_array[R[C]]   (B=array reg)
    OP_SET_IARRAY,    // int_array[R[B]] = R[C]   (A=array reg)
    OP_GET_FARRAY,    // R[A] = float_array[R[C]] (B=array reg)
    OP_SET_FARRAY,    // float_array[R[B]] = R[C] (A=array reg)

    // Functions / closures
    OP_CLOSURE,       // R[A] = closure(proto=K[Bx])
    OP_CALL,          // R[A..A+B-1] = R[A](R[A+1]..R[A+C])
    OP_CALL_SELF,     // R[A..A+B-1] = frame->closure(R[A+1]..R[A+C]) — self-recursion, no upvalue load
    OP_CALL_MULTI,    // call with variable return count
    OP_RETURN,        // return R[A..A+B-1]  (B=0 means return 0 values)
    OP_RETURN0,       // return (no values)
    OP_TAIL_CALL,     // tail call
    OP_VARARG,        // R[A..A+B-1] = varargs (B=0 means all varargs)

    // For loops
    OP_FOR_PREP,       // init numeric for (idx=A, limit=A+1, step=A+2)
    OP_FOR_LOOP,       // step; if done jump sBx
    OP_FOR_LOOP_STEP1, // FOR_LOOP with hardcoded step=1, no type checks
    OP_FOR_LOOP_INT,   // FOR_LOOP with int step, no type checks
    OP_FOR_IN_INIT,    // init for-in iterator
    OP_FOR_IN_NEXT,    // advance for-in; if done jump sBx

    // Channels
    OP_NEW_CHAN,      // R[A] = new channel(cap=R[B])
    OP_CHAN_SEND,     // R[A] <- R[B]
    OP_CHAN_RECV,     // R[A] = <-R[B]
    OP_CHAN_CLOSE,    // close(R[A])
    OP_CHAN_TRY_SEND, // non-blocking send; R[A] = success bool
    OP_CHAN_TRY_RECV, // non-blocking recv; R[A]=val, R[A+1]=ok bool

    // Goroutines
    OP_GO,            // spawn goroutine: R[A](R[B..B+C-1])
    OP_YIELD,         // cooperative yield

    // Select
    OP_SELECT_BEGIN,  // begin select block
    OP_SELECT_CASE,   // add case
    OP_SELECT_END,    // execute select

    // Error handling
    OP_NEW_ERROR,     // R[A] = error(R[B])
    OP_IS_ERROR,      // R[A] = is_error(R[B])
    OP_ERROR_MSG,     // R[A] = R[B].message
    OP_PROPAGATE,     // if is_error(R[A]) return it; else R[A] = unwrap

    // Panic / defer
    OP_DEFER,         // push deferred call: fn=R[A], args=R[A+1..A+B] (B <= SEA_DEFER_MAX_ARGS)
    OP_PANIC,         // panic(R[A])
    OP_RECOVER,       // recover()

    // Standard built-ins (fast dispatch)
    OP_TYPE,          // R[A] = type(R[B])
    OP_TOSTRING,      // R[A] = tostring(R[B])
    OP_TONUMBER,      // R[A] = tonumber(R[B])
    OP_PRINT,         // print(R[A..A+B-1])
    OP_ASSERT,        // assert(R[A], R[B]?)

    // Memory management
    OP_RELEASE,      // if sea_is_obj(R[A]) { sea_obj_release(R[A]); R[A] = NIL; }

    // Escape-analysis arena allocation (non-escaping objects, no RC)
    OP_NEW_STRUCT_LOCAL,  // R[A] = arena_alloc struct(field_count=B); no RC
    OP_SET_FIELD_BORROW,  // R[A].K[B] = R[C]; no val_retain (borrow from local)
    OP_NEW_ARRAY_LOCAL,   // R[A] = array with arena-allocated header (heap data buffer); B=cap hint

    // Maps
    OP_NEW_MAP,      // R[A] = new map()

    // Delete
    OP_DEL,          // del R[A][R[B]] — remove from array (with shift) or table/map
    OP_MARK_CAPTURED, // R[A] (a local some closure captures) was just assigned: if its
                      // open upvalue is shared with a goroutine, mark the value shared

    // Ownership (docs/memory.md): named locals own their value (+1)
    OP_RETAIN,        // retain R[A] (a borrowed value becomes owned)
    OP_SETLOCAL,      // old = R[A]; R[A] = R[B]; if C: retain R[A]; release old
    OP_APPEND,        // R[A] = R[A] .. R[B]; R[A] is an owned local, consumed
                      // (extends a rope in place when nothing else holds it)
    OP_JMP_NOT_ERR,   // if !is_error(R[A]) then PC += sBx  (the `?` fast path)
    OP_LOAD_SELF,     // R[A] = the running closure (borrowed): a local function
                      // naming itself, without a self-capturing upvalue cycle
    OP_SET_FIELD_MOVE,  // R[A].K[B] = R[C], consuming R[C]'s reference (an owned temporary)
    OP_ARRAY_PUSH_MOVE, // R[A].push(R[B]), consuming R[B]'s reference
    OP_COVER,         // source line Bx ran (coverage; emitted only when
                      // sea_cover_enabled, see sea_cover.c)

    OP_COUNT
} SeaOpcode;

// ---- Instruction encoding / decoding ----

// Field extraction
#define SEA_INSTR_OP(i)    ((SeaOpcode)((i) & 0xFF))
#define SEA_INSTR_A(i)     (((i) >> 8) & 0xFF)
#define SEA_INSTR_B(i)     (((i) >> 16) & 0xFF)
#define SEA_INSTR_C(i)     (((i) >> 24) & 0xFF)
#define SEA_INSTR_Bx(i)    (((i) >> 16) & 0xFFFF)
#define SEA_INSTR_sBx(i)   ((int)(((i) >> 16) & 0xFFFF) - 32767)

// Instruction construction
#define SEA_MAKE_ABC(op, a, b, c) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 24))
#define SEA_MAKE_ABx(op, a, bx) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(bx) << 16))
#define SEA_MAKE_AsBx(op, a, sbx) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)((int)(sbx) + 32767) << 16))

// Opcode name (for disassembly)
const char* sea_opcode_name(SeaOpcode op);

/* Registers an instruction READS, as up to 3 inclusive ranges [lo, hi] —
 * the single source of truth derived from the VM handlers (sea_vm.c). The
 * compiler's dead-write elimination relies on it: a register read that is
 * missing here lets a live write be NOPed (Bugs 10 and 14 in docs/bugs.md).
 * Keep it in sync with every handler that reads a register. */
typedef struct { int n; int lo[3], hi[3]; } SeaRegRanges;
void sea_op_reads(uint32_t instr, SeaRegRanges *out);
