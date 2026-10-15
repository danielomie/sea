# libs/json

JSON (RFC 8259) encode and decode for Sea, in C. The full reference (options,
type mapping, rules) is in [docs/libs.md](../../docs/libs.md#json).

## Build

```sh
make -C libs/json     # json.so
```

## Usage

```sea
local json = require("../libs/json/json")   -- relative to your file

local s = json.encode({ name = "sea", version = 1 }, { sort_keys = true })
-- {"name":"sea","version":1}

local v = json.decode('{"name": "sea", "tags": ["fast", "simple"]}')
print(v.name, v.tags[0])          -- sea  fast

-- failures are error values, with the position
local bad = json.decode('{"port": 08}')
if is_error(bad) then print(bad.message) end
-- json.decode: invalid number (leading zero) at line 1, column 10

-- or pass them up with ?
local function port(text)
    local cfg = json.decode(text)?
    return cfg.port
end

print(json.encode(v, { indent = 2, sort_keys = true }))   -- pretty-printed
print(json.valid("[1, 2"))                                -- false
```

| Function | Returns |
|----------|---------|
| `json.encode(value [, {indent, sort_keys, ascii, max_depth}])` | string, or an error (a cycle, a function at the top level, too deep) |
| `json.decode(text [, {max_depth}])` | value (`null` → `nil`), or an error with line and column |
| `json.valid(text)` | bool |

Notes:

- Decoding is strict (no leading zeros, trailing commas, single quotes, raw
  control characters in strings) and nesting is limited (512 by default), so
  hostile input gets an error, not a crash.
- Arrays decode 0-indexed; objects decode to tables.
- Floats keep their type through a round trip (`3.0` → `"3.0"`), and print in
  their shortest exact form (`0.1`).
- Integer, number and bool keys of tables and maps become JSON strings;
  function-valued fields (methods) are left out of objects.
- Tables have no key order: use `sort_keys = true` for stable output.

Examples: `bin/sea libs/json/examples/encode.sea`, `libs/json/examples/decode.sea`.
Tests: `tests/unit/36_json.sea`, `tests/unit/111_json.sea`.
