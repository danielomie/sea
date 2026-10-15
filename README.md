# Sea

Sea is a small scripting language with Lua-like syntax and Go-like concurrency.
It compiles to bytecode for a register-based virtual machine written in C, with
an optional JIT compiler.

```sea
local function fetch(id)
    if id % 4 == 0 then return error("item " .. id .. " not found") end
    return {id = id, price = id * 2.5}
end

local results = chan(10)
for id = 1, 6 do
    go (function()
        local item = fetch(id)          -- each goroutine sees its own `id`
        if is_error(item) then results <- item.message
        else results <- string.format("item %d: %.2f", item.id, item.price) end
    end)()
end
for i = 1, 6 do print(<-results) end
```

## Highlights

- **Familiar syntax** — Lua-style blocks, closures, multiple returns, varargs.
- **Concurrency** — goroutines on an M:N scheduler, buffered/unbuffered
  channels, `select`, `sync.mutex` / `sync.waitgroup`.
- **Errors as values** — `error()`, `is_error()`, the `?` propagation operator,
  plus `defer`, `panic` and `recover`.
- **Data types** — arrays (`[]`), hash maps (`[=]`), sealed structs (`{x = 1}`),
  dynamic tables, typed `int_array` / `float_array`.
- **No tracing GC** — reference counting (atomic only for values shared between
  goroutines) plus a cycle collector; see [memory](docs/memory.md).
- **Fast** — NaN-boxed values, computed-goto dispatch, an optimizing compiler,
  and an optional JIT (libgccjit) that speeds hot numeric code up 2–4×.
- **Batteries** — `math`, `string`, `io`, `os`, `sync`, `time`; external
  libraries for JSON, XML, HTTP, TLS, JWT, CLI flags and a terminal UI.

## Quick start

Requirements: GCC, GNU Make, pthreads. The JIT additionally needs the libgccjit
headers (`libgccjit-13-dev` or similar).

```sh
make                              # build bin/sea
bin/sea examples/nbody/nbody.sea  # run a program
bin/sea                           # interactive REPL

bin/sea compile app.sea -o app.seac   # compile to bytecode …
bin/sea app.seac                      # … and run it

make sea-jit                                      # build bin/sea-jit
bin/sea-jit --jit examples/nbody/nbody.sea        # run with the JIT
```

Native libraries are built per library, e.g. `make -C libs/json`.

## Documentation

| Document | Contents |
|----------|----------|
| [Tour](docs/tour.md) | Example-driven introduction to the language |
| [Reference](docs/reference.md) | Syntax, operators, built-ins and the standard library |
| [Libraries](docs/libs.md) | `json`, `xml`, `console`, `http`, `ssl`, `jwt`, `flags`, `parser` |
| [Memory model](docs/memory.md) | Reference counting without a GC: ownership, cycles, goroutines |
| [Architecture](docs/architecture.md) | Compiler, VM, values, scheduler — how it fits together |
| [JIT design](docs/feature_jit.md) | The optional JIT: design, phases, measurements (pt-BR) |
| [Benchmarks](docs/benchmarks.md) | Sea, Sea+JIT, Lua 5.4 and Python 3.12 compared |
| [Bugs & changes](docs/bugs.md) | Known issues and notable fixes (pt-BR) |

## Examples

29 runnable programs live in [`examples/`](examples/) — `bin/sea examples/<name>/<name>.sea`.
Those marked ⚡ are compute-bound and print their run time (compare `bin/sea`
with `bin/sea-jit --jit`).

| Area | Examples |
|------|----------|
| Numeric / JIT ⚡ | `nbody` (N-body simulation), `game_of_life` (Conway), `spectral_norm` (power method) |
| Algorithms | `graphs` (BFS maze + Dijkstra with a binary heap), `dynamic_programming` (LCS, edit distance, knapsack, memoization) |
| Closures | `generators` (generators, memoization, per-iteration loop variables), `closures` |
| Text & data | `word_count`, `calculator` (lexer + recursive-descent parser), `csv_report` (CSV → aggregation → JSON), `strings`, `verbos` |
| Language & stdlib | `collections`, `structs`, `errors`, `os_demo`, `time_demo`, `file_watcher` |
| Concurrency | `pipeline`, `worker_pool`, `pubsub`, `timeout`, `rate_limiter`, `circuit_breaker`, `pool`, `logger`, `webhook`, `metrics_server` |

## Performance

A few results from [benchmarks.md](docs/benchmarks.md) (wall-clock, lower is better):

| Benchmark | Sea | Sea + JIT | Lua 5.4 | Python 3.12 |
|-----------|-----|-----------|---------|-------------|
| Insertion sort, 10K numbers | 837 ms | 246 ms | 777 ms | 7.96 s |
| Loop sum, 50M iterations | 262 ms | 78 ms | 383 ms | 6.86 s |
| Mandelbrot 400×400 | 410 ms | 165 ms | 518 ms | 4.58 s |
| Binary trees, depth 16 | 2.90 s | 2.43 s | 4.87 s | 2.67 s |

## Development

```sh
make test        # unit tests (tests/unit/*.sea vs *.expected)
make test-dwe    # tests with the optimizer's diagnostic checks
make test-seac   # tests run from compiled .seac bytecode
make test-jit    # tests with every chunk JIT-compiled
make test-asan   # tests under AddressSanitizer
bash tests/compare/run_benchmarks.sh   # regenerate docs/benchmarks.md
```

Layout: `src/` (compiler, VM, runtime, stdlib), `libs/` (external libraries),
`examples/`, `tests/` (unit tests, benchmarks), `docs/`.
