# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause
# Word frequencies of a text: totals and the 20 most common words.
import re
import sys
from collections import Counter

counts = Counter()
with open(sys.argv[1]) as f:
    for line in f:
        counts.update(re.findall(r"[a-z]+", line.lower()))

total = sum(counts.values())
print(f"words: {total}, distinct: {len(counts)}")
for word, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:20]:
    print(f"{word:<12} {n:>7} {100 * n / total:6.2f}%")
