#!/bin/bash
set -euo pipefail
export LC_ALL=C

# OEM, cloud and custom-kernel repositories can supply their tracking package.
if [[ -n ${XDMA_HEADER_DEPENDS:-} ]]; then
    if [[ $XDMA_HEADER_DEPENDS == *$'\n'* || $XDMA_HEADER_DEPENDS == *$'\r'* ]]; then
        echo "XDMA_HEADER_DEPENDS must be one Debian dependency field." >&2
        exit 1
    fi
    printf '%s\n' "$XDMA_HEADER_DEPENDS"
    exit 0
fi

source "${1:-/etc/os-release}"
case ${ID:-} in
    ubuntu)
        tracking="linux-headers-generic"
        hwe="linux-headers-generic-hwe-${VERSION_ID:?Missing Ubuntu VERSION_ID}"
        candidate=$(apt-cache policy "$hwe" | awk '/Candidate:/ {print $2; exit}')
        if [[ -n $candidate && $candidate != '(none)' ]]; then
            # An alternative would be satisfied by old GA headers and omit HWE.
            tracking="$hwe, $tracking"
        fi
        printf '%s\n' "$tracking"
        ;;
    debian)
        echo 'linux-headers-amd64 | linux-headers-arm64 | linux-headers'
        ;;
    *)
        echo "Unknown header tracking for ${ID:-unknown}; set XDMA_HEADER_DEPENDS explicitly." >&2
        exit 1
        ;;
esac
