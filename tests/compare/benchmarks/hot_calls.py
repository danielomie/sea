def kernel(a, n):
    s = 0
    for i in range(0, n + 1):
        s = s + a * i - i
    return s

total = 0
for k in range(1, 300001):
    total += kernel(k % 100, 100)
print(total)
