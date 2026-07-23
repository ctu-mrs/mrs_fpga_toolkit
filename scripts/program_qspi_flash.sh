#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    echo "program_qspi_flash.sh must be run as root; use: sudo $0 $*" >&2
    exit 1
fi

DEVICE_INDEX="${DEVICE_INDEX:-0}"
QSPI_BASE="${QSPI_BASE:-0x10000}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'USAGE'
Usage:
  program_qspi_flash.sh --identify
  program_qspi_flash.sh IMAGE.bit --yes
  program_qspi_flash.sh IMAGE.bin --yes

Identify or program the boot flash through /dev/xdmaN_user and the AXI Quad
SPI controller.  IMAGE.bit is parsed locally and its Xilinx header is removed;
IMAGE.bin must be a raw Xilinx configuration image.  Programming erases only
the 64-KiB sectors occupied by the image, verifies every changed sector, and
performs a final byte-for-byte verification.

The --yes flag is mandatory for an erase/program operation.  The current FPGA
design has no ICAP/IPROG reload controller, so power-cycle the board after a
successful flash update.

Environment:
  DEVICE_INDEX  XDMA device index (default: 0)
  QSPI_BASE     AXI Quad SPI offset in user BAR (default: 0x10000)
USAGE
}

if (( $# == 0 )); then
    usage >&2
    exit 2
fi
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

exec python3 "${SCRIPT_DIR}/axi_qspi_flash.py" \
    --device "/dev/xdma${DEVICE_INDEX}_user" \
    --base "$QSPI_BASE" \
    "$@"
