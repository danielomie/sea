# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause
# Inventory of a directory tree: files and bytes by extension, the largest files.
import os
import sys
from collections import defaultdict

root = sys.argv[1]
count = defaultdict(int)
size = defaultdict(int)
files = []
for dirpath, dirnames, filenames in os.walk(root):
    for name in filenames:
        path = os.path.join(dirpath, name)
        n = os.path.getsize(path)
        ext = os.path.splitext(name)[1] or "(none)"
        count[ext] += 1
        size[ext] += n
        files.append((n, os.path.relpath(path, root)))

print(f"files: {len(files)}, bytes: {sum(size.values())}")
for ext in sorted(count):
    print(f"  {ext:<8} {count[ext]:>5} {size[ext]:>10}")
print("largest:")
for n, path in sorted(files, key=lambda f: (-f[0], f[1]))[:5]:
    print(f"  {n:>6} {path}")
