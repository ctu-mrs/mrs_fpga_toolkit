#!/bin/bash

# Build a source-only DKMS package from one immutable upstream XDMA revision.
set -euo pipefail

if [[ $(id -u) != 0 ]]; then
    echo "This script must be run as root. Please use sudo." >&2
    exit 1
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

ARCH="all"
XDMA_VERSION="2025.2"
XDMA_COMMIT="b8466090b4e812e191da9e9305ffb11cb7ace768"
# Keep the public package version equal to the upstream module version.
PACKAGE_VERSION="${DKMS_PACKAGE_VERSION:-$XDMA_VERSION}"
dpkg --validate-version "$PACKAGE_VERSION"
PACKAGE_NAME="mrs-fpga-dkms"
PACKAGE_FILENAME="${PACKAGE_NAME}_${PACKAGE_VERSION}_${ARCH}.deb"
PACKAGE_FILE="$REPOSITORY_ROOT/$PACKAGE_FILENAME"

WORK_ROOT=$(mktemp -d -t mrs-fpga-dkms-build-XXXXXXXX)
SOURCE_ROOT="$WORK_ROOT/dma_ip_drivers"
PACKAGE_ROOT="$WORK_ROOT/package"
trap 'rm -rf -- "$WORK_ROOT"' EXIT
mkdir -p "$PACKAGE_ROOT/DEBIAN"

apt-get -y update
apt-get -y install git debhelper
# Select tracking packages for the target distribution, never the CI host's uname.
HEADER_DEPENDS=$("$SCRIPT_DIR/dkms/header_depends.sh")

git init --quiet "$SOURCE_ROOT"
git -C "$SOURCE_ROOT" remote add origin https://github.com/Xilinx/dma_ip_drivers.git
git -C "$SOURCE_ROOT" fetch --depth 1 origin "$XDMA_COMMIT"
git -C "$SOURCE_ROOT" checkout --quiet --detach FETCH_HEAD
RESOLVED_XDMA_COMMIT=$(git -C "$SOURCE_ROOT" rev-parse HEAD)
if [[ "$RESOLVED_XDMA_COMMIT" != "$XDMA_COMMIT" ]]; then
    echo "XDMA source verification failed: expected $XDMA_COMMIT, got $RESOLVED_XDMA_COMMIT" >&2
    exit 1
fi

XDMA_USR_SRC="/usr/src/xdma-$XDMA_VERSION"
mkdir -p "$PACKAGE_ROOT$XDMA_USR_SRC"
cp "$SOURCE_ROOT"/XDMA/linux-kernel/xdma/* "$PACKAGE_ROOT$XDMA_USR_SRC/"
cp "$SOURCE_ROOT/XDMA/linux-kernel/include/libxdma_api.h" "$PACKAGE_ROOT$XDMA_USR_SRC/"
cat > "$PACKAGE_ROOT$XDMA_USR_SRC/dkms.conf" <<EOF
PACKAGE_NAME="xdma"
PACKAGE_VERSION="$XDMA_VERSION"
BUILT_MODULE_NAME[0]="xdma"
DEST_MODULE_LOCATION[0]="/updates"
AUTOINSTALL="yes"
EOF
chmod 644 "$PACKAGE_ROOT$XDMA_USR_SRC/dkms.conf"

mkdir -p "$PACKAGE_ROOT/etc/modules-load.d" "$PACKAGE_ROOT/etc/udev/rules.d"
echo xdma > "$PACKAGE_ROOT/etc/modules-load.d/xdma.conf"
echo 'KERNEL=="xdma*" MODE="0777"' > "$PACKAGE_ROOT/etc/udev/rules.d/60-xdma.rules"
chmod 644 "$PACKAGE_ROOT/etc/modules-load.d/xdma.conf" "$PACKAGE_ROOT/etc/udev/rules.d/60-xdma.rules"

cat > "$PACKAGE_ROOT/DEBIAN/control" <<EOF
Package: $PACKAGE_NAME
Version: $PACKAGE_VERSION
Section: admin
Priority: optional
Architecture: $ARCH
Maintainer: Vojtech Vrba <vrba.vojtech@fel.cvut.cz>
Depends: dkms (>= 2.1.0.0), $HEADER_DEPENDS, udev, build-essential
Description: Xilinx XDMA DKMS for MRS UAV system
EOF

for script in postinst prerm postrm; do
    sed "s/@XDMA_VERSION@/$XDMA_VERSION/g" "$SCRIPT_DIR/dkms/$script.in" > "$PACKAGE_ROOT/DEBIAN/$script"
    chmod 755 "$PACKAGE_ROOT/DEBIAN/$script"
done
find "$PACKAGE_ROOT/etc" -type f | sed "s#^$PACKAGE_ROOT##" | sort > "$PACKAGE_ROOT/DEBIAN/conffiles"
(
    cd "$PACKAGE_ROOT"
    find . -type f ! -path './DEBIAN/*' -print0 | xargs -0 md5sum > ./DEBIAN/md5sums
)

dpkg-deb --root-owner-group --build "$PACKAGE_ROOT" "$PACKAGE_FILE"
if [[ -d /var/tmp ]]; then
    cp "$PACKAGE_FILE" "/var/tmp/$PACKAGE_FILENAME"
    chmod 644 "/var/tmp/$PACKAGE_FILENAME"
fi
stat "$PACKAGE_FILE"
dpkg-deb --info "$PACKAGE_FILE"
echo "APT validation copy: /var/tmp/$PACKAGE_FILENAME"
