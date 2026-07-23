#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    echo "set_pcie_mrrs.sh must be run as root; use: sudo $0 $*" >&2
    exit 1
fi

DEVICE_INDEX="${DEVICE_INDEX:-0}"
MRRS_BYTES="${1:-${MRRS_BYTES:-1024}}"

usage() {
    cat <<'USAGE'
Usage: set_pcie_mrrs.sh [512|1024]

Set and verify the PCIe Max Read Request Size used by the XDMA endpoint.
The uav42 dual-128 board passed full parity at both values; 1024 improved
large simultaneous H2C transfers. MRRS 4096 is intentionally rejected because
the real board produced XDMA MAGIC_STOPPED and timeout errors at that setting.

Environment:
  DEVICE_INDEX  XDMA index used for automatic BDF detection (default 0)
  PCI_BDF       optional explicit endpoint BDF override
  MRRS_BYTES    value used when no positional argument is supplied
USAGE
}

case "$MRRS_BYTES" in
    512)  mrrs_bits=2000 ;;
    1024) mrrs_bits=3000 ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "unsupported MRRS: $MRRS_BYTES (validated values: 512 or 1024)" >&2
        usage >&2
        exit 2
        ;;
esac

if [[ -z "${PCI_BDF:-}" ]]; then
    xdma_device_link="/sys/class/xdma/xdma${DEVICE_INDEX}_user/device"
    if [[ ! -e "$xdma_device_link" ]]; then
        echo "cannot auto-detect PCI BDF: missing $xdma_device_link" >&2
        exit 1
    fi
    PCI_BDF="$(basename "$(readlink -f "$xdma_device_link")")"
fi

if [[ ! -e "/sys/bus/pci/devices/$PCI_BDF/config" ]]; then
    echo "PCIe endpoint does not exist: $PCI_BDF" >&2
    exit 1
fi

before="$(setpci -s "$PCI_BDF" CAP_EXP+8.w)"
setpci -s "$PCI_BDF" "CAP_EXP+8.w=${mrrs_bits}:7000"
after="$(setpci -s "$PCI_BDF" CAP_EXP+8.w)"

expected_masked=$((16#$mrrs_bits & 16#7000))
actual_masked=$((16#$after & 16#7000))
if (( actual_masked != expected_masked )); then
    echo "MRRS verification failed: requested=$MRRS_BYTES DeviceControl=0x$after" >&2
    exit 1
fi

echo "PCIe MRRS updated: BDF=$PCI_BDF bytes=$MRRS_BYTES DeviceControl=0x$before->0x$after"
lspci -s "$PCI_BDF" -vv | grep -A2 'DevCtl:' || true
