#!/bin/bash
# GEMIBOOT - Automated Bochs Virtual Machine Test Suite (xHCI USB Boot)
# Dedicated to the Public Domain (CC0)

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

DISK_FILE="build/gemiboot.img"
LOG_BOCHS="/tmp/gemiboot_test_bochs.log"
BOCHS_SIM_LOG="/tmp/gemiboot_bochs_sim.log"
BOCHSRC_TMP="/tmp/gemiboot_bochsrc_test.txt"

echo "=========================================================="
echo " Running GEMIBOOT Bochs Virtual Machine Test Suite"
echo " (Boot Media: USB Drive attached to xHCI Host Controller)"
echo "=========================================================="

# Check if bochs is installed
if ! command -v bochs &> /dev/null; then
    echo "[-] FAILED: bochs is not installed on this system!"
    exit 1
fi

# 1. Build project
echo "[TEST 1/2] Building GEMIBOOT (make clean && make)..."
make clean > /dev/null
make

if [ ! -f "$DISK_FILE" ] || [ ! -f "build/BOOTIA32.EFI" ] || [ ! -f "build/BOOTX64.EFI" ] || [ ! -f "build/mbr.bin" ]; then
    echo "[-] FAILED: Build outputs missing!"
    exit 1
fi
echo "[+] PASSED: All bootloader targets successfully compiled."

# 2. Test xHCI USB Drive Boot in Bochs
echo "[TEST 2/2] Testing xHCI USB Drive Boot in Bochs VM..."
rm -f "$LOG_BOCHS" "$BOCHS_SIM_LOG" "$DISK_FILE.lock"

# Detect BIOS supporting xHCI USB boot (i440fx.bin or SeaBIOS)
BX_BIOS="/usr/share/bochs/i440fx.bin"
if [ ! -f "$BX_BIOS" ] && [ -n "$BXSHARE" ] && [ -f "$BXSHARE/i440fx.bin" ]; then
    BX_BIOS="$BXSHARE/i440fx.bin"
elif [ ! -f "$BX_BIOS" ] && [ -f "/usr/local/share/bochs/i440fx.bin" ]; then
    BX_BIOS="/usr/local/share/bochs/i440fx.bin"
elif [ ! -f "$BX_BIOS" ] && [ -f "/usr/share/seabios/bios-256k.bin" ]; then
    BX_BIOS="/usr/share/seabios/bios-256k.bin"
elif [ ! -f "$BX_BIOS" ] && [ -f "/usr/share/seabios/bios.bin" ]; then
    BX_BIOS="/usr/share/seabios/bios.bin"
fi

BX_VGABIOS="/usr/share/bochs/VGABIOS-lgpl-latest.bin"
if [ ! -f "$BX_VGABIOS" ] && [ -n "$BXSHARE" ] && [ -f "$BXSHARE/VGABIOS-lgpl-latest.bin" ]; then
    BX_VGABIOS="$BXSHARE/VGABIOS-lgpl-latest.bin"
elif [ ! -f "$BX_VGABIOS" ] && [ -f "/usr/local/share/bochs/VGABIOS-lgpl-latest.bin" ]; then
    BX_VGABIOS="/usr/local/share/bochs/VGABIOS-lgpl-latest.bin"
fi

cat << BOCHS_EOF > "$BOCHSRC_TMP"
display_library: nogui
romimage: file=$BX_BIOS, options=fastboot
vgaromimage: file=$BX_VGABIOS
vga: extension=vbe
memory: guest=256, host=256
cpu: count=1, ips=50000000, reset_on_triple_fault=1
pci: enabled=1, chipset=i440fx, slot1=usb_xhci
mouse: enabled=1
clock: sync=none, time0=local

# USB xHCI Controller with Bootable USB Drive, Keyboard, and Mouse
plugin_ctrl: usb_xhci=1
usb_xhci: enabled=1, model=uPD720202, n_ports=6, port1=disk, options1="speed:super, proto:bbb, path:$DISK_FILE", port5=keyboard, port6=mouse
boot: usb

com1: enabled=1, mode=file, dev="$LOG_BOCHS"
log: $BOCHS_SIM_LOG
BOCHS_EOF

# Run Bochs
bochs -q -f "$BOCHSRC_TMP" &
BOCHS_PID=$!

# Wait for xHCI USB boot detection, boot countdown (5s), and RTOS startup
sleep 16
kill -9 $BOCHS_PID 2>/dev/null || true
wait $BOCHS_PID 2>/dev/null || true
rm -f "$DISK_FILE.lock" "$BOCHSRC_TMP"

if [ -f "$LOG_BOCHS" ] && \
   grep -q "GEMIBOOT - Multiboot x86 Boot Loader for GEMIOS (BIOS)" "$LOG_BOCHS" && \
   grep -q "GEMIOS - 32-bit x86 Preemptive Real-Time OS" "$LOG_BOCHS" && \
   grep -q "Found USB xHCI Controller" "$LOG_BOCHS" && \
   grep -q "Bound USB Keyboard" "$LOG_BOCHS" && \
   grep -q "Bound USB Mouse" "$LOG_BOCHS" && \
   grep -q "=== GEMIOS RTOS Interactive Console Ready ===" "$LOG_BOCHS"; then
    echo "[+] PASSED: Bochs xHCI USB Boot loaded gemios.elf and initialized USB Mass Storage, Keyboard, and Mouse on xHCI."
else
    echo "[-] FAILED: Bochs xHCI USB Boot log did not contain expected GEMIOS banner!"
    if [ -f "$LOG_BOCHS" ]; then
        echo "=== Serial Output Log ==="
        cat "$LOG_BOCHS"
    fi
    if [ -f "$BOCHS_SIM_LOG" ]; then
        echo "=== Bochs Simulation Log ==="
        cat "$BOCHS_SIM_LOG"
    fi
    exit 1
fi

echo "=========================================================="
echo " GEMIBOOT BOCHS xHCI USB BOOT TEST PASSED! (100% OK)"
echo "=========================================================="
