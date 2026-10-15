nums = []
x = 12345
for i in range(10000):
    x = (x * 1103515245 + 12345) % 4194304
    nums.append(x % 100000)
n = len(nums)
for i in range(1, n):
    key = nums[i]
    j = i - 1
    while j >= 0 and nums[j] > key:
        nums[j+1] = nums[j]
        j -= 1
    nums[j+1] = key
print(sum(nums))
