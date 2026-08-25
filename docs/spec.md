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
- **No implicit widening or narrowing.** Mixed-width arithmetic is only legal
  through `khuStdMath.convertTo(<type>, <expr>)`.
- Overflow wraps. A checked/trapping mode is a future toggle.
- `null` means "not instantiated yet", never "empty".

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

JVM-style member access. `private` members and locals are neither readable nor
modifiable from another loaded object; `public` allows both. This applies to
fields *and* to locals -- locals may carry a visibility modifier:

```khudra
private i32 result = khuStdMath.add(x, y);
```

Enforcement is an access-control pass in the type checker that tracks whether
the accessing code is inside the owning object ("current object") or outside it.

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

## 7. Allocation sites

An allocation site is a call whose callee resolves to a class name:

```khudra
Sprite s      = Sprite(x, y);        // class-default strategy
FrameBuffer f = FrameBuffer(manual); // manual override
Sprite t      = Sprite(standard, x, y);
```

The optional leading `manual` / `standard` token selects the memory strategy and
is consumed by the allocator; the remaining arguments are bound to the class's
`Procedures` parameters. See `docs/memory-model.md`.

## 8. Entry point

Both forms are supported and `khudra run` dispatches accordingly:

1. an explicit `func main()`, or
2. **root-class materialization** -- when no `main` exists, the first top-level
   class is materialized and its `Procedures` block fires.
