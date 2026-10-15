import io

# string concat
s = ""
for i in range(50000):
    s = s + "hello world "
print(len(s))

# string upper/lower
t = "The quick brown fox jumps over the lazy dog"
count = 0
for i in range(100000):
    u = t.upper()
    l = u.lower()
    count += len(l)
print(count)

# string split
parts = []
csv = ",".join("field" + str(i+1) for i in range(1000))
parts = csv.split(",")
print(len(parts))
