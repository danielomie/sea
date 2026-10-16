# Sea External Libraries Reference

External libraries live under `libs/`. Native libraries (`.so`) must be built before use;
pure-Sea libraries work out of the box.

---

## Library Overview

| Library | Path | Kind | Build |
|---------|------|------|-------|
| `json` | `libs/json/json` | native `.so` | `make -C libs/json` |
| `yaml` | `libs/yaml/yaml` | native `.so` | `make -C libs/yaml` |
| `csv` | `libs/csv/csv` | native `.so` | `make -C libs/csv` |
| `xml` | `libs/xml/xml` | native `.so` | `make -C libs/xml` |
| `console` | `libs/console/console` | native `.so` | `make -C libs/console` |
| `http` | `libs/http/http` | Sea + native | `make -C libs/http` |
| `ssl` | `libs/ssl/ssl` | Sea + native | `make -C libs/ssl` |
| `jwt` | `libs/jwt/jwt` | Sea + native | `make -C libs/jwt` |
| `flags` | `libs/flags/flags` | pure Sea | — |
| `test` | `libs/test/test` | pure Sea | — |
| `tensor` | `libs/tensor/tensor` | native `.so` | `make -C libs/tensor` |
| `duckdb` | `libs/duckdb/duckdb` | Sea + native | `make -C libs/duckdb` (downloads libduckdb) |
| `sqlite` | `libs/sqlite/sqlite` | Sea + native | `make -C libs/sqlite` (downloads the SQLite source) |

Require paths are relative to the calling file. Examples within a lib's `examples/` directory use `"../libname"`.

Writing a native library: see `src/sea.h` for the module convention and
[memory.md](memory.md#writing-native-functions) for the ownership rules. In
short, functions borrow their arguments and return an owned value
(`sea_val_ref()` for an argument or a container element). For UTF-8, use
`src/sea_utf8.h` (decode, encode, validate, case mapping, terminal widths)
instead of a private copy.

---

## `json`

JSON (RFC 8259) in C. Build: `make -C libs/json`.

```sea
local json = require("../libs/json/json")   -- relative to the requiring file
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `json.encode` | `encode(v [, opts]) → string\|error` | Serialize a Sea value |
| `json.decode` | `decode(s [, opts]) → value\|error` | Parse a JSON document |
| `json.valid` | `valid(s) → bool` | Check that `s` is valid JSON without building values |

Failures are **error values** (`is_error(e)`, `e.message`), so a JSON `null`
(→ `nil`) is never confused with a failure and `json.decode(s)?` passes a
failure up. Messages say where: `json.decode: invalid number (leading zero) at
line 3, column 8`, `json.encode: cycle detected (a value contains itself)`.

**`encode` options** (table, map or struct):

| Key | Default | Description |
|-----|---------|-------------|
| `indent` | — | Pretty-print: a number of spaces (up to 16) or a string such as `"\t"` |
| `sort_keys` | `false` | Sort object keys (byte order). Tables, maps and structs have no key order of their own, so use this for stable output |
| `ascii` | `false` | Escape every non-ASCII character as `\uXXXX` (a surrogate pair above U+FFFF) |
| `max_depth` | `512` | Deepest nesting allowed |

**`decode` options:** `max_depth` (default 512, at most 10000). Deeper input is
an error ("nesting too deep"), never a stack overflow. `bigint = true` turns
integers beyond Sea's int range into exact `bigint`s (IDs of 64 bits and more)
instead of numbers, which lose digits past 2^53.

**Type mapping — encode:**

| Sea value | JSON |
|-----------|------|
| `nil` | `null` |
| `bool` | `true` / `false` |
| `int` | integer |
| `bigint` | integer, every digit |
| `number` | shortest form that reads back exactly (`0.1`, `2.5e-08`); integral values keep a `.0` (`3.0`) so they decode as floats again; `NaN`/`Inf` → `null` |
| `string` | `"..."` with `\"`, `\\`, `\n`, `\r`, `\t`, `\b`, `\f` and `\u00XX` for other control characters; invalid UTF-8 bytes become U+FFFD |
| `array`, `int_array`, `float_array` | array |
| table with keys `0..n-1` (`{a, b, c}`) | array |
| other table, map `[=]`, struct | object. Integer, number and bool keys become strings (`{"1": …}`); other keys are skipped |
| function, channel, error, goroutine | left out of objects, `null` in arrays, an error at the top level |

A value that contains itself is an error; the same value appearing twice (not
nested in itself) is fine.

**Type mapping — decode:**

| JSON | Sea value |
|------|-----------|
| `null` | `nil` |
| `true` / `false` | `bool` |
| integer within ±2^47 | `int` |
| other numbers (fraction, exponent, larger integers) | `number` (larger integers: `bigint` with `bigint = true`) |
| string | `string` (escapes and surrogate pairs decoded to UTF-8; a lone surrogate becomes U+FFFD) |
| array | `array` (**0-indexed**) |
| object | table (string keys; for a repeated key the last value wins) |

Decoding is strict: leading zeros, `1.`, `-`, trailing commas, single quotes,
raw control characters or unknown escapes in strings, and data after the value
are rejected. A UTF-8 byte-order mark at the start is skipped. Object keys are
shared: one string per distinct key per document, and the program's own
constant when it has one, so `obj.name` lookups compare pointers.

---

## `yaml`

YAML 1.2 in C: a complete reader (block and flow styles, every scalar style,
anchors and aliases, tags and `%TAG` directives, multiple documents, merge
keys), an encoder, and a low-level event API to read or write YAML with its
styles, tags and anchors kept. Conformance: every case of the official
[yaml-test-suite](https://github.com/yaml/yaml-test-suite) passes (events and
errors). Build: `make -C libs/yaml`.

```sea
local yaml = require("../libs/yaml/yaml")   -- relative to the requiring file

local cfg = yaml.decode(os.readfile("config.yaml"))?
print(cfg.server.port)
print(yaml.encode(cfg, {sort_keys = true}))
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `yaml.decode` | `decode(s [, opts]) → value\|error` | Parse a stream holding one document (none: `nil`; more than one: an error) |
| `yaml.decode_all` | `decode_all(s [, opts]) → array\|error` | Parse every document of a stream |
| `yaml.valid` | `valid(s) → bool` | Check syntax, anchors and aliases without building values |
| `yaml.encode` | `encode(v [, opts]) → string\|error` | Serialize a Sea value as one document |
| `yaml.encode_all` | `encode_all(docs [, opts]) → string\|error` | Serialize an array of values as a stream of documents (`---` between them) |
| `yaml.events` | `events(s) → array\|error` | The parser's events, as tables |
| `yaml.emit` | `emit(events [, opts]) → string\|error` | Write a stream from events (the inverse of `events`) |

Failures are **error values** with the position: `yaml.decode: mapping values
are not allowed here at line 3, column 9`, `yaml.decode: duplicate mapping key
'port' at line 7, column 3`, `yaml.decode: found undefined alias 'base' at
line 2, column 8`. Input must be UTF-8 (a byte-order mark is skipped);
non-printable characters are rejected. A tag handler that panics stops
the decode, and the panic goes on.

**`decode` / `decode_all` options:**

| Key | Default | Description |
|-----|---------|-------------|
| `schema` | `"core"` | How plain scalars resolve: `"core"` (YAML 1.2), `"json"` (only JSON's literals; anything else is a string), `"failsafe"` (every scalar is a string) or `"1.1"` (YAML 1.1 as PyYAML reads it: `yes`/`no`/`on`/`off`, `0777` octal, `0b1010`, `1_000`, `190:20:30`). A document starting with `%YAML 1.1` uses `"1.1"` unless a schema is given |
| `bigint` | `false` | Integers beyond Sea's int range (±2^47) become exact `bigint`s instead of numbers |
| `string_keys` | `false` | Mapping keys stay strings as written (`1: a` → key `"1"`, `~: b` → key `"~"`) |
| `duplicate_keys` | `false` | Allow a key twice in a mapping (the last wins) instead of an error |
| `merge` | `true` | Merge keys: `<<: *base` or `<<: [*a, *b]` copy the keys the mapping does not set itself (earlier sources win) |
| `tags` | — | Handlers for custom tags: a table `{["!point"] = function(value, tag) ... end}`; `["*"]` catches any other. The handler's result replaces the value (an error value it returns stays in the data) |
| `tagged` | `false` | Without a handler, a value with a custom tag becomes `{tag = "!point", value = ...}` (by default the tag is dropped) |
| `max_depth` | `512` | Deepest nesting allowed (at most 10000) |

**Type mapping — decode** (core schema):

| YAML | Sea value |
|------|-----------|
| `null`, `Null`, `NULL`, `~`, empty | `nil` |
| `true`/`True`/`TRUE`, `false`/… | `bool` |
| `12`, `-3`, `0o17`, `0x1F` within ±2^47 | `int` (larger: `number`, or `bigint` with `bigint = true`) |
| `1.5`, `1e3`, `.inf`, `-.inf`, `.nan` | `number` |
| any other plain scalar, quoted or block scalar | `string` |
| sequence | `array` (**0-indexed**) |
| mapping | table; keys are strings, ints, numbers or bools as resolved. A `null` or collection key is an error (use `string_keys` for the first; `yaml.events` reads anything) |
| `&a` / `*a` | the same Sea object: `a: &x [1]` and `b: *x` share one array. A recursive structure (`&r [*r]`) is a cycle the cycle collector frees |

Standard tags: `!!str`, `!!int`, `!!float`, `!!bool`, `!!null` convert the
scalar (an invalid value is an error: `!!int abc`); `!!binary` decodes base64
into a string of bytes; `!!timestamp` stays a string; `!!set` (`? a`) gives
a table with `true` values; `!!seq`, `!!map`, `!!omap`, `!!pairs` keep the
collection as is. The non-specific `!` makes a string. Every other tag goes to
`tags`/`tagged` (or is dropped). Integer keys and string keys are distinct:
`1: a` and `"1": b` are two keys.

**`encode` / `encode_all` options:**

| Key | Default | Description |
|-----|---------|-------------|
| `indent` | `2` | Spaces per level (1 to 9). Sequences inside mappings are indented |
| `sort_keys` | `false` | Sort mapping keys (byte order of their text). Tables, maps and structs have no key order of their own, so use this for stable output |
| `flow` | `false` | Write everything in flow style on one line (`{a: [1, 2]}`) |
| `anchors` | `false` | A table or array reached more than once is written once with `&id001` and then as `*id001`; this also encodes cycles (otherwise an error) |
| `explicit_start` | `false` | Start each document with `---` |
| `explicit_end` | `false` | End each document with `...` |
| `max_depth` | `512` | Deepest nesting allowed |

**Type mapping — encode:**

| Sea value | YAML |
|-----------|------|
| `nil` | `null` |
| `bool` | `true` / `false` |
| `int`, `bigint` | integer (every digit) |
| `number` | shortest form that reads back exactly; integral values keep `.0` (`3.0`), exponents get a dot (`1.0e+20`, as YAML 1.1 needs); `.inf`, `-.inf`, `.nan` |
| `string` | plain when that reads back as the same string under the core schema and YAML 1.1 (`'true'`, `'123'`, `'yes'`, `'2001-12-14'`, `''` are quoted); multi-line text as a literal block (`|`, with `-`/`+` chomping and an indentation indicator when needed); otherwise single- or double-quoted with escapes. Bytes that are not UTF-8 become `!!binary` (base64) |
| `array`, `int_array`, `float_array`, table with keys `0..n-1` | sequence (empty: `[]`) |
| other table, map `[=]`, struct | mapping (empty: `{}`); string, int, number, bool and bigint keys; other keys are skipped |
| function, channel, error, goroutine | left out of mappings, `null` in sequences, an error at the top level |

Output is never wrapped. Decoding what `encode` writes gives the same value
back, and other YAML readers (PyYAML, libyaml) read it the same way.

**Events.** `yaml.events(s)` returns one table per event, in order:
`stream_start`, `document_start`, `mapping_start` … `mapping_end`,
`sequence_start` … `sequence_end`, `scalar`, `alias`, `document_end`,
`stream_end`. Each has `type`, `line` and `column` (1-based, where the node
starts), and as they apply:

| Field | Events | Meaning |
|-------|--------|---------|
| `value` | scalar | The text after escapes, folding and chomping |
| `style` | scalar | `"plain"`, `"single"`, `"double"`, `"literal"`, `"folded"` |
| `style` | mapping/sequence start | `"block"` or `"flow"` |
| `anchor` | scalar, mapping/sequence start | `&name` |
| `alias` | alias | `*name` |
| `tag` | scalar, mapping/sequence start | The resolved tag: `tag:yaml.org,2002:str` for `!!str`, `!local`, `!` |
| `implicit` | document start/end | `false` when written with `---` / `...` |
| `version`, `tags` | document start | `%YAML` version (`"1.2"`) and `%TAG` directives (`{["!e!"] = "tag:example.com,2000:"}`) |

`yaml.emit(events [, opts])` writes events back (the `stream_*` events are
optional; `implicit`, `version` and `tags` are honoured). Styles are kept
where they are valid for the value and the place (a plain scalar that would
not parse is quoted, a block scalar inside a flow collection becomes double
quoted); a scalar without a `style` is quoted when its text would read as
something other than a string. Options: `indent`, `flow`. Comments are not
events, so `events` → `emit` drops them; everything else (key order, styles,
tags, anchors) survives.

```sea
-- rewrite a file keeping its order and styles, changing one value
local evs = yaml.events(src)?
for _, e in evs do
    if e.type == "scalar" and e.value == "8080" then e.value = "9090" end
end
print(yaml.emit(evs))
```

---

## `csv`

CSV (RFC 4180) in C: decode a string, encode rows, or stream a file of any
size row by row. Build: `make -C libs/csv`.

```sea
local csv = require("../libs/csv/csv")   -- relative to the requiring file

for _, r in csv.decode(fs.read("people.csv")?, {header = true, numbers = true})? do
    print(r.name, r.age + 1)
end
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `csv.decode` | `decode(s [, opts]) → array\|error` | Every row: arrays of strings, or tables keyed by the header with `header = true` |
| `csv.encode` | `encode(rows [, opts]) → string\|error` | Rows as CSV text |
| `csv.each` | `each(path, fn [, opts]) → int\|error` | Read a file in 64 KB chunks and call `fn(row)` for each row (`false` from `fn` stops); returns the number of rows. Memory stays flat however big the file is |

Fields may be quoted with `"`: a quoted field can hold the separator, line
breaks and `""` for a quote. `\n`, `\r\n` and `\r` end a record; a final
line break does not make an empty row; a UTF-8 byte-order mark is skipped.
Failures are error values with the line: `csv.decode: unterminated quoted
field at line 12`, `csv.decode: line 7 has 4 fields, the header has 3`.

**`decode` / `each` options:**

| Key | Default | Description |
|-----|---------|-------------|
| `sep` | `","` | Field separator (one character: `";"`, `"\t"`, ...) |
| `quote` | `"\""` | Quote character |
| `header` | `false` | The first row names the columns: rows become tables `{name = value}`, and a row with a different number of fields is an error |
| `numbers` | `false` | Unquoted fields in decimal syntax become numbers (`ints` within ±2^47, else floats: `31`, `-2.5`, `1e3`); `"42"` (quoted), `007`, `0x10` and empty fields stay strings |
| `trim` | `false` | Strip spaces and tabs around fields (outside quotes) |
| `comment` | — | A character that starts a comment line (skipped whole) |
| `skip_empty` | `true` | Skip blank lines (`false`: they are rows with one empty field) |
| `strict` | `false` | Reject text after a closing quote, quotes inside unquoted fields, and (without a header) rows whose field count differs from the first row's. Without it, such text is kept as written (like Python's reader) |

**`encode`**: a row is a list (array, or table with keys `0..n-1`), written in
order, or a record (table, map, struct), written in the order of
`opts.columns` (default: every key of every record, sorted) under a header
line (`header = false` leaves it out). Strings, numbers (shortest form that
reads back exactly), bools and bigints are written; `nil`, NaN and infinities
as empty fields; other values are an error. A field is quoted when it holds
the separator, a quote, a line break, or leading or trailing spaces. Options:
`sep`, `quote`, `columns`, `header`, `crlf` (end lines with `\r\n`, as RFC 4180
asks; default `\n`), `quote_all`.

```sea
local out = csv.encode(people, {columns = {"name", "age", "city"}})
fs.write("people.csv", out)?
local adults = 0
local n = csv.each("big.csv", function(r)
    if r.age >= 18 then adults = adults + 1 end
end, {header = true, numbers = true})?
```

---

## `xml`

XML 1.0 in C: a strict parser, an encoder, queries (a small XPath subset), a
SAX mode and helpers to build and edit trees. Build: `make -C libs/xml`.

```sea
local xml = require("../libs/xml/xml")   -- relative to the requiring file
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `xml.parse` | `parse(s [, opts]) → element\|document\|error` | Parse a document into a tree |
| `xml.valid` | `valid(s) → bool` | Check well-formedness without building a tree |
| `xml.sax` | `sax(s, handlers [, opts]) → bool\|error` | Call handlers as the document is read (no tree) |
| `xml.encode` | `encode(node [, opts]) → string\|error` | Serialize an element or a document |
| `xml.escape` | `escape(s [, attr]) → string` | Escape `& < >` (and `" ' \n \t` when `attr` is true) |
| `xml.find` | `find(node, tag) → element\|nil` | First child element with that tag (`"*"`: any) |
| `xml.findall` | `findall(node, tag) → array` | Child elements with that tag (`"*"`: all) |
| `xml.descendants` | `descendants(node [, tag]) → array` | Every element under `node` in document order, optionally only one tag |
| `xml.query` | `query(node, path) → array\|error` | Elements, attribute values or text matching a path (see below) |
| `xml.query_one` | `query_one(node, path) → value\|nil\|error` | First match of `query` |
| `xml.attr` | `attr(node, name [, default]) → value` | An attribute, or `default` (nil) when absent |
| `xml.text` | `text(node [, deep]) → string` | The element's own text; with `deep`, all text under it in order |
| `xml.element` | `element(tag [, attrs [, content...]]) → element` | Build an element; content is strings, elements, comment/cdata/pi/text nodes or arrays of them |
| `xml.append` | `append(node, x) → x` | Append a string, element or comment/cdata/pi/text node |
| `xml.set_text` | `set_text(node, s) → node` | Replace the whole content with the text `s` |
| `xml.set_attr` | `set_attr(node, name, value) → node` | Set an attribute (string, number or bool) keeping `attr_order`; `nil` removes it |
| `xml.remove` | `remove(node, child) → bool` | Remove a child (element or node by identity, a string by value) |

**Elements** are tables:

```sea
{ tag = "item",
  attrs = {id = "1", lang = "pt"},   -- attribute values are strings
  attr_order = ["id", "lang"],       -- names in document order
  children = [ ... ],                -- child elements only
  nodes = [ ... ],                   -- all content, in document order
  text = "..." }                     -- the element's own text, joined
```

`nodes` holds text as plain strings, the same element tables as `children`,
and (when kept) `{type = "comment", value}`, `{type = "cdata", value}` and
`{type = "pi", target, value}`. So `<p>Hello <b>world</b>!</p>` has
`nodes = ["Hello ", <b>, "!"]`, `children = [<b>]` and `text = "Hello !"`.
Test an entry with `type(n) == "string"` (text), `n.tag` (element) or `n.type`.

**`parse` options** (table, map or struct):

| Key | Default | Description |
|-----|---------|-------------|
| `ignore_whitespace` | `false` | Drop text that is only whitespace (indentation) from `nodes` and `text` |
| `comments` | `false` | Keep comments as nodes |
| `cdata` | `false` | Keep CDATA sections as nodes (otherwise they are text) |
| `pi` | `false` | Keep processing instructions as nodes |
| `namespaces` | `false` | Resolve namespaces: each element gets `ns` (the URI, absent when none) and `name` (the local name); `tag` keeps the prefix. An unbound prefix is an error |
| `document` | `false` | Return a document: `{type = "document", root, nodes, doctype, version, encoding, standalone}`; `nodes` holds the top-level comments/PIs, a `{type = "doctype", value}` node and the root |
| `max_depth` | `512` | Deepest nesting allowed (deeper input is an error; the parser never recurses) |

Parsing is strict: one root element, matching end tags, quoted and unique
attributes, no `<` in attribute values, no bare `&`, only the five predefined
entities and valid character references, no `]]>` in text or `--` in comments,
valid UTF-8 (other encodings are rejected) and no control characters. Line ends
become `"\n"` and whitespace characters in attribute values become spaces, as
the spec says. A DOCTYPE is skipped, never interpreted: its entities are not
expanded, so using one is an error. A UTF-8 byte-order mark is skipped.
Failures are **error values** with a position, e.g.
`xml.parse: expected </b>, found </a> at line 1, column 7`.

**`encode` options:**

| Key | Default | Description |
|-----|---------|-------------|
| `indent` | — | Pretty-print with a number of spaces (up to 16) or a string such as `"\t"`. Whitespace-only text is replaced by the indentation; elements with mixed content are written inline |
| `declaration` | `false` | Start with `<?xml version="1.0" encoding="UTF-8"?>` |
| `sort_attrs` | `false` | Write attributes sorted by name instead of `attr_order` first |
| `max_depth` | `512` | Deepest nesting allowed (at most 10000); also stops a node that contains itself |

`encode` takes an element or a document; nodes may be tables, maps or structs,
so `{tag = "a", attrs = {k = 1}, text = "hi"}` works. Attributes come in
`attr_order` and then the rest by name, so output is stable. Attribute values
may be strings, numbers or bools. Content comes from `nodes` when present,
otherwise from `text` followed by `children`. When `nodes` disagrees with
`children` or `text` (an element added to `children` by hand, say), `encode`
fails rather than drop the edit. The exception is a node whose `nodes` holds
only text, as in `leaf.text = "new"`: `text` and `children` are used. Edit
through `append`, `set_text` and `remove`, or set `nodes = nil`. Invalid names,
control characters, invalid UTF-8, `--` in a comment and `?>` in a PI are
errors; `]]>` in CDATA is split across two sections.

**Paths** for `query` start at the given node:

| Path | Selects |
|------|---------|
| `book`, `shelf/book` | child elements by tag (qualified names such as `d:title` as written) |
| `*` | any child element |
| `//book`, `shelf//title` | descendants at any depth |
| `/lib/book` | from the top: the first step must match the node itself (or a document's root) |
| `.` | the node itself |
| `@id`, `@*` | attribute values (last step only) |
| `text()` | text nodes (last step only) |
| `[2]`, `[last()]` | position among the matches for each parent (1-based) |
| `[@id]`, `[@id='2']`, `[@id!='2']` | has the attribute / its value equals / differs |
| `[year]`, `[year='1881']` | has a child element / one whose text equals |
| `[text()='x']` | the element's text equals |

Predicates can be chained (`book[@lang][2]`); unquoted numbers are compared as
text (`[year=1881]`). There is no `..` (nodes do not link to their parent).
A bad path returns an error value.

**`sax` handlers** (all optional): `start(tag, attrs [, ns])`, `finish(tag)`,
`text(s)`, `cdata(s)`, `comment(s)`, `pi(target, data)`, `doctype(s)`. CDATA,
comments and PIs are reported only when their handler exists (CDATA otherwise
counts as text). The options are those of `parse` (`ignore_whitespace`,
`namespaces` adds the `ns` argument, `max_depth`). A handler returning `false`
stops the parse and `sax` returns `false`; a panic in a handler propagates.
The checks and errors are the same as for `parse`.

```sea
local doc = xml.parse(src, {ignore_whitespace = true})?
for _, book in xml.query(doc, "//book[@lang='pt']") do
    print(xml.attr(book, "id"), xml.text(xml.find(book, "title")))
end

local feed = xml.element("feed", {version = 2}, xml.element("title", nil, "News & more"))
xml.append(feed, xml.element("item", {id = 1}, "first"))
print(xml.encode(feed, {indent = 2, declaration = true}))
```

---

## `console`

```sea
local console = require("./libs/console/console")
```

### Cursor

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.goto` | `goto(row, col)` | Move cursor to absolute position (1-based) |
| `console.up` | `up([n])` | Move cursor up `n` lines (default 1) |
| `console.down` | `down([n])` | Move cursor down `n` lines |
| `console.left` | `left([n])` | Move cursor left `n` columns |
| `console.right` | `right([n])` | Move cursor right `n` columns |
| `console.save` | `save()` | Save cursor position |
| `console.restore` | `restore()` | Restore saved cursor position |
| `console.home` | `home()` | Move cursor to top-left (1,1) |
| `console.clear` | `clear()` | Clear entire screen |
| `console.clear_line` | `clear_line()` | Clear current line |
| `console.clear_eol` | `clear_eol()` | Clear from cursor to end of line |
| `console.hide_cursor` | `hide_cursor()` | Hide cursor |
| `console.show_cursor` | `show_cursor()` | Show cursor |
| `console.size` | `size() → {width, height}` | Terminal dimensions |

### Text & colors

#### `console.write(text [, opts])` / `console.writeln(text [, opts])`

Print with optional styling. `writeln` appends a newline.

**`opts` fields:**

| Key | Type | Description |
|-----|------|-------------|
| `color` | string / `"#RRGGBB"` / `{r,g,b}` | Foreground color |
| `bg` | string / `"#RRGGBB"` / `{r,g,b}` | Background color |
| `bold` | bool | Bold |
| `dim` | bool | Dim/faint |
| `italic` | bool | Italic |
| `underline` | bool | Underline |
| `blink` | bool | Blink |
| `reverse` | bool | Swap fg/bg |

Named colors: `black`, `red`, `green`, `yellow`, `blue`, `magenta`, `cyan`, `white`,
`bright_black` … `bright_white`, `default`.

#### `console.reset()`

Reset all text attributes to default.

### Input

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.readline` | `readline([prompt]) → string\|nil` | Read a line from stdin with optional prompt |
| `console.readkey` | `readkey() → string` | Read a single keypress (raw mode, no Enter needed) |
| `console.readpass` | `readpass([prompt]) → string\|nil` | Read a line with echo disabled (passwords) |

### Progress bar

#### `console.progress(current, total [, opts])`

Renders a progress bar inline (no newline). Overwrite with `\r` in a loop.

| Option | Default | Description |
|--------|---------|-------------|
| `width` | `30` | Bar width in characters |
| `fill` | `"█"` | Filled character |
| `empty` | `"░"` | Empty character |
| `color` | `"green"` | Bar color |
| `show_percent` | `true` | Show percentage |
| `show_count` | `false` | Show `current/total` |

### Shimmer / glow

#### `console.shimmer(text [, opts])`

One-shot animation: a brightness wave sweeps across the text, then leaves it dim.

| Option | Default | Description |
|--------|---------|-------------|
| `duration` | `1.5` | Animation length in seconds |
| `fps` | `30` | Frames per second |
| `wave_width` | `0.30` | Wave width as fraction of text length |
| `color` | `{r=255,g=255,b=255}` | Peak (brightest) color |
| `base` | `{r=70,g=70,b=70}` | Trough (darkest) color |

#### Background shimmer (non-blocking)

Runs in a background thread while your code does other work.

| Function | Signature | Description |
|----------|-----------|-------------|
| `console.shimmer_start` | `shimmer_start(text [, opts])` | Start background shimmer. `opts`: `fps`, `wave_width`, `bar_width`, `bar_total`, `color`, `base` |
| `console.shimmer_update` | `shimmer_update(text [, cur, total])` | Update text and optional progress counters |
| `console.shimmer_stop` | `shimmer_stop()` | Stop the background thread and clear the line |
| `console.shimmer_run` | `shimmer_run(text [, opts])` | Alias for `shimmer_start` |

```sea
console.shimmer_start("Loading model...", { bar_total = 100 })
for i = 0, 99 do
    -- do real work
    console.shimmer_update("Loading model...", i + 1, 100)
end
console.shimmer_stop()
```

---

## `http`

HTTP/1.1 client and server. Requires `http_native.so` (strict request/response parsers, URL coding), plus `libs/ssl` for HTTPS and `libs/json` for the JSON helpers. Build: `make -C libs/http libs/ssl libs/json` (one `make -C` each); `http_native.so` links zlib (`zlib1g-dev` / `zlib-devel`) for gzip.

```sea
local http = require("../libs/http/http")   -- relative to the requiring file
```

Sockets come from the built-in `net` module: every network wait parks only the goroutine doing it (netpoller), so a slow or idle peer never stalls the others. All timeouts are in milliseconds.

### Client

| Function | Signature | Description |
|----------|-----------|-------------|
| `http.request` | `request(method, url [, opts]) → response` | Any method. The older `request(method, url, body, content_type, headers)` form still works |
| `http.get` / `head` / `delete` | `get(url [, opts]) → response` | Requests without a body |
| `http.post` / `put` / `patch` | `post(url, body [, content_type [, opts]]) → response` | `content_type` defaults to `application/octet-stream` when a `body` is given; `body` may be `nil` when `opts.json`/`opts.form` is used |

**Client `opts`** (table or map):

| Key | Default | Description |
|-----|---------|-------------|
| `headers` | — | Extra request headers (a value may be an array for a repeated header). CR/LF in names or values is rejected |
| `body` | — | Request body (string) |
| `json` | — | Value to send JSON-encoded (sets `Content-Type: application/json`) |
| `form` | — | Map sent as `application/x-www-form-urlencoded` |
| `content_type` | — | Content-Type of `body` |
| `query` | — | Map appended to the URL as a query string |
| `timeout` | `30000` | Per network operation (connect, TLS handshake, each read/write) |
| `follow_redirects` | `true` | `false`, `true` (up to 5 hops) or a max hop count. 303 — and 301/302 after a POST — become GET; `Authorization`/`Cookie` are dropped when redirected to another host |
| `keep_alive` | `true` | Reuse connections (pool of up to 8 idle per host, 30 s max age; a stale reused connection is retried once) |
| `basic_auth` | — | `{user, password}` |
| `verify` | `true` | `false` skips TLS certificate checks |
| `tls` | — | Extra `ssl.connect` opts (`cert`, `key`, `sni`, `min_version`) |
| `max_body_bytes` | 100 MB | Larger responses fail (also the limit for a decompressed body) |
| `decompress` | `true` | Send `Accept-Encoding: gzip` and decode gzip/deflate responses. Off when you set `Accept-Encoding` yourself (the body is then yours to decode) |

The client sends `Host` (with the port when it is not the default), `User-Agent: sea-http/1.1`, `Accept: */*` and `Accept-Encoding: gzip` (unless given), and `Content-Length` (a non-string body is converted with `tostring`); it decodes chunked responses and skips 1xx interim responses.

**Response:**

| Field | Description |
|-------|-------------|
| `status` | Status code; `0` when the request failed (connection, TLS, timeout, malformed response) |
| `ok` | `true` for 2xx |
| `reason` | Reason phrase |
| `headers` | Map with lowercase keys (repeated headers joined with `", "`) |
| `body` | Body string (`""` for HEAD / 204 / 304) |
| `url` | Final URL after redirects |
| `error` | Message, only when `status == 0` |
| `uncompressed` | `true` when a gzip/deflate body was decoded (`Content-Encoding` and `Content-Length` are then removed from `headers`) |
| `resp:json()` | Decodes the body as JSON (an error value if it is not valid JSON) |

### Server

| Function | Signature | Description |
|----------|-----------|-------------|
| `http.listen` | `listen(port, handler [, opts]) → server\|error` | Start serving in the background and return at once. `port` 0 picks a free port |
| `http.serve` | `serve(port, handler [, opts])` | `listen` + print a "listening" line (unless `opts.quiet`) + block until the server is closed |

`handler(req) → response` is a function or a router. Every connection runs in its own goroutine.

**Server object:** `srv.port` (bound port), `srv.tls` (bool), `srv.close()` (stop accepting; idle keep-alive connections are closed, busy ones finish their request), `srv.wait()` (block until closed).

**Server `opts`:**

| Key | Default | Description |
|-----|---------|-------------|
| `host` | all interfaces | Address to bind |
| `tls` | — | Enable HTTPS: `{cert, key [, ca]}` (`ca` requires client certificates) |
| `read_timeout` | `30000` | Time to receive a whole request once it starts (also the TLS handshake); 408 after it |
| `idle_timeout` | `60000` | Time a keep-alive connection may wait for its next request |
| `write_timeout` | `30000` | Per response write |
| `max_header_bytes` | `65536` | Larger request heads get 431 |
| `max_body_bytes` | 10 MB | Larger bodies get 413 |
| `max_requests` | `1000` | Requests per connection before it is closed |
| `keep_alive` | `true` | `false` closes after every response |
| `log` | `false` | Print one line per request: method, target, status, time |
| `gzip` | `false` | `true`: gzip text-like responses (`text/*`, JSON, JavaScript, XML, SVG) of 1 KB or more for clients that accept it, adding `Vary: Accept-Encoding`; a number sets the minimum size. Responses that already have a `Content-Encoding` are left alone |
| `on_error` | prints | `on_error(err, req)`: called when a handler panics or hits a runtime error (the client gets a 500) |
| `quiet` | `false` | (`serve` only) no "listening" line |

The server supports keep-alive and pipelining, chunked request bodies, `Expect: 100-continue`, and HEAD (headers only). It adds `Date`, `Content-Length` and `Connection`. Malformed requests get 400, 501 (unknown Transfer-Encoding) or 505, including request-smuggling shapes: `Content-Length` together with `Transfer-Encoding`, conflicting lengths, obsolete line folding, and bare CR/LF.

**Request** (the handler's argument, a map):

| Field | Description |
|-------|-------------|
| `method` | `"GET"`, `"POST"`, … |
| `target` | Raw request target, e.g. `"/search?q=a%20b"` |
| `path` | Path **without** the query string (`"/search"`), not percent-decoded |
| `query` | Map of decoded query parameters (`req.query["q"] == "a b"`) |
| `headers` | Map with lowercase keys |
| `body` | Body string (chunked bodies already decoded) |
| `version` | `"HTTP/1.1"` or `"HTTP/1.0"` |
| `remote_addr` | `"ip:port"` of the peer |
| `tls` | `true` over HTTPS |
| `cookies` | Map from the `Cookie` header |
| `params` | Route parameters (router only) |
| `req:json()` | Decodes the body as JSON (an error value if it is not valid JSON) |

**Response** (what the handler returns): a table/map `{status = 200, body = "", headers = {...}, close = false}`, or just a string (200, `text/plain`). A non-string `body` is converted with `tostring`. Header values may be arrays (repeated headers). `close = true` closes the connection after the response. Anything else — `nil`, an invalid status, a header with CR/LF — becomes a 500 and is logged. A handler that panics or hits a runtime error (indexing nil, …) also gets the client a 500; the error goes to `on_error` (by default printed with the request) and the server keeps running. Without a `Content-Type`, `text/plain; charset=utf-8` is used.

**Response helpers:**

| Function | Description |
|----------|-------------|
| `http.text(body [, status])` | `text/plain` response |
| `http.html(body [, status])` | `text/html` response |
| `http.json(value [, status [, headers]])` | JSON-encoded `value`, `application/json` (a value that cannot be encoded, e.g. a cycle, panics, so the server answers 500) |
| `http.redirect(url [, status])` | Redirect (302 by default) with `Location` |
| `http.set_cookie(resp, name, value [, opts]) → resp` | Adds a `Set-Cookie`; `opts`: `path, domain, max_age, expires, secure, http_only, same_site` |
| `http.static(dir [, opts]) → handler` | Serves files under `dir` with a MIME type by extension; `opts`: `index` (`"index.html"`), `prefix` (URL prefix to strip). GET/HEAD only; `..` and NUL in paths are refused |

### Router

```sea
local r = http.router()
r.use(function(req, next) ... return next(req) end)
r.get("/users/:id", function(req) return http.json({ id = req.params["id"] }) end)
http.serve(8080, r)
```

| Method | Signature | Description |
|--------|-----------|-------------|
| `router.get` / `post` / `put` / `delete` / `patch` / `head` | `get(path, [mws,] fn)` | Register a handler for that method |
| `router.any` | `any(path, [mws,] fn)` | Handler for any method |
| `router.use` | `use(mw)` | Global middleware: `mw(req, next) → resp` |
| `router.group` | `group(prefix, {mws}) → group` | Sub-router with a shared prefix and middlewares (same methods) |
| `router.handle` | `handle(req) → resp` | Dispatch (what the server calls) |

**Path patterns:** exact segments (`/users`), parameters (`:name` → `req.params["name"]`, percent-decoded), and a trailing `*` (the rest of the path in `req.params["*"]`).

**Automatic responses:** 404 when no path matches; 405 with an `Allow` header when the path matches but the method does not. HEAD falls back to the GET route.

**Middleware:** call `next(req)` to continue, or return a response to short-circuit. Responses are turned into mutable maps, so middleware can change `status`, `body` and `headers`; a `nil` from a handler becomes 500.

### Utilities

| Function | Description |
|----------|-------------|
| `http.parse_url(url) → {scheme, host, port, path, query, target}\|nil` | Splits an http/https URL (IPv6 literals in `[...]`, default ports, fragment dropped) |
| `http.parse_query(s) → map` | Decodes `a=1&b=x%20y` (the part after `?` if `s` is a path or URL); `+` means space |
| `http.build_query(t) → string` | Encodes a map (keys sorted; an array value repeats the key) |
| `http.url_encode(s)` / `http.url_decode(s)` | Percent-encoding (everything but `A-Z a-z 0-9 - _ . ~`); decoding turns `+` into a space |
| `http.status_text(code) → string` | Reason phrase, e.g. `"Not Found"` |

```sea
-- Client
local r = http.get("https://api.example.com/items", { query = { page = 2 }, timeout = 5000 })
if r.ok then print(r:json().total) else print(r.status, r.error) end

local r2 = http.post("https://api.example.com/items", nil, nil, { json = { name = "sea" } })

-- Server, stopped from code
local srv = http.listen(8080, function(req)
    if req.path == "/hello" then
        return http.text("Hello, " .. (req.query["name"] or "world") .. "!\n")
    end
    return { status = 404, body = "Not found\n" }
end, { log = true })
-- ...
srv.close()

-- HTTPS
http.serve(8443, function(req) return "Secure hello!\n" end,
    { tls = { cert = "cert.pem", key = "key.pem" } })
```

**Breaking changes (2026-10):** `req.path` no longer includes the query string (use `req.query`, or `req.target` for the raw form). Handlers returning `nil`, panicking or failing with a runtime error now produce a 500 instead of stopping the server.

---

## `ssl`

Requires `ssl_native.so` (OpenSSL). Build: `make -C libs/ssl`.

```sea
local ssl = require("./libs/ssl/ssl")
```

Full SSL/TLS + cryptography library. All handles (connections, certs, keys, CSRs) are opaque integers; call the matching `_free` function when done.

TLS runs over sockets from the built-in `net` module (see `docs/reference.md`): every wait — handshake, read, write, accept — parks only the calling goroutine through the netpoller (on the main thread it blocks), so one slow peer never holds up the others. Timeouts are in milliseconds and apply per operation.

### TLS Client

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.connect` | `connect(host, port [, opts]) → conn\|nil` | Open a TLS client connection. `opts` (table or map): `{verify, cert, key, sni, min_version, timeout}`. On failure `ssl.last_error()` says why |
| `ssl.dial` | `dial(host, port [, opts]) → conn\|error` | Same as `connect`, but returns an error value (`is_error`) — safe from many goroutines at once |
| `ssl.last_error` | `last_error() → string\|nil` | Message of the last failure of a `nil`-returning function (shared, not per goroutine) |
| `ssl.read` | `read(conn [, n [, timeout]]) → string\|nil` | Without `n`: read until EOF. With `n`: read exactly `n` bytes (fewer if the stream ends) |
| `ssl.recv` | `recv(conn [, max [, timeout]]) → string\|error` | One read of whatever is available (up to `max`, default 65536); `""` at EOF |
| `ssl.read_request` | `read_request(conn [, timeout]) → string\|nil` | Read one full HTTP request (headers + Content-Length body). Prefer `libs/http` for real servers |
| `ssl.write` | `write(conn, data [, timeout]) → n\|nil` | Write all of `data` |
| `ssl.send` | `send(conn, data [, timeout]) → n\|error` | As `write`, with an error value on failure |
| `ssl.close` | `close(conn)` | Send close_notify (without waiting) and close the socket; wakes any goroutine waiting on it |
| `ssl.peer_cert` | `peer_cert(conn) → cert\|nil` | Peer certificate handle (inspect with `cert_info`, free with `cert_free`) |
| `ssl.version` | `version(conn) → string` | Negotiated TLS version, e.g. `"TLSv1.3"` |
| `ssl.cipher` | `cipher(conn) → string` | Negotiated cipher suite |

### TLS Server

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.server` | `server(opts) → ctx\|nil` | Create server context. `opts`: `{cert, key, ca, min_version}`. `ca` enables mTLS |
| `ssl.server_free` | `server_free(ctx)` | Free server context |
| `ssl.listen` | `listen(port [, host]) → fd\|nil` | Bind a TCP socket (a `net` fd) |
| `ssl.accept` | `accept(ctx, fd [, timeout]) → conn\|nil` | Accept one connection + TLS handshake (10 s default) |
| `ssl.handshake_server` | `handshake_server(ctx, fd [, timeout]) → conn\|error` | TLS server handshake on a socket from `net.accept`; closes it on failure |
| `ssl.wrap_server` | `wrap_server(ctx, fd [, timeout]) → conn\|nil` | As `handshake_server`, `nil` + `last_error()` on failure |
| `ssl.serve` | `serve(ctx, port, handler)` | Blocking accept loop; runs `handler(conn)` in a new goroutine per connection |

### Certificates

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.cert_load` | `cert_load(path) → cert\|nil` | Load X.509 cert from PEM file |
| `ssl.cert_parse` | `cert_parse(pem) → cert\|nil` | Parse X.509 cert from PEM string |
| `ssl.cert_info` | `cert_info(cert) → table` | `{subject, issuer, not_before, not_after, serial, fingerprint, san}`. `san` is a 0-indexed array |
| `ssl.cert_pem` | `cert_pem(cert) → string\|nil` | Serialize cert to PEM |
| `ssl.cert_save` | `cert_save(cert, path) → bool` | Save cert to PEM file |
| `ssl.cert_free` | `cert_free(cert)` | Free cert handle |
| `ssl.cert_verify` | `cert_verify(cert, ca_cert) → bool` | Verify cert was signed by ca_cert |
| `ssl.cert_self_signed` | `cert_self_signed(key, opts) → cert\|nil` | Self-signed cert. `opts`: `{cn, org, country, state, city, email, san=[], days=365, is_ca=false}` |
| `ssl.cert_sign` | `cert_sign(csr, ca_key, ca_cert, opts) → cert\|nil` | CA-sign a CSR. `opts`: `{days=365, serial=1}` |

### Keys

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.keygen` | `keygen(opts) → key\|nil` | Generate key pair. `opts`: `{type="rsa"\|"ec", bits=2048, curve="P-256"\|"P-384"\|"P-521"}` |
| `ssl.key_load` | `key_load(path [, password]) → key\|nil` | Load private key from PEM file |
| `ssl.key_parse` | `key_parse(pem [, password]) → key\|nil` | Parse private key from PEM string |
| `ssl.key_pem` | `key_pem(key [, password]) → string\|nil` | Serialize private key to PEM (AES-256-CBC encrypted if password given) |
| `ssl.key_pub_pem` | `key_pub_pem(key) → string\|nil` | Serialize public key to PEM |
| `ssl.key_save` | `key_save(key, path [, password]) → bool` | Save private key to PEM file |
| `ssl.key_free` | `key_free(key)` | Free key handle |

### CSR

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.csr_new` | `csr_new(key, opts) → csr\|nil` | Create a CSR. `opts`: `{cn, org, country, state, city, email, san=[]}` |
| `ssl.csr_pem` | `csr_pem(csr) → string\|nil` | Serialize CSR to PEM |
| `ssl.csr_free` | `csr_free(csr)` | Free CSR handle |

### Hash / HMAC

Supported `algo` strings: `"sha256"`, `"sha384"`, `"sha512"`, `"sha1"`, `"md5"`, `"sha3-256"`, `"sha3-512"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.hash` | `hash(algo, data) → string\|nil` | Hex-encoded digest |
| `ssl.hash_raw` | `hash_raw(algo, data) → string\|nil` | Binary digest |
| `ssl.hmac` | `hmac(algo, key, data) → string\|nil` | Hex-encoded HMAC |
| `ssl.hmac_raw` | `hmac_raw(algo, key, data) → string\|nil` | Binary HMAC |

### Symmetric Encryption

Supported `algo` strings: `"aes-128-gcm"`, `"aes-256-gcm"`, `"aes-128-cbc"`, `"aes-256-cbc"`, `"chacha20-poly1305"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.encrypt` | `encrypt(algo, key, plaintext [, iv]) → {data, iv, tag}\|nil` | Encrypt. For GCM/ChaCha20: IV auto-generated if omitted; `tag` is always filled |
| `ssl.decrypt` | `decrypt(algo, key, iv, ciphertext [, tag]) → string\|nil` | Decrypt. `tag` required for authenticated ciphers |

### Asymmetric Sign / Verify

Supported `algo` strings: `"rsa-sha256"`, `"rsa-sha384"`, `"rsa-sha512"`, `"rsa-pss-sha256"`, `"rsa-pss-sha384"`, `"rsa-pss-sha512"`, `"ecdsa-sha256"`, `"ecdsa-sha384"`, `"ecdsa-sha512"`.

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.sign` | `sign(algo, key, data) → string\|nil` | Sign data; returns binary DER signature |
| `ssl.verify` | `verify(algo, key, data, sig) → bool` | Verify signature |

### Utilities

| Function | Signature | Description |
|----------|-----------|-------------|
| `ssl.rand_bytes` | `rand_bytes(n) → string` | `n` cryptographically random bytes |
| `ssl.pbkdf2` | `pbkdf2(algo, password, salt, iterations, key_len) → string\|nil` | PKCS#5 PBKDF2 key derivation; returns binary |
| `ssl.hkdf` | `hkdf(algo, ikm, salt, info, length) → string\|nil` | RFC 5869 HKDF; returns binary |
| `ssl.base64_encode` | `base64_encode(data) → string` | Base64 encode (no newlines) |
| `ssl.base64_decode` | `base64_decode(str) → string\|nil` | Base64 decode |
| `ssl.hex_encode` | `hex_encode(data) → string` | Hex encode (lowercase) |
| `ssl.hex_decode` | `hex_decode(str) → string\|nil` | Hex decode |

```sea
-- TLS client
local conn = ssl.connect("example.com", 443)
ssl.write(conn, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
print(ssl.read(conn))
ssl.close(conn)

-- TLS server with mTLS
local ctx = ssl.server({cert="server.pem", key="server.key", ca="ca.pem"})
ssl.serve(ctx, 8443, function(conn)
    local data = ssl.read_request(conn)
    ssl.write(conn, "HTTP/1.0 200 OK\r\n\r\nHello\n")
    ssl.close(conn)
end)

-- Self-signed cert + key
local key = ssl.keygen({type="rsa", bits=2048})
local san = []; san[] = "localhost"; san[] = "127.0.0.1"
local cert = ssl.cert_self_signed(key, {cn="localhost", san=san, days=365})
ssl.key_save(key, "server.key")
ssl.cert_save(cert, "server.pem")
ssl.cert_free(cert)
ssl.key_free(key)

-- AES-256-GCM round-trip
local k = ssl.rand_bytes(32)
local r = ssl.encrypt("aes-256-gcm", k, "secret")
local plain = ssl.decrypt("aes-256-gcm", k, r.iv, r.data, r.tag)

-- SHA-256 hash
print(ssl.hash("sha256", "hello"))

-- RSA sign / verify
local k2 = ssl.keygen({type="rsa", bits=2048})
local sig = ssl.sign("rsa-sha256", k2, "data")
print(ssl.verify("rsa-sha256", k2, "data", sig))  -- true
ssl.key_free(k2)
```

---

## `jwt`

Requires `jwt_native.so` (OpenSSL). Build: `make -C libs/jwt`.

```sea
local jwt = require("./libs/jwt/jwt")
```

Supported algorithms: `HS256`, `HS384`, `HS512` (symmetric HMAC), `RS256` (RSA).

| Function | Signature | Description |
|----------|-----------|-------------|
| `jwt.sign` | `sign(claims, key [, opts]) → string\|nil` | Create a signed JWT token |
| `jwt.verify` | `verify(token, key [, opts]) → claims, err` | Verify and decode a token |
| `jwt.decode` | `decode(token) → header, payload` | Decode without verifying signature |
| `jwt.algs` | `algs() → array` | List of supported algorithm names |

**`sign` opts:**

| Key | Default | Description |
|-----|---------|-------------|
| `alg` | `"HS256"` | Algorithm |
| `exp` | — | Expiry in seconds from now (adds `exp` claim) |
| `iss` | — | Issuer |
| `sub` | — | Subject |
| `aud` | — | Audience |
| `nbf` | — | Not-before Unix timestamp |

**`verify` opts:**

| Key | Default | Description |
|-----|---------|-------------|
| `verify_exp` | `true` | Validate `exp` claim |

`verify` returns `claims, nil` on success and `nil, "reason"` on failure.

```sea
-- HS256
local token = jwt.sign({ sub = "user123", role = "admin" }, "my-secret", {
    alg = "HS256",
    exp = 3600,
})

local claims, err = jwt.verify(token, "my-secret")
if claims then
    print(claims.sub)   -- "user123"
else
    print("invalid:", err)
end

-- Inspect without verifying
local header, payload = jwt.decode(token)
print(header.alg)   -- "HS256"
```

---

## `flags`

Pure Sea. No build required.

```sea
local flags = require("./libs/flags/flags")
```

Modeled after Go's `flag` package. All flag definitions must come before `flags.parse()`.

### Flag definition

| Function | Signature | Description |
|----------|-----------|-------------|
| `flags.string` | `string(name, default, usage) → ref` | Define a string flag |
| `flags.int` | `int(name, default, usage) → ref` | Define an integer flag |
| `flags.bool` | `bool(name, default, usage) → ref` | Define a boolean flag |
| `flags.number` | `number(name, default, usage) → ref` | Define a float flag |

Each function returns a **flagref** table with a `.value` field that holds the parsed value after `flags.parse()` is called.

### Parsing

```sea
flags.parse()
```

Reads `os.args[1..n]`. Recognized syntax:
- `-flag value` or `--flag value`
- `-flag=value` or `--flag=value`
- `-boolFlag` (sets to `true`, no value needed)
- `--` ends flag parsing; remaining arguments become positional
- `-h` / `--help` prints usage and exits with code 0

Unknown flags call `flags.usage()` and exit with code 2.

### Accessing results

| Function | Signature | Description |
|----------|-----------|-------------|
| `flags.get` | `get(name) → value\|nil` | Current value of flag `name` |
| `flags.is_set` | `is_set(name) → bool` | `true` if the flag was explicitly passed |
| `flags.args` | `args() → table` | Positional arguments after flag parsing (0-indexed) |
| `flags.narg` | `narg() → int` | Number of positional arguments |
| `flags.nflag` | `nflag() → int` | Number of defined flags |

### Usage / help

| Function | Signature | Description |
|----------|-----------|-------------|
| `flags.usage` | `usage()` | Print default Go-style help to stderr |
| `flags.set_usage` | `set_usage(fn)` | Replace the usage function with a custom one |
| `flags.reset` | `reset()` | Clear all state (useful in tests) |

```sea
local host    = flags.string("host",    "localhost", "server address")
local port    = flags.int("port",       8080,        "server port")
local verbose = flags.bool("verbose",   false,       "verbose output")

flags.parse()

print(host.value)     -- "localhost" (or whatever was passed)
print(flags.narg())   -- number of positional args
```

---

## `test`

A test framework in pure Sea: groups, hooks, assertions with readable
differences, skipping, a pretty or TAP report, and an exit status for CI.

```sea
local t = require("../libs/test/test")   -- relative to the requiring file

t.describe("parser", function()
    local p = nil
    t.before_each(function() p = new_parser() end)
    t.it("reads numbers", function()
        t.eq(p:parse("42"), 42)
    end)
    t.it("rejects junk", function()
        t.is_error(p:parse("4x2"), "unexpected")
    end)
end)

t.main()   -- run, print the report, exit 1 if anything failed
```

```
parser
  ✓ reads numbers
  ✗ rejects junk

1) parser > rejects junk
   expected an error value, got 4

1 passed, 1 failed (0.01 s)
```

**Defining tests:**

| Function | Description |
|----------|-------------|
| `t.describe(name, fn)` | A group (groups nest; full names are `group > sub > test`) |
| `t.it(name, fn)` | A test |
| `t.skip(name, fn)`, `t.describe_skip(name, fn)` | Defined but not run (reported as skipped) |
| `t.only(name, fn)` | When any test uses `only`, only those run |
| `t.todo(name)` | A placeholder, reported as todo |
| `t.before_each(fn)`, `t.after_each(fn)` | Around every test of the group and its subgroups (outer `before_each` first, inner `after_each` first) |
| `t.before_all(fn)`, `t.after_all(fn)` | Once around the group's tests (a failing `before_all` fails them) |

**Assertions** (a failing one stops its test; the others go on). Each takes
an optional last `msg` that prefixes the failure:

| Assertion | Passes when |
|-----------|-------------|
| `t.eq(got, want)` | Deep equality: lists element by element, tables, maps and structs by keys and values, `1 == 1.0`; the failure shows where they differ (`at items[2].name`) |
| `t.ne(got, other)` | Not deeply equal |
| `t.ok(v)`, `t.no(v)` | `v` is truthy / `nil` or `false` |
| `t.is_nil(v)`, `t.not_nil(v)` | |
| `t.near(got, want [, eps])` | Within `eps` (default 1e-9 relative) |
| `t.gt`, `t.ge`, `t.lt`, `t.le(got, bound)` | The comparison holds |
| `t.contains(hay, needle)` | A substring, a list element or a record value (deep equality) |
| `t.matches(s, pattern)` | `s` matches a regular expression (module `re`) |
| `t.is_error(v [, text])` | `v` is an error value, its message containing `text` |
| `t.no_error(v)` | `v` is not an error value (returns `v`) |
| `t.raises(fn [, text])` | `fn` panics (or hits a runtime error), the message containing `text`; returns the message |
| `t.fail([msg])` | Never |

A runtime error or a panic inside a test (or a hook) is reported as an
*error*, with its message (`file.sea:12: attempt to index a nil value`).

**Running:** `t.run([opts]) → result` prints the report and returns
`{passed, failed, skipped, todo, total, ok, failures}` (`failures`: `{name,
kind, message}`); `t.main([opts])` runs and exits with status 1 when
something failed. Options: `filter` (run the tests whose full name contains
it; default: the environment variable `SEA_TEST`), `format` (`"pretty"`,
`"tap"`, `"quiet"`), `timing` (show times; default `true`). Also `t.equal(a,
b)` (the deep equality as a bool), `t.show(v)` (a value as readable text) and
`t.reset()` (forget the tests defined so far).

---

## `tensor`

Numeric kernels on `float_array`: the heavy loops of a small transformer
(matrix-vector products and their backward passes, RMSNorm, causal
attention, softmax, Adam), so a model written in Sea trains at C speed while
the orchestration stays in Sea. On a small transformer written in Sea, swapping
these kernels in for the same functions written in Sea gave identical results,
58× faster.

**Require:** `require("../../libs/tensor/tensor")` (relative to your file)
**Build:** `make -C libs/tensor` (compiled with `-O3 -march=native`)

Every function works in place on float_arrays addressed as `(array, offset)`
and returns `nil` unless noted. Matrices are row-major: `w[i*nout + j]` links
input `i` to output `j`. Backward functions accumulate (`+=`) into their
gradient arguments; the caller zeroes them. A range outside an array, or an
argument of the wrong type, is a runtime error (a panic).

| Function | Effect |
|----------|--------|
| `matvec(x, xo, w, nin, nout, out, oo)` | `out[oo+j] = Σ_i x[xo+i]·w[i·nout+j]` |
| `matvec_back(dout, doo, x, xo, w, dw, nin, nout, dx, dxo)` | `dw += x ⊗ dout`, `dx += w · dout` |
| `matvec_cols(x, xo, w, nin, nout, cols, ncols, out, oo)` | only the outputs listed in `cols` (an `int_array`): `out[oo+k] = Σ_i x[xo+i]·w[i·nout+cols[k]]` |
| `matvec_cols_back(dout, doo, x, xo, w, dw, nin, nout, cols, ncols, dx, dxo)` | its backward |
| `rmsnorm(x, xo, y, yo, n) → r` | `y = x·r` with `r = 1/√(mean(x²) + 1e-5)` |
| `rmsnorm_back(dy, dyo, x, xo, r, n, dx, dxo)` | `dx += r·dy − x·r³·(dy·x)/n` |
| `relu(x, xo, y, yo, n)` | `y = max(x, 0)` |
| `relu_back(d, doff, x, xo, n)` | `d = 0` where `x ≤ 0` |
| `softmax(p, off, n)` | in place, numerically stable |
| `attention(q, k, v, att, y, T, C, H, B)` | causal multi-head attention over `T` positions of width `C`, `H` heads, context `B` (`T ≤ B`); `att` (`H·B·B`) keeps the probabilities for the backward |
| `attention_back(q, k, v, att, dy, dq, dk, dv, T, C, H, B)` | accumulates into `dq`, `dk`, `dv` |
| `zero(a [, off, n])` | zero a range (the whole array by default) |
| `add(dst, doff, src, soff, n)` | `dst[doff+i] += src[soff+i]` |
| `copy(dst, doff, src, soff, n)` | copy a range (overlap allowed) |
| `adam(w, g, m, v, n, lr, beta1, beta2, c1, c2, eps)` | one Adam step; `c1 = 1 − beta1^t`, `c2 = 1 − beta2^t` |
| `save(path, a [, append]) → bytes` | write the array as raw doubles (native byte order); an error value on failure |
| `load(path, a, offset) → next offset` | fill the whole array from the file at a byte offset; an error value if the file is short |

```sea
local tensor = require("../../libs/tensor/tensor")
local x = float_array()
x[] = 1.0
x[] = 2.0
local w = float_array()
for i = 1, 6 do w[] = i end
local out = float_array()
for i = 1, 3 do out[] = 0.0 end
tensor.matvec(x, 0, w, 2, 3, out, 0)      -- out = 9, 12, 15
```

Calls from several goroutines on different arrays run in parallel. Test:
`tests/unit/113_tensor.sea`.

---

## `duckdb`

[DuckDB](https://duckdb.org), the embedded analytical SQL database: CSV,
Parquet and JSON files, window functions, `ATTACH`, all from SQL. Build:
`make -C libs/duckdb`. The first build runs `fetch.sh`, which downloads the
official `libduckdb` release (1.5.6, SHA-256 checked) into
`libs/duckdb/vendor/`. To use an installed copy instead:
`make -C libs/duckdb DUCKDB_INC=/usr/local/include DUCKDB_LIB=/usr/local/lib`.
Tests: `make -C libs/duckdb test`.

```sea
local duckdb = require("../libs/duckdb/duckdb")   -- relative to the requiring file

local db = duckdb.open("sales.duckdb")?           -- nil or ":memory:": in memory
local _ = db:exec("CREATE TABLE IF NOT EXISTS sales (day DATE, item VARCHAR, qty INTEGER, price DECIMAL(10,2))")?
local added = db:exec("INSERT INTO sales VALUES (?, ?, ?, ?)", "2026-10-01", "tea", 3, 4.5)?
for _, r in db:query("SELECT item, sum(qty * price) AS total FROM sales GROUP BY item ORDER BY total DESC")? do
    print(r.item, r.total)
end
print(db:value("SELECT count(*) FROM read_csv('orders.csv')"))
db:close()
```

| Function | Signature | Description |
|----------|-----------|-------------|
| `duckdb.open` | `open([path [, opts]]) → db\|error` | Open (or create) a database file; no path or `":memory:"` is in memory |
| `duckdb.version` | `version() → string` | The DuckDB library version (`"v1.5.6"`) |
| `db:exec` | `exec(sql, params...) → int\|error` | Run SQL; returns the number of rows changed. Without parameters it may hold several statements separated by `;` |
| `db:query` | `query(sql, params...) → array\|error` | All rows, each a table keyed by column name |
| `db:row` | `row(sql, params...) → table\|nil\|error` | The first row |
| `db:value` | `value(sql, params...) → value\|nil\|error` | The first column of the first row |
| `db:columns` | `columns(sql, params...) → table\|error` | Column-wise: `{names, types, count, data = {column = values}}` (see below) |
| `db:each` | `each(sql, fn, params...) → int\|error` | Call `fn(row)` per row (`false` stops); returns the rows seen |
| `db:prepare` | `prepare(sql) → stmt\|error` | A prepared statement |
| `db:appender` | `appender(table [, schema]) → app\|error` | Fast bulk insert into a table |
| `db:transaction` | `transaction(fn) → value\|error` | `BEGIN`, `fn(db)`, then `COMMIT`; `ROLLBACK` if `fn` returns an error value or panics (the panic goes on) |
| `db:connect` | `connect() → conn\|error` | Another connection to the same database, with the same methods |
| `db:interrupt` | `interrupt()` | Stop the query running on this connection (from another goroutine) |
| `db:close` | `close() → bool` | Close; on the database it closes every connection, statement and appender. Closing twice is fine |

**Options for `open`** (table, map or struct): `read_only = true`, `threads`,
`memory_limit` (`"2GB"`) and any other [DuckDB setting](https://duckdb.org/docs/configuration/overview)
by name, with the value as a string, number or bool. An unknown setting or a
bad value is an error.

**Parameters** come after the SQL (`?` or `$1`, `$2`...), or as one positional
table `{a, b}`, or as a table of named values `{name = v}` for `$name`:

```sea
db:value("SELECT name FROM people WHERE id = ? AND age > ?", 7, 18)
db:query("SELECT * FROM people WHERE id = ? AND age > ?", {7, 18})
db:row("SELECT * FROM people WHERE age > $min AND city = $city", {min = 18, city = "Recife"})
```

`nil` is NULL, bools, ints, numbers and strings map to the obvious types
(DuckDB casts them to the parameter's type, so a string works for a `DATE`);
a `bigint` goes as `BIGINT`, `HUGEINT` or `BIGNUM`, the narrowest that holds it.
Arrays, `int_array`, `float_array` and positional tables become LISTs,
tables with string keys STRUCTs and maps `[=]` MAPs. To pass one list as the
only parameter, wrap it: `db:value("SELECT list_sum(?)", {values})`.

**Result values:**

| DuckDB | Sea |
|--------|-----|
| `NULL` | `nil` (row tables keep the key) |
| `BOOLEAN` | bool |
| integers (`TINYINT` … `UBIGINT`, `HUGEINT`, `UHUGEINT`) | exact: an int within Sea's ±2^47, a number up to ±2^53, a `bigint` beyond |
| `BIGNUM` | `bigint` |
| `FLOAT`, `DOUBLE`, `DECIMAL` | number |
| `VARCHAR`, `BLOB`, `ENUM`, `UUID`, `BIT` | string (a BLOB holds its bytes) |
| `DATE`, `TIME`, `TIMESTAMP` (all precisions) | ISO string: `"2024-02-29"`, `"13:45:07.25"`, `"2024-01-02 03:04:05.123456"`; `TIMESTAMPTZ` in UTC with `+00`; `infinity`; BC dates end in ` (BC)` |
| `INTERVAL` | `{months, days, micros}` |
| `LIST`, `ARRAY` | array |
| `STRUCT` | table |
| `MAP` | map `[=]` |
| `UNION` | the value of its member |
| `GEOMETRY` | a GeoJSON-shaped table: `{type = "Point", coordinates = [1, 2]}`, `LineString`, `Polygon`, the `Multi` kinds (Z and M ordinates included), `{type = "GeometryCollection", geometries = [...]}`; cast to `VARCHAR` for WKT or `BLOB` for WKB |
| `VARIANT` | the value it holds, converted as above (objects → tables, arrays → arrays; a MAP inside one arrives as an array of `{key, value}`) |

Dates and times come as strings because they are exact and go back into SQL
as parameters unchanged; `time.unix` builds a Time from `epoch(ts)` when you
need one. VARIANT values are read from DuckDB's internal layout, which the C
API does not describe; the library's test covers every type, so a DuckDB
version that changed it would fail there.

**`columns`** returns the result column by column: integer columns without
NULLs as `int_array`, floating and decimal ones as `float_array` (ready for
`libs/tensor`), the rest as arrays; `types` holds the SQL type of each column
(`"DECIMAL(10,2)"`, `"VARCHAR[]"`, `"STRUCT(a INTEGER)"`).

```sea
local c = db:columns("SELECT x, y FROM points")?
print(c.count, c.types[0], type(c.data.x))      -- 1000  DOUBLE  float_array
```

**Prepared statements** keep the parsed plan: `stmt:query(params...)`,
`stmt:exec`, `stmt:row`, `stmt:value`, `stmt:columns`, `stmt:each(fn, params...)`
and `stmt:close()`. `stmt.params` lists the parameter names (`"1"`, `"2"` for
positional ones). A wrong number of parameters is an error.

**Appenders** insert rows faster than `INSERT`: `app:append(row)` takes a
sequence in column order (`{1, "ana", "2026-10-01"}`) or a table by column
name (absent columns get their `DEFAULT`); `app:append_rows(rows)` takes many;
`app:flush()` writes what is buffered; `app:close()` flushes and frees.
Values are cast to the column types (strings to dates, ints to decimals...).
After an error, close the appender.

**Goroutines.** One connection runs one query at a time (others on it wait
their turn). In a goroutine a query runs on a worker thread (up to 8) and only
that goroutine waits, so the scheduler keeps running the others; on the main
thread the query runs in place. For queries in parallel, give each goroutine
its own `db:connect()`:

```sea
local results = chan(4)
for i = 1, 4 do
    go function()
        local c = db:connect()
        results <- c:value("SELECT count(*) FROM read_parquet(?)", "part" .. i .. ".parquet")
        c:close()
    end ()
end
```

All failures are error values whose message is DuckDB's
(`duckdb.query: Catalog Error: Table with name nope does not exist!`),
including a closed handle (`the connection or statement is closed`).

---

## `sqlite`

SQLite, the embedded transactional SQL database, compiled into the module
(nothing to install). `make -C libs/sqlite` downloads the SQLite 3.53.4
amalgamation into `libs/sqlite/vendor/` (`fetch.sh`, pinned SHA-256) and
builds `sqlite_native.so`; `make -C libs/sqlite test` runs its tests. Built
with FTS5, R*Tree, JSON and math functions; extension loading is off.

```sea
local sqlite = require("../libs/sqlite/sqlite")   -- relative to the requiring file

local db = sqlite.open("app.db", {wal = true})?
local _ = db:exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INT)")?
local _ = db:exec("INSERT INTO users (name, age) VALUES (?, ?)", "ana", 31)?
print(db:last_insert_id())
for _, u in db:query("SELECT * FROM users WHERE age >= :min", {min = 18})? do print(u.id, u.name) end
db:close()
```

| Function | Description |
|----------|-------------|
| `sqlite.open([path [, opts]]) → db\|error` | Open (or create) a database file; `nil` or `":memory:"` for an in-memory one. `opts`: `read_only`, `create` (default `true`), `timeout` (ms to wait for another connection's lock; 5000), `wal` (`PRAGMA journal_mode = WAL`: readers and a writer at once), `foreign_keys` (default `true`) |
| `sqlite.version() → string` | `"3.53.4"` |
| `sqlite.blob(bytes)` | A value that binds as a BLOB (strings bind as TEXT) |
| `db:exec(sql, params...) → int\|error` | Run a statement; the rows changed. Without parameters the SQL may hold several statements separated by `;` |
| `db:query(sql, params...) → array\|error` | The rows, tables keyed by column name |
| `db:row(sql, params...) → row\|nil\|error` | The first row |
| `db:value(sql, params...) → value\|nil\|error` | The first column of the first row |
| `db:each(sql, fn, params...) → int\|error` | Call `fn(row)` for each row as it is read (memory stays flat; `false` from `fn` stops); `fn` may use the same database |
| `db:prepare(sql) → stmt\|error` | A prepared statement: `stmt:query/exec/row/value(params...)`, `stmt:each(fn, params...)`, `stmt:close()`, `stmt.params` (parameter names, `"1"`, `"2"`... for `?`), `stmt.columns` |
| `db:transaction(fn) → value\|error` | `BEGIN`, `fn(db)`, then `COMMIT`; rolls back when `fn` returns an error value or panics (the panic goes on) |
| `db:last_insert_id() → int` | The rowid of the last `INSERT` on this connection |
| `db:backup(path) → true\|error` | A consistent copy of the database into a file (online backup) |
| `db:connect() → db\|error` | Another connection to the same file (for goroutines writing in parallel; open with `wal = true`) |
| `db:interrupt()` | Stop the statement running on the connection (from another goroutine) |
| `db:close()` | Close the connection and its statements; later calls are error values |

**Parameters**: values after the SQL for `?` / `?NNN`, one positional table
`{a, b}`, or a table of named values `{name = v}` for `:name`, `@name` or
`$name`. Sea values bind as: `nil` → NULL, `bool` → 0/1, `int` → INTEGER,
`number` → REAL, `string` → TEXT, `sqlite.blob(s)` → BLOB, `bigint` →
INTEGER (TEXT beyond 64 bits).

**Results**: INTEGER → `int` within ±2^47, `number` while exact (±2^53),
`bigint` beyond; REAL → `number`; TEXT → `string`; BLOB → `string` (bytes);
NULL → `nil`.

**Goroutines**: in a goroutine, `exec`/`query`/`row`/`value` run on a worker
thread (at most 8) and only that goroutine waits; on the main thread they run
in place. One connection runs one statement at a time; use `db:connect()` for
parallel work. Failures are error values: `sqlite.exec: UNIQUE constraint
failed: users.email`, `sqlite.query: expected 2 parameters, got 1`.

---

## `parser`

The Sea parser is a built-in module now: `require("sea/parser")`, with no
build step. See [reference.md](reference.md#module-seaparser).
