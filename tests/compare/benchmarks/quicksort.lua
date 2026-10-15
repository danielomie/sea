local function qsort(arr, lo, hi)
    if lo >= hi then return end
    local pivot = arr[hi]
    local i = lo - 1
    for j = lo, hi-1 do
        if arr[j] <= pivot then
            i = i + 1
            arr[i], arr[j] = arr[j], arr[i]
        end
    end
    i = i + 1
    arr[i], arr[hi] = arr[hi], arr[i]
    qsort(arr, lo, i - 1)
    qsort(arr, i + 1, hi)
end

local N = 300000
local nums = {}
local x = 12345
for i = 1, N do
    x = (x * 1103515245 + 12345) % 4194304
    nums[i] = x % 1000000
end

qsort(nums, 1, N)

local sum = 0
for i = 1, N do sum = sum + nums[i] end
print(sum)
