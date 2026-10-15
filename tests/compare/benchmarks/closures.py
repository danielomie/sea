def make_adder(n):
    return lambda x: x + n
adders = [make_adder(i) for i in range(1, 100001)]
s = sum(a(1) for a in adders)
print(s)
