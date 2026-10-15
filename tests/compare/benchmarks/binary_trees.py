import sys
sys.setrecursionlimit(1000000)

def make_tree(depth):
    if depth == 0:
        return (None, None)
    return (make_tree(depth - 1), make_tree(depth - 1))

def tree_sum(t):
    if t[0] is None:
        return 1
    return 1 + tree_sum(t[0]) + tree_sum(t[1])

MIN_DEPTH = 4
MAX_DEPTH = 16

stretch_depth = MAX_DEPTH + 1
stretch = make_tree(stretch_depth)
stretch_check = tree_sum(stretch)

long_lived = make_tree(MAX_DEPTH)

total = 0
depth = MIN_DEPTH
while depth <= MAX_DEPTH:
    iters = 1 << (MAX_DEPTH - depth + MIN_DEPTH)
    check = 0
    for _ in range(iters):
        check += tree_sum(make_tree(depth))
    total += check
    depth += 2

print(stretch_check)
print(tree_sum(long_lived))
print(total)
