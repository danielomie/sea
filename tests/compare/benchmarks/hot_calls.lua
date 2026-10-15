local function kernel(a, n)
    local s = 0
    for i = 0, n do
        s = s + a * i - i
    end
    return s
end

local total = 0
for k = 1, 300000 do
    total = total + kernel(k % 100, 100)
end
print(total)
