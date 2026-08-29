#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_NAME="khudra"
VERSION="0.1.0-dev"

# ── defaults ──────────────────────────────────────────────────────────
PREFIX="${PREFIX:-/usr/local}"
BIN_DIR="${PREFIX}/bin"
SHARE_DIR="${PREFIX}/share/${APP_NAME}"
EXAMPLES_DIR="${SHARE_DIR}/examples"
DOCS_DIR="${SHARE_DIR}/docs"
MANIFEST="${SHARE_DIR}/.install_manifest"

# ── colours ───────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
RESET='\033[0m'

info()  { printf "${GREEN}[INFO]${RESET}  %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${RESET}  %s\n" "$*"; }
error() { printf "${RED}[ERROR]${RESET} %s\n" "$*" >&2; exit 1; }

# ── pre-flight checks ────────────────────────────────────────────────
if [[ $EUID -eq 0 ]]; then
    SUDO=""
elif [[ -w "$PREFIX" ]] || mkdir -p "$PREFIX" 2>/dev/null; then
    # User owns the prefix (or can create it) – no sudo needed
    SUDO=""
else
    SUDO="sudo"
fi

# ── build ─────────────────────────────────────────────────────────────
BINARY="${SCRIPT_DIR}/build/khudra"
if [[ ! -f "$BINARY" ]]; then
    info "No pre-built binary found. Building ${APP_NAME}..."
    command -v cmake >/dev/null 2>&1 || error "cmake is required but not installed."
    command -v make  >/dev/null 2>&1 || error "make is required but not installed."
    cmake -S "$SCRIPT_DIR" -B "${SCRIPT_DIR}/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${SCRIPT_DIR}/build" -j"$(nproc)"
    info "Build complete."
fi

[[ -f "$BINARY" ]] || error "Build failed – binary not found at ${BINARY}"

# ── create directories ────────────────────────────────────────────────
info "Installing ${APP_NAME} ${VERSION} to ${PREFIX}"
$SUDO mkdir -p "$BIN_DIR"
$SUDO mkdir -p "$EXAMPLES_DIR"
$SUDO mkdir -p "$DOCS_DIR"

# ── install binary ────────────────────────────────────────────────────
$SUDO install -m 755 "$BINARY" "${BIN_DIR}/${APP_NAME}"
info "Installed binary  → ${BIN_DIR}/${APP_NAME}"

# ── install manifest (used by uninstall.sh) ───────────────────────────
mkdir -p "$(dirname "$MANIFEST")"
echo "${BIN_DIR}/${APP_NAME}" | $SUDO tee "$MANIFEST" >/dev/null

# ── install examples ──────────────────────────────────────────────────
if [[ -d "${SCRIPT_DIR}/examples" ]]; then
    for f in "${SCRIPT_DIR}"/examples/*.khu; do
        [[ -f "$f" ]] || continue
        $SUDO install -m 644 "$f" "${EXAMPLES_DIR}/$(basename "$f")"
        echo "${EXAMPLES_DIR}/$(basename "$f")" | $SUDO tee -a "$MANIFEST" >/dev/null
    done
    info "Installed examples → ${EXAMPLES_DIR}/"
fi

# ── install docs ──────────────────────────────────────────────────────
if [[ -d "${SCRIPT_DIR}/docs" ]]; then
    for f in "${SCRIPT_DIR}"/docs/*.md; do
        [[ -f "$f" ]] || continue
        $SUDO install -m 644 "$f" "${DOCS_DIR}/$(basename "$f")"
        echo "${DOCS_DIR}/$(basename "$f")" | $SUDO tee -a "$MANIFEST" >/dev/null
    done
    info "Installed docs    → ${DOCS_DIR}/"
fi

# ── install README & LICENSE ──────────────────────────────────────────
for f in README.md LICENSE; do
    if [[ -f "${SCRIPT_DIR}/${f}" ]]; then
        $SUDO install -m 644 "${SCRIPT_DIR}/${f}" "${SHARE_DIR}/${f}"
        echo "${SHARE_DIR}/${f}" | $SUDO tee -a "$MANIFEST" >/dev/null
    fi
done

echo "" | $SUDO tee -a "$MANIFEST" >/dev/null

# ── done ──────────────────────────────────────────────────────────────
echo ""
info "Installation complete."
info "Run '${APP_NAME} --help' to get started."
echo ""
