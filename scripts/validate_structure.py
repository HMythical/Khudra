#!/usr/bin/env python3
"""Validate that the project layout's required paths exist."""

import sys
from pathlib import Path

REQUIRED_PATHS = [
    "CMakeLists.txt",
    "README.md",
    # The contributor process: the guide and the template it points at.
    "CONTRIBUTING.md",
    ".github/pull_request_template.md",
    "example.khu",
    # The documents that describe the language and the implementation, in the
    # order they build on each other, then the user-facing and forward-looking
    # ones. Every file in docs/ is listed here on purpose: the point of this
    # check is that deleting one fails CI, which a glob would not do.
    "docs/spec.md",
    "docs/memory-model.md",
    "docs/procedures.md",
    "docs/bytecode.md",
    "docs/native.md",
    "docs/installation.md",
    "docs/roadmap.md",
    "src/main",
    "src/lib/diag",
    "src/lib/lexer",
    "src/lib/parser",
    "src/lib/sema",
    "src/lib/codegen",
    "src/lib/bytecode",
    "src/util",
    "src/vm",
    "src/vm/gc",
    "src/vm/manual",
    "utils",
    "utils/proc_engine.h",
    "lib",
    "tests/unit",
    "tests/integration",
    "examples",
]


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    missing = [p for p in REQUIRED_PATHS if not (root / p).exists()]
    if missing:
        print("Structure validation FAILED. Missing paths:")
        for path in missing:
            print(f"  - {path}")
        return 1
    print(f"Structure validation OK ({len(REQUIRED_PATHS)} required paths present).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
