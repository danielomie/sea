#!/usr/bin/env bash
# Copyright (c) 2026 Daniel
# SPDX-License-Identifier: MIT

# Sea VM unit test runner
# Usage: ./tests/run_tests.sh [test_pattern]
#
# Env: SEA=<binary> (default bin/sea), SEA_FLAGS=<flags placed before the file>,
#      e.g. SEA=bin/sea-jit SEA_FLAGS=--jit (see `make test-jit`);
#      TEST_TIMEOUT=<seconds> per test (default 60; a hang counts as a failure);
#      SEAC=1 compiles each test to a .seac next to it, runs that, and deletes it
#      (see `make test-seac`);
#      BUNDLE=1 makes each test a standalone executable with `sea build` and
#      runs that (see `make test-build`).
#
# Each test is a .sea file in tests/unit/ with a companion .expected file.
# The test passes if the output of running the .sea file matches the .expected file.
# If no .expected file exists, the test just checks for successful execution (exit code 0).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SEA="${SEA:-$REPO_DIR/bin/sea}"
SEA_FLAGS="${SEA_FLAGS:-}"
TEST_TIMEOUT="${TEST_TIMEOUT:-60}"
SEAC="${SEAC:-0}"
BUNDLE="${BUNDLE:-0}"
BUNDLE_DIR=""
if [ "$BUNDLE" = "1" ]; then
    BUNDLE_DIR="$(mktemp -d)"
    trap 'rm -rf "$BUNDLE_DIR"' EXIT
fi
UNIT_DIR="$SCRIPT_DIR/unit"

if [ ! -x "$SEA" ]; then
    echo "Error: sea binary not found at $SEA"
    echo "Run 'make' first."
    exit 1
fi

PASS=0
FAIL=0

if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    NC=''
fi

run_test() {
    local file="$1"
    local name
    name="$(basename "$file" .sea)"
    local expected_file="${file%.sea}.expected"

    # Run and capture output + exit code
    local actual
    local exit_code=0
    local target="$file"
    if [ "$SEAC" = "1" ]; then
        # Next to the source so relative require() paths still resolve.
        target="${file%.sea}.seac"
        if ! "$SEA" compile "$file" -o "$target" >/dev/null 2>&1; then
            echo -e "${RED}FAIL${NC} $name (sea compile failed)"
            FAIL=$((FAIL + 1))
            rm -f "$target"
            return
        fi
    fi
    if [ "$BUNDLE" = "1" ]; then
        target="$BUNDLE_DIR/$name"
        if ! "$SEA" build -o "$target" "$file" >/dev/null 2>&1; then
            echo -e "${RED}FAIL${NC} $name (sea build failed)"
            FAIL=$((FAIL + 1))
            return
        fi
        actual=$(timeout "$TEST_TIMEOUT" "$target" 2>&1) || exit_code=$?
        rm -f "$target"
    else
        actual=$(timeout "$TEST_TIMEOUT" "$SEA" $SEA_FLAGS "$target" 2>&1) || exit_code=$?
    fi
    if [ "$SEAC" = "1" ]; then rm -f "$target"; fi

    if [ "$exit_code" -ne 0 ]; then
        echo -e "${RED}FAIL${NC} $name (exit code $exit_code)"
        echo "$actual" | sed 's/^/    /'
        FAIL=$((FAIL + 1))
        return
    fi

    if [ ! -f "$expected_file" ]; then
        echo -e "${GREEN}PASS${NC} $name"
        PASS=$((PASS + 1))
        return
    fi

    local expected
    expected="$(cat "$expected_file")"

    if [ "$actual" = "$expected" ]; then
        echo -e "${GREEN}PASS${NC} $name"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL${NC} $name"
        echo "  Expected:"
        echo "$expected" | sed 's/^/    /'
        echo "  Got:"
        echo "$actual" | sed 's/^/    /'
        FAIL=$((FAIL + 1))
    fi
}

# Filter pattern (optional first argument)
PATTERN="${1:-}"

echo "Sea VM Unit Tests"
echo "================================"

for f in "$UNIT_DIR"/*.sea; do
    name="$(basename "$f" .sea)"
    if [ -n "$PATTERN" ] && [[ "$name" != *"$PATTERN"* ]]; then
        continue
    fi
    run_test "$f"
done

echo "================================"
TOTAL=$((PASS + FAIL))
echo -e "Results: ${GREEN}$PASS${NC}/${TOTAL} passed"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
