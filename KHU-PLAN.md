# Khudra — Compiler Implementation Plan

Canonical design reference for the Khudra language and its compiler. New sessions working on this repository should read this document plus the source tree to recover context.

## Vision

Khudra is an Object-Oriented programming language that fundamentally expands on OOP principles:

1. **Full OOP** — classes, fields, methods, constructors, vtable dispatch.
2. **Memory management choice via objects** — garbage collection and/or manual memory management are selected through an object (`MemoryAllocationTypeObject`), per-class by default with a per-allocation-site override.
3. **Procedures** — a named block in a class body that executes immediately when an object instance is materialized (loaded into memory), instead of waiting to be called by another object.

Source file extension: `.khu`.

## Reference example (`example.khu` at project root)

```khudra
bring khu::stdlib;

public class Example{
    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard(); // Garbage-collected

    public int32 x = 0;
    private int64 y = 0;
    public bool yes = false;
    private Array char = null; // Does not mean empty. Means is not instantiated yet.


    private Procedures{
        khu.stdlibLoadObject();
        pointToOtherInstructionOutsideClass();
        executeInstruction();
        instanceObject();
    }


    public Example(){
        this.char = khu.getType();
        this.char = khu.LoadRuntimeType()   ;
    }



    // Public FUNCTIONS
    func math(int x, int y) -> int32{
        private i32 result = khuStdMath.add(x,y);
        return result;
    }

    // PRIVATE METHOD

    method foo(int x, float y, dfloat z) -> int64{
        khuStdMath.convertTo(int64, y);
        khuStdMath.convertTo(int64, z);

        public int64 result1 = khuStdMath.subtract(x,z);
        public int64 result2 = khuStdMath.add(result1, y);

        return result2;

    }


}
```

## Locked design decisions

### Language & syntax

| Aspect | Decision |
|---|---|
| Source files | `.khu`; imports via `bring khu::stdlib;`, namespaces via `::` |
| Classes | `public class Name { ... }`; visibility modifiers on classes, fields, locals, blocks |
| Members | `func` = **public default**, `method` = **private default**; explicit `public`/`private` modifiers override |
| Visibility | JVM-style member access: `private` members/locals are not readable or modifiable from other loaded objects; `public` allows it. Applies to fields **and** locals. Enforced by an access-control pass in the type checker (tracks "current object" vs "other object" context). |
| Constructors | Named after the class, e.g. `public Example() { }`. Optional field-setup hook. |
| Procedures | Named block `[visibility] Procedures(params) { <statements> }`; runs automatically at materialization; receives allocation-site arguments; runs **before** the constructor body; exactly once per instance, before any member is callable; not invocable by name; may call functions outside the class and runtime (`khu.*`). |
| `null` | Means "not instantiated yet", never "empty". |
| Returns | `-> type` declares a value return; **no arrow = void**; `-> void` accepted as an explicit synonym. |
| Entry point | Support both: explicit `func main()` **and** root-class materialization (first top-level class materializes and its Procedures fire). `khudra run` dispatches accordingly. |
| Memory strategy field | `public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();` — read at allocation time; post-construction reassignment of `type` is a type error (strategy is fixed at materialization). |

### Types (final)

| Category | Types | Aliases |
|---|---|---|
| Signed integers | `int8`, `int16`, `int32`, `int64` | `i8`, `i16`, `i32`, `i64` |
| Unsigned integers | `uint8`, `uint16`, `uint32`, `uint64` | `u8`, `u16`, `u32`, `u64` |
| Byte | `byte` = unsigned 8-bit (0..255) | alias of `uint8` |
| Floats | `float` (32-bit), `dfloat` (64-bit) | — |
| Bool | `bool` | — |
| References | `*byte` raw pointer, `Array`, `string`, class references | — |
| Memory | `MemoryAllocationTypeObject` | — |

Type rules:

- All aliases canonicalize in the checker (`i32` → `int32`, `u8` → `uint8`, `byte` → `uint8`).
- **Integer literals default to `int32`** unless context forces another width.
- **No implicit widening/narrowing** — mixed-width arithmetic resolves only via explicit `khuStdMath.convertTo(type, expr)`.
- Overflow defaults to **wrapping**; checked/trap mode is a future toggle.

### The three core features

| Feature | Decision |
|---|---|
| OOP | Full OOP: classes, fields, methods, constructors, vtable dispatch |
| Memory choice | Class-level `MemoryAllocationTypeObject type` field default (setStandard = GC, default) + per-site override as an allocation-site argument: `FrameBuffer(manual)` wins over the class default |
| Procedures | Auto-running init block; receives allocation-site args; runs before constructor body; exactly once per instance; not callable by name |

### Canonical construction sequence

```
Sprite s = Sprite(x, y);
  └─ 1. allocate per strategy  (class `type` field default, or override token)
       2. bind (x, y) → Procedures(params)
       3. execute Procedures block  ← "immediately executes when loaded into memory"
            · may call outside functions, runtime (khu.*), construct other objects (depth-first)
       4. execute constructor body (optional field-setup hook)
       5. register object as live; return reference to allocating site
```

### Memory interop rules

- **manual → managed**: the manual owner pins/roots the managed object; the collector will not reclaim it while pinned.
- **managed → manual**: **reference-counted pin**. Assigning a manual object into a managed slot increments the manual object's pin count (write barrier); overwrite/drop decrements; when the GC reclaims a managed object it walks the object's reference map and decrements manual-slot pins.
- `free`/`dispose` on a **pinned** manual object → runtime error naming the still-holding managed objects. (Deferred-free is a future toggle.)
- Each class descriptor carries a **reference map** (bitmap: managed-ref / manual-ref / raw per slot) — required for precise GC tracing and doubles as pin bookkeeping.

### Implementation choices

| Aspect | Decision |
|---|---|
| Toolchain | C++17 (frontend, codegen, VM core); **C** for the Procedure engine + runtime support (`utils/*.c`); `extern "C"` ABI; no Rust |
| Backend | Custom **stack-based bytecode** (`.kbc`) + interpreter VM; disassembler included |
| Build | CMake (CLion), one build for both languages |
| Testing | Custom lightweight harness (no external deps); golden-file integration tests |

## Architecture

```
 khudra CLI (src/main, C++)     compile | run | check | emit-bc | disasm
            │
 Lexer → Parser → AST → Sema/Typecheck → Bytecode Emitter → .kbc
 (C++)   (C++)   (C++)   (C++)             (C++)
                                            │
                 ┌──────────────────────────┴────────────────────────────┐
                 │  Khudra VM (src/vm, C++)                              │
                 │  interpreter │ GC (mark-sweep) │ manual arenas │ pin   │
                 │  reference maps │ write barriers │ roots              │
                 └──────────────────────────┬────────────────────────────┘
                                            │  extern "C" ABI
                          ┌─────────────────▼─────────────────┐
                          │  Procedure engine (utils/proc_*)  │  ← C
                          │  materialization pipeline + args  │
                          └───────────────────────────────────┘
                          │  C runtime: utils/bool_impl.c, alloc/free
```

## Project layout

```
Khudra-Lang/
  CMakeLists.txt
  KHU-PLAN.md              # this document
  example.khu              # reference syntax example
  docs/                    # spec.md · memory-model.md · procedures.md · bytecode.md · roadmap.md
  src/
    main/khu_main.cpp      # CLI driver
    lib/                   # compiler (C++)
      diag/                # SourceLocation, SourceManager, diagnostics
      lexer/  parser/      # tokens, AST, pretty printer
      sema/                # symbols, scopes, type checker, access control, memory model
      codegen/             # bytecode emitter
      bytecode/            # opcode defs, .kbc serialization, disassembler
    util/                  # C++ utilities (dynamic array, string, hashmap, file IO)
    vm/                    # runtime (C++)
      vm.cpp/.h            # interpreter, value stack, frames
      object.h             # object header, layout
      gc/                  # mark-sweep collector, root scanning, pinning
      manual/              # arena allocators
      refmap.h             # reference maps / field reference kinds
  utils/                   # C runtime (compiled as C)
    bool_impl.c
    proc_engine.c/.h       # Procedure engine, C ABI
    alloc.c/.h             # manual alloc/free primitives
  lib/                     # Khudra standard library (.khu)
  tests/
    unit/                  # C++ frontend tests (custom harness, no deps)
    integration/           # .khu programs + expected stdout (golden files)
  examples/                # sample programs
  build/                   # CMake output
```

## Phases

### Phase 0 — Scaffolding

- CMake project: C++17 toolchain for `src/`, C11 for `utils/`, `extern "C"` boundary in `proc_engine.h`.
- `src/util/` foundations — dynamic array, string builder, string hashmap, file reader, arena allocator for compiler internals.
- CLI skeleton in `src/main/khu_main.cpp` with subcommands: `compile`, `run`, `check`, `emit-bc`, `disasm` (stubs).
- Diagnostics infrastructure in `src/lib/diag/`: `SourceLocation` (file:line:col) threaded through all errors from day one — retrofitting later is painful.
- Minimal test harness + first failing/passing test.

**Acceptance:** `cmake --build build` succeeds; `khudra compile hello.khu` prints a real "not implemented" error with a file:line.

### Phase 1 — Frontend core

- Lexer for the real syntax: `bring`, `public class`, `MemoryAllocationTypeObject type` field, `Procedures`, `func`/`method`, constructors, `->`, visibility modifiers, `i32`/`int32` aliases, `null`, comments, literals.
- Parser → AST: class declarations, fields, methods, procedures, constructors, expressions, statements, allocation sites with strategy tokens.
- Pretty printer (AST → source) for round-trip debugging.
- Diagnostics: multi-error collection, warning severity.

**Acceptance:** `example.khu` lexes, parses, and pretty-prints losslessly.

### Phase 2 — Semantic analysis

- Symbol tables + scoping; name resolution; class hierarchy.
- Type checker: canonicalize aliases (`i32`→`int32`, `u8`→`uint8`, `byte`→`uint8`); width-aware arithmetic with no implicit widening/narrowing; `null` = uninstantiated; literal default width int32; method signatures; `khuStdMath.convertTo` conversions.
- **Access-control pass**: public/private read/modify enforcement across objects (applies to fields and locals).
- **Memory-model checker**: `type` field must be a valid `MemoryAllocationTypeObject` initializer; allocation-site token valid; pin rules for cross-strategy references; `free`/`dispose` targets only non-pinned manual objects; post-construction reassignment of `type` rejected.
- **Procedures validation**: params match allocation args; no return value; not invocable by name; may call outside functions; references only initialized state.

**Acceptance:** accepts valid programs; rejects calling a Procedure by name, freeing a pinned manual object, and external read/write of private members.

### Phase 3 — Bytecode & VM core

- Stack-based bytecode. Instruction groups: stack ops, arithmetic/comparison (width-specific or width operand), control flow, memory (`alloc`/`manual-alloc`/`free`/`pin`/`unpin`), fields, calls, `materialize`, `convert`.
- `.kbc` format: magic + version, class table, vtable table, const/string pool (typed integer constants), method + procedure bytecode blobs.
- Codegen: AST → bytecode.
- VM core: value stack, call frames, interpreter loop.
- Disassembler (`khudra disasm`).

**Acceptance:** methods, calls, control flow execute end-to-end. `docs/bytecode.md` written.

### Phase 4 — Object model & dispatch

The phase where actual runtime objects exist in memory. Details below in the Object Model section.

**Acceptance:** inheritance + virtual dispatch + fields work end-to-end; golden tests cover a small class hierarchy.

### Phase 5 — Memory systems

- Mark-sweep GC (`src/vm/gc/`): roots = VM stack + globals + pins; precise tracing via reference maps; allocation triggers collection.
- Manual arenas (`src/vm/manual/`): bump/block allocators per class or call-site; explicit `free`/`dispose`.
- Write barriers for pin increment/decrement on managed-slot assignments; pin decrement on collection.
- Pin-count checks at `free` (runtime error naming still-holding managed objects).
- manual→managed rooting (manual owners keep their managed references alive).

**Acceptance:** interleaved GC/manual stress test — zero leaks under ASan/debug allocator, zero premature collection, pin errors raised correctly.

### Phase 6 — Procedure engine (C)

- `utils/proc_engine.c`: the object materialization pipeline — allocate → link object → bind args → **execute the class's Procedures bytecode immediately** → constructor → register object as live → return reference.
- Nested allocations in a Procedures block run depth-first.
- Exactly-once, pre-member-access guarantee.
- Failure semantics if a block aborts mid-run.
- The engine calls into the VM via the `extern "C"` ABI; all other VM logic stays C++.
- Spec: `docs/procedures.md`.

**Acceptance:** `Procedures` runs on GC and manual instances, before the allocation site resumes, receives args, is not callable by name; the engine is C and links cleanly.

### Phase 7 — Standard library & CLI polish

- `lib/`: core types, `string`, `Array`, `io` (print/read), `MemoryAllocationTypeObject` (factories `setStandard()`/`setManual()`), `khu.*` and `khuStdMath` namespaces (matching `example.khu`), full integer-width matrix for `add`/`subtract`/`convertTo`/etc.
- `khudra run` = compile-in-memory + VM execute.
- VM stack traces with Khudra source locations (line/col mapped through bytecode).
- Entry dispatch: `main()` if present, else root-class materialization.

**Acceptance:** a real program (e.g. a linked-list benchmark, one GC and one manual) runs correctly.

### Phase 8 — Hardening & expansion hooks

- Unit + integration golden tests; leak/UB checks (ASan/UBSan builds).
- Expansion hooks (documented in `docs/roadmap.md`):
  - Register-based VM / JIT (swap interpreter loop, keep bytecode + object model)
  - Generics, interfaces, active-object/actor mode (a Procedure that keeps running)
  - More GC algorithms (copying/compacting) behind the `gc/` interface
  - FFI with C libraries through the existing C ABI
  - Deferred-free toggle for pinned manual objects
  - Checked overflow mode
  - REPL, debugger, formatter, LSP (read from existing AST/diag/bytecode layers)

## Object model (Phase 4 detail)

### Object header (`src/vm/object.h`) — first bytes of every object

```
┌──────────────────────────────────────────────┐
│ class_id   (uint32)  → index into class table │
│ flags      (uint32)  bit0=GC  bit1=MANUAL     │
│ pin_count  (uint32)  managed→manual pins      │
│ size       (uint32)  total allocation size    │
│ vtable     (void*)   → method table           │
│ gc_link    (void*)   prev/next for GC lists   │  ← used Phase 5
├──────────────────────────────────────────────┤
│ slot 0   field 0 (width-tagged value)         │
│ slot 1   field 1                              │
│ ...      fields laid out at fixed offsets      │
└──────────────────────────────────────────────┘
```

Decision: `gc_link` is embedded in the header (GC-aware layout from day one) rather than a side table.

### Class descriptor

Loaded from the `.kbc` class table at VM start: name, base class id (single inheritance), object size, field table (offset + type + reference-kind), **reference map** (bitmap: managed-ref / manual-ref / raw per slot), and the vtable (array of method bytecode pointers).

### Dispatch

`invokevirtual obj, method_id`: read `obj->vtable`, index to the method's bytecode, push a new frame. Inheritance: child vtable copies parent, overrides replace entries.

### materialize opcode

Operands: class id + strategy (`GC`/`MANUAL`/`CLASS-DEFAULT`). Sequence: allocate a chunk per strategy → stamp header (class id, flags, pin 0, vtable) → initialize fields to defaults (`null` = uninstantiated) → bind allocation-site args → constructor body (Procedures step stubbed until Phase 6) → return the reference.

### Field ops

`getfield`/`putfield` compute `obj_base + offset`; reference-kind checks use the class's reference map. `putfield` of a manual ref into a managed object becomes a write barrier in Phase 5 (pin inc; overwrite drops pin).

### Tagged values

Object references are one tag in the VM's value union, alongside the 8 integer widths, 2 floats, and `bool`.

## Execution order & risk

Phases are sequential and each has a working end state. Highest-risk items to de-risk early: the memory-model checker rules (Phase 2) and materialization semantics (Phase 6) — write `docs/memory-model.md` and `docs/procedures.md` drafts during Phase 0, before the code that depends on them.
