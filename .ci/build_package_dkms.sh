#!/bin/bash

# Build a source-only DKMS package from one immutable upstream XDMA revision.
set -euo pipefail

# check if the script is run as root
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root. Please use sudo." ;
    exit 1
fi

# load the architecture string - cross-platform compatible
ARCH="all"

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

###################################
### CONFIGURATION SECTION START ###
###################################

XDMA_VERSION="2025.2"
XDMA_COMMIT="8721136e74a66500b02d16cb41922d966139cd46"

# custom package name
PACKAGE_NAME="mrs-fpga-dkms" ;

# package desc
PACKAGE_DESCRIPTION="Xilinx XDMA DKMS for MRS UAV system" ;

# the final package name
PACKAGE_FILENAME="${PACKAGE_NAME}_${XDMA_VERSION}_${ARCH}.deb"
PACKAGE_FILE="$REPOSITORY_ROOT/$PACKAGE_FILENAME"

# package metadata
PACKAGE_MAINTAINER="Vojtech Vrba <vrba.vojtech@fel.cvut.cz>" ;
PACKAGE_DEPENDS="linux-headers-generic, dkms, udev, build-essential" ;

###################################
###  CONFIGURATION SECTION END  ###
###################################

# stage installation into a package root
WORK_ROOT=$(mktemp -d -t mrs-fpga-dkms-build-XXXXXXXX)
SOURCE_ROOT="$WORK_ROOT/dma_ip_drivers"
PACKAGE_ROOT="$WORK_ROOT/package"

cleanup() {
    rm -rf -- "$WORK_ROOT"
}
trap cleanup EXIT

mkdir -p "$PACKAGE_ROOT/DEBIAN"

# install prerequisites
apt-get -y update
apt-get -y install git debhelper

git init --quiet "$SOURCE_ROOT"
git -C "$SOURCE_ROOT" remote add origin https://github.com/Xilinx/dma_ip_drivers.git
git -C "$SOURCE_ROOT" fetch --depth 1 origin "$XDMA_COMMIT"
git -C "$SOURCE_ROOT" checkout --quiet --detach FETCH_HEAD

RESOLVED_XDMA_COMMIT=$(git -C "$SOURCE_ROOT" rev-parse HEAD)
if [[ "$RESOLVED_XDMA_COMMIT" != "$XDMA_COMMIT" ]]; then
    echo "XDMA source verification failed: expected $XDMA_COMMIT, got $RESOLVED_XDMA_COMMIT" >&2
    exit 1
fi

# create directory for XDMA DKMS sources
XDMA_USR_SRC="/usr/src/xdma-$XDMA_VERSION" ;
mkdir -p "$PACKAGE_ROOT/$XDMA_USR_SRC" ;

# copy the XDMA driver sources
cp "$SOURCE_ROOT"/XDMA/linux-kernel/xdma/* "$PACKAGE_ROOT/$XDMA_USR_SRC/"
cp "$SOURCE_ROOT/XDMA/linux-kernel/include/libxdma_api.h" "$PACKAGE_ROOT/$XDMA_USR_SRC/"

# create the DKMS configuration file
cat <<EOF >>$PACKAGE_ROOT/$XDMA_USR_SRC/dkms.conf
PACKAGE_NAME="xdma"
PACKAGE_VERSION="$XDMA_VERSION"
BUILT_MODULE_NAME[0]="xdma"
DEST_MODULE_LOCATION[0]="/updates"
AUTOINSTALL="yes"
EOF
chmod 644 $PACKAGE_ROOT/$XDMA_USR_SRC/dkms.conf ;

# create a configuration file to load the xdma module on boot
mkdir -p "$PACKAGE_ROOT/etc/modules-load.d" ;
echo xdma | tee $PACKAGE_ROOT/etc/modules-load.d/xdma.conf ;
chmod 644 $PACKAGE_ROOT/etc/modules-load.d/xdma.conf ;

# create a udev rule to set user permissions for the xdma device nodes
mkdir -p "$PACKAGE_ROOT/etc/udev/rules.d" ;
echo 'KERNEL=="xdma*" MODE="0777"' | tee $PACKAGE_ROOT/etc/udev/rules.d/60-xdma.rules ;
chmod 644 $PACKAGE_ROOT/etc/udev/rules.d/60-xdma.rules ;

# create package control file
cat > "$PACKAGE_ROOT/DEBIAN/control" <<EOT
Package: $PACKAGE_NAME
Version: $XDMA_VERSION
Section: admin
Priority: optional
Architecture: $ARCH
Maintainer: $PACKAGE_MAINTAINER
Depends: $PACKAGE_DEPENDS
Description: $PACKAGE_DESCRIPTION
EOT

# mark files under /etc as configuration so upgrades preserve local edits
if [ -d "$PACKAGE_ROOT/etc" ] ; then
    find "$PACKAGE_ROOT/etc" -type f | sed "s#^$PACKAGE_ROOT##" | sort > "$PACKAGE_ROOT/DEBIAN/conffiles" ;
fi

# generate md5sums for package contents
(
    cd "$PACKAGE_ROOT" ;
    find . -type f ! -path './DEBIAN/*' -print0 | xargs -0 md5sum > ./DEBIAN/md5sums ;
) ;


# create pre-installation package script
cat > "$PACKAGE_ROOT/DEBIAN/preinst" <<EOF
#!/bin/sh
set -e

echo "Removing xdma kernel module..."
sudo rmmod xdma 2>/dev/null || true

exit 0
EOF
chmod +x "$PACKAGE_ROOT/DEBIAN/preinst" ;

# create post-installation package script
cat > "$PACKAGE_ROOT/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e

echo "Installing xdma DKMS..."
dkms add -m xdma -v $XDMA_VERSION
dkms build -m xdma -v $XDMA_VERSION
dkms install -m xdma -v $XDMA_VERSION

echo "Loading xdma kernel module..."
depmod -a
modprobe xdma

echo "Reloading udev rules..."
udevadm control --reload-rules
udevadm trigger

exit 0
EOF
chmod +x "$PACKAGE_ROOT/DEBIAN/postinst" ;

# create pre-removal package script
cat > "$PACKAGE_ROOT/DEBIAN/prerm" <<EOF
#!/bin/sh
set -e

if [ "\$1" = "remove" ] || [ "\$1" = "deconfigure" ]; then
	echo "Removing xdma kernel module..."
	sudo rmmod xdma 2>/dev/null || true
	echo "Removing xdma DKMS..."
    dkms remove -m xdma -v $XDMA_VERSION --all
fi

exit 0
EOF
chmod +x "$PACKAGE_ROOT/DEBIAN/prerm" ;

# create post-removal package script to keep DKMS state clean on purge
cat > "$PACKAGE_ROOT/DEBIAN/postrm" <<EOF
#!/bin/sh
set -e

if [ "\$1" = "purge" ]; then
	echo "Removing xdma configuration files..."
    rm -f /etc/modules-load.d/xdma.conf || true
    depmod -a
fi

echo "Reloading udev rules..."
udevadm control --reload-rules
udevadm trigger

exit 0
EOF
chmod +x "$PACKAGE_ROOT/DEBIAN/postrm" ;

# create the new deb package
dpkg-deb --root-owner-group --build "$PACKAGE_ROOT" "$PACKAGE_FILE"

# create a world-readable copy outside private home directories for apt validation
if [ -d /var/tmp ] ; then
    cp "$PACKAGE_FILE" "/var/tmp/$PACKAGE_FILENAME"
    chmod 644 "/var/tmp/$PACKAGE_FILENAME"
fi

echo "" ;
echo "###### FINISHED PACKAGE INFO START ######" ;
stat "$PACKAGE_FILE"
dpkg-deb --info "$PACKAGE_FILE"
if [ -f /var/tmp/$PACKAGE_FILENAME ] ; then
    echo "APT validation copy: /var/tmp/$PACKAGE_FILENAME" ;
fi
echo "###### FINISHED PACKAGE INFO END ######" ;
echo "" ;

# terminate successfully
exit 0
