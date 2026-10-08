#!/bin/sh
set -e

case "${1:-}" in
    remove|purge)
        # Also handle an interrupted removal or a package that was never configured.
        if command -v dkms >/dev/null 2>&1 && [ -d /var/lib/dkms/xdma/@XDMA_VERSION@ ]; then
            dkms remove -m xdma -v @XDMA_VERSION@ --all
        fi
        if command -v udevadm >/dev/null 2>&1 && udevadm control --reload-rules 2>/dev/null; then
            udevadm trigger --subsystem-match=xdma || \
                echo "Warning: xdma udev trigger failed." >&2
        fi
        ;;
esac
# dpkg owns the conffiles. In particular, upgrade must not remove new DKMS state.
