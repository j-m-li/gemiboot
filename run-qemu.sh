#!/bin/bash
# GEMIBOOT - x86 Boot Loader for GEMIOS
# QEMU Virtual Machine Launch Script
# Dedicated to the Public Domain (CC0)

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

DISK_FILE="build/gemiboot.img"
MODE="bios"
GRAPHICS_FLAG="-serial stdio"
HEADLESS=0

for arg in "$@"; do
    if [ "$arg" == "--bios" ]; then
        MODE="bios"
    elif [ "$arg" == "--uefi" ] || [ "$arg" == "--uefi64" ]; then
        MODE="uefi64"
    elif [ "$arg" == "--uefi32" ]; then
        MODE="uefi32"
    elif [ "$arg" == "--nographic" ] || [ "$arg" == "-nographic" ]; then
        GRAPHICS_FLAG="-nographic"
    elif [ "$arg" == "--headless" ]; then
        GRAPHICS_FLAG="-display none -serial stdio"
        HEADLESS=1
    fi
done

# Build bootloader and disk image if needed
if [ ! -f "$DISK_FILE" ]; then
    echo "[*] Building GEMIBOOT and disk image..."
    make
fi

OVMF_X64="/usr/share/OVMF/OVMF_CODE.fd"
if [ ! -f "$OVMF_X64" ]; then
    OVMF_X64="/usr/share/edk2/ovmf/OVMF_CODE.fd"
fi
if [ ! -f "$OVMF_X64" ]; then
    OVMF_X64="/usr/share/qemu/OVMF.fd"
fi

echo "=========================================================="
echo " Starting GEMIBOOT for GEMIOS RTOS in QEMU VM"
echo " Configuration:"
echo "   - Boot Mode:       $MODE"
echo "   - Disk Image:      $DISK_FILE"
echo "   - Host Controller: USB 3.0 xHCI"
echo "   - Devices:         USB Keyboard"
echo "=========================================================="

if [ "$MODE" == "uefi64" ]; then
    if [ ! -f "$OVMF_X64" ]; then
        echo "ERROR: OVMF x64 firmware not found ($OVMF_X64)"
        exit 1
    fi
    exec qemu-system-x86_64 \
        -bios "$OVMF_X64" \
        -drive file="$DISK_FILE",format=raw \
        -m 256M \
        $GRAPHICS_FLAG \
        -device qemu-xhci,id=xhci,p2=8,p3=8 \
        -device usb-kbd,bus=xhci.0,port=1
elif [ "$MODE" == "uefi32" ]; then
    OVMF_IA32="/usr/share/edk2/ovmf-ia32/OVMF_CODE.fd"
    if [ ! -f "$OVMF_IA32" ]; then
        OVMF_IA32="/usr/share/ovmf-ia32/OVMF_CODE.fd"
    fi
    if [ -f "$OVMF_IA32" ]; then
        exec qemu-system-i386 \
            -bios "$OVMF_IA32" \
            -drive file="$DISK_FILE",format=raw \
            -m 256M \
            $GRAPHICS_FLAG \
            -device qemu-xhci,id=xhci,p2=8,p3=8 \
            -device usb-kbd,bus=xhci.0,port=1
    else
        echo "[*] 32-bit OVMF firmware not installed on host. Running 32-bit binary check..."
        file build/BOOTIA32.EFI
        echo "[*] Use --uefi64 or --bios to run in QEMU."
    fi
else
    # Default: Old-fashioned BIOS boot
    exec qemu-system-i386 \
        -drive file="$DISK_FILE",format=raw \
        -m 256M \
        $GRAPHICS_FLAG \
        -device qemu-xhci,id=xhci,p2=8,p3=8 \
        -device usb-kbd,bus=xhci.0,port=1
fi
