# Contributing to Khudra

Thank you for your interest in contributing to Khudra, an object-oriented language with a hand-written compiler, a bytecode VM, and a native backend that runs the same programs against raw memory.

This guide covers everything you need to submit a contribution: environment setup, branch and commit conventions, testing expectations, and the review process.

**Quick links**

| Resource | Location |
|---|---|
| Design reference and phases | [`KHU-PLAN.md`](KHU-PLAN.md) |
| Language specification | [`docs/spec.md`](docs/spec.md) |
| Memory model | [`docs/memory-model.md`](docs/memory-model.md) |
| Materialization pipeline | [`docs/procedures.md`](docs/procedures.md) |
| Instruction set and `.kbc` format | [`docs/bytecode.md`](docs/bytecode.md) |
| Native backend | [`docs/native.md`](docs/native.md) |
| Where the project goes next | [`docs/roadmap.md`](docs/roadmap.md) |
| Pull request template | [`.github/pull_request_template.md`](.github/pull_request_template.md) |
| License | [`LICENSE`](LICENSE) |
| Security contact | Discord: **HMythical** |

---

## Table of Contents

1. [Getting Started](#1-getting-started)
2. [Branch Strategy](#2-branch-strategy)
3. [Code Contributions](#3-code-contributions)
4. [Commit Style](#4-commit-style)
5. [Documentation](#5-documentation)
6. [Security Issues](#6-security-issues)
7. [Pull Request Process](#7-pull-request-process)
8. [Code of Conduct](#8-code-of-conduct)
9. [License](#9-license)
10. [Getting Help](#10-getting-help)

---

## 1. Getting Started

### Prerequisites

| Requirement | Notes |
|---|---|
| A C++17 compiler | GCC 9+ or Clang 10+. The front end, codegen and VM are C++17 |
| A C11 compiler | The runtime in `utils/` and the native ABI boundary are C11 |
| CMake 3.16 or later | The only build system |
| Git | Any recent version |
| Python 3 | For the structure and artifact validation scripts |

Khudra has **no external dependencies** — no package manager, no vendored libraries, not even a test framework. If a change appears to need one, raise it in an issue first (see [Language Requirements](#language-requirements)).

Verify your toolchain before starting:

```bash
cc --version
c++ --version
cmake --version
```

### Fork and Clone

1. Fork `HMythical/Khudra` under your own GitHub account.
2. Clone your fork locally:

   ```bash
   git clone https://github.com/<your-username>/Khudra.git
   cd Khudra
   ```

3. Add the upstream remote so you can stay in sync:

   ```bash
   git remote add upstream https://github.com/HMythical/Khudra.git
   git fetch upstream
   ```

4. Confirm your commit identity is set correctly so your work is attributed to you:

   ```bash
   git config user.name "Your Name"
   git config user.email "you@example.com"
   ```

5. Create a branch off `dev` for your work (see [Branch Strategy](#2-branch-strategy)).

### Development Setup

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # Configure
cmake --build build --parallel                 # Build
ctest --test-dir build --output-on-failure     # Run the full suite
```

The memory systems are meant to be exercised under sanitizers, and CI does exactly this:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKHU_SANITIZE=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

Running a subset of the unit tests, by suite or by `suite.name` prefix:

```bash
./build/khudra_unit_tests            # everything
./build/khudra_unit_tests vm         # one suite
./build/khudra_unit_tests native_diff.traps
```

Trying the toolchain on a program:

```bash
./build/khudra run examples/hello.khu            # on the bytecode VM
./build/khudra run --native examples/hello.khu   # through the native backend
./build/khudra build examples/hello.khu -o hello # a standalone executable
./build/khudra disasm examples/hello.khu         # a bytecode listing
```

---

## 2. Branch Strategy

### Branch Naming Conventions

Branch names use a type prefix followed by a short, hyphenated description.

| Prefix | Use for | Example |
|---|---|---|
| `feature/` | New functionality | `feature/interface-dispatch` |
| `fix/` | Bug fixes | `fix/shift-count-wrapping` |
| `docs/` | Documentation changes | `docs/native-safepoints` |
| `refactor/` | Restructuring without behaviour change | `refactor/split-class-table` |
| `test/` | Test additions or changes | `test/collector-edge-cases` |
| `chore/` | Build, CI, and maintenance work | `chore/bump-cmake-minimum` |

> **Note:** Keep descriptions lowercase and hyphen-separated. Avoid branch names that only reference an issue number.

### Workflow

| Branch | Purpose |
|---|---|
| `dev` | Main development branch and the repository default. All contributions target this branch. |
| `main` | Deploy branch. Maintained by the project maintainer, and protected. |

The flow is:

```
your-fork/feature/<description>  ->  HMythical/Khudra:dev  ->  main (releases)
```

Contributors open pull requests against `dev`. Only the maintainer promotes `dev` to `main` for a release. Do not open pull requests against `main`.

Keep your branch current before opening a pull request:

```bash
git fetch upstream
git rebase upstream/dev
```

### Pull Request Requirements

- All four CI checks must pass: **Build & Test (Linux)**, **ASan + UBSan (Linux)**, **Structure Compliance**, **Artifact Detection**.
- The description must explain what changed and why.
- New functionality must include test coverage.
- The full `ctest` suite must be green, not just the tests you added.

---

## 3. Code Contributions

### Language Requirements

| Language | Scope |
|---|---|
| C++17 | The front end, codegen, bytecode, VM and native backend, under `src/` |
| C11 | The runtime in `utils/` and the `extern "C"` ABI headers |
| Khudra | The standard library in `lib/`, embedded into the toolchain at build time |
| CMake | The build system |
| Python 3 | CI helper and validation scripts under `scripts/` |
| Bash | The install and uninstall scripts |

> **Warning:** Khudra deliberately has **no external dependencies**, including for testing. The unit-test harness in `tests/test_harness.h` exists because adding one was not acceptable. A pull request that introduces a library, a package manager, or a vendored third-party source tree will be sent back unless it was agreed in an issue first.

Files that are included from both C and C++ — `utils/proc_engine.h` and `src/native/host/khu_native_abi.h` — must stay valid C11 *and* valid C++17. Both sides compile them.

### Code Style and Conventions

There is no `clang-format` config; match the file you are editing. The house style is:

| Element | Convention | Example |
|---|---|---|
| Indentation | 4 spaces, no tabs; lines wrap around 100 columns | |
| Namespaces | Lowercase, nested under `khu` | `khu::vm`, `khu::bytecode`, `khu::native` |
| Types | PascalCase | `ObjectHeader`, `RuntimeClass`, `NativeHost` |
| Functions and variables | snake_case | `enumerate_roots`, `class_id` |
| Private members | Trailing underscore | `module_`, `frames_`, `prepared_` |
| Constants | `k` prefix, PascalCase | `kMaxCallDepth`, `kObjectGc` |
| Header guards | `KHU_<PATH>_<FILE>_H` | `KHU_VM_OBJECT_H` |
| C ABI symbols | `khu_` prefix; types `Khu`-prefixed PascalCase | `khu_proc_materialize`, `KhuValue` |

Comment conventions matter more here than formatting does:

- Every file opens with a block comment saying what the file is *for* and why it is shaped that way, not what it contains.
- Inline comments explain **why**, never what. The code already says what.
- Cite the document that makes a decision binding — `KHU-PLAN.md`, `docs/procedures.md`, `docs/memory-model.md` — when the code is implementing a promise rather than a preference.
- Update a comment when you change the behaviour it describes. A stale comment is worse than none.
- Do not add comments that restate the signature.

### Invariants

Some properties are not style preferences and are not negotiable in review. If your change needs one of these to move, open an issue before writing code.

| Invariant | Where it lives |
|---|---|
| The materialization order — allocate, link, bind, Procedures, constructor, register — is a language promise. Every allocation path goes through `khu_proc_materialize`; nothing reimplements the pipeline. | [`docs/procedures.md`](docs/procedures.md) |
| Object layout is `src/vm/object.h`. The collector and the C runtime read that memory, so they are the arbiters. | `src/vm/object.h` |
| A program's stdout, stderr and exit code are **byte-identical** under the VM, `run --native`, and a `build` binary. | [`docs/native.md`](docs/native.md) |
| The bytecode verifier is the trust boundary for a `.kbc` image. The interpreter and the C emitter both assume verified input. | `src/lib/bytecode/verifier.cpp` |
| Overflow wraps, and there are no implicit numeric conversions. | [`docs/spec.md`](docs/spec.md) |

### Adding an Opcode

The instruction set is an X-macro so the enum, the name table and the operand decoder cannot drift apart. Adding one means touching every consumer:

1. `src/lib/bytecode/opcode.h` — add the entry to the `KHU_OPCODES` X-macro with its operand format.
2. `src/lib/codegen/` — emit it.
3. `src/lib/bytecode/verifier.cpp` — validate its operands and any jump target.
4. `src/vm/vm.cpp` — execute it in the interpreter.
5. `src/native/cemit/cemit.cpp` — lower it to C, including its stack effect and its abstract-state transfer.
6. `docs/bytecode.md` — document it.
7. Tests — a golden program if it is reachable from Khudra source, and a case in `tests/unit/test_native.cpp` proving both backends agree on it.

A new opcode that runs on the VM but not the native backend breaks the byte-identical invariant, so step 5 is not optional.

### Adding a Native

A `native func` in `lib/*.khu` is a declaration with no body. It is bound either to a **bytecode instruction** (an intrinsic) or to a **runtime call** (a native id), and an unbound declaration is a compile error. Adding one means:

1. `lib/<namespace>.khu` — declare the signature. Khudra has no implicit conversion, so an operation that works on several widths is several declarations; overload resolution picks between them by operand type.
2. `src/lib/bytecode/native.h` — add an entry to the `KHU_NATIVES` X-macro: name, id, qualified name, result count (0 or 1). Take the next free number **inside your namespace's reserved block**, listed at the top of that file. Ids are frozen: a `.kbc` image records the number, so a shipped id is never changed and never reused.
3. `src/lib/sema/builtins.cpp` — bind `(namespace, member, arity)` to the id in that namespace's `bind_*` function.
4. `src/vm/natives.cpp` — implement it, **once**, in `invoke_native`. Both backends call this; there is no second implementation to keep in step. Anything the native needs from the backend it is running under — output, input, a place to put a string it produced — comes through `NativeServices`.
5. Numbers in and out go through `src/vm/format.h`, and a string the native produces goes through `NativeServices::make_string` (`src/vm/runtime_strings.h`). Rendering a number by hand is how the two backends start to disagree.
6. `docs/spec.md` §9.1 and the namespace's doc comment in `lib/` — document what it does, including the failure behaviour.
7. Tests — a golden under `tests/integration/stdlib/`, and an expected-failure case under `tests/integration/errors/` when it has one.

The result count in step 2 is not bookkeeping: the C emitter reads it to know the instruction's stack effect (`src/native/cemit/cemit.cpp`), and `tests/unit/test_natives.cpp` checks it against the declared return type. A native declared to return a value that leaves nothing behind desynchronises the emitted frame.

Overload resolution happens in the checker, but the *binding* is by name and arity only, so every overload of `khuStdConv.toString` shares one id and dispatches on the argument's runtime tag. Two operations that cannot share a runtime dispatch need two names — which is why the per-width parse functions are `parseInt32`, `parseInt64` and so on rather than one overloaded `parseInt`.

A handful of natives cannot be *declared* at all, because their signature is not writable in Khudra. Those are recognized by the checker instead, in `check_call`, and each one is documented in its namespace's `lib/` file beside the declarations:

| Native | Why it cannot be declared |
|---|---|
| `khuStdMath.convertTo` | its first argument is a type |
| `khuStdCollection.arrayCreate` | its first argument is a type |
| `khuStdCollection.hash` / `.sameValue` | their argument is a value of *any* type |
| `io.describe` | the same |

Adding one to that list should be a last resort: a declared signature is what gives overload resolution, arity checking and error messages for free.

### Testing Requirements

Every behavioural change needs a test that would fail without it.

| Kind | Location | What it is for |
|---|---|---|
| Unit | `tests/unit/*.cpp` | Registered with `KHU_TEST(suite, name)`; globbed automatically |
| Golden | `tests/integration/<name>.khu` + `<name>.expected` | A program's exact stdout; registered automatically |
| Stdlib golden | `tests/integration/stdlib/<name>.khu` + `<name>.expected` | The same, for the standard library's per-namespace programs |
| Golden stderr | `<name>.expected-err` beside either of the above | The program's exact stderr, compared when the file exists |
| Expected failure | `tests/integration/errors/<name>.khu` + `<name>.expected-error` | A program that must be rejected, and the wording that must appear |
| Example | `examples/<name>.khu` + `tests/integration/examples/<name>.expected` | Readable sample programs, checked and run |

A new golden or example file is picked up by CMake without editing `CMakeLists.txt`, and a golden automatically gains its native and ahead-of-time variants — the same `.expected` file is compared against `khudra run`, `khudra run --native`, and a binary from `khudra build`.

- Cover the normal path and the edge cases, including malformed input and failure modes.
- Tests must not depend on network access, a specific machine, or the wall clock.
- Everything must stay green under `-DKHU_SANITIZE=ON`, with the leak accounting in `utils/alloc.c` reporting zero leaks.
- Record what you tested, what you expected, and what you observed in the pull request description.

> **Warning:** The collector, the manual arenas and the pin bookkeeping are the parts most likely to be subtly wrong and least likely to fail loudly. Changes to `src/vm/gc/`, `src/vm/manual/`, `utils/alloc.c`, or the native backend's root scanning receive additional scrutiny, and will not be merged without tests that run clean under sanitizers.

---

## 4. Commit Style

### Format

```
(<type>) <description>

<body>

<footer>
```

### Types

| Type | Use for |
|---|---|
| `feat` | New feature |
| `fix` | Bug fix |
| `docs` | Documentation only |
| `style` | Formatting, no behaviour change |
| `refactor` | Restructuring without behaviour change |
| `test` | Tests |
| `chore` | Build, CI, and maintenance |

### Rules

- Use the imperative mood: "Add emitter", not "Added emitter".
- Keep the subject line under 72 characters.
- Leave a blank line between the subject, body, and footer.
- Explain *what* changed and *why* in the body. The diff already shows *how*.
- Reference related issues in the footer with `Closes #<n>` or `Refs #<n>`.
- Note test coverage and any limitations or trade-offs.

### Examples

A feature commit:

```
(feat) Lower the full instruction set to C

The native emitter handled constant loads and arithmetic; every other
opcode fell through to an error. It now translates all of them,
including virtual dispatch through the receiver's own vtable and the
materialize call into khu_proc_materialize.

Vtable slot numbers are only meaningful inside one inheritance chain,
so the emitter tracks the class of each live stack slot and asks the
receiver's class rather than assuming a slot means one thing image-wide.

Every examples/ and tests/integration/ program now produces identical
stdout, stderr and exit codes under both backends.

Closes #17
```

A fix commit:

```
(fix) Wrap the shift count by the operand's width

Shl and Shr passed the raw count to the C shift operators, which is
undefined behaviour once the count reaches the operand width and gave
different answers under the VM and the native backend for uint8.
The count is now taken modulo the width, matching KhudraVm::arithmetic.

Covered by native_diff.shifts_wrap_their_count_by_width.

Refs #23
```

### Common Mistakes

| Avoid | Use instead |
|---|---|
| `fixed stuff` | `(fix) Correct the line lookup for native traps` |
| `(feat): Add emitter` | `(feat) Add emitter` |
| `Update vm.cpp` | A subject describing the behaviour that changed |
| A subject line with no body | A body explaining the motivation and testing |
| Bundling unrelated changes | One logical change per commit |

---

## 5. Documentation

### Code Documentation

- Open every file with a block comment explaining its purpose and the reasoning behind its shape.
- Document non-obvious seams — the places two subsystems meet — rather than individual functions whose names already say enough.
- Use inline comments only where the reasoning is not obvious. Explain why, not what.
- Update existing comments when you change the behaviour they describe.

### Project Documentation

| Change | Document to update |
|---|---|
| Language surface: syntax, types, semantics | [`docs/spec.md`](docs/spec.md) |
| Allocation strategies, pinning, the collector | [`docs/memory-model.md`](docs/memory-model.md) |
| The materialization pipeline | [`docs/procedures.md`](docs/procedures.md) |
| Instruction set or the `.kbc` container | [`docs/bytecode.md`](docs/bytecode.md) |
| The native backend, its host or its safepoints | [`docs/native.md`](docs/native.md) |
| Installation and packaging | [`docs/installation.md`](docs/installation.md) |
| A gap you deliberately left open | [`docs/roadmap.md`](docs/roadmap.md) |

Update [`README.md`](README.md) only when the CLI surface, installation, or build instructions change.

`scripts/validate_structure.py` enforces that the project layout and the core documents exist. Run it before pushing:

```bash
python3 scripts/validate_structure.py
python3 scripts/detect_artifacts.py
```

### Pull Request Documentation

Your pull request should stand on its own for a reviewer who has not seen the code before:

- A clear description of the change and the problem it solves.
- Test results, including what you ran and the exact commands.
- Terminal output for changes to user-visible behaviour, including diagnostics and trap text.
- Any breaking changes to the language, the `.kbc` format, or the CLI, called out explicitly.
- Known limitations or follow-up work.

---

## 6. Security Issues

### Reporting Process

> **Warning:** Do not open a public GitHub issue for a security vulnerability. Public disclosure before a fix is available puts users at risk.

Report vulnerabilities privately via Discord to **HMythical**.

Khudra's sensitive surfaces are worth naming, since they are where a report is most likely to be real:

- The **bytecode verifier**. A `.kbc` image is untrusted input; the interpreter and the native emitter both assume it has been verified. An image that passes verification and then reads or writes out of bounds is a security bug.
- The **native backend**. `khudra run --native` invokes the host C compiler and loads the resulting shared object into the running process, and `khudra build` links an executable. Anything that lets a `.khu` or `.kbc` input influence the command line, the emitted C, or the loaded object beyond its intended program is a security bug.
- The **memory systems**. Use-after-free, double-release, or pin-count corruption reachable from a well-formed Khudra program.

Include the following in your report:

| Field | Description |
|---|---|
| Description | What the vulnerability is and which component is affected |
| Steps to reproduce | A minimal, reliable reproduction, with the `.khu` or `.kbc` input |
| Impact | What an attacker could achieve, and under what preconditions |
| Affected versions | Commit, tag, or release where you observed the issue |
| Suggested fix | Optional, but appreciated |

Please give the maintainer a reasonable opportunity to release a fix before disclosing the issue publicly.

### What Not to Report Here

| Type | Where it belongs |
|---|---|
| General bugs and crashes | GitHub Issues |
| Feature requests | GitHub Issues |
| Usage and design questions | GitHub Issues, or Discord |
| Build or setup problems | GitHub Issues |

### Response Timeline

| Stage | Target |
|---|---|
| Initial acknowledgment | Within 48 hours |
| Assessment and severity triage | Within 1 week |
| Fix and release | Depends on severity and complexity |

---

## 7. Pull Request Process

### Before Submitting

Run the full local check suite and confirm every item passes:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKHU_SANITIZE=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

python3 scripts/validate_structure.py
python3 scripts/detect_artifacts.py
```

Then confirm the following:

- [ ] Your branch is rebased on the latest `upstream/dev`.
- [ ] The build produces no new compiler warnings.
- [ ] New functionality has tests, and the whole suite passes — under sanitizers too.
- [ ] Documentation is updated for any language, format, or user-facing change.
- [ ] Commit messages follow the format in [Commit Style](#4-commit-style).
- [ ] The change contains no unrelated edits, commented-out code, or debug output.
- [ ] No build artifacts, generated files, secrets, or absolute local paths are committed.

### Opening the Pull Request

Open the pull request against `dev`. The [pull request template](.github/pull_request_template.md) is applied automatically. Fill in every section, including:

- **Description** — what changed and why.
- **Type of change** — feature, fix, breaking change, docs, refactor, or test.
- **Backends** — which of the three execution paths you exercised.
- **Testing** — the commands you ran and what they reported, including the sanitizer run.
- **Checklist** and **Invariants** — the latter only where your change touches them.
- **Related issues** — `Closes #<n>` or `Refs #<n>`.
- **Terminal output** — for anything a user would see, pasted rather than described.

Mark the pull request as a draft if you want early feedback on work that is not finished.

### Review Process

| Stage | Detail |
|---|---|
| CI | All four checks must pass before review begins |
| Approval | At least one maintainer approval is required to merge |
| Changes requested | Push additional commits to the same branch; do not force-push mid-review unless asked |
| Merge | Squash and merge, so `dev` keeps a linear history |

If a pull request or commit message is unclear, the maintainer will ask you to explain the change rather than guess at it. The goal is that the next contributor can read the history and understand why the code is the way it is.

### Code Provenance

Do not copy code from other language implementations into this repository. Khudra is written by hand on purpose — there is no parser generator, no lexer generator, and no borrowed runtime — and that is part of what the project is for. Contribute code you wrote or that you have an unambiguous right to relicense under Apache 2.0. If a change is derived from another project, say so explicitly in the pull request so the license can be reviewed.

---

## 8. Code of Conduct

This project adopts the [Contributor Covenant Code of Conduct, version 2.1](https://www.contributor-covenant.org/version/2/1/code_of_conduct/).

### Our Pledge

We as members, contributors, and leaders pledge to make participation in our community a harassment-free experience for everyone, regardless of age, body size, visible or invisible disability, ethnicity, sex characteristics, gender identity and expression, level of experience, education, socio-economic status, nationality, personal appearance, race, caste, color, religion, or sexual identity and orientation.

We pledge to act and interact in ways that contribute to an open, welcoming, diverse, inclusive, and healthy community.

### Our Standards

Examples of behavior that contributes to a positive environment:

- Demonstrating empathy and kindness toward other people
- Being respectful of differing opinions, viewpoints, and experiences
- Giving and gracefully accepting constructive feedback
- Accepting responsibility, apologizing to those affected by our mistakes, and learning from the experience
- Focusing on what is best for the overall community, not just for us as individuals

Examples of unacceptable behavior:

- The use of sexualized language or imagery, and sexual attention or advances of any kind
- Trolling, insulting or derogatory comments, and personal or political attacks
- Public or private harassment
- Publishing others' private information, such as a physical or email address, without their explicit permission
- Other conduct which could reasonably be considered inappropriate in a professional setting

### Enforcement Responsibilities

Project maintainers are responsible for clarifying and enforcing these standards of acceptable behavior and will take appropriate and fair corrective action in response to any behavior they deem inappropriate, threatening, offensive, or harmful.

Maintainers have the right and responsibility to remove, edit, or reject comments, commits, code, issues, and other contributions that are not aligned with this Code of Conduct, and will communicate reasons for moderation decisions when appropriate.

### Scope

This Code of Conduct applies within all community spaces, including the GitHub repository, issues, pull requests, and the project Discord. It also applies when an individual is officially representing the project in public spaces.

### Enforcement

Instances of abusive, harassing, or otherwise unacceptable behavior may be reported to the project maintainer via Discord to **HMythical**. All complaints will be reviewed and investigated promptly and fairly.

Maintainers are obligated to respect the privacy and security of the reporter of any incident. Consequences for violations follow the enforcement guidelines described in the [Contributor Covenant v2.1](https://www.contributor-covenant.org/version/2/1/code_of_conduct/), ranging from a private warning to a permanent ban from the community.

### Attribution

This Code of Conduct is adapted from the [Contributor Covenant](https://www.contributor-covenant.org), version 2.1, available at https://www.contributor-covenant.org/version/2/1/code_of_conduct.html.

---

## 9. License

All contributions to Khudra are licensed under the **Apache License 2.0**.

By submitting a pull request, you agree that your contributions will be licensed under the same terms as the project, and you confirm that you have the right to license them that way.

> **Note:** No Contributor License Agreement is required. Contributing requires nothing beyond opening a pull request.

For the full terms, see the [LICENSE](LICENSE) file.

---

## 10. Getting Help

| Need | Where to go |
|---|---|
| Report a bug | [GitHub Issues](https://github.com/HMythical/Khudra/issues) |
| Request a feature | [GitHub Issues](https://github.com/HMythical/Khudra/issues) |
| Ask a usage or design question | [GitHub Issues](https://github.com/HMythical/Khudra/issues), or Discord |
| Report a security vulnerability | Discord: **HMythical** |
| Report a Code of Conduct violation | Discord: **HMythical** |
| Understand a design decision | [`KHU-PLAN.md`](KHU-PLAN.md) and the [`docs/`](docs/) directory |
| Find something to work on | [`docs/roadmap.md`](docs/roadmap.md), which names the seam each open direction attaches to |

Before opening an issue, search existing issues to see whether it has already been reported.

---

Khudra is early in its life and there is a great deal still to build. Thank you for taking the time to contribute.
