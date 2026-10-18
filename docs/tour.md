# A Tour of Sea

A quick, example-driven introduction. Every snippet here runs as-is
(`bin/sea snippet.sea`). For the complete list of operators, built-ins and
standard-library functions see the [reference](reference.md); for external
libraries (json, http, …) see [libs.md](libs.md).

---

## Values and types

```sea
print(type(nil), type(true), type(42), type(3.14), type("hi"))
-- nil  bool  int  number  string

print(7 // 2, 7 / 2, 2 ^ 10, 0xFF)   -- 3  3.5  1024  255
print(-7 // 2, -7 % 3)               -- -4  2   (// and % floor)
print(1 == 1.0, 10 .. "px")          -- true  10px
```

- `int` (48-bit signed) and `number` (IEEE double) are distinct types; integer
  literals are `int`, literals with `.`/`e` and the result of `/` are `number`.
- Strings are immutable byte strings; `..` concatenates (numbers are converted);
  backquotes interpolate: `` `${n} items` ``.
- Variables are `local`; assigning an undeclared name creates a global.

## Collections

```sea
-- array: 0-indexed, dynamic
local a = [10, 20]
a[] = 30                     -- append
print(a[0], a[-1], len(a))   -- 10  30  3
local grid = [ [1, 2], [3] ] -- nested: a space after "[" ("[[" starts a long string)
del a[0]                     -- remove and shift
print(a[0], len(a))          -- 20  2

-- map: any key type
local m = [=]
m["apple"] = 3
m[42] = "answer"
local v, ok = m["pear"]      -- two-value lookup
print(m["apple"], m[42], v, ok)   -- 3  answer  nil  false

-- struct: fields fixed by the literal (fast, sealed)
local p = {x = 1, y = 2}
p.x = 10
print(p.x + p.y)             -- 12

-- table: `{}` grows freely; `{a, b, c}` is a positional table (keys 0, 1, 2)
local t = {}
t.name = "sea"
t[7] = "seven"
local list = {"a", "b", "c"}
print(t.name, t[7], list[1], len(list))   -- sea  seven  b  3

-- typed arrays: contiguous int64 / double storage, no per-element refcount
local ia = int_array(100)
for i = 1, 5 do ia[] = i * i end
local fa = float_array()
fa[] = 0.5
print(ia[4], len(ia), fa[0])  -- 25  5  0.5
```

## Control flow

```sea
local n = 7
if n % 2 == 0 then
    print("even")
elseif n > 5 then
    print("odd and big")
else
    print("odd")
end

for i = 1, 3 do print(i) end           -- inclusive: 1 2 3
for i = 10, 1, -4 do print(i) end      -- 10 6 2

local words = ["sea", "vm"]
for i, w in words do print(i, w) end   -- 0 sea / 1 vm

local count = 0
while true do
    count = count + 1
    if count % 2 == 1 then continue end
    if count > 6 then break end
end
print(count)                           -- 8
```

`for-in` works on arrays (index, value), maps and tables (key, value), structs
(field, value) and channels (value, until closed).

## Functions and closures

```sea
local function divmod(a, b) return a // b, a % b end
local q, r = divmod(17, 5)
print(q, r)                            -- 3  2

local function sum(...)
    local total = 0
    local args = {...}
    for i = 0, len(args) - 1 do total = total + args[i] end
    return total
end
print(sum(1, 2, 3, 4))                 -- 10

-- closures capture variables by reference
local function counter()
    local n = 0
    return function() n = n + 1; return n end
end
local c = counter()
c(); c()
print(c())                             -- 3

-- each loop iteration has its own loop variable
local fns = []
for i = 1, 3 do fns[] = function() return i end end
print(fns[0](), fns[1](), fns[2]())    -- 1  2  3
```

Methods are plain functions taking the receiver first; `obj:m(x)` is sugar for
`obj.m(obj, x)`:

```sea
local acct = {}
acct.balance = 0
acct.deposit = function(self, v) self.balance = self.balance + v end
acct:deposit(50)
acct.deposit(acct, 25)
print(acct.balance)                    -- 75
```

## Errors, defer and panic

Errors are values. `?` returns an error from the current function immediately;
otherwise it unwraps the value.

```sea
local function parse_age(s)
    local n = tonumber(s)
    if n == nil then return error("not a number: " .. s) end
    if n < 0 then return error("negative age") end
    return n
end

local function describe(s)
    local age = parse_age(s)?          -- propagates the error
    return "age " .. age
end

local r1 = describe("42")
local r2 = describe("abc")
print(r1, is_error(r2), r2.message)    -- age 42  true  not a number: abc
```

`defer` runs a call when the function returns (also on panic), last-in first-out.
The callee and its arguments are evaluated when `defer` executes:

```sea
local function work()
    local step = 1
    defer print("cleanup, step was", step)   -- prints 1
    defer function() print("closure sees step", step) end   -- prints 2
    step = 2
    print("working")
end
work()
-- working / closure sees step 2 / cleanup, step was 1

local function safe()
    defer function()
        local e = recover()
        if e ~= nil then print("recovered:", e) end
    end
    panic("boom")
end
safe()                                 -- recovered: boom
```

## Goroutines and channels

```sea
local jobs = chan(10)
local results = chan(10)

local function worker(id)
    for j in jobs do results <- j * j end
end

for w = 1, 3 do go worker(w) end
for j = 1, 5 do jobs <- j end
close(jobs)

local total = 0
for i = 1, 5 do total = total + <-results end
print(total)                           -- 55
```

`select` waits on several channel operations (a `case default` makes it
non-blocking):

```sea
local a = chan(1)
local b = chan(1)
b <- "from b"
select
    case v = <-a then
        print("a:", v)
    case v = <-b then
        print("b:", v)
    case default then
        print("nothing ready")
end
-- b: from b
```

Goroutines run on an M:N scheduler (a pool of OS threads). Values sent on a
channel, captured by a goroutine's closure or stored in globals are switched to
atomic reference counting automatically; `sync.mutex()` and `sync.waitgroup()`
are available for shared state (see the [reference](reference.md#module-sync)).

## Modules

```sea
-- math_utils.sea
--   local M = {}
--   M.square = function(x) return x * x end
--   return M
--
-- main.sea
--   local mu = require("./math_utils")
--   print(mu.square(9))
local json = require("../libs/json/json")   -- native module (.so); build with make -C libs/json
print(json.encode({ok = true}))
```

`require` resolves a path relative to the requiring file, then the current
directory, then each directory in `SEA_PATH`; it loads `.sea` sources and
native `.so` libraries.

---

Next: the [reference](reference.md) has every operator and built-in, the
[examples](../examples/) directory has 30 runnable programs, and
[architecture](architecture.md) explains how the VM works.
