#!/bin/bash
# GEMIBOOT - x86 Boot Loader for GEMIOS
# Bochs Virtual Machine Launch Script (Booting from xHCI USB Drive)
# Dedicated to the Public Domain (CC0)

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

DISK_FILE="build/gemiboot.img"
GUI_LIB="x"
DEBUG_MODE=0

for arg in "$@"; do
    if [ "$arg" == "--nogui" ] || [ "$arg" == "--headless" ] || [ "$arg" == "--nographic" ] || [ "$arg" == "-nographic" ]; then
        GUI_LIB="nogui"
    elif [ "$arg" == "--term" ]; then
        GUI_LIB="term"
    elif [ "$arg" == "--sdl2" ] || [ "$arg" == "--sdl" ]; then
        GUI_LIB="sdl2"
    elif [ "$arg" == "--x11" ] || [ "$arg" == "--x" ]; then
        GUI_LIB="x"
    elif [ "$arg" == "--debug" ] || [ "$arg" == "-g" ]; then
        DEBUG_MODE=1
    fi
done

# Check if bochs is installed
if ! command -v bochs &> /dev/null; then
    echo "ERROR: bochs emulator not found in PATH."
    exit 1
fi

# Build bootloader and disk image if needed
if [ ! -f "$DISK_FILE" ]; then
    echo "[*] Building GEMIBOOT and disk image..."
    make
fi

# Clean up stale locks
rm -f "$DISK_FILE.lock"

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

BOCHSRC_GEN="build/bochsrc.runtime.txt"
mkdir -p build

cat << BOCHS_EOF > "$BOCHSRC_GEN"
display_library: $GUI_LIB
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

com1: enabled=1, mode=file, dev="build/bochs_com1.log"
log: build/bochs.log
BOCHS_EOF

echo "=========================================================="
echo " Starting GEMIBOOT for GEMIOS RTOS in Bochs VM"
echo " Configuration:"
echo "   - Boot Media:      USB Drive on xHCI ($DISK_FILE)"
echo "   - Controller:      USB 3.0 xHCI Controller (uPD720202)"
echo "   - Attached USB:    USB Mass Storage, USB Keyboard, USB Mouse"
echo "   - Display GUI:     $GUI_LIB"
echo "   - Log file:        build/bochs.log"
echo "   - Serial COM1:     build/bochs_com1.log"
echo "=========================================================="

exec bochs -q -f "$BOCHSRC_GEN"
