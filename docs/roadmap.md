# Roadmap

Where Khudra goes next, and -- more usefully -- **which seam each direction
attaches to**. The implementation was built so that these are extensions rather
than rewrites; this document records where the hook already is.

Nothing here is scheduled. It is a map of the deliberate gaps.

---

## Backend

### Register-based VM, or a JIT

**Seam:** `src/vm/vm.cpp`'s interpreter loop.

The bytecode and the object model do not depend on the loop being a stack
machine. A register VM or a JIT would replace `Vm::execute` and keep:

- the `.kbc` container and its class table, reference maps and vtables,
- `src/vm/object.h` -- the header, field slots and `gc_link`,
- `src/vm/gc/` and `src/vm/manual/`,
- the Procedure engine, which reaches the VM only through `KhuProcHostApi`.

The one thing a new backend must preserve is the **order** in
`docs/procedures.md`: allocate, link, bind, Procedures, constructor, register.
That order is the language's promise, and it lives in C precisely so a new
backend inherits it instead of reimplementing it.

Bytecode already carries a per-method line table, so a JIT can keep source
positions for traps without new format work.

---

## Language

### Generics

**Seam:** `sema::Type` and `ast::TypeNode`.

`Array` is the visible gap: it is a reference with no element type, so it can be
held, passed and compared but not indexed. `Checker::check_index` says so in as
many words, and `emit_expr` reports indexing as unimplemented rather than
guessing.

Adding generics means a parameterized `Type` kind, substitution at
instantiation, and either erasure (one class descriptor, unchecked slots) or
monomorphization (one class id per instantiation, which the class table already
supports since ids are just indices).

### Interfaces

**Seam:** `ClassSymbol::vtable` and `RuntimeClass::vtable`.

Single inheritance is wired end to end: a subclass copies its base's table and
replaces overridden slots. Interfaces need a second dispatch path -- an itable
per (class, interface) pair, or a hashed lookup -- because a slot number can no
longer be assigned by a single linear chain. The `.kbc` class table would grow
one more list; nothing else changes.

### Active objects / actor mode

**Seam:** the Procedure engine's step 4.

A Procedures block today runs to completion during materialization, and step 6
publishes the object. An *active* object would be one whose block does not
return -- a loop, or a suspension point -- which means step 4 hands it to a
scheduler instead of calling it inline, and step 6 happens as soon as the block
first yields.

That is a change to `khu_proc_materialize` and one more host callback
(`schedule_procedures`). The rest of the pipeline, including the failure
semantics, stays as written.

### Checked overflow

**Seam:** `Vm::arithmetic` and the arithmetic opcodes.

Overflow wraps today, and `normalize_int` is the single place it happens. A
checked mode is a flag on the module (or a distinct opcode variant) that traps
instead of truncating. The type checker needs no changes: widths are already
explicit at every operation.

---

## Memory

### More collectors

**Seam:** `src/vm/gc/collector.h` and `Heap`.

The collector reaches the heap through `managed_head()`, `destroy_managed()` and
`adopt_managed_list()`, and reaches the program through `RootSource`. A copying
or compacting collector needs one thing the current design does not provide:
**object references must be updatable**, which means roots have to be handed
over as slot addresses rather than by value.

`RootSource::enumerate_roots` would take a visitor that can write back. Fields
already can be: they are `Value`s at known slot indices, reachable through the
reference map. That is the whole of the change on the tracing side.

### Deferred free for pinned manual objects

**Seam:** `Vm::release_manual`.

`free` on a pinned object is a runtime error today, and the message names the
managed slots that still hold it. A deferred mode would instead mark the object
and release it when its pin count reaches zero -- which means the write barrier
in `Vm::write_barrier`, and the pin drop in `Collector::release_pins`, become
the places that check for a pending release.

The header already has room: `ObjectFlags` has spare bits, and `pin_count` is
the counter that would trigger it.

---

## Interoperation

### FFI with C libraries

**Seam:** `utils/proc_engine.h`, and `bytecode::NativeId`.

The extern "C" boundary already exists and already carries objects across it.
An FFI needs:

- a way to *declare* a foreign function -- `native` declarations and their
  binding table (`src/lib/sema/builtins.cpp`) are the shape, extended with a
  library and symbol name instead of a fixed id;
- a calling convention for Khudra values, which `Value` already is;
- a rule for what a foreign function may do with a reference. `*byte` is already
  a raw, never-traced slot, and pinning already exists for handing manual memory
  to something outside the collector.

---

## Tooling

All four of these read from layers that exist and are already exercised:

| Tool | Reads from |
|---|---|
| REPL | `Compiler` (incremental units are the only new part) |
| Debugger | the per-method line tables, plus `Frame` |
| Formatter | `parser::PrettyPrinter` -- already round-trip stable and structure-preserving |
| LSP | `diag::DiagnosticEngine` for errors, `sema::Program` for symbols and types |

The formatter is closest: `PrettyPrinter` already reprints an AST such that
reparsing it yields an equal tree and reprinting is byte-identical. What it does
not do is preserve comments, which is the work a real formatter would add --
the lexer currently discards them as trivia, so they would need to be attached
to tokens.

---

## Hardening already in place

For reference, so a future change knows what it must not break:

- **`khudra_unit_tests`** -- the whole front end, VM, memory systems and the
  Procedure engine, with no external dependencies.
- **Golden-file tests** (`tests/integration/*.khu` + `.expected`) -- a program
  is run and its stdout compared byte for byte.
- **Expected-failure tests** (`tests/integration/errors/`) -- a program is
  rejected, and the diagnostics must say the right thing. Error messages are
  part of the language's surface.
- **Sanitizers** -- `cmake -DKHU_SANITIZE=ON` builds the whole tree under
  AddressSanitizer and UndefinedBehaviorSanitizer; the suite runs clean under
  both, with `detect_leaks=1`.
- **Leak accounting** -- `utils/alloc.c` counts live manual bytes, so the
  memory tests assert zero leaks without depending on an external allocator.
- **The bytecode verifier** (`src/lib/bytecode/verifier.cpp`) -- every image is
  checked before it runs: opcodes decode, indices exist, jumps land on
  instruction boundaries. The interpreter is written assuming this, so the
  verifier is what makes that assumption safe.
