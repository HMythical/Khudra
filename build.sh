#!/usr/bin/env bash
#
# build.sh -- build the Khudra compiler and standard library.
#
# A thin, repeatable wrapper over cmake with a few common profiles, so a build
# is one command whether you are on a fresh checkout or cleaning up after a
# test run. install.sh calls this when it needs a binary that is not there yet,
# which keeps the "how to build" knowledge in one place.
#
# Usage:
#   ./build.sh                 Release build in ./build
#   ./build.sh --debug         Debug build in ./build (the CI default profile)
#   ./build.sh --asan          Debug + Address/UndefinedBehavior sanitizers in ./build-asan
#   ./build.sh --test          run the test suite after building
#   ./build.sh --clean         remove the generated build directory first
#   ./build.sh --help          show this help
#
# The build directory is named after the profile so Release (./build) and the
# sanitizer build (./build-asan) never collide, matching the layout README.md
# and CONTRIBUTING.md document.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_NAME="khudra"

# ── defaults ──────────────────────────────────────────────────────────
BUILD_TYPE="Release"
BUILD_DIR="${SCRIPT_DIR}/build"
EXTRA_FLAGS=()
RUN_TESTS=0
CLEAN_FIRST=0

# ── colours ───────────────────────────────────────────────────────────
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
RESET='\033[0m'

info()  { printf "${GREEN}[INFO]${RESET}  %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${RESET}  %s\n" "$*"; }
error() { printf "${RED}[ERROR]${RESET} %s\n" "$*" >&2; exit 1; }

usage() {
    sed -nE '2,/^$/p' "${BASH_SOURCE[0]}" | sed -E 's/^# ?//'
}

# ── arguments ─────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --asan)
            BUILD_TYPE="Debug"
            BUILD_DIR="${SCRIPT_DIR}/build-asan"
            EXTRA_FLAGS+=("-DKHU_SANITIZE=ON")
            shift
            ;;
        --test)
            RUN_TESTS=1
            shift
            ;;
        --clean)
            CLEAN_FIRST=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            error "Unknown option: $1 (try --help)"
            ;;
    esac
done

# ── pre-flight checks ────────────────────────────────────────────────
command -v cmake >/dev/null 2>&1 || error "cmake is required but not installed (see CONTRIBUTING.md)."
command -v make  >/dev/null 2>&1 || error "make is required but not installed (see CONTRIBUTING.md)."

# ── clean (optional) ──────────────────────────────────────────────────
if [[ "$CLEAN_FIRST" -eq 1 ]]; then
    if [[ -d "$BUILD_DIR" ]]; then
        info "Removing previous build at ${BUILD_DIR}"
        rm -rf "$BUILD_DIR"
    else
        warn "No existing build at ${BUILD_DIR} to clean."
    fi
fi

# ── configure & build ─────────────────────────────────────────────────
info "Configuring ${APP_NAME} (${BUILD_TYPE}) in ${BUILD_DIR}"
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    "${EXTRA_FLAGS[@]}"

info "Building ${APP_NAME}..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

BINARY="${BUILD_DIR}/${APP_NAME}"
[[ -f "$BINARY" ]] || error "Build finished but binary not found at ${BINARY}"
info "Build complete → ${BINARY}"

# ── test (optional) ───────────────────────────────────────────────────
if [[ "$RUN_TESTS" -eq 1 ]]; then
    info "Running the test suite..."
    if [[ "$BUILD_TYPE" == "Debug" ]] && [[ "${EXTRA_FLAGS[*]:-}" == *KHU_SANITIZE* ]]; then
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
            ctest --test-dir "$BUILD_DIR" --output-on-failure
    else
        ctest --test-dir "$BUILD_DIR" --output-on-failure
    fi
fi

info "Done."
