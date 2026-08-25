# Procedures

Status: implemented. Written as a draft in Phase 0, before the code that
depends on it; `utils/proc_engine.c` implements this document and
`tests/unit/test_procedures.cpp` checks it.

A **Procedure** is Khudra's third core feature: a named block in a class body
that executes *immediately when an object instance is materialized* -- when it is
loaded into memory -- instead of waiting to be called by another object.

```khudra
public class Example {
    private Procedures {
        khu.stdlibLoadObject();
        pointToOtherInstructionOutsideClass();
        executeInstruction();
        instanceObject();
    }
}
```

## 1. Syntax

```
[visibility] Procedures [ ( parameter-list ) ] { statement* }
```

- `Procedures` is a reserved word; it is a block, not a method.
- A parameter list is optional. When present, the parameters are bound to the
  allocation site's arguments.
- There is no return type and no `return <value>;` -- a bare `return;` may be
  used to leave the block early.
- A class declares **at most one** `Procedures` block.
- The visibility modifier controls nothing about invocation (a Procedure is
  never invocable); it documents whether the block is part of the class's
  published behaviour.

## 2. Guarantees

| Guarantee | Detail |
|---|---|
| Automatic | Runs as part of materialization; never scheduled by the programmer. |
| Exactly once | Once per instance, ever. |
| Before the constructor | The Procedures block runs *before* the constructor body. |
| Before any member access | No field or method of the instance is reachable from outside until the block has completed. |
| Not invocable by name | `obj.Procedures()` and `Procedures()` are compile errors. |
| May reach outward | It may call functions outside the class, the runtime (`khu.*`), and materialize other objects. |
| Receives allocation args | Its parameters are bound to the allocation-site arguments. |

## 3. The materialization pipeline

```
Sprite s = Sprite(x, y);

  1. allocate            per strategy: the class `type` field default, or the
                         allocation-site override token (`manual` / `standard`)
  2. link object         stamp the header (class id, flags, pin 0, size, vtable)
                         and set every field to its default; `null` means
                         "not instantiated yet", then run the declared field
                         initializers, base class first
  3. bind arguments      allocation-site args -> Procedures parameters
  4. run Procedures      <-- "immediately executes when loaded into memory"
                         may call outward, may materialize other objects
                         (nested allocations run depth-first)
  5. run constructor     the optional field-setup hook
  6. register live       publish to the GC / manual bookkeeping
  -> return the reference to the allocating site
```

### 3.1 Inheritance

Materializing a derived class runs the whole chain, root first, one stage at a
time:

```
field initializers   Base, then Derived, then Leaf
Procedures blocks    Base, then Derived, then Leaf
constructor bodies   Base, then Derived, then Leaf
```

Base first at every stage, so a derived class never observes uninitialized
inherited state, and a derived constructor may overwrite what the base set.
Running *all* the Procedures blocks before *any* constructor body keeps the
guarantee that "Procedures runs before the constructor" true for the object as a
whole, not just per class.

One allocation site supplies the arguments for the entire chain. Every class in
it that declares materialization parameters must therefore declare the **same**
ones; the checker rejects a chain that disagrees.

Steps 1-6 are driven by `khu_proc_materialize` in `utils/proc_engine.c`. The
engine is **C**; every step that needs VM state calls back into the C++ VM
through the `KhuProcHostApi` table in `utils/proc_engine.h`
(`src/vm/proc_host.cpp` is the VM's half). Keeping the engine in C keeps the
materialization pipeline -- the piece most likely to be reused by an FFI or an
alternate backend -- behind a stable ABI, and puts the ordering guarantees the
language makes promises about in one readable place.

The engine holds no VM state at all: an inheritance chain is walked through
`chain_length` / `chain_at`, a step is performed through `run_*`, and the
half-built object is kept alive across a collection through `retain` /
`release`. Argument binding happens **first**, before the allocation: a
Procedures block may materialize other objects on the same value stack, so the
allocation-site arguments are lifted off it and rooted before anything else can
touch it.

## 4. Ordering and nesting

Allocations performed *inside* a Procedures block run **depth-first**: the inner
object is fully materialized (its own steps 1-6 complete) before the outer
block's next statement runs. `khu_proc_depth()` reports the current nesting
depth.

Because the block may materialize other objects, cycles are possible:

```khudra
public class A { private Procedures { B b = B(); } }
public class B { private Procedures { A a = A(); } }
```

This recurses until `KHU_PROC_MAX_DEPTH` is reached, at which point
materialization fails with `KHU_PROC_DEPTH_EXCEEDED`:

```
cycle.khu:8:9: runtime error: materialization nested too deeply; a Procedures
block that materializes its own class recurses without end
    at B.Procedures (cycle.khu:8:9)
    at A.Procedures (cycle.khu:3:9)
    ... 249 more frames
```

The checker cannot reject this statically in general -- the allocation may be
behind a condition, or several classes deep -- so the guard is a runtime one.

## 5. Failure semantics

If a Procedures block aborts (a runtime error, or the depth guard):

1. The constructor does **not** run.
2. The object is **not** registered as live and its reference is never returned
   to the allocation site.
3. The partially materialized object is discarded: `KhuProcHostApi::discard`
   returns a manual object to its arena, and simply never registers a managed
   one -- nothing refers to it, so the next collection reclaims it. A manual
   object that the block managed to get pinned is left alone rather than freed,
   because releasing it would leave a dangling managed slot.
4. Objects the block already fully materialized *are* live -- they completed
   their own pipelines. Rolling those back would require undoing arbitrary
   outward calls, so Khudra does not attempt it.
5. The status propagates to the allocating site as a runtime error.

## 6. Validation (Phase 2)

The checker rejects:

- calling a Procedure by name (`obj.Procedures()`, `Procedures()`, `this.Procedures()`);
- more than one `Procedures` block in a class;
- a return type on the block, or `return <value>;` inside it;
- an allocation site whose argument count/types do not match the block's
  parameters (after the optional strategy token is removed);
- reading state the block cannot have initialized yet.

## 7. Relationship to constructors

Both run during materialization; they are not redundant:

| | Procedures | Constructor |
|---|---|---|
| Purpose | behaviour that must happen the moment the object exists | field setup |
| Runs | step 4 | step 5 |
| Named | no -- a block | yes -- named after the class |
| Invocable | never | only through materialization |
| Sees allocation args | yes | yes |

A class may declare either, both, or neither.
