# Sea Language Reference

Keywords, operators, and built-in functions available in the Sea scripting language.

---

## Comments & Shebang

```sea
-- single-line comment
--[[ multi-line
     comment ]]
--[=[ also multi-line, level-1 delimiter ]=]
#!/usr/bin/env sea    -- shebang only at the very first line of a file
```

---

## Number Literals

```sea
42          -- integer
3.14        -- float
1e6         -- float (scientific notation)
0xFF        -- hex integer (0x or 0X prefix)
0xDEAD_BEEF -- (any hex digits)
```

Integer (`int`) and float (`number`) are **distinct types**. `1 == 1.0` is **false**.

---

## String Literals

### Quoted strings

```sea
"hello\nworld"   -- double-quoted
'it\'s fine'     -- single-quoted
```

**Escape sequences inside quoted strings:**

| Escape | Meaning |
|--------|---------|
| `\n` | newline |
| `\t` | horizontal tab |
| `\r` | carriage return |
| `\\` | backslash |
| `\'` | single quote |
| `\"` | double quote |
| `\0` | null byte |
| `\a` | bell |
| `\b` | backspace |
| `\f` | form feed |
| `\v` | vertical tab |
| `\xHH` | byte with hex value `HH` (exactly two hex digits) |

### Long strings (no escape processing)

```sea
[[
raw text
no escapes here: \n is literal
]]

[=[
level-1: safe to contain ]] inside
]=]

[==[
level-2: safe to contain ]=] inside
]==]
```

An initial newline immediately after the opening bracket is ignored. The number of `=` signs must match between the opening and closing delimiters.

---

## Keywords

### Literals / values

| Keyword | Description |
|---------|-------------|
| `nil` | The nil value (falsy) |
| `true` | Boolean true |
| `false` | Boolean false |

### Control flow

| Keyword | Description |
|---------|-------------|
| `if` | Conditional — `if cond then ... end` |
| `elseif` | Additional branch in an `if` chain |
| `else` | Fallback branch |
| `then` | Separates condition from body in `if`/`elseif` |
| `end` | Closes `if`, `for`, `while`, `function`, `do` blocks |
| `while` | While loop — `while cond do ... end` |
| `for` | Numeric for — `for i = start, limit [, step] do ... end` |
| `in` | For-in loop — `for k, v in expr do ... end` |
| `do` | Opens a block (`while`/`for`/bare `do...end`) |
| `break` | Exit the nearest `for` or `while` loop |
| `continue` | Skip to the next iteration |
| `return` | Return values from a function |

### Definitions / scope

| Keyword | Description |
|---------|-------------|
| `local` | Declare a local variable — `local x = expr`; multiple: `local a, b = e1, e2`; block form: `local (\n  a = e1\n  b = e2\n)` |
| `const` | Declare a read-only local — `const x = expr`; block form: `const (\n  a = e1\n  b = e2\n)`. Assignment after declaration is a compile error. |
| `function` | Define a function — `function name(params) ... end` or as expression |

### Logic operators

| Keyword | Description |
|---------|-------------|
| `and` | Logical AND (short-circuits, returns one of the operands) |
| `or` | Logical OR (short-circuits, returns one of the operands) |
| `not` | Logical NOT — `not expr` |

### Concurrency / channels

| Keyword | Description |
|---------|-------------|
| `go` | Spawn a goroutine — `go fn(args)` |
| `select` | Select over channel operations |
| `case` | A branch inside a `select` block |
| `default` | Default (non-blocking) branch in `select` |

### Error / resource management

| Keyword | Description |
|---------|-------------|
| `panic` | Start a panic (unwinds, running deferred calls) |
| `recover` | Recover from a panic inside a deferred call |
| `defer` | Schedule a call to run when the enclosing function returns |

### Collections

| Keyword | Description |
|---------|-------------|
| `del` | Delete an element — `del arr[i]` or `del map[k]` |

---

## Operators

### Arithmetic

| Operator | Description |
|----------|-------------|
| `+` | Addition |
| `-` | Subtraction / unary negation |
| `*` | Multiplication |
| `/` | Float division |
| `//` | Integer (floor) division |
| `%` | Modulo |
| `^` | Exponentiation |

### Bitwise

| Operator | Description |
|----------|-------------|
| `&` | Bitwise AND |
| `\|` | Bitwise OR |
| `~` | Bitwise XOR (binary) / bitwise NOT (unary) |
| `<<` | Left shift |
| `>>` | Right shift |

### Comparison

| Operator | Description |
|----------|-------------|
| `==` | Equal (integers and floats are distinct types) |
| `~=` | Not equal |
| `<` | Less than |
| `<=` | Less than or equal |
| `>` | Greater than |
| `>=` | Greater than or equal |

### String

| Operator | Description |
|----------|-------------|
| `..` | String concatenation |
| `...` | Variadic arguments (inside a function body) |

### Channel

| Operator | Description |
|----------|-------------|
| `ch <- v` | Send `v` to channel `ch` |
| `<- ch` | Receive from channel `ch` |

### Error propagation

| Operator | Description |
|----------|-------------|
| `expr?` | If `expr` is an error, return it immediately from the current function; otherwise unwrap the value |

### Assignment

| Operator | Description |
|----------|-------------|
| `=` | Assign |
| `arr[] = v` | Append `v` to array `arr` |

---

## Built-in Functions (globals)

### Core

| Function | Signature | Description |
|----------|-----------|-------------|
| `print` | `print(...)` | Print values separated by tabs, followed by newline |
| `printf` | `printf(fmt, ...)` | Format and print to stdout using `string.format` specifiers (`%d`, `%s`, `%f`, `%x`, etc.); no implicit newline |
| `type` | `type(v) → string` | Return the type name of `v`: `"nil"`, `"bool"`, `"int"`, `"number"`, `"string"`, `"array"`, `"table"`, `"map"`, `"struct"`, `"function"`, `"channel"`, `"int_array"`, `"float_array"` |
| `tostring` | `tostring(v) → string` | Convert any value to its string representation |
| `tonumber` | `tonumber(v) → int\|number\|nil` | Parse a string to a number; returns `nil` on failure |
| `len` | `len(v) → int` | Length of string, array, table, map, struct, int_array, or float_array |
| `error` | `error(msg) → error` | Create an error value |
| `is_error` | `is_error(v) → bool` | Check whether a value is an error |
| `assert` | `assert(v, msg?)` | Abort with `msg` if `v` is falsy |
| `require` | `require(name) → module` | Load a Sea or native module by path/name |
| `int_array` | `int_array(cap?) → IntArray` | Create a typed integer array (no per-element RC) |
| `float_array` | `float_array(cap?) → FloatArray` | Create a typed float array (no per-element RC) |

### Channels

| Function | Signature | Description |
|----------|-----------|-------------|
| `chan` | `chan(cap?) → channel` | Create a channel with optional buffer capacity |
| `close` | `close(ch)` | Close a channel |

### Error handling

| Function | Signature | Description |
|----------|-----------|-------------|
| `panic` | `panic(v)` | Start a panic with value `v` (see [panic and recover](#panic-and-recover)); unrecovered, it prints the traceback and exits with code 1 |
| `recover` | `recover() → value\|nil` | Inside a deferred call, recover the panic value; otherwise returns `nil` |

### Math (global aliases)

| Function | Signature | Description |
|----------|-----------|-------------|
| `isnan` | `isnan(v) → bool` | True if `v` is a float NaN |
| `isinf` | `isinf(v) → bool` | True if `v` is a float ±Inf |

---

## Module: `math`

### Constants

| Name | Value |
|------|-------|
| `math.pi` | π (3.14159…) |
| `math.huge` | Positive infinity |
| `math.maxinteger` | Maximum 64-bit signed integer |
| `math.mininteger` | Minimum 64-bit signed integer |

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `math.abs` | `abs(x) → x` | Absolute value |
| `math.ceil` | `ceil(x) → int` | Round up to nearest integer |
| `math.floor` | `floor(x) → int` | Round down to nearest integer |
| `math.round` | `round(x) → int` | Round to nearest integer |
| `math.trunc` | `trunc(x) → int` | Truncate toward zero |
| `math.sqrt` | `sqrt(x) → number` | Square root |
| `math.exp` | `exp(x) → number` | e^x |
| `math.log` | `log(x [, base]) → number` | Natural log, or log with given base |
| `math.log2` | `log2(x) → number` | Base-2 logarithm |
| `math.log10` | `log10(x) → number` | Base-10 logarithm |
| `math.pow` | `pow(x, y) → number` | x raised to the power y |
| `math.fmod` | `fmod(x, y) → number` | Floating-point remainder |
| `math.sin` | `sin(x) → number` | Sine (radians) |
| `math.cos` | `cos(x) → number` | Cosine (radians) |
| `math.tan` | `tan(x) → number` | Tangent (radians) |
| `math.asin` | `asin(x) → number` | Arc-sine |
| `math.acos` | `acos(x) → number` | Arc-cosine |
| `math.atan` | `atan(x [, y]) → number` | Arc-tangent; two-arg form computes `atan2(x, y)` |
| `math.max` | `max(...) → x` | Maximum of all arguments |
| `math.min` | `min(...) → x` | Minimum of all arguments |
| `math.random` | `random() → number`<br>`random(n) → int ∈ [1,n]`<br>`random(lo, hi) → int ∈ [lo,hi]` | Pseudo-random number (xorshift64; one shared generator, safe to call from several goroutines) |
| `math.randomseed` | `randomseed([seed])` | Seed the RNG; defaults to `time()` |
| `math.isnan` | `isnan(x) → bool` | Same as global `isnan` |
| `math.isinf` | `isinf(x) → bool` | Same as global `isinf` |

---

## Module: `string`

> **Note:** Index arguments (`i`, `j`) are **1-based** and support negative indices (−1 = last character), following Lua convention.

| Function | Signature | Description |
|----------|-----------|-------------|
| `string.len` | `len(s) → int` | Byte length of string |
| `string.sub` | `sub(s, i [, j]) → string` | Substring from byte `i` to `j` (inclusive, 1-based) |
| `string.upper` | `upper(s) → string` | Uppercase copy |
| `string.lower` | `lower(s) → string` | Lowercase copy |
| `string.reverse` | `reverse(s) → string` | Byte-reversed copy |
| `string.rep` | `rep(s, n [, sep]) → string` | Repeat `s` `n` times, optionally joined by `sep` |
| `string.byte` | `byte(s [, i]) → int` | Byte value at position `i` (1-based, default 1) |
| `string.char` | `char(...) → string` | Build a string from byte values |
| `string.format` | `format(fmt, ...) → string` | Printf-style formatting: `%d`, `%i`, `%u`, `%f`, `%e`, `%g`, `%s`, `%c`, `%x`, `%X`, `%%`, with flags, width and precision (`%-8s`, `%05.1f`). No length limit; a plain `%s` copies any bytes. An unknown conversion is written as is |
| `string.find` | `find(s, pattern [, init]) → int\|nil` | Return 1-based start position of first occurrence of `pattern`, or `nil` |
| `string.split` | `split(s, sep) → array` | Split `s` by separator string; returns an array of strings |
| `string.contains` | `contains(s, sub) → bool` | True if `sub` is present in `s` |
| `string.trim` | `trim(s) → string` | Strip leading and trailing whitespace |
| `string.starts_with` | `starts_with(s, prefix) → bool` | True if `s` begins with `prefix` |
| `string.ends_with` | `ends_with(s, suffix) → bool` | True if `s` ends with `suffix` |
| `string.replace` | `replace(s, old, new) → string` | Replace all occurrences of `old` with `new` |
| `string.pad_left` | `pad_left(s, width [, char]) → string` | Right-align `s` in a field of `width`, padding with `char` (default `' '`) |
| `string.pad_right` | `pad_right(s, width [, char]) → string` | Left-align `s` in a field of `width` |
| `string.pad_center` | `pad_center(s, width [, char]) → string` | Center `s` in a field of `width` |
| `string.join` | `join(arr [, sep]) → string` | Join array elements with separator; numbers and other values join as `tostring` shows them. O(N), preferred over incremental `..` |
| `string.normalize_ws` | `normalize_ws(s) → string` | Collapse internal whitespace runs to a single space and strip leading/trailing whitespace |

---

## Module: `io`

### stdin / stdout

| Function | Signature | Description |
|----------|-----------|-------------|
| `io.read` | `read([fmt]) → string\|number\|nil` | Read from stdin. `fmt`: `"*l"` / `"l"` (line, default), `"*n"` / `"n"` (number), `"*a"` / `"a"` (all) |

### Buffered file I/O

Handles are opaque integer values returned by `io.open` and `io.scanner`. Close them with `io.close` when done.

| Function | Signature | Description |
|----------|-----------|-------------|
| `io.open` | `open(path [, mode]) → handle\|nil` | Open a file. `mode`: `"r"` (default), `"w"`, `"a"`, `"rb"`, `"wb"`, `"ab"`. Returns `nil` if the file cannot be opened |
| `io.close` | `close(handle)` | Close a file or scanner handle and free its resources |
| `io.read_line` | `read_line(handle) → string\|nil` | Read one line, stripping the trailing newline. Returns `nil` on EOF |
| `io.read_bytes` | `read_bytes(handle, n) → string\|nil` | Read up to `n` bytes. Returns `nil` on immediate EOF |
| `io.read_all` | `read_all(handle) → string` | Read the entire remaining contents of the file as a string |
| `io.write_string` | `write_string(handle, s)` | Write string `s` to the file (no newline appended) |
| `io.write_line` | `write_line(handle, s)` | Write string `s` followed by a newline |
| `io.flush` | `flush(handle)` | Flush the file's write buffer to the OS |

### Scanner (line-by-line iteration)

`io.scanner` is the idiomatic way to iterate over lines without loading the whole file.

| Function | Signature | Description |
|----------|-----------|-------------|
| `io.scanner` | `scanner(path) → scanner\|nil` | Open a file for line-by-line scanning. Returns `nil` if the file cannot be opened |
| `io.scan` | `scan(scanner) → bool` | Advance to the next line. Returns `true` if a line was read, `false` on EOF |
| `io.scan_text` | `scan_text(scanner) → string` | Return the text of the current line (call after `io.scan` returns `true`) |

```sea
local sc = io.scanner("data.txt")
if sc then
    while io.scan(sc) do
        print(io.scan_text(sc))
    end
    io.close(sc)
end
```

---

## Module: `os`

### Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `os.exit` | `exit([code])` | Terminate the process with exit code (default 0) |
| `os.getenv` | `getenv(name) → string\|nil` | Read an environment variable |
| `os.exec` | `exec(cmd) → {code=int, output=string}` | Run a shell command; captures stdout+stderr |
| `os.readfile` | `readfile(path) → string\|nil` | Read entire file contents as a string |
| `os.filesize` | `filesize(path) → int\|nil` | File size in bytes, or `nil` if not found |
| `os.glob` | `glob(dir, suffix) → array` | Sorted array of full paths in `dir` whose names end with `suffix` |

### Script arguments (`os.args`)

When a `.sea` file is run with `sea file.sea arg1 arg2 ...`, the arguments are available via `os.args`:

| Index | Value |
|-------|-------|
| `os.args[0]` | `nil` (reserved) |
| `os.args[1]` | First argument after the script filename |
| `os.args[N]` | Nth argument |

Use `len(os.args) - 1` to get the count of arguments passed.

```sea
-- sea myscript.sea hello world
local nargs = len(os.args) - 1   -- 2
print(os.args[1])                 -- "hello"
print(os.args[2])                 -- "world"
```

---

## Module: `sync`

| Function | Signature | Description |
|----------|-----------|-------------|
| `sync.mutex` | `mutex() → Mutex` | Create a mutex; methods: `acquire(mu)`, `release(mu)`, `try_lock(mu) → bool` |
| `sync.waitgroup` | `waitgroup() → WaitGroup` | Create a wait-group; methods: `add(wg, n)`, `done(wg)`, `wait(wg)` |
| `sync.once` | `once() → Once` | Create a once-object; method: `do_once(once, fn)` — runs `fn` exactly once across all goroutines |

> **Note:** Sea methods take the receiver as the first explicit argument.
> `mu.acquire(mu)` / `mu.release(mu)`, `wg.add(wg, 1)` / `wg.done(wg)` / `wg.wait(wg)`.

---

## Module: `net`

TCP sockets. Sockets are non-blocking and every wait goes through the netpoller:
inside a goroutine, a call that has to wait **parks only that goroutine** (its
scheduler thread runs other goroutines meanwhile); on the main thread it blocks.
Thousands of idle connections therefore cost no threads. Sockets are plain
integer fds. Failures return an error value (`is_error(x)`, `x.message`); a timeout's
message is `"timeout"`. Timeouts are in milliseconds; omitted or `nil` means wait forever.

| Function | Signature | Description |
|----------|-----------|-------------|
| `net.connect` | `connect(host, port [, timeout]) → fd \| error` | Resolve `host` and connect (tries each address; TCP_NODELAY on). Name lookups run on a small resolver thread pool, so they park too, and the timeout covers them |
| `net.listen` | `listen(port [, host [, backlog]]) → fd \| error` | Bind and listen (SO_REUSEADDR). `port` 0 picks a free port; `host` defaults to all interfaces |
| `net.accept` | `accept(fd [, timeout]) → fd \| error` | Wait for and accept one connection |
| `net.read` | `read(fd, max [, timeout]) → string \| error` | Up to `max` bytes as soon as any arrive; `""` at end of stream |
| `net.write` | `write(fd, data [, timeout]) → int \| error` | Write all of `data` (the timeout applies to each wait); returns `len(data)` |
| `net.close` | `close(fd)` | Close; a goroutine waiting on `fd` wakes with an error |
| `net.shutdown` | `shutdown(fd)` | End both directions without freeing the fd: a goroutine reading or accepting on it wakes (EOF / error) and closes it itself. Use this, not `close`, to stop a socket another goroutine owns — closing it lets the fd number be reused under that goroutine's next read |
| `net.local_port` | `local_port(fd) → int` | Port the socket is bound to |
| `net.peer` | `peer(fd) → string` | `"ip:port"` of the remote end |

`SIGPIPE` is ignored once the module is loaded, so writing to a closed peer
returns an error instead of killing the process.

```sea
local lfd = net.listen(0)
local port = net.local_port(lfd)
go (function()
    local c = net.accept(lfd)
    net.write(c, "echo: " .. net.read(c, 1024, 5000))
    net.close(c)
end)()
local fd = net.connect("127.0.0.1", port, 2000)
net.write(fd, "hi")
print(net.read(fd, 1024, 2000))   -- echo: hi
net.close(fd)
```

Higher-level: `libs/ssl` (TLS over these sockets) and `libs/http` — see [libs.md](libs.md).

---

## Module: `time`

### Duration constants

| Constant | Value |
|----------|-------|
| `time.Nanosecond` | 1 |
| `time.Microsecond` | 1 000 |
| `time.Millisecond` | 1 000 000 |
| `time.Second` | 1 000 000 000 |
| `time.Minute` | 60 000 000 000 |
| `time.Hour` | 3 600 000 000 000 |

> **Warning:** `time.add(t, ns)` takes nanoseconds as a 48-bit signed integer. Max safe value ≈ 140 737 s ≈ 39 hours. For larger offsets (days, weeks) use `time.unix(sec, 0)` directly.

### Module functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `time.now` | `now() → Time` | Current wall-clock time |
| `time.unix` | `unix(sec [, ns]) → Time` | Construct a Time from Unix seconds + optional nanoseconds |
| `time.time` | `time() → int` | Current Unix timestamp in seconds (raw integer) |
| `time.sleep` | `sleep(ns)` | Sleep for `ns` nanoseconds. In a goroutine it parks only that goroutine (the scheduler thread keeps running others); on the main thread it blocks |
| `time.clock` | `clock() → number` | CPU time in seconds since program start |
| `time.since` | `since(t) → number` | Nanoseconds elapsed since Time `t` |
| `time.until` | `until(t) → number` | Nanoseconds until Time `t` |
| `time.hours` | `hours(ns) → number` | Convert nanosecond duration to hours |
| `time.minutes` | `minutes(ns) → number` | Convert nanosecond duration to minutes |
| `time.seconds` | `seconds(ns) → number` | Convert nanosecond duration to seconds |
| `time.milliseconds` | `milliseconds(ns) → int` | Convert nanosecond duration to milliseconds |
| `time.microseconds` | `microseconds(ns) → int` | Convert nanosecond duration to microseconds |
| `time.nanoseconds` | `nanoseconds(ns) → int` | Identity — return nanoseconds as integer |

### Time object methods

Returned by `time.now()`, `time.unix()`, etc. All methods take the Time object as the first argument.

| Method | Signature | Description |
|--------|-----------|-------------|
| `t.unix(t)` | `() → int` | Unix timestamp in seconds |
| `t.unix_nano(t)` | `() → number` | Unix timestamp in nanoseconds (float) |
| `t.year(t)` | `() → int` | Year (local time) |
| `t.month(t)` | `() → int` | Month 1–12 (local time) |
| `t.day(t)` | `() → int` | Day of month 1–31 (local time) |
| `t.hour(t)` | `() → int` | Hour 0–23 (local time) |
| `t.minute(t)` | `() → int` | Minute 0–59 (local time) |
| `t.second(t)` | `() → int` | Second 0–59 (local time) |
| `t.nanosecond(t)` | `() → int` | Sub-second nanoseconds |
| `t.weekday(t)` | `() → int` | Day of week 0=Sunday…6=Saturday |
| `t.yearday(t)` | `() → int` | Day of year 1–366 |
| `t.add(t, ns)` | `(ns) → Time` | New Time shifted by `ns` nanoseconds |
| `t.sub(t, other)` | `(other) → number` | Duration between two Times in nanoseconds |
| `t.before(t, other)` | `(other) → bool` | True if `t` is earlier than `other` |
| `t.after(t, other)` | `(other) → bool` | True if `t` is later than `other` |
| `t.equal(t, other)` | `(other) → bool` | True if the two Times are equal |
| `t.format(t, layout)` | `(layout) → string` | Format using Go reference-time tokens (`2006`, `01`, `02`, `15`, `04`, `05`, etc.) |
| `t.string(t)` | `() → string` | Format as `"YYYY-MM-DD HH:MM:SS"` |

---

## Module: `gc`

Memory is reference counted; the cycle collector frees objects that only keep
each other alive (see [memory.md](memory.md#cycles)). It runs on its own; these
functions are for forcing and inspecting it.

| Function | Signature | Description |
|----------|-----------|-------------|
| `gc.collect()` | `() → int` | Run a cycle collection now (current goroutine); returns the number of objects freed |
| `gc.stats()` | `() → table` | For the current goroutine: `collections`, `freed` (objects), `traversed` (objects examined), `candidates` (pending now), `buffered` (candidates recorded in total), `threshold` (`-1` when the collector is off) |

```sea
local a = {}
local b = {}
a.peer = b
b.peer = a
a = nil
b = nil
print(gc.collect())        -- 2
```

Environment: `SEA_GC=0` turns the cycle collector off (cycles leak, but no
candidates are tracked); `SEA_GC_THRESHOLD=n` sets the minimum number of
candidates before a collection runs (default 10000).

---

## Module: `sea/parser`

The Sea lexer and parser as a library, built into the VM — the same parser the
compiler uses, so its trees always match the language. For linters,
formatters, code generators and editor tooling (`examples/seafmt` is a small
formatter/checker built on it).

```sea
local parser = require("sea/parser")

local ast = parser.parse("local x = 1 + 2")
print(ast.stmts[0].values[0].op)          -- +

local d = parser.check(src, {name = "app.sea"})
if d ~= nil then print(d.text) end        -- app.sea:3:9: expected 'then' but got 'end'

print(parser.format("local   t={a=1}"))   -- local t = {a = 1}
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `parse` | `parse(src [, opts]) → node \| error` | The tree of `src` (a `"block"` node). A syntax error is an error value `"name:line:col: message"` |
| `parse_file` | `parse_file(path [, opts]) → node \| error` | The same, reading a file (`name` defaults to the path) |
| `tokenize` | `tokenize(src [, opts]) → array \| error` | The tokens: `{kind, text, line, col, value}` — `kind` is `"name"`, `"keyword"`, `"number"`, `"string"`, `"op"` or `"comment"`; `value` is the number or the decoded string |
| `check` | `check(src [, opts]) → nil \| table` | Parse and compile without running: `nil`, or the first error as `{phase, message, line, col, text}` — `phase` is `"syntax"` or `"compile"` (compile errors have no `col`); `text` is `"name:line[:col]: message"` |
| `children` | `children(node) → array` | The child nodes, in source order |
| `walk` | `walk(node, fn)` | Calls `fn(node, parent)` on every node, parents first; returning `false` skips that node's children. The children are read after `fn` returns, so `fn` may change them. A panic in `fn` propagates |
| `format` | `format(src_or_node [, opts]) → string \| error` | Source code printed in the canonical layout (below) |

Options: `name` (used in error messages, default `"<input>"`); for `parse` and
`parse_file`, `comments = true` adds the comments to the root as
`comments: [{text, line, col, inline}]` (`inline`: code precedes it on its
line); for `tokenize`, `comments = true` includes comment tokens; for `format`,
`indent` (number of spaces, or a string; default 4).

**Nodes** are tables with `type`, `line` and `col` (1-based; columns count
bytes). A statement's position is its first token; an operator's, call's or
access's is the operator token (`+`, `(`, `[`, `.`, `:`). Blocks, tables and
calls also have `end_line`, the line of the token that closes them (`end`,
`else`, `elseif`, `case`, `}`, `)`). A node written in parentheses has
`paren = true`.

| `type` | Fields |
|--------|--------|
| `"nil"`, `"break"`, `"continue"`, `"vararg"`, `"map_lit"` | — |
| `"bool"` | `value` |
| `"number"` | `value` (int or float), `is_float`, `raw` (the source text, e.g. `0x1F`) |
| `"string"` | `value` (decoded), `raw` (the literal as written, with its quotes) |
| `"ident"` | `name` |
| `"unop"` | `op` (`-`, `not`, `~`), `operand` |
| `"binop"` | `op`, `left`, `right` |
| `"index"` | `object`, `index` (`nil` in `t[] = v`) |
| `"field"` | `object`, `field` (string) |
| `"call"` | `callee`, `args`, `end_line` |
| `"method_call"` | `object`, `method` (string), `args`, `end_line` |
| `"func"` | `params: [{name, is_vararg}]`, `body`, `name` (string or `nil`), `is_method` |
| `"table"` | `fields: [{key, value}]` (`key` is `nil` for positional fields; `x = 1` has a `"string"` key), `end_line` |
| `"array_lit"` | `cap` (node or `nil`) |
| `"chan_recv"` | `channel` |
| `"chan_send"` | `channel`, `value` |
| `"propagate"` | `expr` (`expr?`) |
| `"block"`, `"do"` | `stmts`, `end_line` |
| `"local"` | `names` (strings), `values`, `is_const` |
| `"assign"` | `targets`, `values` |
| `"if"` | `cond`, `then_body`, `else_body` (`nil`, a block, or an `"if"` node for `elseif`) |
| `"while"` | `cond`, `body` |
| `"for_num"` | `var` (string), `start`, `limit`, `step` (or `nil`), `body` |
| `"for_in"` | `vars` (strings), `iter`, `body` |
| `"return"` | `values` |
| `"go"`, `"defer"` | `call` |
| `"select"` | `cases: [{stmt, body, is_default}]` |
| `"panic"` | `value` |
| `"del"` | `target` |
| `"expr_stmt"` | `expr` |

Binary operators: `or and == ~= < <= > >= | ~ & << >> .. + - * / // % ^` (`!=`
in the source is `~=` in the tree). Some syntax is sugar in the tree:
`local function f` is a `"local"` whose value is a `"func"` named `f`;
`function a.b:m(x)` is an `"assign"` to `a.b.m` of a `"func"` with `name = "a"`,
`is_method = true` and `self` added as its first parameter; `f"s"` is a call.
Names declared by `local`, `for` and parameters are strings, not `"ident"`
nodes, so `walk` does not visit them.

**`format`** takes source text or a node (a tree from `parse`, or one built by
hand — nodes may be tables or structs, lists must be arrays). It prints one
statement per line, indents blocks, spaces operators and adds parentheses only
where precedence needs them, keeping the ones written in the source. From
source text (or a tree parsed with `comments = true`) it keeps the comments,
single blank lines, one-line blocks (`if x then return end`), line breaks
inside long expressions and argument lists, and literals as written (`raw`,
when it still matches `value`); trailing comments on consecutive lines are
aligned. Formatting is idempotent, and the formatted code parses to the same
tree.

Limits: the parser rejects code nested more than 1000 levels (expressions and
blocks) or with more than 100 nested functions, with an error rather than a
stack overflow; these limits apply to every program, not just this library.

---

## Module: `sort`

Sorting and binary search. Works on arrays, `int_array` and `float_array`.
Without a comparator the order is the natural one: numbers by value (ints and
floats together, NaN last) and strings by bytes. Every sort is **stable**:
equal elements keep their order.

| Function | Signature | Description |
|----------|-----------|-------------|
| `sort.sort` | `sort(a [, less]) → a` | Sort in place. `less(x, y)` returns true when `x` goes before `y` |
| `sort.by` | `by(a, key [, reverse]) → a` | Sort in place by a key: a function (called once per element) or a field name of tables/structs/maps; keys compare in the natural order |
| `sort.sorted` | `sorted(a [, less]) → array` | A sorted copy (same kind of array); `a` is untouched |
| `sort.reverse` | `reverse(a) → a` | Reverse in place |
| `sort.is_sorted` | `is_sorted(a [, less]) → bool` | Whether `a` is already in order |
| `sort.search` | `search(a, x [, less]) → int` | Binary search in a sorted array: the first index whose element is not less than `x` (`len(a)` if none) — where `x` is, or would be inserted |
| `sort.asc`, `sort.desc` | comparators | The natural order and its reverse: `sort.sort(a, sort.desc)`. Passed to a sort function they run in C, without calling back into Sea |

```sea
local scores = []
scores[] = {name = "ana", points = 7}
scores[] = {name = "bia", points = 9}
scores[] = {name = "caio", points = 7}
sort.by(scores, "points", true)                 -- bia 9, ana 7, caio 7 (stable)

local words = string.split("pear apple fig", " ")
sort.sort(words, function(a, b) return len(a) < len(b) end)   -- fig pear apple
local i = sort.search(sort.sorted(words), "grape")             -- 1
```

Errors are runtime errors (panics), raised before the array is changed:
values the natural order cannot compare (a number with a string, tables
without a comparator), a comparator that is not a function. A comparator
written in Sea works on a copy, so a panic inside it leaves the array as it
was, and a comparator that changes the array is an error.

---

## Collection Literals

| Syntax | Type | Description |
|--------|------|-------------|
| `{}` | table (struct-mode) | Fixed-layout struct; field names set at compile time with `{ k = v }` |
| `[]` | array | Empty dynamic array |
| `[n]` | array | Pre-allocated array with capacity `n` |
| `[=]` | map | Swiss-table hash map (any-type keys) |

### Array operations

```sea
local a = []
a[] = v          -- append
a[i] = v         -- set at index i (0-based; negative: from end, -1 = last)
v = a[i]         -- get (negative indices work)
del a[i]         -- remove element at i and shift down
len(a)           -- number of elements

-- for-in on arrays yields (index, value)
for i, v in a do
    ...
end
```

### Map operations

```sea
local m = [=]
m[k] = v              -- set key k to value v (any type for k)
v = m[k]              -- get; returns nil if missing
v, ok = m[k]          -- two-value lookup: ok=true if key exists
del m[k]              -- delete key k
len(m)                -- number of entries

for k, v in m do      -- iterate all key-value pairs
    ...
end
```

### Struct / table operations

```sea
local s = { x = 1, y = 2 }   -- struct literal (compile-time fields)
s.x = 10
local v = s["x"]              -- string-key indexing also works

for k, v in s do              -- iterates all fields
    ...
end
```

### Typed arrays (int_array / float_array)

No per-element reference counting — more efficient for numeric data.

```sea
local ia = int_array()      -- typed int64 array
local fa = float_array(64)  -- with initial capacity hint
ia[] = 42                   -- append
ia[0] = 99                  -- set (0-based)
v = ia[0]                   -- get
len(ia)                     -- count
-- Note: for-in is NOT supported on typed arrays; use numeric for
for i = 0, len(ia)-1 do ... end
```

---

## Functions

### Basic definitions

```sea
function add(a, b) return a + b end

-- Expression form
local f = function(x) return x * 2 end
```

### Method definition and call

```sea
-- Definition: 'self' is added implicitly as first parameter
function obj:method(a, b)
    -- self is available here
end

-- Call: obj is passed as first argument automatically
obj:method(a, b)       -- equivalent to obj.method(obj, a, b)
```

### Multiple return values

```sea
function divmod(a, b) return a // b, a % b end
local q, r = divmod(10, 3)
```

### Variadic functions

```sea
function sum(...)
    local args = {...}   -- packs varargs into a table (0-based keys)
    local total = 0
    for i = 0, len(args)-1 do total = total + args[i] end
    return total
end

-- Pass-through
function wrap(...) return inner(...) end
```

### Bare `do...end` blocks (scope isolation)

```sea
do
    local tmp = expensive()
    use(tmp)
end
-- tmp not accessible here
```

### Closures and captured variables

A closure captures variables **by reference**: it sees later assignments made
by the enclosing function, and assignments made inside the closure are visible
outside.

**Loop variables are fresh per iteration** (as in Lua 5.4 and Go 1.22): each
iteration of a numeric `for` or a `for-in` has its own loop variable(s) — and
its own body locals — so closures and goroutines created in the body keep that
iteration's values, also when the iteration ends with `continue`.

```sea
local fns = []
for i = 1, 3 do
    fns[] = function() return i end
end
print(fns[0](), fns[1](), fns[2]())   -- 1  2  3

for i = 1, 10 do
    go (function() ch <- i end)()     -- each goroutine sends its own i
end
```

A variable captured by a goroutine may be reassigned by its owner after the
goroutine started; the new value is safely shared with the goroutine (as for
values sent on a channel).

### Semicolons

Semicolons are optional statement separators (permitted but not required).

---

## Error Handling

```sea
-- Create an error value (not a panic)
local err = error("something went wrong")
if is_error(err) then
    print(err.message)   -- access the message field
end

-- Propagate with ? (returns error to caller if result is an error)
local v = might_fail()?

-- Panic + recover
function safe()
    defer function()
        local e = recover()
        if e ~= nil then print("recovered:", e) end
    end
    panic("oops")
    print("not reached")
end
safe()          -- prints "recovered: oops"; safe() returns nil
```

### panic and recover

`panic(v)` and **runtime errors** (indexing `nil`, arithmetic on a table,
calling `nil`, …) unwind the call stack the same way, as in Go:

- Execution stops at the panic. Each function on the stack, innermost first,
  runs its deferred calls (LIFO) and returns.
- A deferred call that calls `recover()` stops the unwinding and gets the panic
  value. The function whose deferred call recovered then returns normally to
  its caller, **with nil results**, after running its remaining deferred calls.
  The caller continues as after any call.
- For a runtime error, `recover()` returns an error value whose message starts
  with the location: `is_error(e)`, `e.message == "main.sea:12: attempt to
  index a nil value"`. For `panic(v)` it returns `v`.
- `recover()` returns `nil` when there is no panic.
- A panic inside a deferred call replaces the one being unwound.
- Panics cross native callbacks (`once.do_once(once, f)`), module bodies run by
  `require`, and JIT-compiled code.
- An unrecovered panic, in the main script or in any goroutine, prints the
  traceback and ends the process (exit code 1).

```sea
local function try(f, ...)
    local err = nil
    local function run(...)
        defer function() err = recover() end
        return f(...)
    end
    local result = run(...)
    return result, err
end

local r, e = try(function()
    local t = nil
    return t.x
end)
print(r, e.message)     -- nil   main.sea:13: attempt to index a nil value
```

Functions unwound by a panic do not release their local variables (the
values leak). Panics are for exceptional paths; use error values and `?` for
expected failures.

**Error object field:**

| Field | Description |
|-------|-------------|
| `err.message` | The string message passed to `error()` |

### defer

`defer` schedules a call to run when the enclosing function returns (normally
or by panic). Deferred calls run in LIFO order — including ones registered in a
loop.

```sea
local function f()
    local x = 1
    defer print("x was", x)           -- call: f and its args evaluated NOW
    defer obj:close("bye")            -- method call: obj, obj.close and args evaluated NOW
    defer function() print(x) end     -- closure: runs later, sees x by reference
    x = 2
end
-- prints: 2   (closure), "bye" via obj:close, then "x was 1"
```

- In the call forms the callee and every argument are evaluated when the
  `defer` statement runs (like Go), so later changes to `x` don't affect them.
- A deferred call takes at most **8 arguments** (for `obj:m(...)`, `self`
  counts as one); more is a compile error.

### Syntax and compile errors

Code that does not parse or compile stops before running, with the position:
`file:line:col: message` for syntax errors (`app.sea:3:9: expected 'then' but
got 'end'`) and `file:line: message` for compile errors (`app.sea:7: cannot
assign to const 'a'`). `require` reports a module's errors the same way.
`sea/parser`'s `check` returns them as data.

---

## Concurrency

```sea
-- Goroutines
go my_func(arg1, arg2)

-- Channels
local ch = chan()       -- unbuffered
local ch = chan(16)     -- buffered (capacity 16)
ch <- value             -- send
local v = <- ch         -- receive
close(ch)               -- close

-- Range over channel (terminates when channel is closed)
for v in ch do
    ...
end

-- Select
select
    case v = <-ch1 then
        -- received from ch1
    case ch2 <- x then
        -- sent to ch2
    case default then
        -- non-blocking fallback
end
```

Goroutines run on a pool of scheduler threads (one per CPU). Blocking on a
channel, `time.sleep` and every wait in the `net` module park the goroutine
without holding its thread. Other blocking calls — file I/O, `io.read`, a
native library that blocks — do hold the thread, so enough of them at
once can stall the other goroutines.

---

## `for-in` Support by Type

| Type | Yields | Notes |
|------|--------|-------|
| `array` | `(index int, value)` | 0-based index |
| `table` / struct | `(key string, value)` | all fields |
| `map` (`[=]`) | `(key, value)` | any-type key |
| `channel` | `(value)` | blocks per value; exits on `close()` |
| `int_array`, `float_array` | — | **not supported**; use numeric `for` |

---

## Gotchas

| Wrong | Right |
|-------|-------|
| expecting `for k, v in arr` to yield 1-based keys | it yields `(index, value)` with 0-based indexes; `for i = 0, len(arr)-1 do` is equivalent |
| `#arr` | `len(arr)` |
| `{1, 2, 3}` as array | `local a = []; a[]=1; a[]=2; a[]=3` |
| `1 == 1.0` is `false` | it is `true`: `==` compares int and float numerically (they remain distinct types: `type(1)` is `"int"`, `type(1.0)` is `"number"`) |
| `!=` | `~=` |
| `string.sub(s, 0, 2)` | `string.sub(s, 1, 2)` (1-based) |
| `mu.lock(mu)` | `mu.acquire(mu)` |
| `mu.unlock(mu)` | `mu.release(mu)` |
| Long ns arithmetic (weeks) | use `time.unix(sec, 0)` instead of `base.add(base, 7*24*time.Hour)` |
| `json.encode([=])` | works — `[=]` maps are serialized as JSON objects |
| expecting a cycle of objects shared between goroutines to be collected | the cycle collector only frees cycles of objects a single goroutine owns; break shared cycles yourself (`a.peer = nil`, see [memory.md](memory.md#cycles)) |

---

## Module: `require`

```sea
local json = require("./libs/json/json")   -- relative to the requiring file
local http = require("./libs/http/http")
```

Built-in modules (`math`, `string`, `io`, `os`, `sync`, `net`, `time`, `gc`,
`sort`, `sea/parser`) are found first. Then the search order is:
1. Directory of the requiring source file + path (`.sea` then `.so`)
2. Current working directory + path
3. Each directory in `SEA_PATH` (colon-separated environment variable)
