local N = 100
local a, b, c = {}, {}, {}
for i = 1, N do
    a[i], b[i], c[i] = {}, {}, {}
    for j = 1, N do
        a[i][j] = ((i-1) * N + (j-1) + 1) % 100
        b[i][j] = ((j-1) * N + (i-1) + 1) % 100
        c[i][j] = 0
    end
end
for iter = 1, 50 do
    for i = 1, N do
        local ai, ci = a[i], c[i]
        for j = 1, N do
            local s = 0
            for k = 1, N do
                s = s + ai[k] * b[k][j]
            end
            ci[j] = s
        end
    end
end
local sum = 0
for i = 1, N do
    for j = 1, N do sum = sum + c[i][j] end
end
print(sum)
