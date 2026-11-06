#!/usr/bin/env bash
# install.sh - build cbfix-bridge and install it plus its systemd user unit.
# Idempotent: safe to run repeatedly. Also reverses itself with --uninstall.
set -euo pipefail

UNIT_NAME="cbfix-bridge.service"
BIN_NAME="cbfix-bridge"
BIN_SRC="build/${BIN_NAME}"
BIN_DST="/usr/local/bin/${BIN_NAME}"
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
  - install the systemd user unit to ${SYSTEMD_USER_DIR}
  - enable and start ${UNIT_NAME} for this user

Options:
  --uninstall   Stop and remove everything installed by this script.
  --help        Show this help text and exit.
EOF
}

# Root may use sudo; an unprivileged user without sudo cannot proceed.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "error: need root privileges to install to ${BIN_DST}," >&2
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
    if [ -e "${BIN_DST}" ]; then
        echo "Removing ${BIN_DST}"
        ${SUDO} rm -f "${BIN_DST}"
    fi
    systemctl --user daemon-reload
    echo "Uninstall complete."
}

case "${1-}" in
    --uninstall)
        uninstall
        exit 0
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    "")
        ;;
    *)
        echo "error: unknown option: ${1}" >&2
        usage >&2
        exit 1
        ;;
esac

echo "Building in ${REPO_ROOT}..."
cd "${REPO_ROOT}"
make

echo "Installing binary to ${BIN_DST}"
${SUDO} install -m 0755 "${BIN_SRC}" "${BIN_DST}"

echo "Installing ${UNIT_SRC} to ${SYSTEMD_USER_DIR}"
mkdir -p "${SYSTEMD_USER_DIR}"
install -m 0644 "${UNIT_SRC}" "${UNIT_DST}"

systemctl --user daemon-reload
systemctl --user enable --now "${UNIT_NAME}"

# Verify that the unit really came up.
if [ "$(systemctl --user is-active "${UNIT_NAME}")" = "active" ]; then
    echo "OK: ${UNIT_NAME} is active."
else
    echo "error: ${UNIT_NAME} is not active; check 'journalctl --user -u ${UNIT_NAME}'." >&2
    exit 1
fi

echo
echo "Next step: run 'cbfix-bridge --selftest' to sanity-check the environment"
echo "(a Wayland compositor with ext-data-control-v1 and a reachable \$DISPLAY)."
echo
echo "Optional: if you prefer niri autostart over systemd, add this line to"
echo "~/.config/niri/config.kdl instead of enabling the unit:"
echo '  spawn-at-startup "cbfix-bridge"'
