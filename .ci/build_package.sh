#!/bin/bash
# expects ARTIFACTS_FOLDER path as the first argument ($1)

# build all artifacts
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
mkdir -p "$1"
ARTIFACTS_FOLDER=$(cd -- "$1" && pwd)
ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)
cd "$REPOSITORY_ROOT"

echo "$0: building the package for XDMA DKMS"
if [[ "$ARCH" == "amd64" ]]; then
  sudo "$SCRIPT_DIR/build_package_dkms.sh"
else
  echo "$0: skipping DKMS duplicate build on architecture $ARCH"
fi

echo "$0: building the development and control-utility packages"
"$SCRIPT_DIR/build_package_dev.sh"
"$SCRIPT_DIR/build_package_fpgactl.sh"

if [[ "$ARTIFACTS_FOLDER" != "$REPOSITORY_ROOT" ]]; then
  packages=("$REPOSITORY_ROOT"/*.deb)
  if [[ ! -e "${packages[0]}" ]]; then
    echo "$0: no Debian packages were produced" >&2
    exit 1
  fi
  mv -- "${packages[@]}" "$ARTIFACTS_FOLDER/"
fi
