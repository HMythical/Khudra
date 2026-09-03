# Installation

## Requirements

- **CMake** >= 3.14 and a C++17 compiler (GCC >= 9, Clang >= 10)
- **Make** (or any CMake-compatible build tool)
- Bash (for `build.sh` / `install.sh` / `uninstall.sh`)

If no pre-built binary exists in `build/`, the install script delegates to
`build.sh` and builds it automatically. The build is a separate script so a
developer can build the compiler without installing it.

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

## Building without installing

`build.sh` is a thin wrapper over CMake with a few common profiles, keeping the
"how to build" knowledge in one place:

```bash
./build.sh               # Release build in ./build
./build.sh --debug       # Debug build in ./build
./build.sh --asan        # Debug + sanitizers (ASan/UBSan) in ./build-asan
./build.sh --test        # build, then run the test suite
./build.sh --clean       # remove the previous build directory first
./build.sh --help        # full usage
```

`install.sh` calls this whenever it needs a `build/khudra` binary that is not
there, so the same build path is used whether you install or build by hand.

## How it works

### build.sh

1. Picks a profile: `Release` (default), `Debug`, or `Debug` + `KHU_SANITIZE=ON`.
   The build directory is `./build` for the default/Debug profiles and
   `./build-asan` for the sanitizer profile, so the two never collide.
2. Runs `cmake -S . -B <dir> -DCMAKE_BUILD_TYPE=<type>` then
   `cmake --build <dir>`. With `--test` it then runs `ctest`.

### install.sh

1. Determines whether the target prefix is writable, deciding whether `sudo`
   is needed.
2. Looks for a pre-built `build/khudra`. If it is missing (or `--clean` was
   passed) it runs `build.sh`; `--debug` and `--asan` are forwarded through so
   a reinstall can pick a different profile.
3. Creates the install tree under `$PREFIX`.
4. Copies the binary, examples, docs, README and LICENSE into the tree.
5. Writes an install manifest to `$PREFIX/share/khudra/.install_manifest`.
   This file lists every installed path and is consumed by `uninstall.sh`.

The script only uses `sudo` when the target prefix is not writable by the
current user.

### uninstall.sh

1. Reads the install manifest produced by `install.sh`.
2. Removes each listed file.
3. Cleans up any directories left empty, working bottom-up.
4. Removes the local build directories produced by `build.sh` (`./build` and
   `./build-asan`) unless `--keep-build` is passed.

The manifest is removed last so that directory cleanup can inspect the share
tree before it disappears.

## Verifying the installation

```bash
khudra version
khudra --help
khudra run ~/.local/share/khudra/examples/hello.khu   # if installed to ~/.local
```
