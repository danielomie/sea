# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause
# Statistics of an HTTP access log: status codes, top clients and paths, p95 latency.
import math
import re
import sys
from collections import Counter

LINE = re.compile(r'^(\S+) \S+ \S+ \[[^:]+:(\d\d):[^\]]+\] "(\S+) (\S+) [^"]*" (\d{3}) (\d+) ([\d.]+)$')
statuses, ips, paths, hours = Counter(), Counter(), Counter(), Counter()
times = []
total_bytes = 0
bad = 0
with open(sys.argv[1]) as f:
    for line in f:
        m = LINE.match(line.rstrip("\n"))
        if not m:
            bad += 1
            continue
        ip, hour, method, path, status, size, secs = m.groups()
        statuses[status] += 1
        ips[ip] += 1
        paths[re.sub(r"/\d+", "/:id", path)] += 1
        hours[hour] += 1
        total_bytes += int(size)
        times.append(float(secs))

times.sort()
p95 = times[math.ceil(0.95 * len(times)) - 1]
print(f"requests: {len(times)} (unparsed: {bad})")
print(f"bytes: {total_bytes}")
print(f"p95: {p95:.3f} s, mean: {sum(times) / len(times):.4f} s")
print("status:")
for s in sorted(statuses):
    print(f"  {s} {statuses[s]}")
print("top clients:")
for ip, n in sorted(ips.items(), key=lambda kv: (-kv[1], kv[0]))[:5]:
    print(f"  {ip} {n}")
print("paths:")
for p, n in sorted(paths.items(), key=lambda kv: (-kv[1], kv[0])):
    print(f"  {p} {n}")
print("busiest hour:", max(sorted(hours), key=lambda h: hours[h]))
