## What the comparison showed

**Speed.** Sea is faster on all 5 scripts: 3× on the CSV report (the CSV
reader is C, the loop is light), about 2× on the JSON transform and on the
file inventory (startup of 4 ms against Python's 16 ms counts on short
scripts), 1.1–1.3× on word counting and 1.2× on regex-heavy line parsing
(`log_stats`). That one started at 0.7×: a match cost 4.5 µs per line
against Python's 1.5 µs (`match` + `groups()`). The search got faster
(each regex program validated once, not per call; loops like `\S+ ` or
`[^"]*"` read their run in one go, since their exit character cannot
continue them; the backtracker's buffers reused) and `re.captures` returns
just the strings, without the match table's `spans` and `named` (32
allocations down to 10): 1.8 µs per line now.

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
