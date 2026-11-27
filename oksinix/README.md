<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oksinix

A 32-bit x86 operating system kernel and Ring-3 userland written in strict ISO C90 (`-std=c89 -pedantic -Wall -Wextra -Werror`), compiled with `clang` and linked with `ld.lld`. Dedicated to the public domain under **CC0 1.0 Universal**.

## Features

- **BIOS MBR Hybrid Boot Image (`oksinix.img`, `boot/mbr.S`), Multiboot v1 ELF (`oksinix.elf`, `boot/boot.S`) & UEFI 32/64-bit Support**:
  - **Custom 512-byte BIOS MBR Bootloader (`boot/mbr.S`)**: Reserves bytes `0x03..0x59` (`.org 0x5A`) for USB-FDD BPB safety, collects the BIOS ACPI E820 memory map (`INT 15h`), enables A20, loads `oksinix.bin` in 16-sector LBA chunks, initializes VESA VBE 2.0 Linear Framebuffer (with VGA Mode 12h fallback), and enters 32-bit protected mode at `_start`
  - **Multiboot v1 ELF (`oksinix.elf`)**: Direct boot via QEMU `-kernel`, Multiboot bootloaders, or external BIOS / UEFI 32-bit / UEFI 64-bit loaders (such as `gemiboot` with OVMF IA32/X64), including clean `CR0`/`CR4` paging handoff, GOP/LFB framebuffer, memory map, and command-line parsing
- **x86 32-bit Protected Mode, Hardware Paging & xv6-like Task Protection (`arch/x86/`, `kernel/vm.c`, `kernel/sched.c`)**:
  - **Ring-0 / Ring-3 Privilege Separation (`arch/x86/gdt.c`, `arch/x86/idt.c`, `arch/x86/isr.S`)**: Flat 32-bit GDT with user segments (`0x1B`/`0x23`), 32-bit TSS (`0x28`) with per-task `esp0` switching, 256-entry IDT (`int $0x80` DPL=3 syscall gate), 8259 PIC with spurious IRQ7/IRQ15 filtering, and Ring-3 fault isolation (`#PF`, `#GP`, `#UD`, `#DE` terminate only the faulting user task without crashing the kernel or parent shell)
  - **Virtual Memory & Stack Overflow Protection (`kernel/vm.c`)**: x86 32-bit hardware paging (`CR4.PSE` 4 MiB kernel superpages + per-task 4 KiB user page directories), unmapped null page (`0x00000000`), unmapped 4 KiB hardware stack guard pages for kernel and user tasks, software stack-canary verification, and strict syscall user-pointer validation (`vm_user_range_ok`, `vm_user_str_ok`)
  - **Preemptive Round-Robin Scheduler (`kernel/sched.c`, `drivers/pit.c`)**: 1000 Hz 8254 PIT (`pit_udelay`/`pit_mdelay`), up to 8 concurrent tasks with 10 ms time slices, per-task `CR3`/`TSS.esp0` switching, `ps`, `spawn`, `kill`, `schedtest`, and a 6-check hardware protection self-test (`prottest`)
  - **ACPI (`drivers/acpi.c`)**: RSDP/RSDT/FADT/DSDT `_S5` parsing, ACPI/E820 memory map reporting (`memmap`), and clean ACPI S5 poweroff (`shutdown` / `poweroff`)
- **Linear Framebuffer / VGA Mode 12h & ANSI/VT100 80x25 Console (`drivers/vga.c`, `drivers/console.c`)**:
  - Supports VBE 2.0 / UEFI GOP / PCI Bochs-QEMU Linear Framebuffer and direct VGA Mode 12h (`640x480x16` planar) with dirty-scanline double-buffered rendering, 8x8 bitmap font, borderless `80x25` console window, software mouse cursor, and an ANSI/VT100 CSI escape parser (alternate screen buffer `\x1b[?1049h`/`l`, cursor positioning, line/screen erase, reverse video)
- **8042 PS/2 Controller, Keyboard & Mouse (`drivers/ps2.c`, `drivers/keyboard.c`, `drivers/mouse.c`)**:
  - Dual-port 8042 PS/2 init with self-test, IRQ1 keyboard driver supporting Swiss French QWERTZ (`fr_CH`) and US QWERTY (`us`) keymaps (`keymap`), and IRQ12 mouse driver
- **PCI & Full USB 1.1 / 2.0 / 3.0 Host Stack (`drivers/pci.c`, `drivers/xhci.c`, `drivers/ehci.c`, `drivers/ohci.c`, `drivers/uhci.c`, `drivers/usbhub.c`, `drivers/usbhid.c`, `drivers/usbmsc.c`, `drivers/usbnet.c`)**:
  - Full 256-bus PCI enumeration with drivers for **xHCI (USB 3.x)**, **EHCI (USB 2.0)**, **OHCI (USB 1.1)**, and **UHCI (USB 1.1)** host controllers, Intel EHCI-to-xHCI port switchover, `USBLEGSUP` BIOS-to-OS handoff, `HCSPARAMS2` Scratchpad Buffer Array, 1024-byte composite configuration descriptor parsing (IAD `0x0B` + CDC Union `0x24/0x06`), and live hotplug detection
  - **USB Hub (`drivers/usbhub.c`)**: Recursive downstream enumeration via 20-bit xHCI Route Strings
  - **USB HID Boot Keyboard & Mouse (`drivers/usbhid.c`)**: Feeds `/dev/kbd`, `/dev/mouse`, and `/dev/console`
  - **USB Mass Storage (`drivers/usbmsc.c`)**: BOT/SCSI driver supporting up to 4 hot-pluggable USB drives (`/dev/sda`..`/dev/sdd`) with automatic mount/umount to `/mnt` and `/media1`..`/media3`
  - **USB Ethernet RNDIS & CDC-ECM (`drivers/usbnet.c`)**: Exposed as `/dev/eth0` `(10,200)`; supports QEMU `usb-net` and real RNDIS hardware (Android USB tethering, Linux `g_ether`/`f_rndis`, 4G/LTE USB modems) with 16 KiB multi-packet aggregation, `MaxTransferSize`/`PacketAlignmentFactor` negotiation, `RESPONSE_AVAILABLE`/`KEEPALIVE`/`INDICATE` handling, and Stall/Babble/XactErr endpoint recovery
- **Hardened IPv4 / TCP / UDP / ICMP / ARP / DHCPv4 / DNS Stack & BSD Sockets (`kernel/net.c`, `include/net.h`)**:
  - **BSD Socket API**: `socket`, `bind`, `connect`, `send`, `recv`, `sendto`, `recvfrom`, `getsockname`, `getpeername`, `close` (`AF_INET`, `SOCK_STREAM`, `SOCK_DGRAM`, `SOCK_RAW`, `MSG_DONTWAIT`, `MSG_PEEK`) plus loopback (`127.x.x.x`)
  - **Network Hardening**: Overlap-safe IPv4 fragment reassembly (RFC 791/5722), anti-poisoning ARP cache (8 entries, 60 s TTL, LRU eviction), strict Ethernet/IPv4 destination filtering, RFC 6528 TCP ISN generation, RFC 5961 TCP RST/SYN/ACK/window validation, RFC 768 UDP checksums, RFC 2131 DHCPv4 client (`net dhcp`), and hardened RFC 1035 DNS A-record resolver
- **Virtual Filesystem, `/dev` & FAT32 with VFAT LFN (`fs/vfs.c`, `fs/devfs.c`, `fs/fat32.c`)**:
  - RAMFS root (`/`, `/dev`, `/bin`, `/etc`, `/mnt`, `/media1`..`/media3`, `/tmp`) with zero-copy embedded read-only files (`vfs_register_rofile`), plus `/dev` device nodes (`/dev/null`, `/dev/zero`, `/dev/console`, `/dev/tty0`, `/dev/ttyS0`, `/dev/kbd`, `/dev/mouse`, `/dev/fb0`, `/dev/eth0`, `/dev/sda`..`/dev/sdd`)
  - **FAT32 (`fs/fat32.c`)**: Read/write/create/mkdir support with both 8.3 and VFAT Long File Name (LFN) entries across up to 4 mounted USB mass storage volumes
- **Linux i386 Syscalls & Ring-3 ELF32 Static/Dynamic Loader (`kernel/syscall.c`, `kernel/sysctl.c`)**:
  - `int $0x80` Linux i386 syscall ABI running user processes in Ring 3 with 4-level nested `fork`/`clone` + `execve` + `wait4`/`waitid` snapshots, memory/file/system syscalls (`mmap2`, `brk`, `read`/`write`/`open`/`openat`/`close`, `chdir`, `getcwd`, `mkdir`, `mount`, `umount2`, `kill`, `clock_gettime`, `reboot`, `OKSINIX_SYS_ctl`), and BSD socket syscalls (`socket`, `bind`, `connect`, `sendto`, `recvfrom`, `getsockname`, `getpeername`, and multiplexed `socketcall`)
  - Runs static-PIE ELF32 executables and dynamically linked ELF32 executables (`PT_INTERP /lib/ld-linux.so.2`) backed by real Ring-3 PIC shared libraries (`LIBTEST.SO`, `libc.so.6`)
- **Standalone Ring-3 Userland (`user/`)**:
  - **`/bin/init` (`user/init.c`)**: Minimal kernel init that mounts RAMFS/devfs/FAT32, registers embedded `/bin/sh`, and launches `/bin/sh` in Ring 3
  - **`/bin/sh` (`user/sh.c` -> `user/sh.elf`, embedded via `user/sh_embed.S` & `/mnt/SH.ELF`)**: Standalone Ring-3 static-PIE shell (`int $0x80` only) supporting `help`, `ls`, `cd`, `pwd`, `cat`, `cp`, `echo` (`>` redirection), `stat`, `mkdir`, `mount`, `umount`, `lsusb`, `net` (`net`, `net dhcp`, `net config <ip> <mask> <gw> [dns]`, `net ping [host]`), `memmap`/`acpi`, `keymap`, `ps`, `kill`, `spawn`, `schedtest`, `prottest`, `uname`, `uptime`, `clear`, `shutdown`/`poweroff`, and spawning external Ring-3 executables (`vi`, `cc`, `telnet`, `/mnt/LINUX.ELF`, `/mnt/DYN.ELF`, etc.) via `fork` + `execve` + `waitpid`
  - **`/mnt/VI.ELF` (`user/vi.c`)**: Standalone Ring-3 full-screen modal text editor using ANSI/VT100 escapes with Normal mode (`h`, `j`, `k`, `l`, `0`, `$`, `g`, `G`, `x`, `D`, `dd`, `yy`, `p`, `J`), Visual mode (`v`, `y`), Insert mode (`i`, `a`, `A`, `o`, `O`), and Ex commands (`:w`, `:w <file>`, `:q`, `:q!`, `:wq`, `:x`)
  - **`/mnt/CC.ELF` (`user/cc.c`)**: Standalone Ring-3 minimal C compiler
  - **`/mnt/TELNET.ELF` (`user/telnet.c`)**: Standalone Ring-3 TCP client with RFC 854 IAC negotiation, UDP DNS A-record resolution, and interactive `telnet>` REPL over BSD socket syscalls
  - **`/mnt/LINUX.ELF` (`user/linux_hello.S`) & `/mnt/DYN.ELF` (`user/linux_dyn.c`, `user/libtest.c`, `user/libc_stub.c`)**: Static-PIE and dynamically linked Ring-3 Linux i386 test programs

## Building & Testing

```sh
# Build kernel ELF (oksinix.elf), standalone Ring-3 userland ELFs (sh, vi, cc, telnet, libc.so.6, ...),
# hybrid BIOS MBR + FAT32 disk image (oksinix.img), and USB disk image (usb_disk.img)
make

# Run automated QEMU integration test suite (Ring-3 /bin/sh, ACPI, VGA, xHCI hub/kbd/mouse/4x storage hotplug,
# FAT32 8.3 & LFN, Ring-3 vi/cc/telnet, static/dynamic ELFs + socket syscalls, prottest, RNDIS DHCP/ARP/ICMP,
# ACPI S5 shutdown, and Multiboot v1 boot)
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
