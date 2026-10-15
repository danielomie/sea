# libs/json

JSON encode and decode for Sea. Implemented entirely in C for correctness and performance.

## Build

```sh
cd libs/json
make          # produces json.so
```

## Usage

```sea
local json = require("./libs/json/json")

local s = json.encode({ name = "sea", version = 1 })
-- '{"name":"sea","version":1}'

local v = json.decode('{"name":"sea","version":1}')
print(v["name"])   -- sea
```

## API

### `json.encode(value) → string`

Converts a Sea value to a JSON string. Returns `nil` if the value cannot be
encoded (e.g. circular reference deeper than 256 levels).

| Sea type | JSON output |
|----------|-------------|
| `nil` | `null` |
| `bool` | `true` / `false` |
| `int` | integer literal |
| `float` | floating-point literal (`%.17g`); `NaN`/`Inf` → `null` |
| `string` | `"..."` with `\"`, `\\`, `\n`, `\r`, `\t`, `\b`, `\f`, `\uXXXX` escaping |
| table with integer keys `0..n-1` | JSON array `[...]` |
| table with string keys | JSON object `{...}` (non-string keys are skipped) |
| `array` (SeaArray) | JSON array `[...]` |
| function / channel / other | `null` |

> **Array detection**: a table is encoded as a JSON array when all its keys are
> non-negative integers forming a consecutive sequence `0..n-1`. This matches
> the layout of Sea's `{v0, v1, v2}` array literals.

### `json.decode(string) → value`

Parses a JSON string and returns the corresponding Sea value. Returns `nil` on
any parse error, including trailing garbage after the root value.

| JSON token | Sea value |
|------------|-----------|
| `null` | `nil` |
| `true` / `false` | `bool` |
| integer number | `int` (48-bit signed) or `float` if out of range |
| floating-point number | `float` |
| `"string"` | `string` (full `\uXXXX` and surrogate-pair decoding to UTF-8) |
| `[...]` | `array` (SeaArray, **0-indexed**) |
| `{...}` | `table` (TABLE_MAP, string keys) |

> **Array indexing**: decoded JSON arrays are returned as SeaArray, which uses
> **0-based** indexing — `arr[0]` is the first element.

## Examples

### Encoding

```sea
local json = require("./libs/json/json")

json.encode(nil)           -- "null"
json.encode(true)          -- "true"
json.encode(42)            -- "42"
json.encode("hi\nthere")   -- "\"hi\\nthere\""

-- Array (table with integer keys 0..n-1)
json.encode({1, 2, 3})                      -- "[1,2,3]"
json.encode({"a", true, nil})               -- "[\"a\",true,null]"

-- Object
json.encode({ name = "sea", ok = true })    -- '{"name":"sea","ok":true}'

-- Nested
json.encode({ tags = {"fast", "small"}, n = 2 })
-- '{"tags":["fast","small"],"n":2}'
```

### Decoding

```sea
local json = require("./libs/json/json")

json.decode("null")          -- nil
json.decode("42")            -- 42
json.decode('"hello"')       -- "hello"

local arr = json.decode("[10,20,30]")
print(arr[0], arr[1], arr[2])   -- 10  20  30

local obj = json.decode('{"name":"alice","age":30}')
print(obj["name"])   -- alice
print(obj["age"])    -- 30

-- Error handling
local bad = json.decode("not json")
print(bad == nil)    -- true
```

### Round-trip

```sea
local json = require("./libs/json/json")

local data    = { x = 1, y = "hello", flags = {true, false} }
local encoded = json.encode(data)
local decoded = json.decode(encoded)

print(decoded["x"])        -- 1
print(decoded["y"])        -- hello
print(decoded["flags"][0]) -- true
```

## Run examples

```sh
bin/sea libs/json/examples/encode.sea
bin/sea libs/json/examples/decode.sea
```
