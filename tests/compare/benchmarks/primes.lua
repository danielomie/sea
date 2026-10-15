local N = 500000
local sieve = {}
for i = 0, N do sieve[i] = true end
sieve[0] = false
sieve[1] = false
local i = 2
while i * i <= N do
    if sieve[i] then
        local j = i * i
        while j <= N do
            sieve[j] = false
            j = j + i
        end
    end
    i = i + 1
end
local count = 0
for i = 2, N do
    if sieve[i] then count = count + 1 end
end
print(count)
