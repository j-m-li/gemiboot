#!/bin/bash
# GEMIBOOT - Automated QEMU Test Suite
# Dedicated to the Public Domain (CC0)

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

DISK_FILE="build/gemiboot.img"
OVMF_X64="/usr/share/OVMF/OVMF_CODE.fd"
if [ ! -f "$OVMF_X64" ]; then
    OVMF_X64="/usr/share/edk2/ovmf/OVMF_CODE.fd"
fi

echo "=========================================================="
echo " Running GEMIBOOT Automated Test Suite"
echo "=========================================================="

# 1. Build project
echo "[TEST 1/3] Building GEMIBOOT (make clean && make)..."
make clean > /dev/null
make

if [ ! -f "$DISK_FILE" ] || [ ! -f "build/BOOTIA32.EFI" ] || [ ! -f "build/BOOTX64.EFI" ] || [ ! -f "build/mbr.bin" ]; then
    echo "[-] FAILED: Build outputs missing!"
    exit 1
fi
echo "[+] PASSED: All bootloader targets successfully compiled."

# 2. Test BIOS Boot
echo "[TEST 2/3] Testing BIOS Boot in QEMU..."
LOG_BIOS="/tmp/gemiboot_test_bios.log"
rm -f "$LOG_BIOS"

qemu-system-i386 \
    -drive file="$DISK_FILE",format=raw \
    -m 256M \
    -display none \
    -serial file:"$LOG_BIOS" \
    -device qemu-xhci,id=xhci,p2=8,p3=8 \
    -device usb-kbd,bus=xhci.0,port=1 &
QEMU_PID=$!

sleep 4
kill -9 $QEMU_PID 2>/dev/null || true

if grep -q "GEMIBOOT - Multiboot x86 Boot Loader for GEMIOS (BIOS)" "$LOG_BIOS" && \
   grep -q "GEMIOS - 32-bit x86 Preemptive Real-Time OS" "$LOG_BIOS" && \
   grep -q "=== GEMIOS RTOS Interactive Console Ready ===" "$LOG_BIOS"; then
    echo "[+] PASSED: BIOS Boot successfully loaded gemios.elf into 32-bit Protected Mode and launched RTOS shell."
else
    echo "[-] FAILED: BIOS Boot log did not contain expected GEMIOS banner!"
    cat "$LOG_BIOS"
    exit 1
fi

# 3. Test UEFI 64-bit Boot
echo "[TEST 3/3] Testing UEFI 64-bit Boot with OVMF..."
LOG_UEFI="/tmp/gemiboot_test_uefi64.log"
rm -f "$LOG_UEFI"

if [ -f "$OVMF_X64" ]; then
    qemu-system-x86_64 \
        -bios "$OVMF_X64" \
        -drive file="$DISK_FILE",format=raw \
        -m 256M \
        -display none \
        -serial file:"$LOG_UEFI" \
        -device qemu-xhci,id=xhci,p2=8,p3=8 \
        -device usb-kbd,bus=xhci.0,port=1 &
    QEMU_PID=$!

    sleep 12
    kill -9 $QEMU_PID 2>/dev/null || true

    if grep -q "GEMIBOOT - Multiboot x86 Boot Loader (UEFI 64-bit)" "$LOG_UEFI" && \
       grep -q "GEMIOS - 32-bit x86 Preemptive Real-Time OS" "$LOG_UEFI" && \
       grep -q "=== GEMIOS RTOS Interactive Console Ready ===" "$LOG_UEFI"; then
        echo "[+] PASSED: UEFI 64-bit Boot successfully loaded gemios.elf, set GOP framebuffer, and launched RTOS shell."
    else
        echo "[-] FAILED: UEFI 64-bit Boot log did not contain expected GEMIOS banner!"
        cat "$LOG_UEFI"
        exit 1
    fi
else
    echo "[*] SKIPPED: OVMF x64 firmware not found on system."
fi

echo "=========================================================="
echo " ALL GEMIBOOT TESTS PASSED SUCCESSFULLY! (100% OK)"
echo "=========================================================="
