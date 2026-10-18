# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause
# Load orders from JSON, keep the paid and shipped ones, total them per
# customer, write the top customers as JSON and print a summary.
import json
import sys

with open(sys.argv[1]) as f:
    orders = json.load(f)

per_customer = {}
kept = 0
for o in orders:
    if o["status"] not in ("paid", "shipped"):
        continue
    kept += 1
    total = sum(item["qty"] * item["price"] for item in o["items"])
    c = per_customer.setdefault(o["customer"], {"customer": o["customer"], "orders": 0, "total": 0.0})
    c["orders"] += 1
    c["total"] += total

top = sorted(per_customer.values(), key=lambda c: (-c["total"], c["customer"]))[:10]
for c in top:
    c["total"] = round(c["total"], 2)
with open(sys.argv[2], "w") as f:
    json.dump(top, f, indent=2, sort_keys=True)
print(f"orders: {len(orders)}, kept: {kept}, customers: {len(per_customer)}")
for c in top[:3]:
    print(f"  {c['customer']} {c['orders']} {c['total']:.2f}")
