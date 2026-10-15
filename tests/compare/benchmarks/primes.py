N = 500000
sieve = bytearray([1]) * (N + 1)
sieve[0] = 0
sieve[1] = 0
i = 2
while i * i <= N:
    if sieve[i]:
        j = i * i
        while j <= N:
            sieve[j] = 0
            j += i
    i += 1
print(sum(sieve))
