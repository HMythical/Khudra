# Khudra Bytecode

The compiler emits a stack-based bytecode (`.kbc`) that the VM in `src/vm/`
interprets. This document is the format's specification: `src/lib/bytecode/`
implements it and `khudra disasm` prints it.

## 1. Design

**Stack machine.** Operands are pushed and popped from a value stack; each call
gets a frame with its own local slots and a receiver.

**One type operand instead of ten opcodes.** Khudra has ten numeric types and no
implicit conversion, so every arithmetic and comparison instruction has to be
width-specific. Rather than `add_i8 … add_f64`, arithmetic carries a one-byte
`TypeTag`. The instruction set stays readable and the semantics stay exact.

**Values are tagged.** A stack slot, a constant-pool entry and an object field
all use the same `TypeTag`: the 8 integer widths, 2 floats, `bool`, `string`,
`Array`, `ref`, `ptr`, `MemoryAllocationTypeObject` and `null`.

## 2. Type tags

| Tag | Meaning |
|---|---|
| `void` | no value |
| `int8` `int16` `int32` `int64` | signed integers |
| `uint8` `uint16` `uint32` `uint64` | unsigned integers |
| `float` `dfloat` | 32- and 64-bit floating point |
| `bool` | one byte, 0 or 1 |
| `string` | an immutable string reference |
| `Array` | an array reference |
| `ref` | a class instance |
| `ptr` | a raw pointer (`*T`) |
| `MemoryAllocationTypeObject` | a strategy tag (1 = standard, 2 = manual) |
| `null` | "not instantiated yet" |

## 3. Instruction set

Operands are little-endian. Jump offsets are **relative to the end of the
instruction**, so a method's code is position independent.

### Stack

| Instruction | Operands | Effect |
|---|---|---|
| `nop` | -- | -- |
| `pop` | -- | `a` -> |
| `dup` | -- | `a` -> `a a` |
| `dup_x1` | -- | `a b` -> `b a b` |
| `dup_x2` | -- | `a b c` -> `c a b c` |
| `ldc` | u16 index | -> constant |
| `null` / `true` / `false` | -- | -> literal |

`dup_x1` and `dup_x2` exist so an assignment used as an expression can leave its
value behind. `putfield` pops the value and then the receiver, so one copy has
to be tucked under two slots; `arrayset` also consumes an index, so its copy
goes under three.

### Frame slots and fields

| Instruction | Operands | Effect |
|---|---|---|
| `load` | u16 slot | -> local |
| `store` | u16 slot | `a` -> |
| `this` | -- | -> receiver |
| `getfield` | u16 slot | `obj` -> value |
| `putfield` | u16 slot | `obj value` -> |

Field slots are indices into the class layout: inherited fields first, then the
class's own. `FieldEntry::offset` records the packed byte offset a future
compact layout would use.

### Arithmetic and bitwise

`add` `sub` `mul` `div` `rem` `neg` `and` `or` `xor` `not` `shl` `shr`

Each takes one `TypeTag` operand. Binary forms pop two operands **of that same
type** and push one; `neg` and `not` are unary. Integer overflow **wraps**.
Integer `div`/`rem` by zero traps. `shr` is arithmetic for signed types and
logical for unsigned ones; the shift count is taken modulo the width.

### Comparison

`cmpeq` `cmpne` `cmplt` `cmple` `cmpgt` `cmpge` take a `TypeTag` and push a
`bool`. `refeq` / `refne` compare references by identity (strings by contents),
and `lnot` negates a `bool`.

### Conversion

`convert <from> <to>` is the only instruction that changes a value's width. It
is emitted for `khuStdMath.convertTo(<type>, expr)` and nothing else. Integer
results wrap; float-to-integer truncates toward zero.

### Control flow

| Instruction | Operands |
|---|---|
| `jmp` | i32 relative offset |
| `jmpf` | i32 -- jumps when the popped value is false |
| `jmpt` | i32 -- jumps when the popped value is true |
| `ret` | -- returns void |
| `retval` | -- pops and returns a value |

`&&` and `||` compile to `dup` + a conditional jump, so the right operand is
never evaluated when the left already decides the answer.

### Calls

| Instruction | Operands |
|---|---|
| `call` | u16 method index -- non-virtual |
| `invokevirtual` | u16 vtable slot, u8 argument count, u8 leaves-a-value |
| `callnative` | u16 native id, u8 argument count |

The receiver is pushed first, then the arguments. `call` takes its argument
count from the callee's method entry; `invokevirtual` carries one, because the
receiver sits *underneath* the arguments and the slot alone does not say how
many to skip past to reach it.

`call` is used where dispatch cannot vary -- a `private` member or a
constructor -- and `invokevirtual` everywhere else: it reads the receiver's
`class_id`, indexes that class's vtable, and pushes a frame. A subclass's vtable
is a copy of its base's with overridden slots replaced, so the receiver's own
table is the whole of dynamic dispatch.

`invokevirtual`'s third operand says whether the call leaves a value behind. The
interpreter does not need it -- it resolves the method and then knows -- but the
native emitter walks a method with an abstract operand stack *before* it writes
any code, and a slot number on its own does not answer the question: slot 2 is a
getter in one inheritance chain and a void method in an unrelated one. The front
end knows at the call site, so it records the answer rather than leaving the
backend to infer it from the image. The host still re-checks it against the
method it resolved.

Native ids are listed in `src/lib/bytecode/native.h` and are shared by the front
end, both backends and the C emitter, so an image written by one loads in the
other. The same table records how many values each native leaves behind, which
is what the emitter's abstract stack reads.

### Memory

| Instruction | Operands | Meaning |
|---|---|---|
| `materialize` | u16 class id, u8 strategy | the full pipeline (docs/procedures.md) |
| `alloc` | u16 class id | raw collected allocation |
| `manualalloc` | u16 class id | raw manual allocation |
| `free` | -- | release a manual object |
| `pin` / `unpin` | -- | adjust a manual object's pin count |
| `arraynew` | type | `n` -> a fresh array of `n` elements |
| `arraylen` | -- | `array` -> `int32` |
| `arrayget` | -- | `array index` -> element |
| `arrayset` | -- | `array index value` -> |

An array is a header followed by its elements and nothing else, so its length is
read back out of `size` rather than stored again. It carries **no class
descriptor**: `Array<T>` is erased, so an element is whatever tagged value was
put there, and the collector traces an array by reading those tags instead of
consulting a reference map. `arraynew`'s type operand is not part of the array
-- it only decides what a fresh one is filled with, which is `0` for a number,
`false` for a `bool` and `null` for a reference.

`arrayget` and `arrayset` trap on a null array and on an index outside it;
`arrayset` applies the same pin barrier `putfield` applies, so a manual object
stored in an array slot is pinned while the slot holds it. `arraynew` is an
allocation, so it is a safepoint.

`materialize`'s strategy byte is 1 (`standard`) or 2 (`manual`), already
resolved by the checker from the class default and the allocation-site override.
`alloc` / `manualalloc` are the pieces the Procedure engine drives; codegen
emits `materialize`.

### Termination

`halt` stops the interpreter.

### Raw code

| Instruction | Operands |
|---|---|
| `inlinec` | u16 constant index |
| `inlineasm` | u16 constant index |

`inlinec` splices the string constant it names into the C translation unit the
native backend emits, at this point in the method (`docs/native.md`,
section 4). It counts as an instruction boundary like any other, and the
constant must be a string -- the verifier enforces both. The bytecode VM
refuses to execute an image that contains one: raw C has no interpreter
representation, so the opcode's only semantics are "the native backend runs
this text" (see `docs/spec.md`, section 5.1).

`inlineasm` is the sibling: the named string becomes the guts of a
`__asm__ volatile(...)` statement in the emitted translation unit instead of
straight-line C. Same operand, same boundary, same verifier rule, same VM
refusal (`docs/spec.md`, section 5.2). Both set the module's inline flag.

## 3.1 Object layout

An object is an `ObjectHeader` followed by one tagged `Value` per field slot:

```
class_id   uint32   index into the class table
flags      uint32   bit0 = GC, bit1 = MANUAL, bit2 = materialized
pin_count  uint32   managed -> manual pins
size       uint32   total allocation size
vtable     void*    the runtime class descriptor
gc_link    Object*  intrusive collector list
--------------------------------------------
slot 0 ... one Value per field
```

`gc_link` lives in the header rather than a side table, so the layout is
GC-aware from day one and tracing never needs a second lookup. Keeping fields as
tagged `Value`s means the collector can decide how to treat a slot without one
either.

`getfield` / `putfield` index slots directly. Slot numbers run inherited fields
first, then the class's own, so a base reference reads the same slot in a
derived object.

## 4. The `.kbc` container

All integers are little-endian.

```
magic        "KHUB"                      4 bytes
version      major u16, minor u16
source_file  u32   constant index of the source path
main_method  i32   -1 when there is none
root_class   i32   -1 when there is none
flags        u32   module-wide flags (1.3+; absent before that)

constants    u32 count, then per entry:
               tag u8
               String  -> u32 length + bytes
               float   -> f64 bit pattern
               other   -> u64 payload

classes      u32 count, then per class:
               name u32, base i32, flags u32, object_size u32
               constructor i32, procedures i32, field_init i32
               materialize_argc u8
               fields u32 count, then per field:
                 name u32, type u8, offset u32, class_ref u32,
                 ref_kind u8, is_public u8
               vtable u32 count, then u32 method index per slot

methods      u32 count, then per method:
               name u32, owner_class u32, flags u32
               param_count u8, frame_size u16, return_type u8, native_id u32
               code   u32 length + bytes
               lines  u32 count, then (offset u32, line u32, column u32)
               locals u32 count, then u32 constant index per frame slot
                      (1.3+; absent before that, and only meaningful when
                      the method carries the inline flag)
```

`field_init` names a synthetic method holding the class's field initializers;
it runs during object linking, base class first. `materialize_argc` is how many
allocation-site arguments the class binds -- one count, because the Procedures
block and the constructor receive the same ones.

`ref_kind` is 0 raw, 1 managed-ref, 2 manual-ref -- the reference map the
collector traces through and the pin bookkeeping uses
(`docs/memory-model.md`). `flags` on a class is bit0 = GC, bit1 = manual.

The **module flags** word is bit0 = some method contains `inlinec` blocks. A
method's own flags carry bit5 = `kMethodHasInline` for the same image, and its
**locals table** then names one frame slot per entry -- parameters first, then
locals in declaration order -- by constant index, `0xffffffffu` when a slot's
name is not an identifier the native emitter may use. Version 1.3 introduced
the flags word and the table; a 1.2 image predates both and reads them as
absent, which the deserializer keys off the minor version.

The **line table** maps a code offset back to a source position. The VM uses it
for runtime error messages and stack traces:

```
example.khu:12:24: runtime error: division by zero
    at Counter.average (example.khu:12:24)
    at Counter.main (example.khu:31:9)
```

A version mismatch in the major number is refused rather than misread, and a
truncated image is reported as truncated.

## 5. Entry dispatch

Both entry forms from KHU-PLAN.md are recorded in the image:

- `main_method >= 0` -- an explicit `func main()`. Khudra has no static
  context, so `main` is an ordinary member: the VM materializes the class that
  declares it and then calls `main` on that instance. The checker requires that
  class to materialize with no arguments.
- otherwise `root_class >= 0` -- the first top-level class is materialized and
  its `Procedures` block fires. Nothing is called afterwards.

## 6. Reading a listing

```
$ khudra disasm examples/control_flow.khu

method #1  ControlFlow.fizz  params=1  frame=3  returns=int32
    0  ldc             4    ; 1
    3  store           1
   ...
   24  rem             int32
   26  cmpeq           int32
   28  jmpf            43    ; -> 76
```

`khudra disasm` accepts either a `.khu` source (compiled in memory) or a `.kbc`
image, and the two produce identical listings.
