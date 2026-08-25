#!/usr/bin/env python3
"""Validate that required paths from KHU-PLAN.md (Project layout) exist."""

import sys
from pathlib import Path

REQUIRED_PATHS = [
    "CMakeLists.txt",
    "KHU-PLAN.md",
    "docs",
    "src/main",
    "src/lib/diag",
    "src/lib/lexer",
    "src/lib/parser",
    "src/lib/sema",
    "src/lib/codegen",
    "src/lib/bytecode",
    "src/util",
    "src/vm",
    "utils",
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
