#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    echo "reload_board_from_flash.sh must be run as root; use: sudo $0 $*" >&2
    exit 1
fi

FPGA_RELOAD_CMD="${FPGA_RELOAD_CMD:-}"
SETTLE_SECONDS="${SETTLE_SECONDS:-3}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'USAGE'
Usage: reload_board_from_flash.sh

Ask the already-flashed FPGA design to re-enumerate on PCIe, then reload XDMA.
This does not program a new bitstream into flash. The script only removes the
current PCIe endpoint when FPGA_RELOAD_CMD is set; without that platform hook it
limits itself to a PCIe rescan plus XDMA probe/reload.

Environment:
  FPGA_RELOAD_CMD   optional command run between PCIe remove and rescan
  SETTLE_SECONDS    delay before PCIe rescan after reload hook (default: 3)
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

write_sysfs_one() {
    local path="$1"
    printf '1\n' >"$path"
}

find_xdma_bdf() {
    local dev
    for dev in /sys/bus/pci/drivers/xdma/*:*; do
        [[ -e "$dev" ]] || continue
        basename "$dev"
        return 0
    done
    lspci -D -n | awk '$0 ~ /10ee:7011/ {print $1; exit}'
}

if [[ -n "$FPGA_RELOAD_CMD" ]]; then
    BDF="$(find_xdma_bdf || true)"
    if [[ -n "$BDF" && -e "/sys/bus/pci/devices/${BDF}/remove" ]]; then
        echo "[reload_board_from_flash] removing PCIe function ${BDF}"
        write_sysfs_one "/sys/bus/pci/devices/${BDF}/remove"
    fi
    echo "[reload_board_from_flash] running FPGA_RELOAD_CMD"
    bash -lc "$FPGA_RELOAD_CMD"
else
    echo "[reload_board_from_flash] no FPGA_RELOAD_CMD set; skipping PCIe remove and performing rescan/probe only"
fi

sleep "$SETTLE_SECONDS"

echo "[reload_board_from_flash] rescanning PCIe bus"
write_sysfs_one /sys/bus/pci/rescan

sleep "$SETTLE_SECONDS"

"${SCRIPT_DIR}/reload_xdma.sh"
