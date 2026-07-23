#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    echo "recover_after_timeout.sh must be run as root; use: sudo $0 $*" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cat <<'MSG'
[recover_after_timeout] Recovering from an XDMA stream timeout.
[recover_after_timeout] This is intentionally outside test_fimd_proc; the app
does not unload drivers or reset PCIe while validating the IP.
MSG

"${SCRIPT_DIR}/reload_board_from_flash.sh"
