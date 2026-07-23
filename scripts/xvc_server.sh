#!/usr/bin/env bash
set -euo pipefail

if (( EUID != 0 )); then
    echo "xvc_server.sh must be run as root; use: sudo $0 $*" >&2
    exit 1
fi

DEVICE_INDEX="${DEVICE_INDEX:-0}"
XVC_DEVICE="${XVC_DEVICE:-/dev/xdma${DEVICE_INDEX}_xvc}"
XVC_PORT="${XVC_PORT:-10200}"
XVC_SERVER_BIN="${XVC_SERVER_BIN:-}"
XVC_TEST="${XVC_TEST:-false}"
state_dir="${XDG_RUNTIME_DIR:-/tmp}"
PID_FILE="${PID_FILE:-${state_dir}/xdma${DEVICE_INDEX}_xvc_${XVC_PORT}.pid}"
LOG_FILE="${LOG_FILE:-${state_dir}/xdma${DEVICE_INDEX}_xvc_${XVC_PORT}.log}"

usage() {
    cat <<'USAGE'
Usage: xvc_server.sh {start|stop|restart|status|foreground}

Control AMD's XVC-over-PCIe server for the XDMA XVC character device. The
server bridges TCP XVC traffic to the AXI Debug Bridge exposed by the XDMA
driver (BAR/offset are configured by the driver; this board uses BAR0+0x40000).

Environment:
  DEVICE_INDEX   XDMA index (default: 0)
  XVC_DEVICE     character device (default: /dev/xdmaN_xvc)
  XVC_PORT       TCP port (default: 10200)
  XVC_SERVER_BIN optional explicit xvc_pcie executable
  XVC_TEST       server startup loopback (default: false)
  PID_FILE       optional PID-file override
  LOG_FILE       optional log-file override

XVC_TEST defaults to false because the installed 2018.3 server's loopback
left the current Debug Bridge undiscoverable by Vivado 2025.2 until restart.
USAGE
}

find_server() {
    local candidate
    local invoking_home=""
    local -a candidates=()
    if [[ -n "$XVC_SERVER_BIN" ]]; then
        printf '%s\n' "$XVC_SERVER_BIN"
        return
    fi
    if command -v xvc_pcie >/dev/null 2>&1; then
        command -v xvc_pcie
        return
    fi
    if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "root" ]]; then
        invoking_home="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    fi
    if [[ -n "$invoking_home" ]]; then
        candidates+=("$invoking_home/Downloads/xvcserver/bin/xvc_pcie")
    fi
    candidates+=(
        "$HOME/Downloads/xvcserver/bin/xvc_pcie"
        /usr/local/bin/xvc_pcie
        /usr/bin/xvc_pcie
    )
    for candidate in "${candidates[@]}"; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return
        fi
    done
    echo "cannot find xvc_pcie; set XVC_SERVER_BIN" >&2
    return 1
}

read_live_pid() {
    local pid
    [[ -r "$PID_FILE" ]] || return 1
    pid="$(<"$PID_FILE")"
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    printf '%s\n' "$pid"
}

start_server() {
    local server
    local pid
    if pid="$(read_live_pid)"; then
        echo "xvc_pcie already running: pid=$pid port=$XVC_PORT device=$XVC_DEVICE"
        return
    fi
    [[ -e "$XVC_DEVICE" ]] || {
        echo "missing XVC device: $XVC_DEVICE" >&2
        return 1
    }
    server="$(find_server)"
    rm -f "$PID_FILE"
    nohup "$server" -d "$XVC_DEVICE" -s "TCP::$XVC_PORT" \
        -test "$XVC_TEST" -quiet >"$LOG_FILE" 2>&1 </dev/null &
    pid=$!
    printf '%s\n' "$pid" >"$PID_FILE"
    sleep 0.5
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "xvc_pcie failed to start; log follows:" >&2
        cat "$LOG_FILE" >&2 || true
        rm -f "$PID_FILE"
        return 1
    fi
    echo "xvc_pcie started: pid=$pid url=$(hostname):$XVC_PORT device=$XVC_DEVICE"
    echo "log: $LOG_FILE"
}

stop_server() {
    local pid
    local i
    if ! pid="$(read_live_pid)"; then
        rm -f "$PID_FILE"
        echo "xvc_pcie is not running for pid file $PID_FILE"
        return
    fi
    kill "$pid"
    for ((i = 0; i < 50; ++i)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$PID_FILE"
            echo "xvc_pcie stopped: pid=$pid"
            return
        fi
        sleep 0.1
    done
    echo "timeout stopping xvc_pcie pid=$pid" >&2
    return 1
}

status_server() {
    local pid
    if pid="$(read_live_pid)"; then
        echo "xvc_pcie running: pid=$pid url=$(hostname):$XVC_PORT device=$XVC_DEVICE"
    else
        echo "xvc_pcie not running"
        return 1
    fi
}

case "${1:-}" in
    start)      start_server ;;
    stop)       stop_server ;;
    restart)    stop_server || true; start_server ;;
    status)     status_server ;;
    foreground)
        server="$(find_server)"
        exec "$server" -d "$XVC_DEVICE" -s "TCP::$XVC_PORT" -test "$XVC_TEST" -verbose
        ;;
    -h|--help)  usage ;;
    *)          usage >&2; exit 2 ;;
esac
