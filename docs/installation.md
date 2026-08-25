# Installation

## Requirements

- **CMake** >= 3.14 and a C++17 compiler (GCC >= 9, Clang >= 10)
- **Make** (or any CMake-compatible build tool)
- Bash (for `install.sh` / `uninstall.sh`)

If no pre-built binary exists in `build/`, the install script will run the
build automatically.

## Quick start

```bash
# Install system-wide (default prefix: /usr/local)
sudo ./install.sh

# Uninstall
sudo ./uninstall.sh
```

## What gets installed

| Destination | Contents |
|---|---|
| `$PREFIX/bin/khudra` | The compiler/VM binary |
| `$PREFIX/share/khudra/examples/*.khu` | Sample programs |
| `$PREFIX/share/khudra/docs/*.md` | Language and design documentation |
| `$PREFIX/share/khudra/README.md` | Project readme |
| `$PREFIX/share/khudra/LICENSE` | License file |

The standard library is embedded into the binary at build time, so no runtime
files are required.

## Custom prefix

Override `PREFIX` to install anywhere:

```bash
# Install to your home directory (no sudo needed)
PREFIX=~/.local ./install.sh

# Install to a staging directory for packaging
PREFIX=/tmp/staging ./install.sh
```

## How it works

### install.sh

1. Checks for a pre-built `build/khudra` binary. If missing, runs
   `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` followed by
   `cmake --build build`.
2. Creates the install tree under `$PREFIX`.
3. Copies the binary, examples, docs, README and LICENSE into the tree.
4. Writes an install manifest to `$PREFIX/share/khudra/.install_manifest`.
   This file lists every installed path and is consumed by `uninstall.sh`.

The script only uses `sudo` when the target prefix is not writable by the
current user.

### uninstall.sh

1. Reads the install manifest produced by `install.sh`.
2. Removes each listed file.
3. Cleans up any directories left empty, working bottom-up.

The manifest is removed last so that directory cleanup can inspect the share
tree before it disappears.

## Verifying the installation

```bash
khudra version
khudra --help
khudra run ~/.local/share/khudra/examples/hello.khu   # if installed to ~/.local
```
