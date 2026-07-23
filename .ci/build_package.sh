#!/bin/bash

set -e

trap 'last_command=$current_command; current_command=$BASH_COMMAND' DEBUG
trap 'echo "$0: \"${last_command}\" command failed with exit code $?"' ERR

ARTIFACTS_FOLDER=$1
ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)

mkdir -p "$ARTIFACTS_FOLDER"

echo "$0: building the package for XDMA DKMS"
if [ "$ARCH" = "amd64" ]; then
  sudo ./.ci/build_package_dkms.sh
else
  echo "$0: skipping DKMS duplicate build on architecture $ARCH"
fi

echo "$0: building the package for toolkit"
sudo ./.ci/build_package_toolkit.sh

mv ./*.deb "$ARTIFACTS_FOLDER"

