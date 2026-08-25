#!/usr/bin/env python3
"""Detect build artifacts accidentally committed to git."""

import fnmatch
import subprocess
import sys
from pathlib import Path

FORBIDDEN_PATTERNS = [
    "*.o",
    "*.obj",
    "*.a",
    "*.lib",
    "*.so",
    "*.so.*",
    "*.dll",
    "*.exe",
    "*.kbc",
    "CMakeCache.txt",
]

FORBIDDEN_DIR_PARTS = [
    "CMakeFiles/",
    "CMakeScratch/",
]

ALLOWED = {
    "build/iniial.md",
}


def tracked_files(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    return [
        line
        for line in result.stdout.decode().split("\0")
        if line
    ]


def is_forbidden(path: str) -> bool:
    if path in ALLOWED:
        return False
    if any(part in path for part in FORBIDDEN_DIR_PARTS):
        return True
    name = path.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatch(name, pattern) for pattern in FORBIDDEN_PATTERNS)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    offenders = [f for f in tracked_files(root) if is_forbidden(f)]
    if offenders:
        print("Artifact detection FAILED. Forbidden files tracked in git:")
        for path in offenders:
            print(f"  - {path}")
        return 1
    print("Artifact detection OK (no build artifacts tracked in git).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
