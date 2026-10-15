m = {}
for i in range(1, 10000001):
    m[str(i)] = i
s = sum(m[str(i)] for i in range(1, 10000001))
print(s)
