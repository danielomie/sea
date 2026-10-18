#!/usr/bin/env bash
# Copyright (c) 2026 Daniel
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BENCH_DIR="$SCRIPT_DIR/benchmarks"
SEA_BIN="${SEA_BIN:-$REPO_DIR/bin/sea}"
# JIT-enabled binary (built by `make sea-jit`); run with --jit for the Sea+JIT column.
SEA_JIT_BIN="${SEA_JIT_BIN:-$REPO_DIR/bin/sea-jit}"
LUA_BIN="${LUA_BIN:-$(command -v lua5.4 || command -v lua5.3 || command -v lua || echo "")}"
PYTHON_BIN="${PYTHON_BIN:-$(command -v python3 || command -v python || echo "")}"
RESULTS_DIR="$SCRIPT_DIR/results"
BENCH_DOC="$SCRIPT_DIR/../../docs/benchmarks.md"
RUNS=3   # Number of runs to average

mkdir -p "$RESULTS_DIR"

# Ensure the JIT binary exists for the Sea+JIT column (needs libgccjit / `make sea-jit`).
if [ ! -x "$SEA_JIT_BIN" ]; then
    echo "Building bin/sea-jit (make sea-jit) ..."
    make -C "$REPO_DIR" sea-jit >/dev/null 2>&1 || echo "  (could not build sea-jit; Sea+JIT column will show N/A)"
fi

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║     Sea / Sea+JIT / Lua / Python Benchmark Comparison    ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

SEA_VER=$("$SEA_BIN" version 2>/dev/null || echo "sea (unknown)")
if [ -x "$SEA_JIT_BIN" ]; then
    SEAJIT_VER="$("$SEA_JIT_BIN" version 2>/dev/null) (--jit)"
else
    SEAJIT_VER="(not built — run 'make sea-jit')"
fi
LUA_VER=$("$LUA_BIN" -v 2>&1 | head -1 || echo "lua (not found)")
PY_VER=$("$PYTHON_BIN" --version 2>&1 || echo "python3 (not found)")

echo -e "  Sea:     ${GREEN}${SEA_VER}${NC}"
echo -e "  Sea+JIT: ${GREEN}${SEAJIT_VER}${NC}"
echo -e "  Lua:     ${GREEN}${LUA_VER}${NC}"
echo -e "  Python:  ${GREEN}${PY_VER}${NC}"
echo ""

# run_bench <cmd> <arg> -> outputs "time_ms mem_kb"
run_bench() {
    local cmd="$1"
    local file="$2"
    # Resolve the actual binary (first word of cmd) and bail early if missing
    local bin="${cmd%% *}"
    if [ -z "$bin" ] || ! command -v "$bin" &>/dev/null && [ ! -x "$bin" ]; then
        echo "N/A N/A"
        return 0
    fi
    if [ ! -f "$file" ]; then
        echo "N/A N/A"
        return 0
    fi
    local total_ms=0
    local total_mem=0
    local best_ms=999999

    for run in $(seq 1 $RUNS); do
        local tmpfile
        tmpfile=$(mktemp)
        local start_ns
        start_ns=$(date +%s%N)
        /usr/bin/time -v $cmd "$file" > /dev/null 2>"$tmpfile"
        local end_ns
        end_ns=$(date +%s%N)
        local elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
        local mem_kb
        mem_kb=$(grep "Maximum resident" "$tmpfile" | awk '{print $NF}')
        rm -f "$tmpfile"

        total_ms=$((total_ms + elapsed_ms))
        total_mem=$((total_mem + mem_kb))
        if [ "$elapsed_ms" -lt "$best_ms" ]; then
            best_ms=$elapsed_ms
        fi
    done

    local avg_ms=$((total_ms / RUNS))
    local avg_mem=$((total_mem / RUNS))
    echo "${avg_ms} ${avg_mem}"
}

format_time() {
    local ms=$1
    [ "$ms" = "N/A" ] && { echo "N/A"; return; }
    if [ "$ms" -ge 1000 ]; then
        echo "$(awk "BEGIN{printf \"%.2f\", $ms/1000}")s"
    else
        echo "${ms}ms"
    fi
}

format_mem() {
    local kb=$1
    [ "$kb" = "N/A" ] && { echo "N/A"; return; }
    if [ "$kb" -ge 1024 ]; then
        echo "$(awk "BEGIN{printf \"%.1f\", $kb/1024}")MB"
    else
        echo "${kb}KB"
    fi
}

# Returns percentage difference Sea vs other (positive = Sea faster, negative = Sea slower)
format_pct() {
    local sea_t=$1
    local other_t=$2
    if [ "$sea_t" = "N/A" ] || [ "$other_t" = "N/A" ] || [ "$sea_t" -le 0 ] 2>/dev/null || [ "$other_t" -le 0 ] 2>/dev/null; then
        echo "N/A"
        return
    fi
    awk -v s="$sea_t" -v o="$other_t" 'BEGIN{
        pct = (o - s) / o * 100
        if (pct >= 0) printf "+%.1f%%", pct
        else printf "%.1f%%", pct
    }'
}

# Prints color-coded percentage for terminal, padded to 10 visible chars
print_pct() {
    local sea_t=$1
    local other_t=$2
    if [ "$sea_t" = "N/A" ] || [ "$other_t" = "N/A" ] || [ "$sea_t" -le 0 ] 2>/dev/null || [ "$other_t" -le 0 ] 2>/dev/null; then
        printf "%-10s" "N/A"
        return
    fi
    local pct
    pct=$(awk -v s="$sea_t" -v o="$other_t" 'BEGIN{
        pct = (o - s) / o * 100
        if (pct >= 0) printf "+%.1f%%", pct
        else printf "%.1f%%", pct
    }')
    local spaces=$(( 10 - ${#pct} ))
    if [[ "$pct" == +* ]]; then
        printf "${GREEN}%s${NC}" "$pct"
    else
        printf "${RED}%s${NC}" "$pct"
    fi
    printf "%${spaces}s" ""
}

# Benchmarks list: "name:description"
BENCHMARKS=(
    "fib_recursive:Fibonacci(35) recursive"
    "fib_iterative:Fibonacci(1M) iterative"
    "loops:Loop sum 50M iterations"
    "primes:Sieve primes to 500K"
    "string_ops:String ops (1K concat + 10K upper/lower)"
    "closures:100K closures + 100K calls"
    "map_ops:Map 10M insert + lookup"
    "sort:Insertion sort 10K numbers"
    "matrix_mul:Matrix multiply 100x100 x50"
    "binary_trees:Binary trees depth 16"
    "quicksort:Quicksort 300K numbers"
    "mandelbrot:Mandelbrot 400x400 200iter"
    "hot_calls:Hot int fn: 300K calls x100-loop (JIT-friendly)"
    "hot_float:Hot float fn: 300K calls x100-loop (JIT-friendly)"
)

# Store results as associative arrays
declare -A SEA_TIME SEA_MEM SEA_JIT_TIME SEA_JIT_MEM LUA_TIME LUA_MEM PY_TIME PY_MEM

echo -e "${YELLOW}Running benchmarks (${RUNS} runs each)...${NC}"
echo ""

for bench_entry in "${BENCHMARKS[@]}"; do
    bench="${bench_entry%%:*}"
    desc="${bench_entry#*:}"

    echo -ne "  ${bench} ... "

    sea_result=$(run_bench "$SEA_BIN run" "$BENCH_DIR/${bench}.sea" 2>/dev/null || echo "0 0")
    SEA_TIME[$bench]=$(echo $sea_result | cut -d' ' -f1)
    SEA_MEM[$bench]=$(echo $sea_result | cut -d' ' -f2)
    echo -ne "sea✓ "

    seajit_result=$(run_bench "$SEA_JIT_BIN --jit run" "$BENCH_DIR/${bench}.sea" 2>/dev/null || echo "0 0")
    SEA_JIT_TIME[$bench]=$(echo $seajit_result | cut -d' ' -f1)
    SEA_JIT_MEM[$bench]=$(echo $seajit_result | cut -d' ' -f2)
    echo -ne "jit✓ "

    lua_result=$(run_bench "$LUA_BIN" "$BENCH_DIR/${bench}.lua" 2>/dev/null || echo "0 0")
    LUA_TIME[$bench]=$(echo $lua_result | cut -d' ' -f1)
    LUA_MEM[$bench]=$(echo $lua_result | cut -d' ' -f2)
    echo -ne "lua✓ "

    py_result=$(run_bench "$PYTHON_BIN" "$BENCH_DIR/${bench}.py" 2>/dev/null || echo "0 0")
    PY_TIME[$bench]=$(echo $py_result | cut -d' ' -f1)
    PY_MEM[$bench]=$(echo $py_result | cut -d' ' -f2)
    echo -e "py✓"
done

echo ""
echo -e "${BLUE}══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════${NC}"
printf "%-32s  %-15s  %-15s  %-15s  %-15s  %-9s  %-9s  %-9s\n" \
    "Benchmark" "Sea" "Sea+JIT" "Lua" "Python" "JITvsSea" "vsLua" "vsPython"
echo -e "${BLUE}══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════${NC}"

for bench_entry in "${BENCHMARKS[@]}"; do
    bench="${bench_entry%%:*}"
    desc="${bench_entry#*:}"

    sea_t="${SEA_TIME[$bench]}"
    sea_m="${SEA_MEM[$bench]}"
    sj_t="${SEA_JIT_TIME[$bench]}"
    sj_m="${SEA_JIT_MEM[$bench]}"
    lua_t="${LUA_TIME[$bench]}"
    lua_m="${LUA_MEM[$bench]}"
    py_t="${PY_TIME[$bench]}"
    py_m="${PY_MEM[$bench]}"

    printf "%-32s  %-15s  %-15s  %-15s  %-15s  " \
        "$desc" \
        "$(format_time $sea_t)/$(format_mem $sea_m)" \
        "$(format_time $sj_t)/$(format_mem $sj_m)" \
        "$(format_time $lua_t)/$(format_mem $lua_m)" \
        "$(format_time $py_t)/$(format_mem $py_m)"
    print_pct "$sj_t" "$sea_t"   # JIT vs Sea (positive = JIT faster)
    printf "  "
    print_pct "$sea_t" "$lua_t"
    printf "  "
    print_pct "$sea_t" "$py_t"
    echo ""
done

echo -e "${BLUE}══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════${NC}"
echo ""

# Save JSON results
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
RESULT_FILE="$RESULTS_DIR/results_$(date +%Y%m%d_%H%M%S).json"

{
echo "{"
echo "  \"timestamp\": \"$TIMESTAMP\","
echo "  \"versions\": {"
echo "    \"sea\": \"$("$SEA_BIN" version 2>/dev/null)\","
echo "    \"sea_jit\": \"$([ -x "$SEA_JIT_BIN" ] && "$SEA_JIT_BIN" version 2>/dev/null || echo "n/a") (--jit)\","
echo "    \"lua\": \"$("$LUA_BIN" -v 2>&1 | head -1)\","
echo "    \"python\": \"$("$PYTHON_BIN" --version 2>&1)\""
echo "  },"
echo "  \"benchmarks\": {"

first=true
for bench_entry in "${BENCHMARKS[@]}"; do
    bench="${bench_entry%%:*}"
    desc="${bench_entry#*:}"
    if [ "$first" = true ]; then first=false; else echo "    ,"; fi
    cat << EOF
    "$bench": {
      "description": "$desc",
      "sea":     {"time_ms": ${SEA_TIME[$bench]}, "mem_kb": ${SEA_MEM[$bench]}},
      "sea_jit": {"time_ms": ${SEA_JIT_TIME[$bench]}, "mem_kb": ${SEA_JIT_MEM[$bench]}},
      "lua":     {"time_ms": ${LUA_TIME[$bench]}, "mem_kb": ${LUA_MEM[$bench]}},
      "python":  {"time_ms": ${PY_TIME[$bench]}, "mem_kb": ${PY_MEM[$bench]}}
    }
EOF
done
echo "  }"
echo "}"
} > "$RESULT_FILE"

echo -e "  ${GREEN}Results saved to: $RESULT_FILE${NC}"
echo ""

# Write docs/benchmarks.md (generated file — edit this script, not the doc)
write_bench_doc() {
    local out="$1"

    # Build the benchmark section
    local section
    section="# Benchmarks\n\n"
    section+="Comparison of Sea, Sea with JIT (\`--jit\`), Lua 5.4, and Python 3.12 on common workloads.\n"
    section+="Each benchmark was run ${RUNS} times; results show average time and peak memory.\n\n"
    section+="> Generated: ${TIMESTAMP}\n\n"
    section+="| Benchmark | Sea | Sea+JIT | Lua | Python |\n"
    section+="|-----------|-----|---------|-----|--------|\n"

    for bench_entry in "${BENCHMARKS[@]}"; do
        bench="${bench_entry%%:*}"
        desc="${bench_entry#*:}"

        sea_t="${SEA_TIME[$bench]}"
        sea_m="${SEA_MEM[$bench]}"
        sj_t="${SEA_JIT_TIME[$bench]}"
        sj_m="${SEA_JIT_MEM[$bench]}"
        lua_t="${LUA_TIME[$bench]}"
        lua_m="${LUA_MEM[$bench]}"
        py_t="${PY_TIME[$bench]}"
        py_m="${PY_MEM[$bench]}"

        section+="| ${desc} | $(format_time $sea_t) / $(format_mem $sea_m) | $(format_time $sj_t) / $(format_mem $sj_m) | $(format_time $lua_t) / $(format_mem $lua_m) | $(format_time $py_t) / $(format_mem $py_m) |\n"
    done

    section+="\n_Time: wall-clock average. Memory: peak RSS. Lower is better. The JIT (off by default)\n"
    section+="compiles hot functions and hot loops (incl. the top-level chunk, entered mid-loop via OSR)\n"
    section+="in a background compile-server process, so the interpreter never waits for it. It speeds up\n"
    section+="compute-bound code — integer/float arithmetic, numeric loops, calls, array and field access\n"
    section+="(e.g. \`loops\`, \`sort\`, \`matrix_mul\`, \`fib_recursive\`). Programs shorter than one compile\n"
    section+="(~30-50ms: \`primes\`, \`fib_iterative\`, \`closures\`, \`string_ops\`) finish before native\n"
    section+="code is ready and run at interpreter speed; workloads dominated by the C runtime (\`map_ops\`)\n"
    section+="gain little._\n\n"
    section+="### Benchmark Details\n\n"
    section+="| Name | Description | JIT vs Sea | Sea vs Lua | Sea vs Python |\n"
    section+="|------|-------------|------------|------------|---------------|\n"
    for bench_entry in "${BENCHMARKS[@]}"; do
        bench="${bench_entry%%:*}"
        desc="${bench_entry#*:}"
        sea_t="${SEA_TIME[$bench]}"
        sj_t="${SEA_JIT_TIME[$bench]}"
        lua_t="${LUA_TIME[$bench]}"
        py_t="${PY_TIME[$bench]}"

        jit_ratio=$(format_pct "$sj_t" "$sea_t")
        lua_ratio=$(format_pct "$sea_t" "$lua_t")
        py_ratio=$(format_pct "$sea_t" "$py_t")

        section+="| \`${bench}\` | ${desc} | ${jit_ratio} | ${lua_ratio} | ${py_ratio} |\n"
    done
    section+="\n> Run benchmarks: \`bash tests/compare/run_benchmarks.sh\` (auto-builds \`bin/sea-jit\` via \`make sea-jit\`)\n"

    {
        printf "%b" "${section}"   # %b: expand \n but treat "%" in the text literally
        printf "\n<!-- Generated by tests/compare/run_benchmarks.sh — do not edit by hand. -->\n"
    } > "$out"
    echo "wrote $out"
}

echo -ne "  Writing docs/benchmarks.md ... "
write_bench_doc "$BENCH_DOC"
echo ""
echo -e "  ${GREEN}Done!${NC}"
