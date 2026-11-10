<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oksinix

A 32-bit x86 operating system kernel written in strict ISO C90 (`-std=c89 -pedantic`), compiled with `clang` and linked with `ld.lld`. Dedicated to the public domain under **CC0 1.0 Universal**.

## Features

- **GRUB2 Hybrid BIOS + UEFI ISO (`oksinix.iso`, `boot/grub.cfg`) & BIOS MBR Disk Image (`oksinix.img`, `boot/mbr.S`, `boot/boot.S`)**:
  - **GRUB2 Hybrid ISO/USB Image (`oksinix.iso`)**: Built with `grub2-mkrescue`, boots on **both 64-bit UEFI (`x86_64-efi`) and Legacy BIOS (`i386-pc`)** real hardware, initializes a 640x480 VBE/GOP Linear Framebuffer, and passes `usb_disk.img` as a Multiboot module
  - **Custom 512-byte BIOS MBR Bootloader (`boot/mbr.S`)**: Reserves bytes `0x03..0x59` (`.org 0x5A`) so USB-FDD BIOSes that patch the BPB never corrupt code, normalizes `CS:IP` (`ljmp $0x0000`), enables A20, reads the kernel in safe 16-sector (`8 KiB`) LBA chunks with retry + disk reset, calls Video BIOS `INT 10h AX=0x0012` (VGA 640x480x16) in Real Mode, and enters `_start`
- **x86 32-bit Protected Mode Core & Hardware Timer (`arch/x86/`, `drivers/pit.c`)**:
  - Flat 32-bit Global Descriptor Table (`gdt.c`) and hardware store-load memory barriers (`cpu_memory_barrier`, `cpu_pause`)
  - 256-entry Interrupt Descriptor Table (`idt.c`, `isr.S`) with CPU exception handlers (`0..31`) and hardware IRQ routing (`32..47`)
  - 8259 Programmable Interrupt Controller (`pic.c`) with In-Service Register (`OCW3`) Spurious IRQ7 / IRQ15 filtering
  - 8254 Programmable Interval Timer (`pit.c`) running at 1000 Hz (`IRQ0`) with hardware-latched sub-millisecond `pit_udelay` / `pit_mdelay` delays
- **VBE / UEFI GOP Linear Framebuffer & VGA Mode 12h 640x480 (`drivers/vga.c`, `drivers/console.c`)**:
  - Automatically detects Multiboot v1 VESA VBE / EFI GOP Linear Framebuffer (`32/24/16-bpp` RGB) when booted via GRUB on real GPUs/monitors, with seamless fallback to direct VGA Mode 12h (`640x480x16` 4-plane planar at `0xA0000`)
  - Dirty-scanline double-buffered rendering (`vga_present`), 8x8 ASCII bitmap font, graphical console terminal (`75x33` character grid), and software mouse cursor sprite
- **8042 PS/2 Controller, Keyboard & Mouse (`drivers/ps2.c`, `drivers/keyboard.c`, `drivers/mouse.c`)**:
  - Dual-port 8042 PS/2 initialization with controller self-test (`0xAA`), configuration restoration, and bus-paced timeouts
  - IRQ1 PS/2 Keyboard driver with US QWERTY Scancode Set 1 translation, Shift/Ctrl/Alt/CapsLock tracking, and event ring buffer
  - IRQ12 PS/2 Mouse driver with ACK-checked initialization, 3-byte packet decoding, 9-bit signed delta movement, 640x480 coordinate clamping, and Left/Middle/Right button state
- **PCI & xHCI USB 3.0 Stack (`drivers/pci.c`, `drivers/xhci.c`)**:
  - Full 256-bus PCI Configuration Space (#1) enumeration (`0xCF8`/`0xCFC`) with MMIO BAR0 setup, Bus Mastering, and Intel Panther Point / Lynx Point EHCI-to-xHCI port routing switchover
  - Real-hardware xHCI initialization: Extended Capability `USBLEGSUP` BIOS-to-OS SMM handoff, `HCSPARAMS2` 4 KiB-aligned Scratchpad Buffer Array allocation, RW1C-safe `PORTSC` port power/reset sequencing, Full-Speed 8-byte `bMaxPacketSize0` discovery with `EVALUATE_CONTEXT`, and DMA memory barriers
  - **USB Hub (`usb-hub`)**: Hub descriptor parsing (`0x29`/`0x2A`), `bPwrOn2PwrGood` power stabilization delay, xHCI Slot Context `Hub` bit + port count setup, polled downstream `PORT_RESET`, and recursive 20-bit xHCI Route String device enumeration
  - **USB HID Boot Keyboard (`usb-kbd`)**: 8-byte boot report parser and modifier/usage-ID translation feeding into `/dev/kbd` and `/dev/console`
  - **USB HID Boot Mouse (`usb-mouse`)**: Boot report delta `(dx, dy)` and button state feeding into `/dev/mouse` and the 640x480 cursor
  - **USB Mass Storage (`usb-storage`, `usb_disk.img`)**: Bulk-Only Transport (BOT CBW/CSW) with endpoint stall recovery (`Reset Endpoint`, `Set TR Dequeue Pointer`, `CLEAR_FEATURE(ENDPOINT_HALT)`), spin-up retries, and SCSI `INQUIRY`, `TEST UNIT READY`, `REQUEST SENSE`, `READ CAPACITY (10)`, `READ (10)`, and `WRITE (10)` exposed as block device `/dev/sda` `(8,0)`
- **Virtual Filesystem, `/dev` & FAT32 Filesystem (`fs/vfs.c`, `fs/devfs.c`, `fs/fat32.c`)**:
  - Hierarchical path resolution (`/`, `/dev`, `/bin`, `/etc`, `/mnt`, `/tmp`) and file descriptor table (`vfs_open`, `vfs_read`, `vfs_write`, `vfs_close`, `vfs_readdir`, `vfs_exec`)
  - Devices mounted in `/dev`: `/dev/null` `(1,3)`, `/dev/zero` `(1,5)`, `/dev/console` `(5,1)`, `/dev/tty0` `(4,0)`, `/dev/ttyS0` `(4,64)`, `/dev/kbd` `(13,64)`, `/dev/mouse` `(13,63)`, `/dev/fb0` `(29,0)`, and `/dev/sda` `(8,0)`
  - **FAT32 Filesystem (`fs/fat32.c`)**: Parses MBR partition table and FAT32 BPB/EBPB, mounts FAT32 partition at `/mnt`, supports multi-cluster directory traversal, 8.3 directory entries, FAT cluster chain reading, free cluster allocation, and file/directory creation and writing
- **Userland `/bin/init`, `/bin/sh` & `/bin/vi` (`user/init.c`, `user/sh.c`, `user/vi.c`)**:
  - `/bin/init` (PID 1): Opens `/dev/console` on FDs `0, 1, 2`, displays `/etc/motd` and `/etc/hostname`, verifies `/dev` nodes, xHCI USB bus status, and `/mnt` FAT32 mount, and spawns `/bin/sh`
  - `/bin/sh` (PID 2): Interactive shell supporting `help`, `ls`, `cd`, `pwd`, `cat`, `echo` (with `>` redirection), `stat`, `mkdir`, `mount`, `lsusb`, `ps`, `uname`, `clear`, `vi`, and binary execution
  - `/bin/vi`: Full-screen modal `vi` text editor with screen save/restore, Normal mode (`h`, `j`, `k`, `l`, `0`, `$`, `g`, `G`, `x`, `D`, `dd`), Insert mode (`i`, `a`, `A`, `o`, `O`, `Enter`, `Backspace`, `ESC`), and Ex Command mode (`:w`, `:w <file>`, `:q`, `:q!`, `:wq`, `:x`) reading and writing files on RAMFS and FAT32 (`/mnt`)

## Building & Testing

```sh
# Build the kernel ELF image (oksinix.elf)
make

# Run automated integration tests in QEMU (boot, VGA 640x480, VFS /dev, /bin/init, /bin/sh, IRQ1/IRQ12)
make test

# Run interactively in QEMU (MBR disk image or GRUB ISO)
make run
make run-iso

# Recommended for real x86 hardware (boots on BOTH UEFI and Legacy BIOS / CSM via GRUB2):
# sudo dd if=oksinix.iso of=/dev/sdX bs=4M status=progress && sync

# Or write the raw MBR + FAT32 hybrid disk image (Legacy BIOS / CSM boot):
# sudo dd if=oksinix.img of=/dev/sdX bs=1M status=progress && sync
```

## License

All source code and documentation in this repository are released into the public domain under [CC0 1.0 Universal](LICENSE).
