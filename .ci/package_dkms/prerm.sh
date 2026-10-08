#!/bin/sh
set -e

case "${1:-}" in
    remove|deconfigure)
        if [ -d /var/lib/dkms/xdma/@XDMA_VERSION@ ]; then
            dkms remove -m xdma -v @XDMA_VERSION@ --all
        fi
        ;;
esac
# Never forcibly unload an in-use module during a package operation.
