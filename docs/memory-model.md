# Khudra Memory Model

Status: implemented. Written as a draft in Phase 0, before the code that
depends on it; the Phase 2 checker and the Phase 5 runtime are validated
against it.

Khudra's second core feature is that *memory management is chosen through an
object*. A class declares its strategy as an ordinary field, and an allocation
site may override it. There is no global GC-vs-manual switch.

## 1. Selecting a strategy

### 1.1 The class default

```khudra
public class Example {
    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();
}
```

`MemoryAllocationTypeObject` has exactly two factories:

| Factory | Strategy | Meaning |
|---|---|---|
| `setStandard()` | `GC` | garbage collected (the default) |
| `setManual()` | `MANUAL` | manually allocated, freed with `free`/`dispose` |

Rules enforced by the memory-model checker (Phase 2):

- A class may declare at most one field of type `MemoryAllocationTypeObject`.
- Its initializer must be a direct call to one of the two factories. Any other
  initializer -- a variable, a method result, `null` -- is a compile error.
- The field is read **at allocation time only**.
- **Post-construction reassignment of `type` is a type error.** The strategy is
  fixed at materialization; letting it change afterwards would mean an object
  could migrate between the collector's heap and a manual arena, which nothing
  downstream (reference maps, pin counts, arena headers) is prepared for.
- A class with no `MemoryAllocationTypeObject` field defaults to `GC`.

### 1.2 The allocation-site override

```khudra
FrameBuffer fb = FrameBuffer(manual);      // overrides the class default
Sprite s      = Sprite(standard, x, y);    // explicit GC
Sprite t      = Sprite(x, y);              // class default
```

`manual` and `standard` are reserved words. They are only meaningful as the
**first argument of an allocation site**; the strategy token is consumed by the
allocator and is *not* passed to the class's `Procedures` block. The site
override always wins over the class default.

## 2. The object header

Every object -- managed or manual -- begins with the same header
(`src/vm/object.h`):

```
class_id   uint32   index into the class table
flags      uint32   bit0 = GC, bit1 = MANUAL
pin_count  uint32   number of managed slots holding this manual object
size       uint32   total allocation size in bytes
vtable     void*    method table
gc_link    void*    intrusive list link, used by the collector
```

`gc_link` lives in the header rather than in a side table so that the layout is
GC-aware from day one and tracing never needs a second lookup.

## 3. Reference maps

Each class descriptor carries a **reference map**: one entry per object slot,
tagged as

| Kind | Meaning |
|---|---|
| `RAW` | a scalar (integer, float, bool) or a `*byte` -- never traced |
| `MANAGED_REF` | a reference to a GC object -- traced by the collector |
| `MANUAL_REF` | a reference to a manual object -- pinned, never traced |

The reference map is what makes tracing *precise*: the collector never has to
guess whether a slot holds a pointer. It doubles as the pin bookkeeping table --
when the collector reclaims an object it walks the map and decrements the pin
count of every `MANUAL_REF` slot.

## 4. Cross-strategy references

The two halves of the heap can point at each other, and each direction has its
own rule.

### 4.1 manual -> managed: rooting

A manual object holding a managed reference is a **GC root**. Manual memory is
outside the collected heap, so anything a live manual object still points at is
reachable by definition.

The implementation does not keep a separate root set: the collector walks the
live manual list at the start of every mark phase and traces each manual
object's `MANAGED_REF` slots. Freeing the manual owner takes it off that list,
and the managed object becomes collectable at the next cycle if nothing else
holds it.

### 4.2 managed -> manual: reference-counted pinning

A managed object holding a manual reference **pins** it:

- Assigning a manual reference into a managed slot increments the manual
  object's `pin_count` (a **write barrier** on `putfield`).
- Overwriting or dropping that slot decrements it.
- When the collector reclaims a managed object it walks the object's reference
  map and decrements the pin count of every `MANUAL_REF` slot it held.

Pinning is *not* ownership. It does not free the manual object when the count
reaches zero; it only records that managed code can still reach it.

### 4.3 free / dispose

```khudra
free(buffer);
```

`free` (and its synonym `dispose`) is valid only on a **manual** object. At
runtime:

1. If `pin_count > 0`, raise a runtime error naming the managed slots that
   still hold the target. Nothing is freed. The collector walks the managed heap
   to find them -- a pin count alone does not tell the programmer what to clear:

   ```
   cannot free 'Node': it is still held by 'Holder.head';
   clear the reference before releasing the object
   ```

2. Otherwise, take the object off the live manual list and return its chunk to
   its class's arena. A released chunk is zeroed and reused, so a second `free`
   of the same reference is caught by the live list rather than by reading a
   header that no longer means anything:

   ```
   cannot free this object: it was already released
   ```

Freeing a pinned object is a hard error rather than a silent leak because a
dangling manual pointer inside a traced object would corrupt the next GC cycle.
A **deferred-free** toggle (mark the object and release it when the last pin
drops) is listed as a future expansion in `docs/roadmap.md`.

The Phase 2 checker rejects the statically detectable cases: `free` on a
GC-strategy class, `free` on a non-reference expression, and double-`free` of a
local within one block.

## 5. The collector

A **mark-sweep** collector (`src/vm/gc/collector.cpp`):

- **Roots** = the VM value stack, every live frame's receiver and locals, the
  values a native frame is holding, and every live manual object (4.1).
- **Mark** traces precisely through reference maps, following `MANAGED_REF`
  slots only. A `MANUAL_REF` is pinned rather than traced, and a `RAW` slot is
  never a pointer -- so the collector never has to guess.
- **Sweep** walks the intrusive `gc_link` list, reclaims unmarked objects, and
  decrements pins for their `MANUAL_REF` slots before releasing them. Survivors
  are relinked into a fresh list, so nothing is unlinked one at a time.
- Collection is triggered by allocation once the managed heap passes a
  threshold, which then grows with the live set (never below a floor).

### 5.1 Native roots

Materialization holds a partly built object and its allocation arguments in the
VM's own C++ frame, where the interpreter's value stack cannot see them -- and a
`Procedures` block is free to allocate, which can trigger a cycle right there.
Those values are pushed onto a small **native root** array for the duration, so
an object is never collected between being allocated and being returned to its
allocation site.

## 6. Manual arenas

`src/vm/manual/arena.cpp` allocates manual objects from **per-class** arenas.
Because every object of a class is the same size, an arena is a bump pointer
over large blocks plus a free list of released chunks: no size-class
bookkeeping, and a released object is reused immediately.

Blocks come from `khu_manual_alloc` in the C runtime (`utils/alloc.c`), which
tracks live bytes and blocks -- so the stress tests assert zero leaks without an
external allocator, and `ASAN_OPTIONS=detect_leaks=1` agrees. Build with
`-DKHU_SANITIZE=ON` to run the suite under ASan and UBSan.

## 7. Worked example

```khudra
public class Node {
    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setManual();
    public Node next = null;
    public int32 value = 0;
}

public class List {
    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();
    public Node head = null;   // managed slot holding a manual ref -> pinned
}
```

- `List` is collected; `Node` is manual.
- `list.head = node;` runs the write barrier and sets `node.pin_count = 1`.
- `free(node);` while `list.head` still refers to it raises
  `cannot free pinned object 'Node'; still held by 'List.head'`.
- `list.head = null;` decrements the pin to 0, after which `free(node)` succeeds.
- If `list` itself is collected first, the sweep walks `List`'s reference map,
  sees `head` is a `MANUAL_REF`, and decrements `node.pin_count` to 0.
