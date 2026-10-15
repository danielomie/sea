a = 0
b = 1
mod = 1000000007
for i in range(2, 1000001):
    c = (a + b) % mod
    a = b
    b = c
print(b)
