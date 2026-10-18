#!/usr/bin/env python3
# Copyright (c) 2026 The Sea Authors
# SPDX-License-Identifier: BSD-3-Clause
"""Generates the input data of the script comparison, deterministically,
into the directory given as argument: sales.csv, access.log, corpus.txt,
tree/ and orders.json."""
import json
import os
import random
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rnd = random.Random(42)

# sales.csv: 300k rows
regions = ["north", "south", "east", "west", "center"]
products = [f"product-{i:03d}" for i in range(200)]
with open(os.path.join(out, "sales.csv"), "w") as f:
    f.write("date,region,product,qty,unit_price\n")
    for _ in range(300_000):
        m = rnd.randint(1, 12)
        d = rnd.randint(1, 28)
        f.write(f"2025-{m:02d}-{d:02d},{rnd.choice(regions)},{rnd.choice(products)},"
                f"{rnd.randint(1, 20)},{rnd.randint(100, 99999) / 100:.2f}\n")

# access.log: 300k lines
paths = ["/", "/login", "/api/items", "/api/items/{}", "/api/users/{}", "/static/app.js", "/search"]
statuses = [200] * 40 + [201, 204, 301, 302, 304] * 2 + [400, 401, 403, 404, 404, 500, 502, 503]
with open(os.path.join(out, "access.log"), "w") as f:
    for i in range(300_000):
        ip = f"10.{rnd.randint(0, 3)}.{rnd.randint(0, 20)}.{rnd.randint(1, 50)}"
        h = rnd.randint(0, 23)
        p = rnd.choice(paths).format(rnd.randint(1, 5000))
        method = "POST" if p == "/login" else rnd.choice(["GET", "GET", "GET", "PUT", "DELETE"])
        f.write(f'{ip} - - [18/Oct/2026:{h:02d}:{rnd.randint(0, 59):02d}:{rnd.randint(0, 59):02d} +0000] '
                f'"{method} {p} HTTP/1.1" {rnd.choice(statuses)} {rnd.randint(0, 50000)} '
                f'{rnd.randint(1, 3000) / 1000:.3f}\n')

# corpus.txt: ~5 MB of words with a Zipf-like distribution
syll = ["ka", "lo", "mi", "ra", "te", "su", "no", "vi", "pe", "do", "an", "el", "or", "ti", "ma"]
vocab = sorted({"".join(rnd.choice(syll) for _ in range(rnd.randint(1, 4))) for _ in range(8000)})
weights = [1 / (i + 1) for i in range(len(vocab))]
rnd.shuffle(vocab)
with open(os.path.join(out, "corpus.txt"), "w") as f:
    words = rnd.choices(vocab, weights, k=900_000)
    for i in range(0, len(words), 12):
        line = " ".join(words[i:i + 12])
        if rnd.random() < 0.3:
            line = line.capitalize() + "."
        f.write(line + "\n")

# tree/: 3000 files in nested directories
exts = [".txt", ".log", ".json", ".csv", ".md", ".py", ".sea", ".bin"]
for i in range(3000):
    d = os.path.join(out, "tree", f"d{rnd.randint(0, 9)}", f"s{rnd.randint(0, 19)}")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, f"file{i:04d}{rnd.choice(exts)}"), "wb") as f:
        f.write(b"x" * rnd.randint(0, 20000))

# orders.json: 100k orders
orders = []
for i in range(100_000):
    items = [{"sku": rnd.choice(products), "qty": rnd.randint(1, 5), "price": rnd.randint(100, 9999) / 100}
             for _ in range(rnd.randint(1, 4))]
    orders.append({"id": i, "customer": f"c{rnd.randint(1, 5000):05d}",
                   "status": rnd.choice(["paid", "paid", "paid", "shipped", "cancelled", "refunded"]),
                   "items": items})
with open(os.path.join(out, "orders.json"), "w") as f:
    json.dump(orders, f)
