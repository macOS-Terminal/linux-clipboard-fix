#!/usr/bin/env bash
# install.sh - build cbfix-bridge and install it plus its systemd user unit.
# Idempotent: safe to run repeatedly. Also reverses itself with --uninstall.
set -euo pipefail

UNIT_NAME="cbfix-bridge.service"
BIN_NAME="cbfix-bridge"
BIN_SRC="build/${BIN_NAME}"
BIN_DST="/usr/bin/${BIN_NAME}"
LEGACY_BIN_DST="/usr/local/bin/${BIN_NAME}"
# The unit file is installed into the calling user's systemd user directory.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "${SCRIPT_DIR}")"
UNIT_SRC="${REPO_ROOT}/systemd/${UNIT_NAME}"
SYSTEMD_USER_DIR="${HOME}/.config/systemd/user"
UNIT_DST="${SYSTEMD_USER_DIR}/${UNIT_NAME}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--uninstall] [--help]

Build and install the cbfix-bridge clipboard bridge:
  - build the binary with make
  - install it to ${BIN_DST}
  - install the systemd user unit to ${UNIT_DST}
  - reload the user systemd daemon (systemctl --user daemon-reload)
  - print the highlighted post-install notice (from packaging/notice.sh)

Options:
  --uninstall   Stop/disable ${UNIT_NAME} and remove ${UNIT_DST},
                ${BIN_DST}, and legacy ${LEGACY_BIN_DST}.
  -h, --help    Show this help text and exit.
EOF
}

case "${1-}" in
    --help|-h)
        usage
        exit 0
        ;;
    --uninstall|"")
        ;;
    *)
        echo "error: unknown option: ${1}" >&2
        usage >&2
        exit 1
        ;;
esac

# Root may run directly; an unprivileged user uses sudo for /usr/bin.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "error: need root privileges to write ${BIN_DST}," >&2
        echo "but sudo is not available. Re-run this script as root," >&2
        echo "or install ${BIN_DST} manually." >&2
        exit 1
    fi
fi

uninstall() {
    echo "Stopping and disabling ${UNIT_NAME} (ignoring errors if not present)..."
    systemctl --user stop "${UNIT_NAME}" 2>/dev/null || true
    systemctl --user disable "${UNIT_NAME}" 2>/dev/null || true
    if [ -e "${UNIT_DST}" ]; then
        echo "Removing ${UNIT_DST}"
        rm -f "${UNIT_DST}"
    fi
    for bin in "${BIN_DST}" "${LEGACY_BIN_DST}"; do
        if [ -e "${bin}" ]; then
            echo "Removing ${bin}"
            ${SUDO} rm -f "${bin}"
        fi
    done
    systemctl --user daemon-reload || true
    echo "Uninstall complete."
}

if [ "${1-}" = "--uninstall" ]; then
    uninstall
    exit 0
fi

echo "Building in ${REPO_ROOT}..."
cd "${REPO_ROOT}"
make

echo "Installing binary to ${BIN_DST}"
${SUDO} install -Dm 0755 "${BIN_SRC}" "${BIN_DST}"

echo "Installing ${UNIT_SRC} to ${UNIT_DST}"
mkdir -p "${SYSTEMD_USER_DIR}"
install -m 0644 "${UNIT_SRC}" "${UNIT_DST}"

systemctl --user daemon-reload || true

# shellcheck source=../packaging/notice.sh
source "${REPO_ROOT}/packaging/notice.sh"
cbfix_notice install
