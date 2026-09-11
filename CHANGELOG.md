# Changelog

All notable changes to Khudra are recorded here. Kinds: **added**, **changed**,
**fixed**. The format loosely follows [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Added

- **Inline assembly blocks** (`inline_asm { ... }`): the sibling of `inline_c`,
  splices raw assembly into the native backend's emitted translation unit as
  the guts of a GCC/Clang `__asm__ volatile(...)` statement. Same aliased
  locals (`KhuValue* const` into the frame), same reserved-name skips, same
  native-only design.
  - Lexer: `inline_asm` keyword joins `inline_c` on the raw-block path.
  - Parser/AST/printer: `InlineAsmStmt` (full span + inner body), round-trips.
  - Bytecode: new `inlineasm` opcode (u16 string-constant index), reuses the
    `kMethodHasInline` / `kModuleHasInline` flags.
  - Emitter: wraps the body in `__asm__ volatile(...)`; volatile,
    side-effect-only, no safepoint inside assembly.
  - VM: dispatches `inlineasm` to a "native-only" trap.
  - Driver: the no-compiler refusal already covers inline images of both kinds.
  - Verifier: operand must name a string constant.
  - Docs: `docs/spec.md` (section 5.2), `docs/bytecode.md`, `docs/native.md`,
    `docs/roadmap.md`.
  - Tests: unit coverage (lexer, parser, printer, verifier, VM trap, native
    emission, strict-flag compile, an extended-asm write through an alias) and
    a native-only integration golden (`tests/integration/native/inline_asm.khu`).

### Changed

- The `run --native` no-compiler message now says "inline blocks
  (inline_c / inline_asm)" instead of naming only `inline_c`.
- `docs/native.md` "Inline C" section broadened to "Inline blocks: C and asm".

### Added (Phase 1, earlier in this release)

- **Inline C blocks** (`inline_c { ... }`): a statement that splices raw C
  verbatim into the native backend's emitted translation unit, with the
  enclosing method's named params and locals in scope as `KhuValue* const`
  aliases into the frame. Native-only by design.
  - Lexer: `inline_c` keyword, a `RawBlock` token holding the whole
    brace-delimited text (braces inside C strings, char literals and comments
    do not nest), and an "unterminated inline block" diagnostic.
  - Parser/AST/printer: `InlineCStmt` with the full span (`text`) and inner
    body (`body`); pretty-printer round-trips the raw text exactly.
  - Bytecode (`.kbc` **v1.3**): new `inlinec` opcode (u16 string-constant
    index), `MethodEntry` locals table, `kMethodHasInline` and a new module
    flags word (`kModuleHasInline`). 1.2 images still load; the new fields
    read as absent.
  - Emitter: aliases emitted after frame setup, reserved names skipped, the
    block published (`F.ip`/`F.height`) before the text, no stack effect.
  - VM: dispatches `inlinec` to a "native-only" trap.
  - Driver: `run --native` refuses to fall back to the bytecode VM when an
    image has inline blocks and no host C compiler is present (it still falls
    back for ordinary images).
  - Verifier: the operand must name a string constant; the locals table must
    fit the frame and only the inline methods may carry one.
  - Docs: `docs/spec.md` (syntax + contract), `docs/bytecode.md` (opcode +
    container format), `docs/native.md` (aliases/publish), `docs/roadmap.md`
    (inline_c shipped, inline_asm next).
  - Tests: unit coverage across lexer, parser, printer, bytecode round trip
    incl. 1.2 compat, verifier, VM trap and native emission/execution, plus
    native-only integration goldens (`tests/integration/native/`).