local function make_adder(n)
    return function(x) return x + n end
end
local adders = {}
for i = 1, 100000 do
    adders[i] = make_adder(i)
end
local sum = 0
for i = 1, 100000 do
    sum = sum + adders[i](1)
end
print(sum)
