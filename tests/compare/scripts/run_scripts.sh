#!/usr/bin/env bash
# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause

# Everyday scripts written in Sea and in Python: checks that both give the
# same output, times them (best of RUNS) and counts their lines of code;
# writes docs/scripts.md. Usage: bash tests/compare/scripts/run_scripts.sh

set -euo pipefail
export LC_ALL=C
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$DIR/../../.." && pwd)"
SEA="${SEA_BIN:-$REPO/bin/sea}"
PY="${PYTHON_BIN:-$(command -v python3)}"
DATA="${DATA_DIR:-${TMPDIR:-/tmp}/sea-scripts-data}"
RUNS="${RUNS:-3}"
DOC="$REPO/docs/scripts.md"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/py" "$OUT/sea"

[ -f "$DATA/orders.json" ] || { echo "generating data in $DATA ..."; "$PY" "$DIR/gen_data.py" "$DATA"; }

# name|description|arguments (DATA and OUT are replaced)
SCRIPTS=(
  "csv_report|Sales report from a 300k-row CSV (sums by region, month, product)|DATA/sales.csv"
  "log_stats|HTTP access log, 300k lines: regex per line, counters, p95|DATA/access.log"
  "word_freq|Word frequencies of a 7 MB text (regex findall, counter)|DATA/corpus.txt"
  "file_tree|Inventory of 3000 files: walk, stat, totals by extension|DATA/tree"
  "json_transform|19 MB of JSON orders: decode, filter, aggregate, encode|DATA/orders.json OUT/result.json"
)

best() {   # best wall time of RUNS runs, in seconds
  local b=""
  for _ in $(seq "$RUNS"); do
    local s e
    s=$(date +%s.%N)
    "$@" > "$OUT/last.out" || { echo "failed: $*" >&2; exit 1; }
    e=$(date +%s.%N)
    local t
    t=$(echo "$e - $s" | bc)
    if [ -z "$b" ] || (( $(echo "$t < $b" | bc) )); then b=$t; fi
  done
  echo "$b"
}

loc() { grep -cvE '^\s*($|#|--)' "$1"; }   # lines that are not blank or comments

rows=""
total_sea=0
total_py=0
for entry in "${SCRIPTS[@]}"; do
  IFS='|' read -r name desc args <<< "$entry"
  a_sea=${args//DATA/$DATA}; a_sea=${a_sea//OUT/$OUT/sea}
  a_py=${args//DATA/$DATA}; a_py=${a_py//OUT/$OUT/py}
  t_py=$(best "$PY" "$DIR/$name.py" $a_py); cp "$OUT/last.out" "$OUT/$name.py.out"
  t_sea=$(best "$SEA" "$DIR/$name.sea" $a_sea); cp "$OUT/last.out" "$OUT/$name.sea.out"
  same="yes"
  cmp -s "$OUT/$name.py.out" "$OUT/$name.sea.out" || same="**NO**"
  if [ "$name" = json_transform ]; then
    "$PY" -c "import json,sys; a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); sys.exit(a!=b)" \
      "$OUT/py/result.json" "$OUT/sea/result.json" || same="**NO** (file)"
  fi
  ratio=$(printf '%.1f' "$(echo "scale=3; $t_py / $t_sea" | bc)")
  total_sea=$(echo "$total_sea + $t_sea" | bc)
  total_py=$(echo "$total_py + $t_py" | bc)
  printf "%-15s python %6.2fs  sea %6.2fs  x%-5s same: %s\n" "$name" "$t_py" "$t_sea" "$ratio" "$same"
  rows+="| \`$name\` | $desc | $(printf '%.2f' "$t_py") s | $(printf '%.2f' "$t_sea") s | ${ratio}× | $(loc "$DIR/$name.py") / $(loc "$DIR/$name.sea") | $same |"$'\n'
done

{
  echo "# Everyday scripts: Sea and Python"
  echo
  echo "The kind of scripts one asks an assistant for, written in both languages"
  echo "the way each is usually written (no tuning for speed), on the same"
  echo "generated data. Both versions must print the same output (the JSON script"
  echo "also writes the same file). Wall time, best of $RUNS runs; lines of code"
  echo "without blank and comment lines. Regenerate with"
  echo "\`bash tests/compare/scripts/run_scripts.sh\` (sources in \`tests/compare/scripts/\`)."
  echo
  echo "| Script | What it does | Python | Sea | Python time / Sea time | Lines (Py / Sea) | Same output |"
  echo "|--------|--------------|--------|-----|------------------------|------------------|-------------|"
  printf "%s" "$rows"
  echo "| **total** | | **$(printf '%.2f' "$total_py") s** | **$(printf '%.2f' "$total_sea") s** | **$(printf '%.1f' "$(echo "scale=3; $total_py / $total_sea" | bc)")×** | | |"
  echo
  echo "Machine: $(uname -m), $(nproc) CPUs; $("$PY" --version 2>&1); $("$SEA" version)."
  if [ -f "$DIR/NOTES.md" ]; then echo; cat "$DIR/NOTES.md"; fi
} > "$DOC"
echo "wrote $DOC"
