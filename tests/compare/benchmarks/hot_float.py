def fkernel(x, n):
    s = 0.0
    for i in range(0, n + 1):
        s = s + x * 1.5 - 0.5
    return s

total = 0.0
for k in range(1, 300001):
    total += fkernel(1.0 * (k % 100), 100)
print(total)
