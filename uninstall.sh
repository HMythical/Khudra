#!/usr/bin/env bash
set -euo pipefail

APP_NAME="khudra"

# ── defaults (must match install.sh) ──────────────────────────────────
PREFIX="${PREFIX:-/usr/local}"
SHARE_DIR="${PREFIX}/share/${APP_NAME}"
MANIFEST="${SHARE_DIR}/.install_manifest"

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

echo ""
info "Uninstall complete. ${REMOVED} file(s) removed."
echo ""
