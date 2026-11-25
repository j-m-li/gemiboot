<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oksinix

A 32-bit x86 operating system kernel written in strict ISO C90 (`-std=c89 -pedantic -Wall -Wextra -Werror`), compiled with `clang` and linked with `ld.lld`. Dedicated to the public domain under **CC0 1.0 Universal**.

## Features

- **BIOS MBR Hybrid Boot Image (`oksinix.img`, `boot/mbr.S`) & Multiboot v1 ELF (`oksinix.elf`, `boot/boot.S`)**:
  - **Custom 512-byte BIOS MBR Bootloader (`boot/mbr.S`)**: Reserves bytes `0x03..0x59` (`.org 0x5A`) for USB-FDD BPB safety, collects the BIOS ACPI E820 memory map (`INT 15h`), enables A20, loads `oksinix.bin` in 16-sector LBA chunks, initializes VESA VBE 2.0 Linear Framebuffer (with VGA Mode 12h fallback), and enters 32-bit protected mode at `_start`
  - **Multiboot v1 ELF (`oksinix.elf`)**: Direct boot via QEMU `-kernel` or Multiboot bootloaders with memory map and command-line parsing
- **x86 32-bit Protected Mode Core, ACPI & Hardware Timer (`arch/x86/`, `drivers/acpi.c`, `drivers/pit.c`)**:
  - Flat 32-bit GDT (`gdt.c`), 256-entry IDT (`idt.c`, `isr.S`), 8259 PIC (`pic.c`) with spurious IRQ7/IRQ15 filtering, and 1000 Hz 8254 PIT (`pit.c`) with hardware-latched `pit_udelay`/`pit_mdelay`
  - **ACPI (`drivers/acpi.c`)**: RSDP/RSDT/FADT/DSDT `_S5` parsing, ACPI/E820 memory map reporting (`memmap`), and clean ACPI S5 poweroff (`shutdown` / `poweroff`)
- **Linear Framebuffer / VGA Mode 12h & 80x25 Borderless Console (`drivers/vga.c`, `drivers/console.c`)**:
  - Supports VBE 2.0 / PCI Bochs-QEMU Linear Framebuffer and direct VGA Mode 12h (`640x480x16` planar) with dirty-scanline double-buffered rendering, 8x8 bitmap font, borderless `80x25` console window, and software mouse cursor
- **8042 PS/2 Controller, Keyboard & Mouse (`drivers/ps2.c`, `drivers/keyboard.c`, `drivers/mouse.c`)**:
  - Dual-port 8042 PS/2 init with self-test, IRQ1 keyboard driver supporting Swiss French QWERTZ (`fr_CH`) and US QWERTY (`us`) keymaps (`keymap`), and IRQ12 mouse driver
- **PCI & xHCI USB 3.0 Stack (`drivers/pci.c`, `drivers/xhci.c`, `drivers/usbhub.c`, `drivers/usbhid.c`, `drivers/usbmsc.c`, `drivers/usbnet.c`)**:
  - Full 256-bus PCI enumeration, Intel EHCI-to-xHCI port switchover, `USBLEGSUP` BIOS-to-OS handoff, `HCSPARAMS2` Scratchpad Buffer Array, 1024-byte composite configuration descriptor parsing (IAD `0x0B` + CDC Union `0x24/0x06`), and live hotplug detection
  - **USB Hub (`drivers/usbhub.c`)**: Recursive downstream enumeration via 20-bit xHCI Route Strings
  - **USB HID Boot Keyboard & Mouse (`drivers/usbhid.c`)**: Feeds `/dev/kbd`, `/dev/mouse`, and `/dev/console`
  - **USB Mass Storage (`drivers/usbmsc.c`)**: BOT/SCSI driver supporting up to 4 hot-pluggable USB drives (`/dev/sda`..`/dev/sdd`) with automatic mount/umount to `/mnt` and `/media1`..`/media3`
  - **USB Ethernet RNDIS & CDC-ECM (`drivers/usbnet.c`)**: Exposed as `/dev/eth0` `(10,200)`; supports QEMU `usb-net` and real RNDIS hardware (Android USB tethering, Linux `g_ether`/`f_rndis`, 4G/LTE USB modems) with 16 KiB multi-packet aggregation, `MaxTransferSize`/`PacketAlignmentFactor` negotiation, `RESPONSE_AVAILABLE`/`KEEPALIVE`/`INDICATE` handling, and Stall/Babble/XactErr endpoint recovery
- **Hardened IPv4 / TCP / UDP / ICMP / ARP / DHCPv4 / DNS Stack & BSD Sockets (`kernel/net.c`, `include/net.h`)**:
  - **BSD Socket API**: `socket`, `bind`, `connect`, `send`, `recv`, `sendto`, `recvfrom`, `getsockname`, `getpeername`, `close` (`AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_RAW`, `MSG_DONTWAIT`, `MSG_PEEK`) plus loopback (`127.x.x.x`)
  - **Network Hardening**: Overlap-safe IPv4 fragment reassembly (RFC 791/5722), anti-poisoning ARP cache (8 entries, 60 s TTL, LRU eviction), strict Ethernet/IPv4 destination filtering, RFC 6528 TCP ISN generation, RFC 5961 TCP RST/SYN/ACK/window validation, RFC 768 UDP checksums, RFC 2131 DHCPv4 client (`net dhcp`), and hardened RFC 1035 DNS A-record resolver
- **Virtual Filesystem, `/dev` & FAT32 with VFAT LFN (`fs/vfs.c`, `fs/devfs.c`, `fs/fat32.c`)**:
  - RAMFS root (`/`, `/dev`, `/bin`, `/etc`, `/mnt`, `/media1`..`/media3`, `/tmp`), `/dev` device nodes (`/dev/null`, `/dev/zero`, `/dev/console`, `/dev/tty0`, `/dev/ttyS0`, `/dev/kbd`, `/dev/mouse`, `/dev/fb0`, `/dev/eth0`, `/dev/sda`..`/dev/sdd`)
  - **FAT32 (`fs/fat32.c`)**: Read/write/create/mkdir support with both 8.3 and VFAT Long File Name (LFN) entries across up to 4 mounted USB mass storage volumes
- **Linux i386 Syscalls & ELF32 Static/Dynamic Loader (`kernel/syscall.c`)**:
  - `int $0x80` Linux i386 syscall compatibility including process/memory/file syscalls (`fork`/`clone`, `execve`, `wait4`/`waitid`, `mmap2`, `brk`, `read`/`write`/`openat`/`close`) and BSD socket syscalls (`socket`, `bind`, `connect`, `sendto`, `recvfrom`, `getsockname`, `getpeername`, and multiplexed `socketcall`)
  - Runs static PIE ELF32 binaries (`/mnt/LINUX.ELF`) and dynamically linked ELF32 binaries (`/mnt/DYN.ELF` with `PT_INTERP /lib/ld-linux.so.2`, `LIBTEST.SO`, and `libc.so.6`)
- **Userland `/bin/init`, `/bin/sh`, `/bin/vi` & `/bin/cc` (`user/`)**:
  - **`/bin/sh`**: Built-in commands `help`, `ls`, `cd`, `pwd`, `cat`, `cp`, `echo` (`>` redirection), `stat`, `mkdir`, `mount`, `umount`, `lsusb`, `net` (`net`, `net dhcp`, `net config <ip> <mask> <gw> [dns]`, `net ping [host]`), `telnet [host [port]]` (TCP client with RFC 854 IAC negotiation & interactive `telnet>` prompt), `memmap`/`acpi`, `keymap`, `ps`, `uname`, `uptime`, `clear`, `vi`, `cc`, `shutdown`/`poweroff`, and ELF binary execution
  - **`/bin/vi`**: Full-screen modal text editor with Normal mode (`h`, `j`, `k`, `l`, `0`, `$`, `g`, `G`, `x`, `D`, `dd`, `yy`, `p`, `J`), Visual mode (`v`, `y`), Insert mode (`i`, `a`, `A`, `o`, `O`), and Ex commands (`:w`, `:w <file>`, `:q`, `:q!`, `:wq`, `:x`)
  - **`/bin/cc`**: Built-in minimal C compiler (`user/cc.c`)

## Building & Testing

```sh
# Build kernel ELF (oksinix.elf), hybrid BIOS MBR + FAT32 disk image (oksinix.img), and USB disk image (usb_disk.img)
make

# Run automated QEMU integration test suite (boot, ACPI, VGA, xHCI hub/kbd/mouse/4x storage hotplug,
# FAT32 8.3 & LFN, vi, Linux i386 static/dynamic ELFs + socket syscalls, RNDIS DHCP/ARP/ICMP/TCP telnet, ACPI shutdown)
make test

# Run interactively in QEMU with xHCI USB hub, kbd, mouse, storage, and USB RNDIS networking
make run

# Boot directly via Multiboot v1 ELF in QEMU
make run-multiboot

# Write the hybrid MBR + FAT32 disk image to a USB flash drive (Legacy BIOS / CSM boot):
# sudo dd if=oksinix.img of=/dev/sdX bs=1M status=progress && sync
```

## Networking on Real RNDIS Hardware

Plug in a real USB RNDIS device (e.g., an Android phone with USB tethering enabled, Linux `g_ether`/`f_rndis` gadget, or 4G/LTE USB modem) and run in `/bin/sh`:

```sh
net dhcp
net
net ping
telnet example.com 80
```

## License

All source code and documentation in this repository are released into the public domain under [CC0 1.0 Universal](LICENSE).
