#!/bin/bash
set -e ; # terminate the script if any command fails

# check if the script is run as root
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root. Please use sudo." ;
    exit 1
fi

# load the architecture string - cross-platform compatible
ARCH="all" ;

###################################
### CONFIGURATION SECTION START ###
###################################

XDMA_VERSION="2025.2" ;
XDMA_COMMIT="8721136" ;

# custom package name
PACKAGE_NAME="mrs-fpga-dkms" ;

# package desc
PACKAGE_DESCRIPTION="Xilinx XDMA DKMS for MRS UAV system" ;

# the final package name
PACKAGE_FILENAME=$PACKAGE_NAME"_"$XDMA_VERSION"_"$ARCH".deb" ;

# package metadata
PACKAGE_MAINTAINER="Vojtech Vrba <vrba.vojtech@fel.cvut.cz>" ;
PACKAGE_DEPENDS="linux-headers-generic, dkms, udev, build-essential" ;

###################################
###  CONFIGURATION SECTION END  ###
###################################

# stage installation into a package root
PACKAGE_ROOT=$(mktemp -d) ;
mkdir -p "$PACKAGE_ROOT/DEBIAN" ;

# install prerequisites
apt-get -y update ;
apt-get -y install git debhelper ;

# clone the XDMA driver sources and checkout the specified commit
git clone https://github.com/Xilinx/dma_ip_drivers.git --depth 1 ;

cd dma_ip_drivers ;
git checkout $XDMA_COMMIT ;
cd .. ;

# create directory for XDMA DKMS sources
XDMA_USR_SRC="/usr/src/xdma-$XDMA_VERSION" ;
mkdir -p "$PACKAGE_ROOT/$XDMA_USR_SRC" ;

# copy the XDMA driver sources
cp dma_ip_drivers/XDMA/linux-kernel/xdma/* $PACKAGE_ROOT/$XDMA_USR_SRC/ ;
cp dma_ip_drivers/XDMA/linux-kernel/include/libxdma_api.h $PACKAGE_ROOT/$XDMA_USR_SRC/ ;

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

# remove the repo dir
rm -rf dma_ip_drivers ;

# enable experimental mode and disable all plugins in the packaged systemd unit
BLUETOOTH_SERVICE="$PACKAGE_ROOT/usr/lib/systemd/system/bluetooth.service" ;
if [ ! -f "$BLUETOOTH_SERVICE" ] ; then
    BLUETOOTH_SERVICE="$PACKAGE_ROOT/lib/systemd/system/bluetooth.service" ;
fi
if [ -f "$BLUETOOTH_SERVICE" ] ; then
    sed -Ei '/^[[:space:]]*ExecStart=/ {
        /(^|[[:space:]])-E([[:space:]]|$)/! s#$# -E#
        /(^|[[:space:]])-P[[:space:]]+\*([[:space:]]|$)/! s#$# -P *#
    }' "$BLUETOOTH_SERVICE" ;
fi

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

# create post-installation package script
cat > "$PACKAGE_ROOT/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e

dkms add -m xdma -v $XDMA_VERSION
dkms build -m xdma -v $XDMA_VERSION
dkms install -m xdma -v $XDMA_VERSION

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
    rm -f /etc/modules-load.d/xdma.conf || true
fi

udevadm control --reload-rules
udevadm trigger

exit 0
EOF
chmod +x "$PACKAGE_ROOT/DEBIAN/postrm" ;

# create the new deb package
dpkg-deb --build "$PACKAGE_ROOT" "$PACKAGE_FILENAME" ;

# create a world-readable copy outside private home directories for apt validation
if [ -d /var/tmp ] ; then
    cp ./$PACKAGE_FILENAME /var/tmp/$PACKAGE_FILENAME ;
    chmod 644 /var/tmp/$PACKAGE_FILENAME ;
fi

echo "" ;
echo "###### FINISHED PACKAGE INFO START ######" ;
stat ./$PACKAGE_FILENAME ;
dpkg-deb --info ./$PACKAGE_FILENAME ;
if [ -f /var/tmp/$PACKAGE_FILENAME ] ; then
    echo "APT validation copy: /var/tmp/$PACKAGE_FILENAME" ;
fi
echo "###### FINISHED PACKAGE INFO END ######" ;
echo "" ;

# terminate successfully
exit 0
