# Everyday scripts: Sea and Python

The kind of scripts one asks an assistant for, written in both languages
the way each is usually written (no tuning for speed), on the same
generated data. Both versions must print the same output (the JSON script
also writes the same file). Wall time, best of 3 runs; lines of code
without blank and comment lines. Regenerate with
`bash tests/compare/scripts/run_scripts.sh` (sources in `tests/compare/scripts/`).

| Script | What it does | Python | Sea | Python time / Sea time | Lines (Py / Sea) | Same output |
|--------|--------------|--------|-----|------------------------|------------------|-------------|
| `csv_report` | Sales report from a 300k-row CSV (sums by region, month, product) | 1.45 s | 0.48 s | 3.0× | 27 / 22 | yes |
| `log_stats` | HTTP access log, 300k lines: regex per line, counters, p95 | 2.15 s | 2.97 s | 0.7× | 37 / 44 | yes |
| `word_freq` | Word frequencies of a 7 MB text (regex findall, counter) | 0.52 s | 0.39 s | 1.3× | 11 / 10 | yes |
| `file_tree` | Inventory of 3000 files: walk, stat, totals by extension | 0.09 s | 0.04 s | 2.3× | 21 / 17 | yes |
| `json_transform` | 19 MB of JSON orders: decode, filter, aggregate, encode | 0.81 s | 0.45 s | 1.8× | 22 / 22 | yes |
| **total** | | **5.03 s** | **4.33 s** | **1.2×** | | |

Machine: x86_64, 4 CPUs; Python 3.12.3; sea version 0.2.0.

## What the comparison showed

**Speed.** Sea was faster on 4 of the 5 scripts: 3× on the CSV report (the
CSV reader is C, the loop is light), about 2× on the JSON transform and on
the file inventory (startup of 4 ms against Python's 16 ms counts on short
scripts), 1.3× on word counting. It is slower on regex-heavy line parsing
(`log_stats`, 0.7×): a regex match costs about 4.5 µs per line against
Python's 1 µs — half in the search (Sea's engine guarantees linear time;
Python's `sre` backtracks without that guarantee), half in building the
match table (`text`, `groups`, `spans`, `named`), which Python creates
lazily. That is the place to improve next.

**Length.** The Sea versions are about as long as the Python ones (10–44
lines). What makes them longer here and there:

- no `defaultdict` / `Counter`: `m[k] = (m[k] or 0) + 1`;
- sorting by two keys needs a comparator
  (`function(a, b) return a[1] > b[1] or (a[1] == b[1] and a[0] < b[0]) end`)
  where Python writes `key=lambda kv: (-kv[1], kv[0])`.

**What the exercise fixed in Sea** (each found by writing these scripts):

- `stream.scanner` copied the rest of its buffer for every token (quadratic):
  log parsing took 18 s; the built-in split modes now read the buffer in place;
- the regex engine got a bounded backtracker (RE2's BitState) for short texts
  and windows of long ones, ASCII bitmaps for classes, a per-thread cache of
  patterns given as strings, and lighter match tables: 3–5× faster;
- `f()?` is now a statement (`fs.write(path, data)?`), instead of
  `local _ = fs.write(path, data)?`;
- the AI guide gained two rules: a regex with `]]` cannot be written in
  `[[...]]` (use `[=[...]=]`), and `print(a, b)` separates with a tab.
