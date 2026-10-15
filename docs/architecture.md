# Architecture

Sea is implemented in C11 (~18 000 lines in `src/`). This page is a map of the
implementation; the language itself is described in the [tour](tour.md) and the
[reference](reference.md).

```
source .sea ──► lexer ──► parser (AST) ──► compiler ──► bytecode (SeaChunk) ──► VM
                                              │                                │
                                     optimizer passes                  optional JIT
                                  (copy prop., const fold,         (libgccjit, hot loops
                                   dead writes, escape analysis)    and functions)
```

## Components

| Component | Files | Role |
|-----------|-------|------|
| Lexer | `sea_lexer.c` | Tokens, keywords, number/string literals |
| Parser | `sea_parser.c`, `sea_ast.c` | Recursive descent → AST |
| Compiler | `sea_compiler.c`, `sea_chunk.c` | Single pass AST → register bytecode; register allocation, upvalue resolution, optimizer (below) |
| VM | `sea_vm.c` | Register-based interpreter, computed-goto dispatch |
| Values & objects | `sea_value.h`, `sea_object.c` | NaN-boxed values, reference-counted heap objects |
| Cycle collector | `sea_gc.c` | Frees reference cycles (trial deletion over candidate roots, per goroutine); the `gc` module |
| Data types | `sea_string.c`, `sea_rope.c`, `sea_array.c`, `sea_typed_array.c`, `sea_table.c`, `sea_map.c`, `sea_struct.c` | Strings (interned constants, lazy ropes for long concatenations), arrays, int/float arrays, tables, Swiss-table maps, sealed structs |
| Concurrency | `sea_sched.c`, `sea_goroutine.c`, `sea_channel.c` | M:N scheduler (worker threads, work stealing), goroutines, channels and `select` |
| Netpoller | `sea_netpoll.c`, `sea_lib_net.c` | epoll + deadline heap on one poller thread: parks goroutines waiting on sockets or timers; the `net` module |
| Standard library | `sea_stdlib.c`, `sea_lib_*.c` | Built-in functions and the `math`, `string`, `io`, `os`, `sync`, `time`, `net`, `gc` modules |
| JIT (optional) | `sea_jit.c` | Method/loop JIT on libgccjit — see [feature_jit.md](feature_jit.md) |
| Driver | `sea_main.c` | CLI (`run`, `compile`, `bytecode`, REPL, `--jit`) |

## Values

Every value is one 64-bit word (**NaN-boxing**, `sea_value.h`): a real IEEE-754
double, or — inside the quiet-NaN space — a tagged `nil`, boolean, 48-bit
signed integer, or a pointer to a heap object. Scalars never allocate.

## Bytecode and dispatch

- Fixed 32-bit instructions in three formats: **ABC** (three 8-bit operands),
  **ABx** (8 + 16 bits) and **AsBx** (8 + signed 16 bits). ~110 opcodes
  (`sea_opcode.h`), including int-specialized arithmetic chosen by the
  compiler's type inference.
- Each function has up to 256 registers in a window of the VM's register file;
  call frames overlap the caller's argument registers, so calls copy nothing.
- The interpreter dispatches with GCC computed goto (`goto *table[op]`). `ip`
  lives in a machine register; `frame->ip` is written back only before
  operations that may raise or call.
- Compiled chunks can be saved as `.seac` bytecode (`sea compile`) and run
  directly.

## Compiler optimizations

Done in the single compile pass, with per-register facts (`SeaRegMeta`):
constant folding, copy propagation (deferred `MOVE`s), dead-write elimination
for temporaries, int-type inference (selects unchecked int opcodes), and escape
analysis (non-escaping struct/array literals are allocated in a per-frame
arena). Facts are kept sound across branches and loop back-edges (see Bug 15 in
[bugs.md](bugs.md)); the registers each opcode reads come from one table,
`sea_op_reads()`. `make test-dwe` runs the tests in the optimizer's diagnostic
modes.

## Memory management

Sea has **no tracing garbage collector**. Heap objects are reference counted,
with atomic counts only for objects shared between goroutines, and a cycle
collector (`sea_gc.c`) frees reference cycles a goroutine owns.
[memory.md](memory.md) describes how ownership flows through registers,
containers, closures, arenas and threads, and what that means for programs.

## Concurrency

`go f(x)` creates a goroutine with its own small VM that shares globals and
loaded modules with its parent. The scheduler runs goroutines on a pool of
worker threads (one per CPU); a goroutine that blocks on a channel parks and its
worker picks up other work. Values that cross goroutines (channel sends,
captured variables, globals) are marked *shared* so their reference counts
become atomic.

Waiting on I/O or time also parks. A native function that would block calls
`sea_io_wait(vm, fd, events)` (`sea_netpoll.h`). In a goroutine this registers
the fd and/or deadline with the netpoller and returns `SEA_IO_PARK`; the native
then returns `SEA_PARK_VAL`, and the VM rewinds `ip` to the `CALL` and suspends
the goroutine. When the fd is ready, the deadline passes or the fd is closed,
the poller thread requeues the goroutine, which re-executes the `CALL`. The
native finds its saved progress in `vm->io_call` (`sea_io_begin`/`sea_io_end`),
so every native that can park must be safe to re-run from the top. On the main
thread, or under a nested `sea_vm_call` (`vm->native_depth > 0`) where the C
stack cannot be suspended, `sea_io_wait` falls back to a blocking `poll`.
`time.sleep` and the `net` module use this. `libs/ssl` and `libs/http` build on
`net`.

## JIT

Optional (`make sea-jit`, run with `--jit`). Hot functions and hot loops are
compiled to native code by libgccjit in a background compile-server process and
entered mid-loop (on-stack replacement); VM registers are promoted to native
locals inside compiled loops. Details, design history and measurements:
[feature_jit.md](feature_jit.md); results: [benchmarks.md](benchmarks.md).

## Tests

| Command | What it runs |
|---------|--------------|
| `make test` | `tests/unit/NN_name.sea`, comparing stdout with `NN_name.expected` |
| `make test-dwe` | the same with the optimizer's diagnostic checks (`SEA_DWE_CHECK`, `SEA_TYPE_CHECK`, `SEA_NO_DWE`) |
| `make test-seac` | each test compiled to `.seac` and run from bytecode |
| `make test-jit` | every chunk JIT-compiled (sync, async, default thresholds) |
| `make test-asan` | under AddressSanitizer |
