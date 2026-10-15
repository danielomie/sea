# Memory model

Sea has **no garbage collector**. Every heap object (string, array, map, table,
struct, closure, channel, error) carries a reference count. When the last
reference to an object goes away, the object is freed **immediately**. There are
no collection pauses, the heap never grows past what the program actually uses,
and nothing has to be tuned.

This page covers what programs can rely on, the one case refcounting does not
handle (cycles), and how the compiler and the VM keep the counts right. That last
part matters if you work on the VM or write native libraries.

---

## What a program can rely on

A value stays alive as long as something references it:

- a local variable or parameter (until it goes out of scope or is reassigned);
- a field of a table or struct, an element of an array or map, a key;
- a global variable;
- a closure that captured the variable holding it;
- a channel buffer, between a send and the matching receive;
- a pending `defer` (its function and arguments);
- a running goroutine (its function and arguments).

When the last of these goes away, the value is freed right then. That covers the
end of a block, an early `return`, `break` or `continue`, an error propagated
with `?`, and `x = nil`. Freeing an object releases what it references in turn.

```sea
local function load()
    local rows = string.split(read_file(), "\n")   -- one array + N strings
    local first = rows[0]
    return first          -- `rows` and every other line are freed here;
end                       -- `first` stays alive: the caller now owns it
```

Strings, numbers and booleans behave like values: assigning or passing them
never aliases mutable state. Arrays, maps, tables and structs are shared by
reference.

### Cycles are not collected

Refcounting cannot free a group of objects that reference each other. Each one
keeps the next alive, so none of the counts ever reaches zero.

```sea
local a = {}
local b = {}
a.peer = b
b.peer = a          -- a ↔ b: both leak when the locals go out of scope

local obj = {}
obj.handler = function() return obj.name end   -- obj → closure → obj
```

A one-off structure like this costs little. It matters when such structures are
created over and over, for example one per request in a long-running server. To
avoid the leak, break the cycle when you are done (`a.peer = nil`), or keep one
direction as a plain key (an id or a name) instead of a reference. Parent links in
trees and back-references in graphs are the usual cases.

A `local function` that calls or names itself does **not** form a cycle: inside
its own body it refers to the running closure directly. Two exceptions: if the
enclosing function ever assigns to that name, or if a closure nested inside the
function refers to it, the reference becomes an ordinary capture, and that is a
cycle.

A cycle collector (in the style of CPython's) is possible future work. It would
only ever scan containers and closures.

### Goroutines

Objects are born *local*, with plain increments and decrements. A value that
crosses a goroutine boundary is first marked *shared* (recursively), and from
then on its count is updated atomically. A value crosses when it is sent on a
channel, passed to `go f(...)`, stored in a global, or captured by a closure a
goroutine runs. Code that never shares a value never pays for atomics.

---

## How the VM keeps the counts right

Registers do not adjust counts on every move. That would put a retain/release on
almost every instruction. Instead, ownership is decided **at compile time**, and
the compiler emits explicit `RETAIN` / `RELEASE` instructions only where a
reference changes hands.

### Ownership rules

| Holder | Owns its value? |
|--------|-----------------|
| A named local (parameters included) | **Yes**, from declaration to every exit of its scope |
| A temporary register | Only if the compiler marked it *owned* (see below) |
| An argument register | **Yes**: arguments are passed owned and the callee owns its parameters |
| A return value | **Yes**: results are delivered owned to the caller |
| Containers, globals, upvalues, channel buffers, defer entries | **Yes**: storing retains, overwriting or removing releases |

Each expression result is classified when it is compiled:

- **scalar**: needs no counting (nil, booleans, numbers, results of arithmetic and
  comparisons, string constants);
- **borrowed**: a reference someone else owns (a local's value, a field, an element,
  a global, an upvalue, a vararg);
- **owned**: a new reference the code must consume (call results, constructors, `..`,
  closures, channel receives).

Consuming an owned value means moving it into a local, passing it as an argument,
returning it, storing it with a "move" store (`SET_FIELD_MOVE`, `ARRAY_PUSH_MOVE`),
or releasing it. A borrowed value that becomes owned (stored in a local, passed,
returned) is retained once.

### Where the compiler releases

- **End of a block**: every owning local of the block (`end_scope`).
- **`break` / `continue`**: the locals of the loop body that are in scope.
- **`return`**: every local of the function. `return x` hands over the reference `x`
  already holds, with no retain and no release.
- **`e?`**: when `e` is an error, the locals *and* the owned temporaries still live
  (e.g. arguments already evaluated for an enclosing call) are released, then the
  error is returned.
- **Assignment** (`SETLOCAL`): the new value is stored, and retained if borrowed,
  **before** the old one is released. So `x = x` and `x = x.next` are safe.
  Multiple assignment (`a, b = b, a`) makes every value owned before the first store.

Captured locals are not released at scope exit. `CLOSE_UP` moves their reference
into the closure's upvalue and clears the register.

### Avoiding needless work

- **Scalar facts.** The compiler tracks which locals provably hold a number, a bool,
  nil or a constant string. It uses the same sound dataflow as the int inference:
  meets at branch merges and a fixpoint at loop heads. For those locals, scope exit
  emits nothing, and an assignment compiles to a plain register write. A local
  used as an operand of a successful arithmetic operation is known to be a number
  from then on. Numeric loops therefore compile exactly as before.
- **String constants are pinned.** Constants loaded from the chunk need no retain
  or release (`SEA_RC_PINNED_BIT`). This also makes them safe to share between
  goroutines.
- **Arena objects.** Escape analysis allocates non-escaping `{...}` / `[]` locals in
  the frame arena. They are counted like any object (`SEA_RC_ARENA_BIT`), but when
  the count reaches zero only their contents are released; the header goes with the
  frame.
- **Program exit.** The main script's top-level locals are not released when it
  ends. The process is exiting, and freeing a large structure object by object
  only delays that. Modules still release theirs.

### Runtime pieces

| Mechanism | Rule |
|-----------|------|
| `OP_CALL` to a C function | args are released after the call; a discarded result is released |
| `OP_RETURN` | releases results nobody takes, missing ones are nil; the frame's varargs are released |
| `for-in` | `R[iter]` holds the container for the whole loop; loop variables own their values (each step releases the old ones) |
| Channels | a buffered or handed-off value holds a reference, which the receiver gets |
| `defer` | the entry retains the function; the arguments move into it; released after the deferred call |
| `go f(...)` | the goroutine retains `f`; the arguments move into it; released when it finishes |
| Open upvalues | the open list holds a reference, so a closure freed early cannot leave a dangling entry |

---

## Writing native functions

C functions registered with `sea_vm_register_native` / `sea_mod_set`:

- **Arguments are borrowed.** Do not release them. Retain an argument only if you
  store it somewhere that outlives the call (`sea_table_set`, `sea_array_push` and
  `sea_map_set` already retain).
- **Return an owned value (+1).** A freshly created object (RC = 1) is returned as
  is. When returning an argument, or an element read from a container, retain it:
  `return sea_val_ref(args[0]);`.
- **`sea_vm_call(vm, fn, args, argc)`**: the arguments are borrowed, and the result is
  owned. Release it if you do not keep or return it.
- **`sea_open`** returns the module table owned (as created by `sea_mod_new`).

```c
static SeaValue fn_first(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_array(args[0]) || SEA_AS_ARRAY(args[0])->len == 0)
        return SEA_NIL_VAL;
    return sea_val_ref(SEA_AS_ARRAY(args[0])->data[0]);   /* borrowed → owned */
}

static SeaValue fn_pair(SeaVM *vm, int argc, SeaValue *args) {
    SeaArray *a = sea_array_new(2);
    SeaString *s = sea_string_from_cstr("x");
    sea_array_push(a, sea_obj_val(&s->obj));   /* the array retains it */
    sea_obj_release(&s->obj);                  /* drop our reference */
    return sea_obj_val(&a->obj);               /* new object: already +1 */
}
```

---

## Known limits

- **Cycles leak** (see above).
- **A function that drops the last reference to itself while running.** For
  example, an anonymous handler stored only in a table that removes itself from
  that table: the closure is freed under the running frame. Keep another reference
  (a local) for the duration of the call.
- **`panic` without `recover`** ends the process, so nothing is released.

## Checking for leaks

Build with AddressSanitizer and LeakSanitizer (see `make asan`). The main script
keeps its top-level data until exit, so a fixed amount of "leaked" memory at exit
is expected. To find a real leak, compare a workload at two sizes: memory that
grows with N is a leak.

```sh
ASAN_OPTIONS=detect_leaks=1 ./sea-asan probe.sea 200
ASAN_OPTIONS=detect_leaks=1 ./sea-asan probe.sea 400   # same total → no leak per iteration
```

`tests/unit/105_ownership.sea` covers the ownership rules. Each case in it used
to be a use-after-free or a leak.
