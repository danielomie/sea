import sys
sys.setrecursionlimit(1000000)

def qsort(arr, lo, hi):
    if lo >= hi:
        return
    pivot = arr[hi]
    i = lo - 1
    for j in range(lo, hi):
        if arr[j] <= pivot:
            i += 1
            arr[i], arr[j] = arr[j], arr[i]
    i += 1
    arr[i], arr[hi] = arr[hi], arr[i]
    qsort(arr, lo, i - 1)
    qsort(arr, i + 1, hi)

N = 300000
x = 12345
nums = []
for i in range(N):
    x = (x * 1103515245 + 12345) % 4194304
    nums.append(x % 1000000)

qsort(nums, 0, N - 1)
print(sum(nums))
