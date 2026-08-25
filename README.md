# Khudra

An object-oriented language with a hand-written compiler and a bytecode VM.

Khudra expands on OOP in three directions:

1. **Full OOP** -- classes, fields, methods, constructors, vtable dispatch.
2. **Memory management chosen through an object** -- a class picks garbage
   collection or manual allocation with a `MemoryAllocationTypeObject` field,
   and an allocation site can override it.
3. **Procedures** -- a named block in a class body that runs the moment an
   instance is loaded into memory, instead of waiting to be called.

```khudra
bring khu::stdlib;

public class Session {
    public MemoryAllocationTypeObject type = MemoryAllocationTypeObject.setStandard();

    public int32 id = 0;

    // Runs during materialization, before the constructor body and before the
    // allocation site resumes.
    private Procedures(int32 handle) {
        io.print("session ");
        io.print(handle);
        io.printLine(" loaded");
    }

    public Session(int32 handle) {
        this.id = handle;
    }

    func main() {
        Session s = Session(7);
        io.printLine(s.id);
    }
}
```

## Building

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Add `-DKHU_SANITIZE=ON` to build the whole tree under AddressSanitizer and
UndefinedBehaviorSanitizer.

## Using the toolchain

```
khudra check   file.khu      parse and typecheck
khudra run     file.khu      compile in memory and execute
khudra compile file.khu      write a .kbc bytecode image
khudra disasm  file.khu      print a bytecode listing
khudra run     file.kbc      execute a compiled image
```

`--dump-tokens` and `--dump-ast` print the intermediate forms.

## Layout

| Path | What is there |
|---|---|
| `src/lib/` | the compiler: diagnostics, lexer, parser, sema, codegen, bytecode |
| `src/vm/` | the VM: interpreter, object model, collector, manual arenas |
| `utils/` | the C runtime: the Procedure engine, manual allocation, bool support |
| `lib/` | the standard library, written in Khudra and embedded in the binary |
| `examples/` | sample programs |
| `tests/` | unit tests and golden-file integration tests |
| `docs/` | the specification, memory model, Procedures and bytecode formats |

## Documentation

- [`KHU-PLAN.md`](KHU-PLAN.md) -- the design decisions and the implementation plan
- [`docs/spec.md`](docs/spec.md) -- the language
- [`docs/memory-model.md`](docs/memory-model.md) -- strategies, pinning, the collector
- [`docs/procedures.md`](docs/procedures.md) -- the materialization pipeline
- [`docs/bytecode.md`](docs/bytecode.md) -- the instruction set and the `.kbc` format
