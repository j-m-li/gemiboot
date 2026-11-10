#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# Public Domain (CC0 1.0 Universal)
# Oksinix x86 32-bit kernel - Automated QEMU integration test suite
# Tests VGA 640x480, VFS (/dev), /bin/init, /bin/sh, PS/2 and xHCI USB Hub + (kbd, mouse, storage)

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def send_hmp_cmd(sock, cmd, delay=0.06):
    sock.sendall((cmd + "\n").encode("ascii"))
    time.sleep(delay)
    try:
        sock.recv(4096)
    except BlockingIOError:
        pass


def send_keys_str(sock, text):
    # Swiss French (fr_CH QWERTZ) key mapping for QEMU sendkey
    keymap = {
        " ": "spc",
        "/": "shift-7",
        "-": "slash",
        "_": "shift-slash",
        ".": "dot",
        ":": "shift-dot",
        "!": "shift-bracket_right",
        ">": "shift-less",
        "y": "z",
        "z": "y",
        "Y": "shift-z",
        "Z": "shift-y",
        "\x1b": "esc",
        "\n": "ret",
    }
    for ch in text:
        if ch in keymap:
            k = keymap[ch]
        elif "A" <= ch <= "Z":
            k = f"shift-{ch.lower()}"
        else:
            k = ch
        send_hmp_cmd(sock, f"sendkey {k}", delay=0.05)


def wait_for_serial(serial_path, substring, timeout=6.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(serial_path):
            with open(serial_path, "r", errors="replace") as f:
                data = f.read()
            if substring in data:
                return data
        time.sleep(0.05)
    with open(serial_path, "r", errors="replace") as f:
        data = f.read()
    raise RuntimeError(
        f"Timed out waiting for '{substring}' in serial log.\n--- Serial Output ---\n{data}"
    )


def verify_ppm_screendump(ppm_path):
    if not os.path.exists(ppm_path):
        raise RuntimeError(f"Screendump file not created: {ppm_path}")
    with open(ppm_path, "rb") as f:
        content = f.read()
    lines = []
    idx = 0
    while len(lines) < 3 and idx < len(content):
        nl = content.find(b"\n", idx)
        if nl == -1:
            break
        line = content[idx:nl].strip()
        if not line.startswith(b"#") and len(line) > 0:
            lines.append(line.decode("ascii"))
        idx = nl + 1

    if len(lines) < 3 or lines[0] != "P6":
        raise RuntimeError(f"Invalid PPM header: {lines}")
    dims = lines[1].split()
    width, height = int(dims[0]), int(dims[1])
    if (width, height) != (640, 480):
        raise RuntimeError(f"Expected 640x480 VGA Mode 12h resolution, got {width}x{height}")

    pixel_data = content[idx:]
    expected_bytes = width * height * 3
    if len(pixel_data) < expected_bytes:
        raise RuntimeError(f"Incomplete pixel buffer: {len(pixel_data)} < {expected_bytes}")

    colors = set()
    for i in range(0, expected_bytes, 3):
        colors.add((pixel_data[i], pixel_data[i + 1], pixel_data[i + 2]))

    if len(colors) < 4:
        raise RuntimeError(f"Expected multiple UI colors in VGA screendump, found only {len(colors)}")
    return width, height, len(colors)


def main():
    boot_img_path = sys.argv[1] if len(sys.argv) > 1 else "oksinix.img"
    base_disk_path = sys.argv[2] if len(sys.argv) > 2 else "usb_disk.img"
    kernel_elf_path = sys.argv[3] if len(sys.argv) > 3 else "oksinix.elf"

    if not os.path.exists(boot_img_path):
        print(f"Error: bootable disk image '{boot_img_path}' not found.", file=sys.stderr)
        sys.exit(1)
    if not os.path.exists(base_disk_path):
        print(f"Error: USB disk image '{base_disk_path}' not found.", file=sys.stderr)
        sys.exit(1)

    with tempfile.TemporaryDirectory(prefix="oksinix_test_") as tmpdir:
        mon_sock_path = os.path.join(tmpdir, "monitor.sock")
        serial_log_path = os.path.join(tmpdir, "serial.log")
        ppm_path = os.path.join(tmpdir, "vga_dump.ppm")
        usb_disk_path = os.path.join(tmpdir, "usb_disk_test.img")
        usb_disk2_path = os.path.join(tmpdir, "usb_disk2_test.img")

        # Copy usb_disk.img into temporary test working copy so writes don't dirty repo image
        shutil.copyfile(base_disk_path, usb_disk_path)
        with open(usb_disk2_path, "wb") as f2:
            f2.write(b"OKSINIX_SECOND_USB_DISK_SDB_OK\n\0" + b"\x00" * (65536 - 32))

        # Boot directly from the BIOS MBR bootable disk image (oksinix.img)
        # and attach usb-hub + usb-kbd + usb-mouse + 2x usb-storage (/dev/sda, /dev/sdb) on xHCI!
        qemu_cmd = [
            "qemu-system-i386",
            "-drive", f"file={boot_img_path},format=raw",
            "-vga", "std",
            "-display", "none",
            "-device", "qemu-xhci,id=xhci",
            "-device", "usb-hub,bus=xhci.0,port=1",
            "-device", "usb-kbd,bus=xhci.0,port=1.1",
            "-device", "usb-mouse,bus=xhci.0,port=1.2",
            "-drive", f"file={usb_disk_path},if=none,format=raw,id=usbdisk",
            "-device", "usb-storage,bus=xhci.0,port=1.3,drive=usbdisk",
            "-drive", f"file={usb_disk2_path},if=none,format=raw,id=usbdisk2",
            "-device", "usb-storage,bus=xhci.0,port=1.4,drive=usbdisk2",
            "-serial", f"file:{serial_log_path}",
            "-monitor", f"unix:{mon_sock_path},server,nowait",
            "-no-reboot",
        ]

        proc = subprocess.Popen(
            qemu_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        try:
            # 1. Verify boot, ACPI RSDP/RSDT & ACPI memory map, PIT timer, PS/2 self-test, VGA 640x480, PCI xHCI, USB Hub, and FAT32
            wait_for_serial(serial_log_path, "[BOOT] Multiboot v1 magic verified")
            wait_for_serial(serial_log_path, "[ACPI] RSDP")
            wait_for_serial(serial_log_path, "[ACPI-MMAP] #0:")
            wait_for_serial(serial_log_path, "[ACPI-MMAP] Source: ACPI E820 (INT 15h)")
            wait_for_serial(serial_log_path, "[PIT] 8254 timer initialized at 1000 Hz")
            wait_for_serial(serial_log_path, "[FB] Linear Framebuffer active via VESA VBE 2.0 BIOS")
            wait_for_serial(serial_log_path, "[VGA] Mode 12h framebuffer initialized (640x480x16 at 0x")
            wait_for_serial(serial_log_path, "[TEST] VGA 640x480 framebuffer self-test PASSED")
            wait_for_serial(serial_log_path, "[PS2] 8042 controller initialized (selftest=0x55")
            wait_for_serial(serial_log_path, "[XHCI] Host controller running")
            wait_for_serial(serial_log_path, "[XHCI-HUB] USB Hub initialized on root port")
            wait_for_serial(serial_log_path, "route 0x1, EP DCI 3)")
            wait_for_serial(serial_log_path, "route 0x2, EP DCI 3)")
            wait_for_serial(serial_log_path, "route 0x3, IN=3 OUT=4)")
            wait_for_serial(serial_log_path, "route 0x4, IN=3 OUT=4)")
            wait_for_serial(serial_log_path, "[XHCI-MSC] USB Mass Storage")
            wait_for_serial(serial_log_path, "[DEVFS] Registered block device /dev/sda (8,0)")
            wait_for_serial(serial_log_path, "[DEVFS] Registered block device /dev/sdb (8,16)")
            wait_for_serial(serial_log_path, "[FAT32] Mounted volume \"OKSINIXFAT \" at /mnt")
            wait_for_serial(serial_log_path, "[/bin/init] xHCI USB bus active (usb-hub usb-kbd usb-mouse usb-storage:/dev/sda,/dev/sdb)")
            wait_for_serial(serial_log_path, "[/bin/init] Mounted FAT32 filesystem on /mnt")
            wait_for_serial(serial_log_path, "oksinix:/# ")
            print("[PASS] 1/9 Boot, VGA 640x480, PCI xHCI, USB Hub, /dev/sda + /dev/sdb & FAT32 mount on /mnt")

            # Connect to QEMU HMP monitor socket
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(mon_sock_path)
            sock.setblocking(False)
            time.sleep(0.1)

            # 2. Type 'lsusb' and 'uptime' via xHCI USB Keyboard (behind USB Hub port 1.1)
            send_keys_str(sock, "lsusb\n")
            serial_out = wait_for_serial(serial_log_path, "-> /dev/sdb")
            if "USB Hub (slot 1, 8 ports, 4 downstream)" not in serial_out:
                raise RuntimeError("Expected USB Hub info in lsusb output")
            if "[USB-KBD] key press" not in serial_out:
                raise RuntimeError("Expected key events to arrive via xHCI USB Keyboard ([USB-KBD])")
            send_keys_str(sock, "keymap\n")
            wait_for_serial(serial_log_path, "Current keymap: fr_CH (Swiss French QWERTZ)")
            send_keys_str(sock, "uptime\n")
            wait_for_serial(serial_log_path, "PIT ticks @ 1000 Hz)")
            print("[PASS] 2/9 xHCI USB Keyboard (Swiss French fr_CH QWERTZ keymap), 'lsusb', 'keymap' & 'uptime' in /bin/sh")

            # 3. Verify FAT32 directory listing ('ls /mnt') and reading files ('cat /mnt/README.TXT', 'cat /mnt/DOCS/NOTES.TXT')
            send_keys_str(sock, "ls /mnt\n")
            wait_for_serial(serial_log_path, "-rw-r--r--  README.TXT")
            wait_for_serial(serial_log_path, "drwxr-xr-x  DOCS/")
            send_keys_str(sock, "cat /mnt/README.TXT\n")
            wait_for_serial(serial_log_path, "Oksinix FAT32 filesystem on USB Mass Storage (/dev/sda1 -> /mnt)")
            send_keys_str(sock, "cat /mnt/DOCS/NOTES.TXT\n")
            wait_for_serial(serial_log_path, "Subdirectory file inside FAT32 /mnt/DOCS/NOTES.TXT")
            print("[PASS] 3/8 FAT32 directory traversal ('ls /mnt') & file reads ('/mnt/README.TXT', '/mnt/DOCS/NOTES.TXT')")

            # 4. Verify FAT32 file creation, cluster allocation, 'cp' & write ('echo fat32-write-ok > /mnt/TEST.TXT')
            send_keys_str(sock, "echo fat32-write-ok > /mnt/TEST.TXT\n")
            time.sleep(0.2)
            send_keys_str(sock, "cat /mnt/TEST.TXT\n")
            wait_for_serial(serial_log_path, "fat32-write-ok")
            send_keys_str(sock, "cp /mnt/TEST.TXT /mnt/COPY.TXT\n")
            time.sleep(0.2)
            send_keys_str(sock, "cat /mnt/COPY.TXT\n")
            wait_for_serial(serial_log_path, "fat32-write-ok")
            print("[PASS] 4/9 FAT32 file creation, 'cp', cluster allocation, write & readback ('/mnt/TEST.TXT', '/mnt/COPY.TXT') verified")

            # 5. Test modal 'vi' editor: open '/mnt/VITEST.TXT', enter insert mode ('i'), type text, ESC, ':wq'
            send_keys_str(sock, "vi /mnt/VITEST.TXT\n")
            time.sleep(0.25)
            send_keys_str(sock, "iEdited with Oksinix vi!\x1b:wq\n")
            wait_for_serial(serial_log_path, '[VI] saved "/mnt/VITEST.TXT"')
            send_keys_str(sock, "cat /mnt/VITEST.TXT\n")
            wait_for_serial(serial_log_path, "Edited with Oksinix vi!")
            print("[PASS] 5/8 Modal 'vi' text editor (Normal -> Insert -> Command ':wq') saved to FAT32 '/mnt/VITEST.TXT'")

            # 6. Verify raw /dev/sda and /dev/sdb reading via xHCI USB Mass Storage (SCSI READ 10)
            send_keys_str(sock, "cat /dev/sda\n")
            wait_for_serial(serial_log_path, "OKSINIX_USB_STORAGE_OK")
            send_keys_str(sock, "cat /dev/sdb\n")
            wait_for_serial(serial_log_path, "OKSINIX_SECOND_USB_DISK_SDB_OK")
            print("[PASS] 6/10 Multiple xHCI USB Mass Storage drives (/dev/sda & /dev/sdb) raw block device reads verified")

            # 7. Execute 32-bit Linux x86 (i386) static & dynamically linked ELF binaries
            send_keys_str(sock, "/mnt/LINUX.ELF\n")
            wait_for_serial(serial_log_path, "[ELF32] Launching Linux i386 binary \"/mnt/LINUX.ELF\"")
            wait_for_serial(serial_log_path, "[LINUX-ELF] execve child running via int 0x80 (PID verified)")
            wait_for_serial(serial_log_path, "[LINUX-ELF] All 37 Linux i386 syscalls PASSED!")
            send_keys_str(sock, "memmap\n")
            wait_for_serial(serial_log_path, "ACPI RSDP:")
            wait_for_serial(serial_log_path, "Total Usable RAM:")
            send_keys_str(sock, "/mnt/DYN.ELF\n")
            wait_for_serial(serial_log_path, "at 0x01500000")
            wait_for_serial(serial_log_path, "[DYN] PT_INTERP \"/lib/ld-linux.so.2\" loaded at 0x01600000")
            wait_for_serial(serial_log_path, "[LINUX-DYN] Shared library LIBTEST.SO + libc.so.6 + PT_INTERP PASSED!")
            print("[PASS] 7/10 Linux x86 32-bit static (/mnt/LINUX.ELF) & dynamic (/mnt/DYN.ELF + LIBTEST.SO + ld-linux.so.2) binaries verified")

            # 8. Test xHCI USB Mouse (on Hub port 1.2) movement & button clicks
            send_hmp_cmd(sock, "mouse_move 35 -15", delay=0.1)
            send_hmp_cmd(sock, "mouse_button 1", delay=0.1)
            send_hmp_cmd(sock, "mouse_button 0", delay=0.1)

            serial_out = wait_for_serial(serial_log_path, "[USB-MOUSE] pkt#")
            if "buttons=0x1" not in serial_out:
                raise RuntimeError("USB Mouse button press not recorded")
            print("[PASS] 8/10 xHCI USB Mouse (on Hub port 1.2) HID Boot report verified")

            # 9. Dump rendered VGA 640x480 framebuffer from QEMU and verify dimensions & colors
            send_hmp_cmd(sock, f"screendump {ppm_path}", delay=0.25)
            w, h, num_colors = verify_ppm_screendump(ppm_path)
            print(f"[PASS] 9/10 VGA Framebuffer screendump verified ({w}x{h}, {num_colors} active palette colors)")

            sock.close()

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()

        # 9. Verify Multiboot v1 ELF boot (-kernel oksinix.elf) and Multiboot info parsing
        if os.path.exists(kernel_elf_path):
            mb_serial_log = os.path.join(tmpdir, "serial_mb.log")
            mb_cmd = [
                "qemu-system-i386",
                "-kernel", kernel_elf_path,
                "-append", "console=ttyS0 keymap=fr_CH",
                "-vga", "std",
                "-display", "none",
                "-device", "qemu-xhci,id=xhci",
                "-drive", f"file={usb_disk_path},if=none,format=raw,id=usbdisk",
                "-device", "usb-storage,bus=xhci.0,port=1,drive=usbdisk",
                "-serial", f"file:{mb_serial_log}",
                "-no-reboot",
            ]
            mb_proc = subprocess.Popen(
                mb_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                wait_for_serial(mb_serial_log, "[BOOT] Multiboot v1 magic verified")
                wait_for_serial(mb_serial_log, "[ACPI] RSDP")
                wait_for_serial(mb_serial_log, "[ACPI-MMAP] Source: ACPI/Multiboot E820")
                wait_for_serial(mb_serial_log, "[MULTIBOOT] Memory: lower=")
                wait_for_serial(mb_serial_log, "[MULTIBOOT] Command line:")
                wait_for_serial(mb_serial_log, "[FB] Linear Framebuffer active via PCI Bochs/QEMU VBE")
                wait_for_serial(mb_serial_log, "[FAT32] Mounted volume \"OKSINIXFAT \" at /mnt")
                wait_for_serial(mb_serial_log, "oksinix:/# ")
                print("[PASS] 10/10 Multiboot v1 ELF ('oksinix.elf') boot, LFB, memory info & cmdline verified")
            finally:
                mb_proc.terminate()
                try:
                    mb_proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    mb_proc.kill()

        print("\nAll QEMU kernel tests PASSED!")


if __name__ == "__main__":
    main()
