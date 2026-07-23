#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    echo "program_fpga_over_xvc.sh must be run as root; use: sudo $0 $*" >&2
    exit 1
fi

DEVICE_INDEX="${DEVICE_INDEX:-0}"
XVC_HOST="${XVC_HOST:-127.0.0.1}"
XVC_PORT="${XVC_PORT:-10200}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'USAGE'
Usage: program_fpga_over_xvc.sh IMAGE.bit

Program FPGA SRAM with openFPGALoader through an XVC server.  With the default
local host, the script starts xvc_pcie on /dev/xdmaN_xvc and performs a safe
target-detection preflight.  The current bitstream exposes only the in-fabric
AXI Debug Bridge (IDCODE 0x0a003093), which cannot reconfigure the whole FPGA;
the script therefore fails before programming instead of disrupting XDMA.

An independently hosted, physical-JTAG XVC server can be used by setting
XVC_HOST and XVC_PORT.  That path remains alive while the FPGA is reconfigured,
so the script programs SRAM and then re-enumerates XDMA.

Environment:
  DEVICE_INDEX  XDMA device index for local XVC (default: 0)
  XVC_HOST      XVC server host (default: 127.0.0.1)
  XVC_PORT      XVC server port (default: 10200)
USAGE
}

if (( $# != 1 )); then
    usage >&2
    exit 2
fi
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    exit 0
fi

bitstream="$1"
[[ -f "$bitstream" ]] || {
    echo "bitstream not found: $bitstream" >&2
    exit 1
}
[[ "${bitstream,,}" == *.bit ]] || {
    echo "XVC SRAM programming requires a .bit file: $bitstream" >&2
    exit 1
}
command -v openFPGALoader >/dev/null 2>&1 || {
    echo "openFPGALoader is required but was not found" >&2
    exit 1
}
command -v timeout >/dev/null 2>&1 || {
    echo "timeout is required but was not found" >&2
    exit 1
}

local_xvc=false
case "$XVC_HOST" in
    127.0.0.1|localhost|::1) local_xvc=true ;;
esac

started_local=false
cleanup() {
    if [[ "$started_local" == true ]]; then
        "${SCRIPT_DIR}/xvc_server.sh" stop || true
    fi
}
trap cleanup EXIT

if [[ "$local_xvc" == true ]]; then
    if ! XVC_PORT="$XVC_PORT" "${SCRIPT_DIR}/xvc_server.sh" status >/dev/null 2>&1; then
        DEVICE_INDEX="$DEVICE_INDEX" XVC_PORT="$XVC_PORT" \
            "${SCRIPT_DIR}/xvc_server.sh" start
        started_local=true
    fi
fi

set +e
detect_output="$(timeout 15s openFPGALoader \
    -c xvc-client --ip "$XVC_HOST" --port "$XVC_PORT" --detect --verbose 2>&1)"
detect_status=$?
set -e
printf '%s\n' "$detect_output"

if [[ "$local_xvc" == true ]]; then
    if [[ "$detect_output" == *"0x0a003093"* || "$detect_output" == *"Unknown device"* ]]; then
        echo "ERROR: local XDMA XVC exposes the in-fabric debug_bridge (IDCODE 0x0a003093), not the physical xc7a200t JTAG device." >&2
    else
        echo "ERROR: local XDMA XVC is self-hosted inside the FPGA and cannot remain alive during whole-device reconfiguration." >&2
    fi
    echo "Use physical JTAG, an external physical-JTAG XVC server, or add an ICAP/IPROG field-update controller to the design." >&2
    exit 1
fi

if (( detect_status != 0 )); then
    echo "ERROR: external XVC target detection failed with status $detect_status" >&2
    exit 1
fi

echo "Programming $bitstream through external XVC ${XVC_HOST}:${XVC_PORT}"
openFPGALoader -c xvc-client --ip "$XVC_HOST" --port "$XVC_PORT" \
    --write-sram "$bitstream"

REQUIRED_NODES="${REQUIRED_NODES:-control}" \
    "${SCRIPT_DIR}/reload_xdma.sh" --reenumerate
echo "FPGA programmed through external XVC and XDMA re-enumerated successfully."
