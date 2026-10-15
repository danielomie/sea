local nums = {}
local x = 12345
for i = 1, 10000 do
    x = (x * 1103515245 + 12345) % 4194304
    nums[i] = x % 100000
end
local n = #nums
for i = 2, n do
    local key = nums[i]
    local j = i - 1
    while j >= 1 and nums[j] > key do
        nums[j+1] = nums[j]
        j = j - 1
    end
    nums[j+1] = key
end
local sum = 0
for i = 1, n do sum = sum + nums[i] end
print(sum)
