N = 100
a = [[(i * N + j + 1) % 100 for j in range(N)] for i in range(N)]
b = [[(j * N + i + 1) % 100 for j in range(N)] for i in range(N)]
c = [[0] * N for _ in range(N)]

for _ in range(50):
    for i in range(N):
        ai = a[i]
        ci = c[i]
        for j in range(N):
            s = 0
            for k in range(N):
                s += ai[k] * b[k][j]
            ci[j] = s

print(sum(c[i][j] for i in range(N) for j in range(N)))
