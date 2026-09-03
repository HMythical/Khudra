#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_NAME="khudra"

# Removes the installed khudra files (from the install manifest) and, by
# default, the local build directories that build.sh produces. Pass
# --keep-build to leave ./build and ./build-asan in the source tree.

# ── defaults (must match install.sh) ──────────────────────────────────
PREFIX="${PREFIX:-/usr/local}"
SHARE_DIR="${PREFIX}/share/${APP_NAME}"
MANIFEST="${SHARE_DIR}/.install_manifest"
KEEP_BUILD=0

# ── arguments ─────────────────────────────────────────────────────────
for arg in "$@"; do
    case "$arg" in
        --keep-build) KEEP_BUILD=1 ;;
        -h|--help)
            echo "Usage: $0 [--keep-build]"
            echo "  --keep-build   leave the local build dirs (./build, ./build-asan) behind"
            echo "                 instead of removing them"
            exit 0
            ;;
        *) warn "Ignoring unknown option: $arg (try --help)" ;;
    esac
done

# ── colours ───────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RESET='\033[0m'

info()  { printf "${GREEN}[INFO]${RESET}  %s\n" "$*"; }
warn()  { printf "${YELLOW}[WARN]${RESET}  %s\n" "$*"; }
error() { printf "${RED}[ERROR]${RESET} %s\n" "$*" >&2; exit 1; }

# ── pre-flight checks ────────────────────────────────────────────────
if [[ $EUID -eq 0 ]]; then
    SUDO=""
elif [[ -w "$PREFIX" ]]; then
    SUDO=""
else
    SUDO="sudo"
fi

if [[ ! -f "$MANIFEST" ]]; then
    error "Install manifest not found at ${MANIFEST}. Was ${APP_NAME} installed with install.sh?"
fi

# ── uninstall ─────────────────────────────────────────────────────────
info "Uninstalling ${APP_NAME} (manifest: ${MANIFEST})"

REMOVED=0
while IFS= read -r filepath; do
    [[ -z "$filepath" ]] && continue
    if [[ -f "$filepath" ]]; then
        $SUDO rm -f "$filepath"
        info "Removed ${filepath}"
        REMOVED=$((REMOVED + 1))
    else
        warn "Already removed: ${filepath}"
    fi
done < "$MANIFEST"

# ── remove manifest ───────────────────────────────────────────────────
$SUDO rm -f "$MANIFEST"

# ── clean up empty directories (bottom-up) ────────────────────────────
BIN_DIR="${PREFIX}/bin"
for dir in "${SHARE_DIR}/docs" "${SHARE_DIR}/examples" "${SHARE_DIR}" "${SHARE_DIR%/*}" "${BIN_DIR}"; do
    if [[ -d "$dir" ]] && [[ -z "$(ls -A "$dir")" ]]; then
        $SUDO rmdir "$dir"
        info "Removed empty directory ${dir}"
    fi
done

# ── clean up the local build directories (from build.sh) ──────────────
# These are source-tree artifacts, separate from the installed binary in the
# manifest above. Removed by default so a teardown is complete; kept with
# --keep-build so a user's compiled objects survive.
if [[ "$KEEP_BUILD" -eq 0 ]]; then
    for bdir in "${SCRIPT_DIR}/build" "${SCRIPT_DIR}/build-asan"; do
        if [[ -d "$bdir" ]]; then
            $SUDO rm -rf "$bdir"
            info "Removed local build directory ${bdir}"
        fi
    done
else
    warn "Keeping local build directories (--keep-build)"
fi

echo ""
info "Uninstall complete. ${REMOVED} file(s) removed."
echo ""
