# The native backend

Khudra runs a program in one of two ways. The bytecode VM in `src/vm/` walks the
image with a dispatch loop — **virtual machine state**. The native backend in
`src/native/` compiles the same image to machine code that executes directly
against OS memory — **raw memory**, no interpreter.

```
.khu -> lexer -> parser -> sema -> bytecode emitter -> .kbc image
                                                         |
                          +------------------------------+------------------+
                          |                              |                  |
                   VM interpreter                 native backend      native binary
                   khudra run                     khudra run --native khudra build
```

Everything up to and including the image is shared. The front end does not know
which backend will run its output, and the `.kbc` format did not change to make
this work.

---

## 1. Using it

```
khudra run --native <file.khu|file.kbc>          compile to C, load, run in process
khudra build <file.khu|file.kbc> [-o <path>]     write a standalone executable
khudra build <file> --keep-c                     keep the intermediate C beside it
```

Both need a **host C compiler**, and `build` also needs a C++ compiler for the
link. The backend looks at `$KHUDRA_CC` and `$KHUDRA_CXX` first, then at `cc`,
`clang`, `gcc` and `c++`, `clang++`, `g++` on `PATH`.

- `run --native` on a machine with no compiler prints a notice and **falls back
  to the VM**. The program still runs; the toolchain stays usable.
- `build` has nothing to fall back to — a native binary is the whole request —
  so it reports the missing compiler and exits non-zero.

A built binary is a program in its own right. It carries the lowered methods,
the memory systems and its own `.kbc` image; it does not need `khudra` on the
machine that runs it, and it contains no interpreter (`ctest` asserts this in
`native_binary_has_no_interpreter`).

The archives an ahead-of-time link needs live in the build tree. Set
`KHUDRA_NATIVE_LIB_DIR` to point somewhere else, and `KHUDRA_NATIVE_FLAGS` to
add flags both the compile and the link need — that is how a sanitized build
keeps its binaries consistent with its archives.

---

## 2. Why C

The backend transpiles to C and hands the C to the host system compiler. That
buys register allocation, instruction selection and every platform the host
compiler supports, for none of the work, and it keeps the lowering readable:
`khudra build --keep-c` leaves a file you can read to see exactly what a Khudra
method became.

The C is not a detail of one mode. It is the single lowering, and the two
wrappers differ only in what they do with it:

| | `run --native` | `build` |
|---|---|---|
| Compiles to | a shared object | an object file |
| Then | `dlopen`, call the method table | links with the host into an executable |
| The image | already in memory | embedded in the translation unit |
| Runs | in the `khudra` process | on its own |

A direct machine-code emitter is future work (section 7). It would
replace the transpile step and keep this CLI, this host and this test suite.

---

## 3. What the emitter produces

`src/native/cemit/` turns a `bytecode::Module` into one C11 translation unit.

**The dispatcher is gone.** Every non-native method — including constructors,
Procedures blocks and synthesized field initializers — becomes one C function:

```c
static int khu_m3(KhuValue self, const KhuValue* argv, KhuValue* out);
```

**The operand stack is gone too.** The emitter walks each method with an
abstract stack before it writes anything, so the height at every instruction is
known at compile time. A frame is then a single array — `frame_size` locals
first, then the operand stack — and `ldc 4` is an assignment to a fixed element:

```c
    V[1] = khu_rt_constants[4];
    V[2] = khu_rt_constants[5];
    F.ip = 8u; F.height = 2u;
    if (khu_val_arith(KHU_A_ADD, 3u, &V[1], &V[2], &V[1])) KHU_TRAP(&F, "division by zero");
```

**Control flow is C control flow.** A jump becomes a `goto` to a label on the
target offset — which the bytecode verifier already guarantees is an
instruction boundary, the same guarantee the interpreter relies on.

**Pure operations are inline.** Arithmetic, comparison and conversion lower to
the `static inline` helpers in `khu_native_abi.h`, which carry the
interpreter's exact wrapping, rounding, shift-count and division rules. They
touch no runtime state, so they never leave the translation unit.

**Everything else is a `khu_rt_*` call** into the host: fields, array and
pointer elements, allocation, materialization, calls, natives, traps. Those are
the operations with the heap, the collector or the Procedure engine behind them,
and the point of the design is that they are *shared with the VM* rather than
reimplemented.

The natives are shared more literally still. `src/vm/natives.cpp` holds one
implementation of every one of them, and `Vm::call_native` and
`NativeHost::call_native` are its two callers: what a native needs from the
backend running it -- somewhere to put output, somewhere to put a string it
produced, a line of input -- arrives through a `NativeServices` interface, and
everything else about it is in one place where the two cannot drift.

The emitted C compiles clean under `-std=c11 -Wall -Wextra -Wpedantic -Werror`;
`native_emit.output_compiles_under_a_strict_compiler_mode` holds it there.

### Virtual dispatch

A vtable slot number only means something inside one inheritance chain — slot 0
is `area` in the `Shape` chain and `main` in an unrelated class in the same
image. So the emitter cannot ask "does slot 0 return a value" globally, and the
abstract stack needs that answer before any receiver exists to ask.

The front end knows at the call site, so `invokevirtual` carries it: a third
operand saying whether the call leaves a value behind
([`bytecode.md`](bytecode.md)). The emitter still tracks the class of each live
slot alongside the stack height — from `this`, from a materialization, from an
allocation, from a typed field read, through locals — and where the class *is*
known it checks the operand against that class's own method rather than
believing it. The host re-checks it again against the method it resolved.

Before the operand existed, the emitter inferred the answer by scanning every
class in the image for that slot and failing if they disagreed. That held only
while an image's classes were the program's own; the standard library's classes
made a disagreement ordinary rather than exceptional.

---

## 4. The host

`src/native/host/` is everything native code still needs once the dispatcher is
gone. `NativeHost` owns the **same** `Heap` and `Collector` the VM owns,
implements the same `RootSource`, and installs the same `KhuProcHostApi`. It is
not a second runtime; it is the same runtime with a different thing calling it.

`khu_vm_core` was split out of `khu_vm` so this could be literal: a built binary
links the object model, the collector and the arenas without linking
`src/vm/vm.cpp`.

### Materialization

`khu_proc_materialize` stays the single driver of the pipeline. The emitted C
does not inline the seven steps, does not reorder them and does not have its own
version of them — a `materialize` instruction becomes a call into the host,
which calls the engine, which calls back through the same seventeen callbacks
the interpreter exposes. The order in [`procedures.md`](procedures.md) is a
language promise, and native execution inherits it rather than repeating it.

Allocation-site arguments travel on a small host-side stack, which is exactly
where the interpreter leaves them on its operand stack: the emitted code hands
them to `khu_rt_materialize`, `take_arguments` lifts them off, and
`push_arguments` puts them back for the Procedures block and the constructor.

### Garbage collection and safepoints

This is the one genuinely new mechanism, and the highest-risk part of the
design.

A collection may only happen at a **safepoint**: an allocation site or a call
boundary. Between safepoints the emitted code is free to keep values wherever
the C compiler likes. At a safepoint, every live reference must be somewhere the
collector can find it — so it lives in the frame's **scanned-locals region**,
the same `V[]` array the operand stack lives in, and the emitted code publishes
its live height immediately before crossing:

```c
    F.ip = 18u; F.height = 1u;
    if (khu_rt_call_native(&F, 2u, 1u, &V[1], &V[1], 0)) KHU_UNWIND(&F);
```

`F.height` is what makes the scan **precise** rather than conservative: the host
walks `slots[0 .. local_count + height)` and no further, so a dead operand left
in a higher slot never keeps an object alive. `frame_enter` zeroes the whole
region first, so a frame is safe to scan from its first instruction.

Frames link into a chain the host walks in `enumerate_roots`, alongside the
handshake stack, the staged allocation arguments, and the values a
half-finished materialization is holding. Manual objects are roots as always,
and the collector walks those itself.

`ip` is published for the same reason: the host looks the offset up in the
method's line table to build a trap message, and it points *past* the
instruction in flight so the interpreter's "back up one byte" rule lands in the
same place. That is why a native stack trace is not merely similar to the VM's
but identical to it.

### Write barriers, pinning and manual memory

Assigning a manual reference into a managed slot pins it; overwriting that slot
drops the pin. The emitted C does not implement that — `khu_rt_putfield` does,
in the host, with the same code path the interpreter uses. `free`, pin counts
and the "free of a pinned object names the holders" error come through the same
door, so leak accounting from `utils/alloc.c` and the diagnostics both carry
over unchanged.

---

## 5. The invariant

> A program that printed byte-identical output under the VM prints
> byte-identical output under both native modes.

Not "close". Identical stdout, identical stderr, identical exit code — including
trap text, source positions and stack-trace frame counts.

This is enforced, not asserted:

| Where | What it checks |
|---|---|
| `differential_*` (ctest) | every `examples/` and `tests/integration/` program under all three backends, comparing stdout, stderr and exit code — no golden file involved, so trapping programs count too |
| `golden_native_*` (ctest) | the native run against the *same* `.expected` file the VM run uses |
| `build_*` (ctest) | a binary from `khudra build`, executed on its own, against the same golden |
| `native_diff.*` (unit) | both backends in one process: widths, overflow, floats, dispatch, traps, input, manual memory, GC churn |
| `native_abi.*` (unit) | `KhuValue`/`KhuObjectHeader` against `src/vm/value.h` and `src/vm/object.h`, plus the type tags, object flags and call-depth guard |
| `natives.*` (unit) | every native id is known, sits in its namespace's reserved block, and declares the stack effect its signature implies |
| `native_binary_has_no_interpreter` | no `khu::vm::Vm` symbol in a built binary |
| `system.*` (unit) | `khuStdSystem` under both backends: handles, files, sockets, `argv()`, and the exit code |
| `http_server_over_tcp` (script) | `examples/http_server.khu` served by all three backends and driven by a real `curl` |

Everything stays green under `-DKHU_SANITIZE=ON`.

### Sockets and the system library

`khuStdSystem` (docs and declarations in `lib/system.khu`) is the newest thing
that has to honour the invariant, and it honours it the same way everything
else does: **one implementation, two callers**. The natives live in
`src/vm/natives.cpp` like all the others, and everything platform-shaped sits
behind `src/vm/platform.{h,cpp}` -- so `khudra run`, `khudra run --native` and a
built binary open the same files, bind the same sockets and answer the same
`errno()`.

Three parts of it needed something genuinely new:

- **A native that produces a Khudra object.** `argv()` answers a real
  `List<string>`, which `NativeOutcome` cannot carry. `NativeServices` grew
  `materialize`, `invoke` and a `RootScope`, implemented once by `Vm::Services`
  and once by `NativeHost::Services`, both running the ordinary materialization
  pipeline. A native cannot build something the language could not.
- **Exit.** `khuStdSystem.exit(code)` unwinds exactly the way a trap does --
  frames come off, `run()` answers false -- with the difference that no error
  text is set. Each driver reads `exit_requested()`/`exit_code()`, so
  `differential_system_exit_code` can compare a status of 7 across all three
  backends.
- **Program arguments.** Threaded from three separate `main`s:
  `khu_main.cpp` (after a `--` on the command line), `run_jit`'s `program_args`,
  and `aot_main.cpp`'s own `argv`.

Sockets themselves have no golden, and cannot: a port and a moment in time are
not byte-comparable. They are covered by the `system.*` unit tests, which run
every exchange under both backends in one process, and by
`scripts/test_http_server.sh`, which drives `examples/http_server.khu` with a
real `curl` once per backend.

---

## 6. Layout

```
src/native/
  cemit/
    cemit.h            how the lowering maps the bytecode model
    cemit.cpp          Module -> C translation unit
  host/
    khu_native_abi.h   the C11/C++17 boundary: value and header layout,
                       the khu_rt_* calls, the inline value operations
    native_host.h/.cpp NativeHost: heap, collector, roots, KhuProcHostApi
    toolchain.h/.cpp   finding cc/c++, running them, scratch directories
    loader.cpp         the JIT wrapper: compile to .so, dlopen, run
    aot.cpp            the AOT wrapper: compile, link, write an executable
    aot_main.cpp       main() for the binaries `khudra build` writes
```

`khu_native_abi.h` is embedded into the toolchain at build time
(`scripts/embed_native_abi.cmake`) and written into the top of every generated
translation unit. One source of truth: the header the host compiles against is
the header the emitted C sees, and a generated file needs nothing from the
Khudra source tree to build.

---

## 7. Limits

- POSIX hosts and Windows/MinGW-w64. The platform-tied parts are the loader
  (`dlopen` against `LoadLibrary`), the toolchain (`posix_spawn` against
  `CreateProcess`) and the symbol model; each is one shim, and the lowered
  translation unit is the same portable C11 either way. **MSVC is not
  supported**: it needs a generated `.def`, an import library and
  `__declspec(dllexport)` on the four boundary symbols, which is the one place
  the emission side would have to change.
- The symbol model differs between the two, and it is worth knowing which you
  are on. On POSIX the `dlopen`'d object leaves every `khu_rt_*` dangling and
  the loader satisfies it from the executable's dynamic table (`-rdynamic`,
  which CMake spells `ENABLE_EXPORTS`). Windows has no dangling-symbol model,
  so `khudra.exe` is linked with `--export-all-symbols` and the emitted DLL
  *imports* those symbols, with the exe itself on the DLL's link line.
- A host compiler is required, as described in section 1.
- `run --native` pays for a C compile at startup. It is a way to *run* native
  code, not a low-latency JIT; `khudra build` is where the compile happens once.
- Reflection, a REPL and a debugger are not available for native builds — the
  same as for the VM today.
