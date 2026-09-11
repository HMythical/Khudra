# Roadmap

Where Khudra goes next, and -- more usefully -- **which seam each direction
attaches to**. The implementation was built so that these are extensions rather
than rewrites; this document records where the hook already is.

Nothing here is scheduled. It is a map of the deliberate gaps.

---

## Backend

### Native execution -- shipped

**Seam:** `src/vm/vm.cpp`'s interpreter loop. It was the right seam.

Khudra now has a second execution path. `khudra run --native` and
`khudra build` compile the `.kbc` image to native code and run it against raw
memory with no dispatch loop, and the interpreter is untouched. See
[`native.md`](native.md) for the design.

What the backend kept, exactly as this section predicted it would:

- the `.kbc` container and its class table, reference maps and vtables,
- `src/vm/object.h` -- the header, field slots and `gc_link`,
- `src/vm/gc/` and `src/vm/manual/`, shared rather than reimplemented
  (`khu_vm_core` was split out of `khu_vm` so a native binary can link the
  memory systems without the interpreter),
- the Procedure engine, reached through `KhuProcHostApi` and still the single
  driver of the materialization pipeline,
- the per-method line table, which is what lets a native trap report the same
  source position the VM reports.

The order in `docs/procedures.md` -- allocate, link, bind, Procedures,
constructor, register -- is preserved because the emitted code calls
`khu_proc_materialize` rather than doing the work itself.

### A direct machine-code emitter

**Seam:** `src/native/cemit/`.

The native backend reaches machine code by transpiling to C and calling the
host compiler. A direct x86-64 or ARM64 emitter would replace that one pass and
keep everything around it: the CLI surface (`--native`, `build`), the host, the
safepoint and scanned-locals scheme, and the differential test suite that
already proves a backend agrees with the interpreter.

The obvious cost it would remove is the host-compiler requirement, and with it
the fallback-to-the-VM path in `khudra run --native`.

### Register-based VM

**Seam:** `Vm::execute`.

Still open, and now less pressing: the reason to want one was speed, and the
native backend is the answer to that. A register VM would still be the better
*interpreter* -- fewer dispatches per operation for the same image -- and
nothing about the native work forecloses it.

---

## Language

### Generics -- done, by erasure

`Array<T>` and `class Name<T, ...>` both exist. The choice recorded here is
**erasure**: one class descriptor per class whatever the arguments were, and a
`T` slot holding whatever tagged value an instantiation put there. It was the
option that fits the runtime rather than the one that fits the checker -- values
are already tagged, so an erased slot is exactly as safe to trace as a declared
one, and monomorphization would have bought reified arguments at the cost of a
class id per instantiation and a second copy of every method.

What erasure left on the table, and what is still open:

- **Generics and inheritance are not combined.** A class with type parameters
  cannot `extends` another, because the vtable a subclass copies no longer
  records the arguments its base was written with. Substituting through an
  inherited signature needs somewhere to keep them.
- **No variance.** Arguments are invariant, because there is nowhere to declare
  anything else.
- **No constraints.** A `T` is any type, so nothing inside a generic class can
  do anything to a `T` but store it and hand it back.
- **A `T` slot starts as `null`**, whatever `T` is. A container must not hand
  back a slot it has not written; the standard library's containers trap
  instead.
- **Reified arguments** -- asking a value what it was instantiated with -- would
  need the descriptor erasure deliberately does not keep, and belongs with
  reflection below.

### `const` members in a namespace

**Seam:** `Checker::declare_namespace_members`, which today requires every
namespace member to be `native`.

Several places in the standard library want a constant and have to ship a
zero-argument function instead: `khuStdMath.pi()`, `khuStdMath.e()`, and the
whole `khuErrors` code table. They read as calls because there is nothing else
to write, and every one of them is a `callnative` at run time for a value that
never changes.

A `const` member would be a constant-pool entry the checker folds at the use
site. The values do not change when it lands; only the parentheses go.

### Closures and iteration

**Seam:** `ast::MethodDecl` and the frame layout.

Two things in the standard library are shaped by their absence:

- **Iteration is an index loop.** `List`, `Stack` and `Queue` are walked with
  `while (i < size())`, and `Map` answers `keys()` with a `List<K>` because it
  has no index to walk. A `for`-over-collection needs somewhere to put the
  iterator's state and a way to name the element.
- **`Result.recover(func)` is not there.** It is the one member of `Result` that
  PLAN.md asked for and that could not be written: it takes a function. The
  manual idiom -- `if (r.ok())` -- is what the documentation offers instead.

### A uniform `toString`

**Seam:** the vtable, and `io`'s per-type overload matrix.

`io.print` is declared once per type, so there is no overload for a class
reference and no way to write one. `io.describe` fills the gap by rendering a
value of any type -- an object as `<ClassName>` -- but that is identity, not
content, because a class has no way to say how it wants to be printed.

Interfaces (below) would give one; so would a special-cased `toString` the
emitter looks for. Either way, `io.describe` becomes the fallback rather than
the only answer.

### `khuStdSystem` -- shipped

Declared in `lib/system.khu`, implemented in `src/vm/natives.cpp` over the
platform shim in `src/vm/platform.{h,cpp}`. Files and streams, the process
(`argc`/`argv`/`getEnv`/`hasEnv`/`envKeys`/`exit`), the file namespace
(`exists`/`isFile`/`rename`/`createDirectory`/... plus `pathSeparator`), and
blocking TCP sockets (`listen`/`accept`/`connect`/`send`/`recv`/`shutdown`).
`examples/http_server.khu` is a real HTTP server over a real socket.

The two things this needed that no other namespace did, and that are now
available to anything else that wants them:

- **A native can produce a Khudra object.** `NativeServices::materialize` and
  `invoke` run the ordinary materialization pipeline from inside a native, with
  `RootScope` keeping the result alive across a collection. That is what lets
  `argv()` answer a real `List<string>`. `listDirectory` and `envKeys`-shaped
  members ride the same capability.
- **Exit unwinds like a trap.** `khuStdSystem.exit(code)` comes off the frames
  the way a fatal error does but sets no error text, and every driver -- the VM,
  `run --native`, and a built binary's `main` -- reports the code. Program
  arguments are threaded from those same three entry points.

Still deferred here: threads and non-blocking I/O, UDP/`select`/TLS, process
spawning and signals, `listDirectory`/`tempFile`/`isTerminal`, the
`System.getProperty` pack (`osName`/`hostName`/`userName`/...), and `const`
members in a namespace -- which is what would turn `errno()` from a call into a
table of constants.

### The Windows native backend -- shipped for MinGW-w64

`khudra run --native` and `khudra build` work on Windows under MinGW-w64 GCC,
guarded by `.github/workflows/ci-windows.yml`. The three OS-tied parts each
became one shim: the toolchain (`CreateProcess`, `;`-separated PATH, `%TEMP%`),
the loader (`LoadLibrary`, a `.dll` rather than a `.so`), and the symbol model
(`--export-all-symbols` on `khudra.exe`, which the emitted DLL imports from).

**MSVC is deferred.** It does not do exe-imports-from-DLL the MinGW way: it
needs a generated `.def`, an import library, and `__declspec(dllexport)` on the
four boundary symbols in `khu_native_abi.h` -- the one place the emission side
would have to change. Sanitizer parity on Windows is deferred with it.

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

### Inline blocks -- shipped (inline_c and inline_asm)

**Seam:** the `inline_c { ... }` / `inline_asm { ... }` statements, lowered by
`src/native/cemit/`.

Raw C text, and raw assembly, with direct access to the enclosing method's
frame through named `KhuValue* const` aliases. They are the source-level cousin
of the FFI: same trust boundary, no symbol table. `inline_c` splices ordinary C
into the emitted translation unit; `inline_asm` wraps its text in a
`__asm__ volatile(...)` statement, so it is volatile side-effect-only by
construction and restricted to pass-by-bytecode effects on the same aliased
locals.

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

The kernel tier (`khuAdvKernel*`) is the first deliberate instance of this seam:
fixed native ids instead of a symbol table, and the same trust boundary made
explicit with the `--allow-kernel` gate, defaulting off because a program that
talks to the kernel is not portable.

Status on that seam:
- `khuAdvKernel` (shared) and `khuAdvKernelLinux` (raw `invoke` / `number`)
  **shipped**; the Linux curated tier, `khuAdvKernelWindows` and
  `khuAdvKernelMac` follow in later phases of PLAN.md.
- Windows gets no raw tier by design (PLAN.md, section 4.2); that is the FFI
  item this whole section is about.

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
