#!/bin/bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)
PACKAGE_NAME="mrs-fpgactl"
WORK_ROOT=$(mktemp -d -t mrs-fpgactl-build-XXXXXXXX)
PACKAGE_ROOT=$(mktemp -d -t mrs-fpgactl-package-XXXXXXXX)
SDK_ROOT="$WORK_ROOT/sdk"

cleanup() {
  rm -rf -- "$WORK_ROOT" "$PACKAGE_ROOT"
}
trap cleanup EXIT

# find the newest build dev package and link against it
shopt -s nullglob
DEV_PACKAGES=("$REPOSITORY_ROOT"/mrs-fpga-dev_*_"$ARCH".deb)
shopt -u nullglob
if [[ ${#DEV_PACKAGES[@]} -eq 0 ]]; then
  echo "missing mrs-fpga-dev package for $ARCH; run .ci/build_package_dev.sh first" >&2
  exit 1
fi
DEV_PACKAGE=${DEV_PACKAGES[0]}
for CANDIDATE in "${DEV_PACKAGES[@]:1}"; do
  if [[ "$CANDIDATE" -nt "$DEV_PACKAGE" ]]; then
    DEV_PACKAGE=$CANDIDATE
  fi
done

# dev package version, arch etc.
DEV_PACKAGE_NAME=$(dpkg-deb --field "$DEV_PACKAGE" Package)
DEV_PACKAGE_ARCH=$(dpkg-deb --field "$DEV_PACKAGE" Architecture)
VERSION=$(dpkg-deb --field "$DEV_PACKAGE" Version)
if [[ "$DEV_PACKAGE_NAME" != "mrs-fpga-dev" || "$DEV_PACKAGE_ARCH" != "$ARCH" || -z "$VERSION" ]]; then
  echo "invalid development package metadata in $DEV_PACKAGE" >&2
  exit 1
fi
PACKAGE_FILE="$REPOSITORY_ROOT/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"

dpkg-deb -x "$DEV_PACKAGE" "$SDK_ROOT"

# build the fpgactl tool
cmake \
  -S "$REPOSITORY_ROOT/fpgactl" \
  -B "$WORK_ROOT/fpgactl-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DMRS_FPGACTL_VERSION="$VERSION" \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH="$SDK_ROOT/usr" \
  -DCMAKE_INSTALL_RPATH=""
cmake --build "$WORK_ROOT/fpgactl-build" --parallel
DESTDIR="$PACKAGE_ROOT" cmake --install "$WORK_ROOT/fpgactl-build"

# control file for the deb package
mkdir -p "$PACKAGE_ROOT/DEBIAN"
cat > "$PACKAGE_ROOT/DEBIAN/control" <<EOF
Package: $PACKAGE_NAME
Version: $VERSION
Section: admin
Priority: optional
Architecture: $ARCH
Maintainer: Vojtech Vrba <vrba.vojtech@fel.cvut.cz>
Depends: mrs-fpga-dev (= $VERSION), libc6, libstdc++6, kmod, coreutils, openfpgaloader
Description: Control utility for AMD/Xilinx FPGA devices
 Provides PCIe link, XDMA driver and USB JTAG controls for FPGA configuration and management.
EOF

(
  cd "$PACKAGE_ROOT"
  find . -type f ! -path './DEBIAN/*' -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums
)

dpkg-deb --root-owner-group --build "$PACKAGE_ROOT" "$PACKAGE_FILE"
dpkg-deb --info "$PACKAGE_FILE"
