#!/usr/bin/env bash
set -euo pipefail

NAME=dsvram
VER=0.1.0
SRC=/usr/src/${NAME}-${VER}

sudo mkdir -p "${SRC}"
sudo cp dsvram_drv.c dsvram_uapi.h Makefile dkms.conf "${SRC}/"

sudo dkms add -m "${NAME}" -v "${VER}"
sudo dkms build -m "${NAME}" -v "${VER}"
sudo dkms install -m "${NAME}" -v "${VER}"

sudo modprobe dsvram_drv

echo "Driver installed."
echo "Device: /dev/dsvram"
echo "For ROCm managed memory on supported AMD GPUs:"
echo "  export HSA_XNACK=1"
