#!/bin/bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
VERSION="1.0.0"
PACKAGE_NAME="mrs-fpga-dev"
PACKAGE_FILE="$REPOSITORY_ROOT/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"
BUILD_ROOT=$(mktemp -d -t mrs-fpga-dev-build-XXXXXXXX)
PACKAGE_ROOT=$(mktemp -d -t mrs-fpga-dev-package-XXXXXXXX)

cleanup() {
  rm -rf -- "$BUILD_ROOT" "$PACKAGE_ROOT"
}
trap cleanup EXIT

# build the development library
cmake \
  -S "$REPOSITORY_ROOT/dev" \
  -B "$BUILD_ROOT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR="lib/$MULTIARCH"
cmake --build "$BUILD_ROOT" --parallel
DESTDIR="$PACKAGE_ROOT" cmake --install "$BUILD_ROOT"

# debian package control file
mkdir -p "$PACKAGE_ROOT/DEBIAN"
cat > "$PACKAGE_ROOT/DEBIAN/control" <<EOF
Package: $PACKAGE_NAME
Version: $VERSION
Section: libdevel
Priority: optional
Architecture: $ARCH
Maintainer: Vojtech Vrba <vrba.vojtech@fel.cvut.cz>
Depends: libc6, libstdc++6
Description: C++ development library for AMD/Xilinx FPGA devices
 Provides XDMA transport, a common AXI IP driver base, and several IP driver implementations.
EOF

(
  cd "$PACKAGE_ROOT"
  find . -type f ! -path './DEBIAN/*' -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums
)

dpkg-deb --root-owner-group --build "$PACKAGE_ROOT" "$PACKAGE_FILE"
dpkg-deb --info "$PACKAGE_FILE"
