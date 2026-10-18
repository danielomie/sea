# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause
# Sales report from a CSV: revenue in total, by region, by month, top products.
import csv
import sys
from collections import defaultdict

by_region = defaultdict(float)
by_month = defaultdict(float)
by_product = defaultdict(float)
total = 0.0
rows = 0
with open(sys.argv[1], newline="") as f:
    for r in csv.DictReader(f):
        revenue = int(r["qty"]) * float(r["unit_price"])
        total += revenue
        rows += 1
        by_region[r["region"]] += revenue
        by_month[r["date"][:7]] += revenue
        by_product[r["product"]] += revenue

print(f"rows: {rows}")
print(f"total: {total:.2f}")
print("by region:")
for region in sorted(by_region):
    print(f"  {region:<8} {by_region[region]:>14.2f}")
print("by month:")
for month in sorted(by_month):
    print(f"  {month} {by_month[month]:>14.2f}")
print("top products:")
for product, revenue in sorted(by_product.items(), key=lambda kv: (-kv[1], kv[0]))[:5]:
    print(f"  {product} {revenue:.2f}")
