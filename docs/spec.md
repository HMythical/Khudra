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
| References | `*byte`, `Array`, `string`, class references | -- |
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
[visibility] class Name [extends Base] { member* }
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

### 4.3 Inheritance

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

## 6.1 Element types

Indexing is defined for `*T` (yielding `T`) and for `string` (yielding
`uint8`). `Array` has no element type, so it cannot be indexed yet -- typed
containers arrive with generics (`docs/roadmap.md`).

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

## 9. Built-in namespaces

`khuStdMath`, `io` and `khu` are namespaces, not classes: they are never
materialized and their members are reached as `Namespace.member(...)`.

| Namespace | Members |
|---|---|
| `khuStdMath` | `add` `subtract` `multiply` `divide` `remainder`, declared once per numeric width; `convertTo(<type>, expr)` |
| `io` | `print` / `printLine` over `string`, `bool` and every numeric width; `readLine() -> string` |
| `khu` | the runtime namespace; populated in Phase 7 from `lib/` |

`khuStdMath.convertTo` is an intrinsic: its first argument is a type, and it
lowers to a `convert` instruction rather than a call. It is the only sanctioned
way to move a value between widths.

## 10. A note on `example.khu`

`example.khu` at the project root is the **syntax** reference -- it is what
Phase 1 round-trips. It is not a valid program: it calls placeholder functions
that do not exist (`pointToOtherInstructionOutsideClass()`) and it mixes widths
without converting (`khuStdMath.subtract(x, z)` on an `int` and a `dfloat`), so
`khudra check example.khu` reports errors by design. The programs under
`examples/` are the ones that type-check.
