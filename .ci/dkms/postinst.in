#!/bin/sh
set -e

case "${1:-}" in
    configure) ;;
    *) exit 0 ;;
esac

# DKMS owns registration, replacement, signing, depmod and kernel selection.
# Build for all available installed kernels so rollback boots use PCIe XDMA too.
# Missing headers are reported by the helper; actual build errors still fail apt.
echo "Installing xdma DKMS..."
autoinstall_all_kernels=yes /usr/lib/dkms/common.postinst \
    xdma @XDMA_VERSION@ /usr/share/mrs-fpga-dkms "" "${2:-}"

# Do not unload a live driver on upgrade. Runtime activation is best effort:
# package installation must also work in a chroot or with Secure Boot restrictions.
if [ -d "/lib/modules/$(uname -r)" ] && [ ! -f /etc/dkms/no-autoinstall ]; then
    if ! modprobe xdma; then
        echo "Warning: xdma installed but could not be loaded; check Secure Boot and modprobe diagnostics." >&2
    elif [ -d /sys/module/xdma ] && [ ! -f /sys/module/xdma/version ]; then
        echo "Warning: the platform xdma driver is already loaded; reboot to select the installed PCIe XDMA driver." >&2
    fi
fi

if command -v udevadm >/dev/null 2>&1 && udevadm control --reload-rules 2>/dev/null; then
    udevadm trigger --subsystem-match=xdma || \
        echo "Warning: xdma udev trigger failed; rules will apply on the next device event." >&2
fi
