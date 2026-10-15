W = 400
H = 400
MAXITER = 200

count = 0
for py in range(H):
    ci = (py / H) * 2.0 - 1.0
    for px in range(W):
        cr = (px / W) * 3.5 - 2.5
        zr = 0.0
        zi = 0.0
        n = 0
        while n < MAXITER:
            zr2 = zr * zr
            zi2 = zi * zi
            if zr2 + zi2 > 4.0:
                break
            zi = 2.0 * zr * zi + ci
            zr = zr2 - zi2 + cr
            n += 1
        if n == MAXITER:
            count += 1
print(count)
