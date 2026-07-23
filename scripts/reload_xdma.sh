#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    echo "reload_xdma.sh must be run as root; use: sudo $0 $*" >&2
    exit 1
fi

DEVICE_INDEX="${DEVICE_INDEX:-0}"
PCI_BDF="${PCI_BDF:-}"
PCI_VENDOR_ID="${PCI_VENDOR_ID:-0x10ee}"
PCI_DEVICE_ID="${PCI_DEVICE_ID:-}"
MODPROBE_ARGS="${MODPROBE_ARGS:-}"
REQUIRED_NODES="${REQUIRED_NODES-control}"
WAIT_TIMEOUT_SECONDS="${WAIT_TIMEOUT_SECONDS:-30}"
SET_DEVICE_PERMISSIONS="${SET_DEVICE_PERMISSIONS:-1}"
MODE="reenumerate"

usage() {
    cat <<'USAGE'
Usage: reload_xdma.sh [--reenumerate|--driver-only|--probe-only|--remove-only]

Generic Linux XDMA endpoint/driver control. The default post-programming flow
unloads XDMA, removes the stale PCI function, rescans PCIe, reloads XDMA, and
waits for the requested device nodes. It does not assume an application IP,
channel count, BAR register map, MRRS value, or bitstream signature.

Modes:
  --reenumerate  unload, remove, rescan, and reload (default; post-JTAG/XVC)
  --driver-only  unload and reload the driver without PCIe re-enumeration
  --probe-only   rescan and load without unloading or removing anything
  --remove-only  unload and remove, but do not rescan (use before programming)
  --force-unload compatibility alias for --reenumerate

Environment:
  DEVICE_INDEX           XDMA device index (default: 0)
  PCI_BDF                optional endpoint BDF override
  PCI_VENDOR_ID          fallback PCI vendor match (default: 0x10ee)
  PCI_DEVICE_ID          optional fallback PCI device match
  MODPROBE_ARGS          optional extra arguments passed to modprobe xdma
  REQUIRED_NODES         whitespace-separated /dev/xdmaN_ suffixes
                         (default: "control"; empty disables node waiting)
  WAIT_TIMEOUT_SECONDS   endpoint/driver/node timeout (default: 30)
  SET_DEVICE_PERMISSIONS set to 0 to skip chmod a+rw (default: 1)

Example for a design with two H2C streams, one C2H stream, and XVC:
  REQUIRED_NODES="control user xvc h2c_0 h2c_1 c2h_0" reload_xdma.sh

Do not add an automatic PCI bus/function reset here. Some FPGA XDMA designs,
including the validated Artix-7 image, lose their fabric configuration-BAR
signature after a Linux bus reset and require the bitstream to be programmed
again. PCI remove/rescan is the safe post-programming re-enumeration method.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --reenumerate|--force-unload)
            MODE="reenumerate"
            shift
            ;;
        --driver-only)
            MODE="driver-only"
            shift
            ;;
        --probe-only|--rescan-only)
            MODE="probe-only"
            shift
            ;;
        --remove-only)
            MODE="remove-only"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

write_one() {
    local path="$1"
    printf '1\n' >"$path"
}

wait_for_path() {
    local path="$1"
    local start
    start="$(date +%s)"
    while [[ ! -e "$path" ]]; do
        if (( $(date +%s) - start >= WAIT_TIMEOUT_SECONDS )); then
            echo "timeout waiting for $path" >&2
            return 1
        fi
        sleep 0.2
    done
}

wait_for_absence() {
    local path="$1"
    local start
    start="$(date +%s)"
    while [[ -e "$path" ]]; do
        if (( $(date +%s) - start >= WAIT_TIMEOUT_SECONDS )); then
            echo "timeout waiting for removal of $path" >&2
            return 1
        fi
        sleep 0.2
    done
}

normalize_bdf() {
    local bdf="${1,,}"
    if [[ "$bdf" =~ ^[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[[:xdigit:]]$ ]]; then
        printf '0000:%s\n' "$bdf"
    elif [[ "$bdf" =~ ^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[[:xdigit:]]$ ]]; then
        printf '%s\n' "$bdf"
    else
        echo "invalid PCI BDF: $bdf" >&2
        return 1
    fi
}

detect_xdma_bdf() {
    local class_link
    local dev
    local vendor
    local device
    local -a matches=()

    for class_link in \
        "/sys/class/xdma/xdma${DEVICE_INDEX}_control/device" \
        "/sys/class/xdma/xdma${DEVICE_INDEX}_user/device"; do
        if [[ -e "$class_link" ]]; then
            basename "$(readlink -f "$class_link")"
            return 0
        fi
    done

    for dev in /sys/bus/pci/drivers/xdma/*:*; do
        [[ -e "$dev" ]] || continue
        matches+=("$(basename "$dev")")
    done
    if (( ${#matches[@]} == 1 )); then
        printf '%s\n' "${matches[0]}"
        return 0
    elif (( ${#matches[@]} > 1 )); then
        echo "multiple endpoints are bound to xdma; set PCI_BDF explicitly" >&2
        return 1
    fi

    matches=()
    for dev in /sys/bus/pci/devices/*; do
        [[ -r "$dev/vendor" && -r "$dev/device" ]] || continue
        vendor="$(<"$dev/vendor")"
        device="$(<"$dev/device")"
        [[ "${vendor,,}" == "${PCI_VENDOR_ID,,}" ]] || continue
        if [[ -n "$PCI_DEVICE_ID" && "${device,,}" != "${PCI_DEVICE_ID,,}" ]]; then
            continue
        fi
        matches+=("$(basename "$dev")")
    done

    if (( ${#matches[@]} == 1 )); then
        printf '%s\n' "${matches[0]}"
        return 0
    elif (( ${#matches[@]} > 1 )); then
        echo "multiple fallback PCI matches found; set PCI_BDF or PCI_DEVICE_ID" >&2
    else
        echo "cannot find an XDMA PCI endpoint; set PCI_BDF explicitly" >&2
    fi
    return 1
}

show_xdma_dmesg() {
    echo "[reload_xdma] recent xdma dmesg:"
    (dmesg -T 2>/dev/null || true) |
        grep -i xdma | tail -80 || true
}

if [[ -n "$PCI_BDF" ]]; then
    PCI_BDF="$(normalize_bdf "$PCI_BDF")"
else
    PCI_BDF="$(detect_xdma_bdf)"
fi

endpoint_path="/sys/bus/pci/devices/$PCI_BDF"
if [[ ! -e "$endpoint_path" ]]; then
    echo "PCIe endpoint does not exist: $PCI_BDF" >&2
    exit 1
fi

echo "[reload_xdma] mode:         $MODE"
echo "[reload_xdma] device index: $DEVICE_INDEX"
echo "[reload_xdma] endpoint:     $PCI_BDF"

if [[ "$MODE" == "reenumerate" || "$MODE" == "driver-only" || "$MODE" == "remove-only" ]]; then
    if [[ -d /sys/module/xdma ]]; then
        echo "[reload_xdma] unloading xdma"
        modprobe -r xdma
    fi
fi

if [[ "$MODE" == "reenumerate" || "$MODE" == "remove-only" ]]; then
    echo "[reload_xdma] removing endpoint $PCI_BDF"
    write_one "$endpoint_path/remove"
    wait_for_absence "$endpoint_path"
fi

if [[ "$MODE" == "remove-only" ]]; then
    echo "[reload_xdma] endpoint removed; program the FPGA before rescanning"
    exit 0
fi

if [[ "$MODE" == "reenumerate" || "$MODE" == "probe-only" ]]; then
    echo "[reload_xdma] rescanning PCIe bus"
    write_one /sys/bus/pci/rescan
    wait_for_path "$endpoint_path"
fi

echo "[reload_xdma] loading xdma ${MODPROBE_ARGS}"
if [[ -n "$MODPROBE_ARGS" ]]; then
    # shellcheck disable=SC2086
    modprobe xdma ${MODPROBE_ARGS}
else
    modprobe xdma
fi

wait_for_path "$endpoint_path/driver"
bound_driver="$(basename "$(readlink -f "$endpoint_path/driver")")"
if [[ "$bound_driver" != "xdma" ]]; then
    echo "endpoint bound to unexpected driver: $bound_driver" >&2
    show_xdma_dmesg
    exit 1
fi

for suffix in $REQUIRED_NODES; do
    wait_for_path "/dev/xdma${DEVICE_INDEX}_${suffix}"
done

if [[ "$SET_DEVICE_PERMISSIONS" != "0" ]]; then
    chmod a+rw "/dev/xdma${DEVICE_INDEX}"_* 2>/dev/null || true
fi

if [[ -r "$endpoint_path/current_link_speed" && -r "$endpoint_path/current_link_width" ]]; then
    echo "[reload_xdma] PCIe link: $(<"$endpoint_path/current_link_speed") x$(<"$endpoint_path/current_link_width")"
fi
echo "[reload_xdma] ready nodes:"
ls -l "/dev/xdma${DEVICE_INDEX}"_* 2>/dev/null || true
show_xdma_dmesg
