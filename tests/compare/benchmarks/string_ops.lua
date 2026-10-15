-- string concat
local s = ""
for i = 1, 50000 do
    s = s .. "hello world "
end
print(#s)

-- string upper/lower
local t = "The quick brown fox jumps over the lazy dog"
local count = 0
for i = 1, 100000 do
    local u = string.upper(t)
    local l = string.lower(u)
    count = count + #l
end
print(count)

-- string split simulation (Lua has no built-in split)
local csv = ""
for i = 1, 1000 do
    if i > 1 then csv = csv .. "," end
    csv = csv .. "field" .. i
end
local parts = {}
for field in csv:gmatch("[^,]+") do
    table.insert(parts, field)
end
print(#parts)
