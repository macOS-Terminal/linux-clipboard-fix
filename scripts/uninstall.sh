#!/usr/bin/env bash
# uninstall.sh - remove cbfix-bridge and its systemd user unit.
# Thin wrapper: all logic lives in install.sh --uninstall.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

exec "${SCRIPT_DIR}/install.sh" --uninstall
