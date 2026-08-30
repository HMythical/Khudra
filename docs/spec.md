# Khudra Language Specification

Status: **draft**, grown phase by phase alongside the implementation.
`KHU-PLAN.md` holds the locked design decisions; this document records the
concrete surface syntax the compiler accepts.

Source files use the `.khu` extension.

## 1. Lexical structure

### 1.1 Comments

```khudra
// line comment
/* block comment (does not nest) */
```

### 1.2 Reserved words

```
bring      class      public     private    Procedures
func       method     return     this       null
true       false      if         else       while
free       dispose    manual     standard   extends
void
```

Type names are also reserved: `int8 int16 int32 int64 i8 i16 i32 i64
uint8 uint16 uint32 uint64 u8 u16 u32 u64 byte int float dfloat bool string
Array MemoryAllocationTypeObject`.

### 1.3 Literals

| Kind | Examples |
|---|---|
| Integer | `0`, `42`, `0xff`, `0b1010`, `1_000` |
| Float | `1.5`, `2.0e-3` |
| Bool | `true`, `false` |
| String | `"hello\n"` |
| Null | `null` |

Integer literals default to `int32`.

### 1.4 Operators and punctuation

```
::  ->  .   ,   ;   (  )  {  }  [  ]
=   ==  !=  <   <=  >  >=
+   -   *   /   %
&&  ||  !
&   |   ^   ~   <<  >>
```

## 2. Types

| Category | Types | Aliases |
|---|---|---|
| Signed integers | `int8` `int16` `int32` `int64` | `i8` `i16` `i32` `i64` |
| Unsigned integers | `uint8` `uint16` `uint32` `uint64` | `u8` `u16` `u32` `u64` |
| Byte | `byte` | alias of `uint8` |
| Convenience | `int` | alias of `int32` |
| Floats | `float` (32-bit), `dfloat` (64-bit) | -- |
| Bool | `bool` | -- |
| References | `*byte`, `Array<T>`, `Array`, `string`, class references | -- |
| Memory | `MemoryAllocationTypeObject` | -- |
| Unit | `void` | -- |

`int` is an alias for `int32`; it appears in `example.khu` and canonicalizes the
same way the other aliases do.

Rules:

- All aliases canonicalize in the checker.
- Integer literals default to `int32` unless context forces another width.
- Float literals default to `dfloat` unless context asks for `float`.
- **No implicit widening or narrowing.** Mixed-width arithmetic is only legal
  through `khuStdMath.convertTo(<type>, <expr>)`.
- Overflow wraps. A checked/trapping mode is a future toggle.
- `null` means "not instantiated yet", never "empty".
- A field or local declared without an initializer holds its type's default:
  `0` for numbers, `false` for `bool`, `null` for references.

## 3. Compilation units

```khudra
bring khu::stdlib;

public class Example { ... }
```

A unit is a sequence of `bring` imports followed by class declarations.
Namespaces are written with `::`.

## 4. Classes

```
[visibility] class Name [<T, ...>] [extends Base] { member* }
```

Single inheritance. Members:

| Member | Form |
|---|---|
| Field | `[visibility] Type name [= expr];` |
| Constructor | `[visibility] Name(params) { ... }` |
| Procedures | `[visibility] Procedures [(params)] { ... }` -- see `docs/procedures.md` |
| Function | `[visibility] func name(params) [-> Type] { ... }` -- **public** by default |
| Method | `[visibility] method name(params) [-> Type] { ... }` -- **private** by default |

`func` and `method` differ only in their default visibility; an explicit
`public`/`private` modifier overrides it.

### 4.1 Returns

`-> Type` declares a value return. **No arrow means void**; `-> void` is an
accepted explicit synonym.

### 4.2 Visibility

Member access is **object-level**, not class-level. `private` members and locals
are neither readable nor modifiable from another *loaded object*; `public`
allows both. This applies to fields *and* to locals -- locals may carry a
visibility modifier:

```khudra
private i32 result = khuStdMath.add(x, y);
```

Enforcement is an access-control pass in the type checker that tracks whether
the accessing code is inside the owning object ("current object") or outside it.
Because the rule is per object rather than per class, it is stricter than Java:

```khudra
public class Pair {
    private int32 hidden = 0;
    func copyFrom(Pair other) {
        this.hidden = other.hidden;   // error: `other` is a different object
    }
}
```

Only `this` is the current object. Locals live in the current frame, so every
access to one is a current-object access and always permitted.

### 4.3 Type parameters

A class may declare type parameters, which are types inside its body and nowhere
else:

```khudra
public class Box<T> {
    private T held;
    public func put(T value) { this.held = value; }
    public func get() -> T { return this.held; }
}

Box<int32> numbers = Box<int32>();
```

The rules:

- **Arguments are always written out**, at every use, including the allocation
  site: `Box<int32>()`, never `Box()`. Nothing is inferred.
- **Arguments are invariant.** A `Box<int32>` is not a `Box<string>`, and neither
  is a subtype of the other.
- A type parameter shadows a class of the same name inside the body that
  declares it.
- **Generics and inheritance are not combined yet**: a class with type
  parameters may not `extends` another (`docs/roadmap.md`).
- A generic class cannot be the entry point. Entry dispatch materializes a class
  with nothing written at the site, so there is nowhere for its arguments to
  come from.

Like `Array<T>`, generic classes are **erased**: one class is compiled whatever
the arguments were, and a `T` slot holds whatever tagged value an instantiation
put there. Two consequences follow from that and are worth stating:

- A `T` field starts as `null` -- not instantiated yet -- whatever `T` turns out
  to be. A container must therefore never hand back a slot it has not written;
  the standard library's containers trap on an out-of-range read rather than
  returning a default that would be the wrong type.
- A `T` slot is traced and pinned by the tag of the value in it, so a manual
  object stored in one is pinned exactly as it would be in a declared
  `manual`-class field (`docs/memory-model.md`).

### 4.4 Inheritance

`extends` gives single inheritance. A derived class's layout puts the base's
fields first; a field may not shadow an inherited one. The vtable copies the
base's entries and an override with the same name and parameter types replaces
its slot. An override may not change the return type.

## 5. Statements

```khudra
[visibility] Type name = expr;     // local declaration
expr;                              // expression statement
return;  return expr;              // return
if (cond) { ... } else { ... }
while (cond) { ... }
free(expr);   dispose(expr);       // manual deallocation
{ ... }                            // block
```

## 6. Expressions

Precedence, loosest to tightest:

```
assignment      =
logical or      ||
logical and     &&
bitwise or      |
bitwise xor     ^
bitwise and     &
equality        ==  !=
relational      <  <=  >  >=
shift           <<  >>
additive        +  -
multiplicative  *  /  %
unary           -  !  ~
postfix         .name   (args)   [index]
primary         literal  identifier  this  ( expr )  Type
```

A **type may appear as an argument** where an intrinsic expects one:

```khudra
khuStdMath.convertTo(int64, y);
```

## 6.1 Arrays and element types

`Array<T>` is a fixed-length, garbage-collected block of `T`. It is written with
its element type, created through the one intrinsic that can name one, and
indexed with `[]`:

```khudra
Array<int32> counts = khuStdCollection.arrayCreate(int32, 4);
counts[0] = 5;
io.printLine(counts[0]);
io.printLine(khuStdCollection.arraySize(counts));
```

A fresh array holds its element type's default: `0` for a number, `false` for a
`bool`, `null` for a reference. The element type may itself be an array
(`Array<Array<int32>>`) or a class (`Array<Point>`). An assignment to an element
is an expression whose value is what was stored, like any other assignment.

**Indexing is defined for** `Array<T>` (yielding `T`, and assignable), for
`string` (yielding `uint8`, and *not* assignable -- strings are immutable), and
for `*T` (yielding `T`, and assignable), where the index is scaled by the width
of `T`.

Indexing a `*T` is **not** bounds-checked. A `*T` is an address and carries no
length, which is the whole difference between it and an `Array<T>` -- and the
reason `Array<T>` is what a program should reach for first. A null pointer still
traps. `*byte` converts to and from any other pointer type, the way `void*` does
in C; no other pair of pointer types converts.

**An index out of range is a fatal runtime error**, not a sentinel -- the
opposite of `khuStdString.charAt`, and deliberately so: a string index reaches
into data, where being out of range is an ordinary answer, while an array index
reaches into storage the program itself sized, where it is a bug.

### Erasure

`Array<T>` is **erased**: the checker knows the element type and the runtime
does not. An array is a block of tagged values at run time, with no descriptor
recording what it was declared to hold. The consequences are worth stating
plainly:

- A bare `Array` -- the untyped form that predates generics, and what
  `khu.getType()` returns -- is the same runtime thing. It converts to and from
  any `Array<T>` freely, and it cannot be indexed, because indexing needs an
  element type and it has not got one.
- Arrays are always garbage collected. There is no manual array: the manual side
  allocates fixed-size chunks from per-class arenas, and an array's size is not
  fixed at compile time.
- A manual object stored in an array slot is **pinned** for as long as the slot
  holds it, exactly as a field pins one (`docs/memory-model.md`).

## 7. Allocation sites

An allocation site is a call whose callee resolves to a class name:

```khudra
Sprite s      = Sprite(x, y);        // class-default strategy
FrameBuffer f = FrameBuffer(manual); // manual override
Sprite t      = Sprite(standard, x, y);
```

The optional leading `manual` / `standard` token selects the memory strategy and
is consumed by the allocator; the remaining arguments are bound to the class's
**materialization signature**:

- the `Procedures` block's parameters, when it declares any;
- otherwise the constructor's parameters;
- otherwise none.

Both the `Procedures` block and the constructor receive the same allocation-site
arguments, so if both declare parameters they must be identical. See
`docs/memory-model.md` and `docs/procedures.md`.

## 8. Entry point

Both forms are supported and `khudra run` dispatches accordingly:

1. an explicit `func main()`, or
2. **root-class materialization** -- when no `main` exists, the first top-level
   class is materialized and its `Procedures` block fires.

Khudra has no static context, so `main` is an ordinary member: running it
materializes the class that declares it -- field initializers, `Procedures`,
constructor -- and then calls `main` on that instance. That class must therefore
materialize with **no** allocation arguments, which the checker enforces.

Root-class materialization is the other form: the first top-level class is
materialized and its `Procedures` block fires, with nothing called afterwards.

## 9. Namespaces and native declarations

```
[visibility] namespace Name { native-member* }
```

A namespace is a container of static members that is never materialized. Its
members are reached as `Name.member(...)`, and every one of them must be
`native`: a namespace has no receiver, so its implementations come from the
toolchain.

```khudra
public namespace khuStdMath {
    native func add(int32 a, int32 b) -> int32;
}
```

A `native` member is a declaration, not a definition -- no body, terminated by
`;`. Each one is bound either to a **bytecode instruction** (an intrinsic) or to
a **runtime call**; the binding table is in `src/lib/sema/builtins.cpp`, and an
unbound declaration is a compile error. `native` is only legal on a namespace
member.

### 9.1 The standard library

`lib/*.khu` is the standard library, written in Khudra and embedded in the
`khudra` binary at build time. Because the signatures are real declarations,
overload resolution, arity checking and error messages work there exactly as
they do for user code.

| Namespace | Members |
|---|---|
| `khuStdMath` | arithmetic, utilities, transcendentals, constants -- see below |
| `khuStdConv` | number/text/bool conversion: `toString`, the per-width `parse*`, `isNumeric`, the byte utilities, `boolToInt` / `intToBool` |
| `khuStdString` | the whole string surface: size, access, building, searching, comparing, deriving |
| `khuStdCollection` | `arrayCreate` (an intrinsic), `arraySize`, `hash`, `sameValue` |
| `khuStdMem` | manual buffers: `alloc` / `realloc` / `release`, the byte operations, and the leak counters |
| `khuStdRandom` | a deterministic PRNG |
| `khuStdTime` | the clock |
| `khuErrors` | the error-code table, and `fail` |
| `io` | standard output and standard input: `print` / `printLine`, `describe`, `writeString` / `writeBytes` / `flush`, `readLine` and the typed reads |
| `khuStdErr` | standard error (fd 2): the same output surface, other stream |
| `khu` | `stdlibLoadObject()`, `getType() -> Array`, `LoadRuntimeType() -> Array` |

`khuStdMath` is a matrix rather than a set of generic signatures, because
nothing widens on its own: an operation is declared for each width it means
something for, and a mixed-width call has no match by construction.

| Group | Members | Widths |
|---|---|---|
| Arithmetic (intrinsics) | `add` `subtract` `multiply` `divide` | every numeric width |
| | `remainder` | every integer width |
| | `neg` | signed integers, floats |
| Utilities | `min` `max` `clamp` `pow` | every numeric width |
| | `abs` `signum` | signed integers, floats |
| | `gcd` `lcm` | every integer width |
| Transcendentals | `sqrt` `floor` `ceil` `round` `exp` `log` `log10` `sin` `cos` `tan` `asin` `acos` `atan` `sinh` `cosh` `tanh` `fmod` `atan2` | `float`, `dfloat` |
| Constants | `pi()` `e()` | `dfloat` |
| Conversion | `convertTo(<type>, <expr>)` | intrinsic; see below |

Rules the whole group follows:

- **Overflow wraps**, here as everywhere. `abs` of the most negative value of a
  width wraps back onto itself, and integer `pow` wraps like repeated
  multiplication.
- **Float results round to their width.** A transcendental is computed at 64
  bits and then rounded back to the operand's, so a `float` answer carries
  32-bit precision the way a `float` sum does.
- **Domain errors are values, not traps.** `sqrt(-1.0)` is a NaN and `log(0.0)`
  is `-inf`; both print as such. The exceptions are the two that name no value
  at all: a `clamp` whose low bound is above its high bound, and `pow(0, n)`
  for a negative `n`, which is a division by zero.
- **Constants are functions** because a namespace has no `const` members yet
  (`docs/roadmap.md`). They become members when that lands; the values do not.

Numbers render through one shared formatter (`src/vm/format.h`), so a value
prints the same bytes under the VM, `run --native` and a built binary. Both
float widths print with six significant digits.

`khuStdConv` is the other side of that formatter. `toString` writes exactly what
`io.print` would have; the `parse*` functions read the grammar it writes:

```
integer  ::= [+-]? digit+
float    ::= [+-]? ( digit+ ( '.' digit* )? | '.' digit+ ) ( [eE] [+-]? digit+ )?
```

The whole string has to match -- no skipped whitespace, no trailing characters,
base 10 only -- and a value outside the requested width is a failure rather than
a wrap. A parse is named for the width it produces (`parseInt32`, `parseUInt8`,
`parseFloat`, `parseDFloat`) because every one of them takes a `string`, leaving
overload resolution nothing to choose on.

**A failed parse answers with a sentinel**, documented per function in
`lib/conv.khu`: the width's most negative value for a signed type, its largest
value for an unsigned one, a NaN for a float. Each of those is also a value a
successful parse can produce, so `isNumeric` is what tells them apart. This is
the shape until `khuErrors.Result<T>` arrives with generics (`docs/roadmap.md`);
the signatures change then, the names and argument types do not.

A `string` is a byte string, so `isDigit`, `isLetter`, `isWhitespace`, `toUpper`
and `toLower` are defined over ASCII bytes: a byte above `0x7f` is none of those
things and neither case function touches it. `toChar` is the one operation that
goes the other way, from a byte to a one-byte `string`.

`khuStdString` is the only way to operate on a string: `+` is arithmetic and
does not concatenate, and there is no string literal syntax for anything but a
whole string. Strings are immutable, so every operation that produces one
produces a fresh one, and `length`, `charAt` and `substring` all count in
**bytes** -- a multi-byte UTF-8 code point occupies several positions.

Being handed an index out of range is not the same kind of mistake as
dereferencing null, so it is not treated the same way: `charAt` answers 0,
`substring` clamps both ends and reads a reversed range as empty, and `indexOf`
and `lastIndexOf` answer -1. Those are sentinels of the same kind, and with the
same expiry, as the parse functions'. Passing `null` where a `string` is
expected is the other thing, and it traps.

`concat` is declared at several arities because Khudra has no varargs, and
`split` is absent because it returns an `Array<string>`; both wait on
`docs/roadmap.md`.

`lib/core.khu` documents the built-in types, which are part of the language
rather than declarations.

Two operations cannot be declared in Khudra at all, because their first argument
is a *type* and there is no way to write a type as a parameter. Both are
compiler intrinsics:

- `khuStdMath.convertTo(<type>, <expr>)` lowers to the `convert` instruction and
  is the only sanctioned way to move a value between widths.
- `khuStdCollection.arrayCreate(<type>, <count>)` lowers to `arraynew` and is
  the only way to create an array. The type decides what a fresh array is filled
  with, which is exactly the thing a runtime argument could not carry.

### 9.2 Recoverable errors

Khudra has no exceptions and is not getting any. A runtime failure that means the
*program* is wrong -- dividing by zero, dereferencing null, indexing past the end
of an array -- is a fatal trap. A failure that means the *input* is wrong is an
ordinary answer, carried back as a value.

`lib/errors.khu` provides that shape as ordinary Khudra classes, not as namespace
members: a namespace holds only `native` declarations, and these are written in
Khudra.

| Type | What it says |
|---|---|
| `Result<T>` | a value, or a failure with a code and a message |
| `Option<T>` | a value, or nothing, with no failure attached |
| `Parse` | the conversion surface, reporting failure as a `Result` |

### 9.3 Containers

`lib/collections.khu` holds `List<T>`, `Stack<T>`, `Queue<T>` and `Map<K, V>`,
written in Khudra over `Array<T>`. `Array<T>` is storage; everything with a
growth policy is in the language, over it.

Every one of them **traps** rather than returning a default when asked for
something it has not got -- `get` past the end of a `List`, `pop` on an empty
`Stack`, `get` of a key a `Map` does not hold. Generics are erased, so a `T`
that was never written holds `null` whatever `T` is, and handing that back as an
`int32` would be a lie the checker had already accepted. Where a default is
wanted, the caller supplies it: `getOrElse`, `peekOrElse`.

Iteration is an index loop, because Khudra has neither a `for`-over-collection
nor closures (`docs/roadmap.md`):

```khudra
int32 i = 0;
while (i < items.size()) {
    io.printLine(items.get(i));
    i = khuStdMath.add(i, 1);
}
```

`Map` has no index, so it answers `keys()` with a `List<K>` to walk. Its bucket
order is arbitrary but **deterministic**: `khuStdCollection.hash` gives the same
answer on every run and under every backend, which is what lets a program that
walks a map be byte-identical across all three. A reference hashes to 0 rather
than to its address for exactly that reason, so a `Map` keyed on objects is a
linear scan.

### 9.4 The two streams

`io` is standard output (fd 1) and standard input; `khuStdErr` is standard error
(fd 2). They are separate namespaces because they are separate streams, and
`khuStdErr` is deliberately not `khuErrors`:

| Name | What it is |
|---|---|
| `khuStdErr` | an output stream -- where a program writes what went wrong |
| `khuErrors` | error values -- how a program hands a failure back to itself |

The output surfaces are identical: the same per-type matrix, the same shared
formatter, the same `writeString` / `writeBytes` / `flush`. A message is not
written differently because of which stream it is on. Both channels are
captured and compared separately by the test suite, so the byte-identical
invariant covers stderr the way it covers stdout.

A class reference has no `print` overload -- there is no way to declare one that
accepts any class, and no `toString` for a class to override yet. `io.describe`
is the answer: it renders a value of any type, an object as `<ClassName>` and an
array as `<array of N>`. Like `khuStdMath.convertTo`, it is recognized by the
compiler rather than declared, because its argument type cannot be written.

The typed reads (`readInt32`, `readDFloat`, `readBool`, ...) are `readLine` plus
`khuStdConv`'s parse, sentinels included. `Parse().toInt32(io.readLine())` is the
`Result`-shaped form.

### 9.5 Buffers

`khuStdMem.alloc` is the one thing that produces a `*byte`. It hands out a block
from the same allocator the `manual` class strategy uses, so a buffer appears in
the same leak accounting a manual object does -- `khuStdMem.liveBytes` and
`liveBlocks` report it, and the test suite asserts they return to zero.

A buffer is **not** collected: what `alloc` hands out, `release` takes back. It
is called `release` rather than `free` because `free` is a statement keyword that
releases a manually allocated *object*; a buffer is not an object, and the two
are named apart so the difference stays visible.

Every block carries its length in a header the program cannot reach, and every
operation in `khuStdMem` that takes a count uses it: a `copy`, `move`, `fill`,
`zero` or `compare` that would run past the end of a buffer is a fatal runtime
error rather than a corrupted heap, and so is a `release` of something the
runtime did not hand out -- which is what a double release looks like from the
inside. Liveness is tracked outside the blocks, so answering that question never
reads memory that has been given back.

Indexing stays unchecked, as described in §6.1. That is the one place the raw
layer is raw, and it is named rather than hidden.

### 9.6 Determinism

Everything in the standard library is reproducible except the clock. The same
program produces the same bytes on every run and under every backend -- which is
what makes a golden test possible at all -- and the pieces that could have
broken that were designed not to:

- `khuStdRandom` starts from a fixed constant when a program does not seed it,
  and from the seed when it does. It never reads the clock.
- `khuStdCollection.hash` answers 0 for a reference rather than hashing its
  address, so a `Map`'s bucket order does not depend on the allocator.
- Numbers render through one formatter, so a value prints the same bytes
  wherever it is printed from.

`khuStdTime` is the exception, and nothing else depends on it. A program that
prints the time cannot have a golden; a program that does not is unaffected.

### 9.7 Standard-library names

The standard library declares real classes, so their names -- `List`, `Stack`,
`Queue`, `Map`, `Result`, `Option`, `Parse` -- live in the same namespace a
program's classes do. **A program's own name wins.** A program that declares its
own `List` gets its own everywhere in its own code; the library keeps resolving
its own names first, so its `Stack`, which is written over its `List`, is
unaffected. What a shadowing program gives up is reaching the library's class of
that name at all.

Namespace names (`io`, `khuStdMath`, `khuErrors`, ...) are not shadowable: there
is no receiver to tell them apart by, so declaring a class with one of those
names is an error.

`khuErrors` is the namespace beside them: the error-code table
(`none`, `bounds`, `parse`, `nullReference`, `io`, `divideByZero`, `notFound`,
`invalidArgument`, `unsupported`, `overflow`, `empty`) plus `fail(string)`, the
one way Khudra source raises a fatal runtime error.

There is no static context, so a `Result` is materialized and then told what it
is, in one expression:

```khudra
Result<int32> good = Result<int32>().withValue(7);
Result<int32> bad  = Result<int32>().withError(khuErrors.parse(), "not a number");
```

`value()` **traps** on a failure. It has to: generics are erased, so a `T` that
was never written holds `null` whatever `T` is, and handing that back as an
`int32` would be a lie the checker had already signed off on. `unwrapOr` is the
total form.

`khuStdConv`'s parse functions keep their sentinels: a namespace member is a
native, and a native has no way to materialize a Khudra class. `Parse` is the
`Result`-shaped surface written over them, using the per-width `canParse*`
checks to tell a sentinel from a real answer. The two live side by side rather
than one replacing the other.

## 10. A note on `example.khu`

`example.khu` at the project root is the **syntax** reference -- it is what
Phase 1 round-trips. It is not a valid program: it calls placeholder functions
that do not exist (`pointToOtherInstructionOutsideClass()`) and it mixes widths
without converting (`khuStdMath.subtract(x, z)` on an `int` and a `dfloat`), so
`khudra check example.khu` reports errors by design. The programs under
`examples/` are the ones that type-check.
