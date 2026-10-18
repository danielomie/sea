/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_common.h"
#include "sea_vm.h"

/*
 * Optional method-JIT for the Sea VM (see docs/feature_jit.md).
 *
 * Disabled by default. The whole codegen path is only compiled when the binary
 * is built with -DSEA_JIT (the `make jit` target, which also links -lgccjit).
 * Even in a JIT-enabled binary nothing is compiled unless the VM was started
 * with the `--jit` CLI flag (vm->jit_enabled).
 */

/* Hotness thresholds (defaults; overridable at startup via the SEA_JIT_HOT_CALLS
 * and SEA_JIT_HOT_LOOPS env vars, see sea_jit_init):
 *   calls — number of calls before a function chunk is queued for compilation;
 *   loops — number of loop back-edges before a chunk is queued (this is how the
 *           main chunk, and functions called only a few times, get compiled). */
#define SEA_JIT_THRESHOLD      50
#define SEA_JIT_LOOP_THRESHOLD 1000
extern int sea_jit_hot_calls;
extern int sea_jit_hot_loops;

/* Sentinel stored in SeaChunk.jit_fn when compilation was attempted and refused;
 * prevents repeated compile attempts on a chunk we cannot (yet) handle. */
#define SEA_JIT_FAILED ((void *)-1)

/* Sentinel stored in SeaChunk.jit_fn while the chunk sits in (or is being
 * compiled by) the background compile queue. */
#define SEA_JIT_PENDING ((void *)-2)

/* True when `p` (a jit_fn value) is callable native code, i.e. not NULL,
 * SEA_JIT_FAILED or SEA_JIT_PENDING. */
#define SEA_JIT_IS_CODE(p) ((uintptr_t)(p) - 1u < (uintptr_t)-3)

/* jit_fn is published by the compile thread; read it with acquire semantics so
 * the generated code is visible before the pointer is used. */
#define SEA_JIT_LOAD_FN(chunk) __atomic_load_n(&(chunk)->jit_fn, __ATOMIC_ACQUIRE)

/* Value returned by a SeaJittedFn that hit an unsupported opcode: it wrote the
 * resume ip into frame->ip and the interpreter must continue from there.
 * Normal completion returns the number of return values (>= 0), left in
 * frame->base[0 .. nret-1]. */
#define SEA_JIT_BAILOUT (-1)

/* Value returned by a SeaJittedFn whose goroutine parked on a channel: the
 * resume ip is in frame->ip and the caller (always vm_run — inside goroutines
 * native code is never nested, see sea_jit_call) must return SEA_SUSPENDED.
 * Helpers signal it to the generated code by returning SEA_JIT_HELPER_SUSPEND. */
#define SEA_JIT_SUSPEND (-2)
#define SEA_JIT_HELPER_SUSPEND 2

/* Native entry point produced by the JIT for one SeaChunk.
 * Reads/writes the frame's register window directly (frame->base[...]).
 * Execution starts at frame->ip, which must be either the chunk's first
 * instruction or the target of one of its loop back-edges (OSR entry); any
 * other ip bails out immediately without side effects. */
typedef int (*SeaJittedFn)(SeaVM *vm, SeaCallFrame *frame);

/* Compile `chunk` to native code. Returns a callable function pointer on
 * success, or SEA_JIT_FAILED if the chunk cannot be compiled (unsupported
 * opcode, JIT not built in, etc.). The result is also cached in chunk->jit_fn. */
void *sea_jit_compile(SeaVM *vm, SeaChunk *chunk);

/* Ask for `chunk` to be compiled. By default compilation runs on a background
 * thread (libgccjit costs tens of ms per chunk, mostly in the external as/ld
 * steps) and this returns SEA_JIT_PENDING right away; the interpreter keeps
 * going and picks the native code up once jit_fn is published. With
 * SEA_JIT_SYNC=1 in the environment it compiles inline (deterministic; used by
 * tests). Returns the chunk's jit_fn after the request. */
void *sea_jit_request(SeaVM *vm, SeaChunk *chunk);

/* Read the env overrides (SEA_JIT_SYNC, SEA_JIT_HOT_CALLS, SEA_JIT_HOT_LOOPS).
 * Called once at startup when --jit is on. */
void sea_jit_init(void);

/* Release any native code/result owned by `chunk`. Safe to call on any chunk;
 * a no-op when the chunk was never JIT-compiled. */
void sea_jit_release(SeaChunk *chunk);

/* Runtime helpers invoked by JIT-compiled code to perform OP_CALL / OP_CALL_SELF
 * (defined in sea_vm.c so they can reuse vm_run and the call fast path).
 * They write the call's result into base[a] and return 0 when handled, or 1 when
 * the case is outside the JIT's scope (vararg call, arity mismatch, >1 expected
 * returns, non-callable, stack full) — the generated code then bails to the
 * interpreter at that instruction. */
int sea_jit_call(SeaVM *vm, SeaValue *base, int a, int b, int c);
int sea_jit_call_self(SeaVM *vm, SeaCallFrame *frame, int a, int b, int c);

/* Array-construction helpers used by JIT-compiled code (return 0 if handled,
 * 1 to bail). */
int sea_jit_apush(SeaVM *vm, SeaValue *base, int a, int b);
int sea_jit_new_array(SeaVM *vm, SeaValue *base, int a, int b);
int sea_jit_new_array_local(SeaVM *vm, SeaValue *base, int a, int b);
int sea_jit_load_global(SeaVM *vm, SeaValue *base, int a, SeaValue key, SeaTableIC *ic);

/* Single-opcode helpers (return 0; errors exit through the VM's usual error
 * path — the generated code saves frame->ip before calling the ones that can
 * raise, so the reported line is right). */
int sea_jit_release_reg(SeaVM *vm, SeaValue *base, int a);
int sea_jit_retain_reg(SeaVM *vm, SeaValue *base, int a);
int sea_jit_setlocal(SeaVM *vm, SeaValue *base, int a, int b, int c);
int sea_jit_append(SeaVM *vm, SeaValue *base, int a, int b);
int sea_jit_set_field_move(SeaVM *vm, SeaValue *base, int a, int c, SeaValue key);
int sea_jit_apush_move(SeaVM *vm, SeaValue *base, int a, int b);
int sea_jit_get_field(SeaVM *vm, SeaValue *base, int a, int b, SeaValue key, SeaTableIC *ic);
int sea_jit_set_field(SeaVM *vm, SeaValue *base, int a, int c, SeaValue key);
int sea_jit_set_field_borrow(SeaVM *vm, SeaValue *base, int a, int c, SeaValue key);
int sea_jit_new_obj(SeaVM *vm, SeaValue *base, int op, int a, int b);
int sea_jit_concat(SeaVM *vm, SeaValue *base, int a, int b, int c);
int sea_jit_store_global(SeaVM *vm, SeaValue *base, int a, SeaValue key);
int sea_jit_load_up(SeaCallFrame *frame, SeaValue *base, int a, int b);

/* Helpers that may park the goroutine (return SEA_JIT_HELPER_SUSPEND after
 * setting frame->ip = resume_ip), or return 1 to bail for an error the
 * interpreter should raise. */
int sea_jit_for_in_next(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, int a, int b,
                        uint32_t *resume_ip);
int sea_jit_chan_send(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, int a, int b,
                      uint32_t *resume_ip);
int sea_jit_chan_recv(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, int a, int b,
                      uint32_t *resume_ip);
int sea_jit_chan_close(SeaVM *vm, SeaValue *base, int a);
int sea_jit_go(SeaVM *vm, SeaValue *base, int a, int b, int c);
int sea_jit_closure(SeaVM *vm, SeaCallFrame *frame, SeaValue *base, int a, int bx);
int sea_jit_close_up(SeaVM *vm, SeaValue *base, int a);
int sea_jit_del(SeaVM *vm, SeaValue *base, int a, int b);
int sea_jit_mark_captured(SeaVM *vm, SeaValue *base, int a);
int sea_jit_get_table(SeaVM *vm, SeaValue *base, int a, int b, int c);
int sea_jit_set_table(SeaVM *vm, SeaValue *base, int a, int b, int c);
int sea_jit_get_table2(SeaVM *vm, SeaValue *base, int a, int b, int c);
int sea_jit_store_up(SeaCallFrame *frame, SeaValue *base, int a, int b);
