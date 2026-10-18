## What the comparison showed

**Speed.** Sea was faster on 4 of the 5 scripts: 3× on the CSV report (the
CSV reader is C, the loop is light), about 2× on the JSON transform and on
the file inventory (startup of 4 ms against Python's 16 ms counts on short
scripts), 1.2–1.3× on word counting. It is slower on regex-heavy line parsing
(`log_stats`, 0.7–0.8×): a regex match costs about 4.5 µs per line against
Python's 1 µs — half in the search (Sea's engine guarantees linear time;
Python's `sre` backtracks without that guarantee), half in building the
match table (`text`, `groups`, `spans`, `named`), which Python creates
lazily. That is the place to improve next.

**Length.** The Sea versions are as long as the Python ones or shorter
(8–36 lines against 11–37). The first versions were longer (up to 44
lines): Sea had no `Counter` (`m[k] = (m[k] or 0) + 1` everywhere) and
sorting by two keys needed a comparator
(`function(a, b) return a[1] > b[1] or (a[1] == b[1] and a[0] < b[0]) end`)
where Python writes `key=lambda kv: (-kv[1], kv[0])`. That gave
`list.inc(m, k [, n])`, `list.most_common(m [, n])` and keys of several
fields in `sort.by` (`sort.by(a, ["-total", "name"])`, or a key function
returning an array, compared like a tuple).

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
