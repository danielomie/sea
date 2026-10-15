# Sea / Lua / Python Benchmark Suite

This directory contains a benchmark suite that compares the performance of Sea, Lua 5.4, and Python 3.12 across a range of workloads.

## Structure

```
tests/compare/
├── benchmarks/          # Individual benchmark programs
│   ├── fib_recursive.*  # Fibonacci(35) naive recursive
│   ├── fib_iterative.*  # Fibonacci(1M) iterative with modular arithmetic
│   ├── loops.*          # Simple counting loop, 50M iterations
│   ├── primes.*         # Sieve of Eratosthenes up to 500,000
│   ├── string_ops.*     # String concatenation + upper/lower (10K iterations)
│   ├── closures.*       # 100K closure creation + 100K calls
│   ├── map_ops.*        # Hash map: 10M inserts + 10M lookups
│   └── sort.*           # Insertion sort on 10K pseudo-random numbers
├── results/             # JSON output from each benchmark run
├── run_benchmarks.sh    # Main runner script
└── README.md            # This file
```

## Benchmarks

| Name | Description | What it tests |
|------|-------------|---------------|
| `fib_recursive` | fib(35) via naive recursion | Function call overhead, recursion depth |
| `fib_iterative` | fib(1,000,000) mod 1e9+7 | Tight numeric loop performance |
| `loops` | Sum integers 1..50,000,000 | Raw loop and arithmetic throughput |
| `primes` | Sieve of Eratosthenes to 500,000 | Array read/write, branching |
| `string_ops` | 1K concat + 10K upper/lower | String allocation and mutation |
| `closures` | 100K closures, 100K calls | Closure creation and dispatch |
| `map_ops` | 10M insert + 10M lookup | Hash map / table operations |
| `sort` | Insertion sort 10K numbers | Array random access, comparisons |

## Running

```bash
bash tests/compare/run_benchmarks.sh
```

The script:
- Runs each benchmark 3 times and reports the average
- Measures wall-clock time and peak RSS memory via `/usr/bin/time -v`
- Saves a timestamped JSON result to `results/`
- Updates the `## Benchmarks` section in the top-level `README.md`

## Language Notes

- **Sea**: uses 0-indexed arrays (`array()`, `push()`, `len()`), Lua-style `for i = a, b` (inclusive), `tostring()`, `string.upper()` / `string.lower()`
- **Lua 5.4**: 1-indexed tables, `#t` for length, `string.upper()` / `string.lower()`
- **Python 3.12**: 0-indexed lists, built-in `len()`, `str.upper()` / `str.lower()`

All benchmark variants are verified to produce identical numeric output across all three languages.

## Interpreting Results

- **Time**: wall-clock average over 3 runs. Lower is better.
- **Memory**: peak RSS (Maximum Resident Set Size) in KB or MB. Lower is better.
- Ratios in the detail table compare Sea against Lua and Python. "2.00x faster" means Sea ran in half the time.
