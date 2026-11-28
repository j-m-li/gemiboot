#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
# Public Domain (CC0 1.0 Universal)
# Oksinix x86 32-bit kernel - Automated QEMU integration test suite
# Tests VGA 640x480, VFS (/dev), /bin/init, /bin/sh, PS/2 and xHCI USB Hub + (kbd, mouse, storage)

import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def start_mock_telnet_server():
    """Start a local TCP mock telnet server reachable via QEMU SLIRP at 10.0.2.2:<port>."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(2)
    srv.settimeout(30.0)
    port = srv.getsockname()[1]

    def worker():
        try:
            conn, _ = srv.accept()
            conn.settimeout(10.0)
            # Send RFC 854 IAC DO TTYPE + IAC WILL SGA + greeting banner
            conn.sendall(b"\xff\xfd\x18\xff\xfb\x03WELCOME TO OKSINIX TELNET TEST\r\n")
            buf = b""
            while b"\n" not in buf:
                chunk = conn.recv(256)
                if not chunk:
                    break
                buf += chunk
            # Strip any 3-byte Telnet IAC responses (0xFF <cmd> <opt>)
            cleaned = bytearray()
            i = 0
            while i < len(buf):
                if buf[i] == 0xFF and i + 2 < len(buf):
                    i += 3
                else:
                    cleaned.append(buf[i])
                    i += 1
            line = cleaned.decode("ascii", errors="replace").strip()
            conn.sendall(f"ECHO: {line}\r\n".encode("ascii"))
            conn.close()
        except Exception:
            pass
        finally:
            srv.close()

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    return port, srv, t


def find_ccia_binary(tmpdir):
    """Locate the ccia-i386 static-PIE compiler binary if available."""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    oksinix_dir = os.path.dirname(script_dir)
    repo_root = os.path.dirname(oksinix_dir)

    candidates = []
    env_bin = os.environ.get("CCIA_BIN")
    if env_bin:
        candidates.append(env_bin)
    candidates.extend([
        os.path.join(repo_root, "..", "ccia", "ccia-i386_stage3"),
        os.path.join(oksinix_dir, "..", "ccia", "ccia-i386_stage3"),
    ])
    for path in candidates:
        if os.path.isfile(path):
            return os.path.abspath(path)

    # Fallback: extract ::ccia from gemiboot.img if present and mcopy is available
    gemiboot_img = os.path.join(repo_root, "build", "gemiboot.img")
    if os.path.isfile(gemiboot_img) and shutil.which("mcopy"):
        extracted = os.path.join(tmpdir, "ccia_extracted")
        res = subprocess.run(
            ["mcopy", "-o", "-i", gemiboot_img, "::ccia", extracted],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if res.returncode == 0 and os.path.isfile(extracted) and os.path.getsize(extracted) > 0:
            return extracted

    return None


def make_fat32_usb_image(out_path, label11, fname83, content_bytes, mbr_banner=None, extra_files=None):
    """Create a FAT32 USB disk image (with MBR partition at LBA 64)."""
    all_files = [(fname83, content_bytes)]
    if extra_files:
        all_files.extend(extra_files)

    part_lba = 64
    rsvd_sec = 32
    num_fats = 2
    if extra_files:
        used_clus = sum(max(1, (len(data) + 511) // 512) for _, data in all_files)
        data_clus = max(128, used_clus + 256)
        fat_sz = max(4, ((data_clus + 3) * 4 + 511) // 512)
        part_secs = rsvd_sec + num_fats * fat_sz + data_clus
        total_sectors = part_lba + part_secs
    else:
        total_sectors = 256
        part_secs = total_sectors - part_lba
        fat_sz = 4
    img = bytearray(total_sectors * 512)

    # Sector 0: MBR with optional signature banner + FAT32 partition entry
    if mbr_banner:
        img[0:len(mbr_banner)] = mbr_banner
    struct.pack_into("<BBBBBBBBII", img, 446, 0x00, 0, 1, 0, 0x0C, 0, 1, 0, part_lba, part_secs)
    img[510] = 0x55
    img[511] = 0xAA

    # Partition sector 0: FAT32 VBR
    p0 = part_lba * 512
    img[p0:p0 + 3] = b"\xeb\x58\x90"
    img[p0 + 3:p0 + 11] = b"OKSINIX "
    struct.pack_into("<H", img, p0 + 0x0B, 512)
    img[p0 + 0x0D] = 1
    struct.pack_into("<H", img, p0 + 0x0E, rsvd_sec)
    img[p0 + 0x10] = num_fats
    img[p0 + 0x15] = 0xF8
    struct.pack_into("<I", img, p0 + 0x1C, part_lba)
    struct.pack_into("<I", img, p0 + 0x20, part_secs)
    struct.pack_into("<I", img, p0 + 0x24, fat_sz)
    struct.pack_into("<I", img, p0 + 0x2C, 2)
    img[p0 + 0x42] = 0x29
    img[p0 + 0x47:p0 + 0x52] = label11
    img[p0 + 0x52:p0 + 0x5A] = b"FAT32   "
    img[p0 + 510] = 0x55
    img[p0 + 511] = 0xAA

    # Build FAT cluster chains for root dir (cluster 2) and all files
    fat_entries = [0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF]
    file_starts = []
    for _, data in all_files:
        start_c = len(fat_entries)
        n_clus = max(1, (len(data) + 511) // 512)
        for i in range(n_clus):
            c = start_c + i
            fat_entries.append((c + 1) if (i + 1 < n_clus) else 0x0FFFFFFF)
        file_starts.append(start_c)

    # Write FAT1 & FAT2
    for f in range(num_fats):
        foff = (part_lba + rsvd_sec + f * fat_sz) * 512
        for idx, val in enumerate(fat_entries):
            struct.pack_into("<I", img, foff + idx * 4, val)

    data_lba = part_lba + rsvd_sec + num_fats * fat_sz
    # Cluster 2 (root dir): volume label + file entries
    r_off = data_lba * 512
    img[r_off:r_off + 11] = label11
    img[r_off + 11] = 0x08
    dir_pos = 32
    for (name83, data), start_c in zip(all_files, file_starts):
        img[r_off + dir_pos:r_off + dir_pos + 11] = name83
        img[r_off + dir_pos + 11] = 0x20
        struct.pack_into("<H", img, r_off + dir_pos + 20, (start_c >> 16) & 0xFFFF)
        struct.pack_into("<H", img, r_off + dir_pos + 26, start_c & 0xFFFF)
        struct.pack_into("<I", img, r_off + dir_pos + 28, len(data))
        dir_pos += 32

        f_off = (data_lba + (start_c - 2)) * 512
        img[f_off:f_off + len(data)] = data

    with open(out_path, "wb") as fp:
        fp.write(img)


def send_hmp_cmd(sock, cmd, delay=0.06):
    sock.sendall((cmd + "\n").encode("ascii"))
    time.sleep(delay)
    while True:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                break
        except BlockingIOError:
            break


def send_keys_str(sock, text):
    # Swiss French (fr_CH QWERTZ) key mapping for QEMU sendkey
    keymap = {
        " ": "spc",
        "/": "shift-7",
        "-": "slash",
        "_": "shift-slash",
        ".": "dot",
        ":": "shift-dot",
        "=": "shift-0",
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
        delay = 0.075 if k.startswith("shift-") else 0.055
        send_hmp_cmd(sock, f"sendkey {k} 30", delay=delay)


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


def wait_for_serial_after(serial_path, anchor, substring, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(serial_path):
            with open(serial_path, "r", errors="replace") as f:
                data = f.read()
            idx = data.rfind(anchor)
            if idx != -1 and substring in data[idx:]:
                return data[idx:]
        time.sleep(0.05)
    with open(serial_path, "r", errors="replace") as f:
        data = f.read()
    raise RuntimeError(
        f"Timed out waiting for '{substring}' after '{anchor}' in serial log.\n--- Serial Output ---\n{data[-4000:]}"
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
        usb_disk3_path = os.path.join(tmpdir, "usb_disk3_test.img")
        usb_disk4_path = os.path.join(tmpdir, "usb_disk4_test.img")

        ccia_bin_path = find_ccia_binary(tmpdir)
        media2_extra = []
        if ccia_bin_path:
            with open(ccia_bin_path, "rb") as fp:
                ccia_bytes = fp.read()
            ccia_test_c = (
                b"#ifndef VAL\n"
                b"#define VAL 7\n"
                b"#endif\n"
                b"int add(int a, int b) {\n"
                b"    return a + b;\n"
                b"}\n"
                b"int main(void) {\n"
                b"    const char *s = \"hi\";\n"
                b"    return add(VAL, s[0] == 'h' ? 1 : 0);\n"
                b"}\n"
            )
            media2_extra = [
                (b"CCIA       ", ccia_bytes),
                (b"CCIA    C  ", ccia_test_c),
            ]

        # Copy usb_disk.img into temporary test working copy so writes don't dirty repo image
        shutil.copyfile(base_disk_path, usb_disk_path)
        make_fat32_usb_image(
            usb_disk2_path, b"MEDIA1     ", b"DISK1   TXT",
            b"Hello from USB Mass Storage /media1!\n",
            mbr_banner=b"OKSINIX_SECOND_USB_DISK_SDB_OK\n\0",
        )
        make_fat32_usb_image(
            usb_disk3_path, b"MEDIA2     ", b"DISK2   TXT",
            b"Hello from USB Mass Storage /media2!\n",
            extra_files=media2_extra if media2_extra else None,
        )
        make_fat32_usb_image(
            usb_disk4_path, b"MEDIA3     ", b"DISK3   TXT",
            b"Hello from USB Mass Storage /media3!\n",
        )

        # Boot directly from the BIOS MBR bootable disk image (oksinix.img)
        # and attach usb-hub + usb-kbd + usb-mouse + 4x usb-storage (/dev/sda..sdd -> /mnt, /media1..3) on xHCI!
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
            "-device", "usb-storage,id=usbdev0,bus=xhci.0,port=1.3,drive=usbdisk",
            "-drive", f"file={usb_disk2_path},if=none,format=raw,id=usbdisk2",
            "-device", "usb-storage,id=usbdev1,bus=xhci.0,port=1.4,drive=usbdisk2",
            "-drive", f"file={usb_disk3_path},if=none,format=raw,id=usbdisk3",
            "-device", "usb-storage,id=usbdev2,bus=xhci.0,port=1.5,drive=usbdisk3",
            "-drive", f"file={usb_disk4_path},if=none,format=raw,id=usbdisk4",
            "-device", "usb-storage,id=usbdev3,bus=xhci.0,port=1.6,drive=usbdisk4",
            "-netdev", "user,id=net0",
            "-device", "usb-net,netdev=net0,bus=xhci.0,port=2,mac=52:54:00:12:34:56",
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
            wait_for_serial(serial_log_path, "[ACPI] S5 poweroff ready")
            wait_for_serial(serial_log_path, "[ACPI-MMAP] #0:")
            wait_for_serial(serial_log_path, "[ACPI-MMAP] Source: ACPI E820 (INT 15h)")
            wait_for_serial(serial_log_path, "[VM] x86 32-bit paging enabled")
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
            wait_for_serial(serial_log_path, "[DEVFS] Registered block device /dev/sdc (8,32)")
            wait_for_serial(serial_log_path, "[DEVFS] Registered block device /dev/sdd (8,48)")
            wait_for_serial(serial_log_path, "[FAT32] Mounted volume \"OKSINIXFAT \" at /mnt")
            wait_for_serial(serial_log_path, "[FAT32] Mounted volume \"MEDIA1     \" at /media1")
            wait_for_serial(serial_log_path, "[FAT32] Mounted volume \"MEDIA2     \" at /media2")
            wait_for_serial(serial_log_path, "[FAT32] Mounted volume \"MEDIA3     \" at /media3")
            wait_for_serial(serial_log_path, "[USBNET] USB Ethernet (RNDIS) ready on slot")
            wait_for_serial(serial_log_path, "MAC=52:54:00:12:34:56")
            wait_for_serial(serial_log_path, "[DEVFS] Registered network device /dev/eth0 (10,200)")
            wait_for_serial(serial_log_path, "[/bin/init] xHCI USB bus active (usb-hub usb-kbd usb-mouse usb-net usb-storage:/dev/sda,/dev/sdb,/dev/sdc,/dev/sdd)")
            wait_for_serial(serial_log_path, "[/bin/init] Mounted FAT32 filesystem on /mnt")
            wait_for_serial(serial_log_path, "[/bin/init] Mounted FAT32 filesystem on /media1")
            wait_for_serial(serial_log_path, "[/bin/init] Mounted FAT32 filesystem on /media2")
            wait_for_serial(serial_log_path, "[/bin/init] Mounted FAT32 filesystem on /media3")
            wait_for_serial(serial_log_path, "[TEST] Preemptive multitasking self-test PASSED")
            wait_for_serial(serial_log_path, "[TEST] xv6-style hardware task protection self-test PASSED")
            wait_for_serial(serial_log_path, "[ELF32] Launching Linux i386 binary \"/bin/sh\" in Ring 3")
            wait_for_serial(serial_log_path, "oksinix:/# ")
            print("[PASS] 1/12 Boot, VGA 640x480, PCI xHCI, USB Hub, 4x USB storage auto-mounted (/mnt, /media1..3)")

            # Connect to QEMU HMP monitor socket
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(mon_sock_path)
            sock.setblocking(False)
            time.sleep(0.1)

            # 2. Type 'lsusb' and 'uptime' via xHCI USB Keyboard (behind USB Hub port 1.1)
            send_keys_str(sock, "lsusb\n")
            serial_out = wait_for_serial(serial_log_path, "-> /dev/sdd")
            if "USB Hub (slot 1, 8 ports, 6 downstream)" not in serial_out:
                raise RuntimeError("Expected USB Hub info in lsusb output")
            if "[USB-KBD] key press" not in serial_out:
                raise RuntimeError("Expected key events to arrive via xHCI USB Keyboard ([USB-KBD])")
            send_keys_str(sock, "keymap\n")
            wait_for_serial(serial_log_path, "Current keymap: fr_CH (Swiss French QWERTZ)")
            send_keys_str(sock, "uptime\n")
            wait_for_serial(serial_log_path, "PIT ticks @ 1000 Hz)")
            print("[PASS] 2/9 xHCI USB Keyboard (Swiss French fr_CH QWERTZ keymap), 'lsusb', 'keymap' & 'uptime' in /bin/sh")

            # 3. Verify FAT32 directory listing ('ls /mnt', 'ls /mnt/DOCS') and reading 8.3 & LFN files
            send_keys_str(sock, "ls /mnt\n")
            wait_for_serial(serial_log_path, "-rw-r--r--  README.TXT")
            wait_for_serial(serial_log_path, "drwxr-xr-x  DOCS/")
            send_keys_str(sock, "cat /mnt/README.TXT\n")
            wait_for_serial(serial_log_path, "Oksinix FAT32 filesystem on USB Mass Storage (/dev/sda1 -> /mnt)")
            send_keys_str(sock, "ls /mnt/DOCS\n")
            wait_for_serial(serial_log_path, "-rw-r--r--  Long_Filename_Example.txt")
            send_keys_str(sock, "cat /mnt/DOCS/NOTES.TXT\n")
            wait_for_serial(serial_log_path, "Subdirectory file inside FAT32 /mnt/DOCS/NOTES.TXT")
            send_keys_str(sock, "cat /mnt/DOCS/Long_Filename_Example.txt\n")
            wait_for_serial(serial_log_path, "FAT32 Long File Name (VFAT LFN) read test passed!")
            print("[PASS] 3/11 FAT32 8.3 & VFAT LFN directory listing & file reads verified")

            # 4. Verify FAT32 file creation, LFN create/mkdir/write, cluster allocation, 'cp' & readback
            send_keys_str(sock, "echo fat32-write-ok > /mnt/TEST.TXT\n")
            time.sleep(0.2)
            send_keys_str(sock, "cat /mnt/TEST.TXT\n")
            wait_for_serial(serial_log_path, "fat32-write-ok")
            send_keys_str(sock, "cp /mnt/TEST.TXT /mnt/COPY.TXT\n")
            time.sleep(0.2)
            send_keys_str(sock, "cat /mnt/COPY.TXT\n")
            wait_for_serial(serial_log_path, "fat32-write-ok")
            send_keys_str(sock, "mkdir /mnt/LongDirectoryName\n")
            time.sleep(0.2)
            send_keys_str(sock, "echo lfn-write-ok > /mnt/LongDirectoryName/MyLongFileNameTest.txt\n")
            time.sleep(0.2)
            send_keys_str(sock, "ls /mnt/LongDirectoryName\n")
            wait_for_serial(serial_log_path, "-rw-r--r--  MyLongFileNameTest.txt")
            send_keys_str(sock, "cat /mnt/LongDirectoryName/MyLongFileNameTest.txt\n")
            wait_for_serial(serial_log_path, "lfn-write-ok")
            print("[PASS] 4/11 FAT32 8.3 & VFAT LFN file/directory creation, 'cp', write & readback verified")

            # 5. Test modal 'vi' editor (standalone Ring-3 /mnt/VI.ELF): insert, visual 'v' + 'y' + 'p', join 'J', 'yy' + 'p', ':wq'
            send_keys_str(sock, "vi /mnt/VITEST.TXT\n")
            wait_for_serial(serial_log_path, "[ELF32] Launching Linux i386 binary \"/mnt/VI.ELF\" in Ring 3")
            time.sleep(0.15)
            send_keys_str(sock, "iEdited with Oksinix vi!\nLine2\x1b0vlllyllllpkJyyp:wq\n")
            wait_for_serial(serial_log_path, '[VI] saved "/mnt/VITEST.TXT"')
            send_keys_str(sock, "cat /mnt/VITEST.TXT\n")
            serial_out = wait_for_serial(serial_log_path, "Edited with Oksinix vi! Line2Line")
            if serial_out.count("Edited with Oksinix vi! Line2Line") < 2:
                raise RuntimeError("Expected 'yy' + 'p' to duplicate joined line in /mnt/VITEST.TXT")
            print("[PASS] 5/11 Modal 'vi' editor (Ring-3 /mnt/VI.ELF: Insert, Visual 'v', 'y'/'yy', 'p', 'J', ':wq') verified on FAT32")

            # 6. Verify 4x USB Mass Storage mounts (/mnt, /media1..3), raw /dev/sda..sdb, manual umount/mount, and hot-unplug auto-umount + hot-replug auto-mount
            send_keys_str(sock, "cat /dev/sda\n")
            wait_for_serial(serial_log_path, "OKSINIX_USB_STORAGE_OK")
            send_keys_str(sock, "cat /dev/sdb\n")
            wait_for_serial(serial_log_path, "OKSINIX_SECOND_USB_DISK_SDB_OK")
            send_keys_str(sock, "cat /media1/DISK1.TXT\n")
            wait_for_serial(serial_log_path, "Hello from USB Mass Storage /media1!")
            send_keys_str(sock, "cat /media2/DISK2.TXT\n")
            wait_for_serial(serial_log_path, "Hello from USB Mass Storage /media2!")
            send_keys_str(sock, "cat /media3/DISK3.TXT\n")
            wait_for_serial(serial_log_path, "Hello from USB Mass Storage /media3!")
            send_keys_str(sock, "umount /media3\n")
            wait_for_serial(serial_log_path, "[FAT32] Unmounted /media3 (/dev/sdd)")
            send_keys_str(sock, "mount /dev/sdd /media3\n")
            time.sleep(0.2)
            # Hot-unplug 4th USB drive via QEMU HMP and verify automatic umount + devfs unregister
            send_hmp_cmd(sock, "device_del usbdev3", delay=0.5)
            wait_for_serial(serial_log_path, "[XHCI-MSC] USB Mass Storage disconnected")
            wait_for_serial(serial_log_path, "[DEVFS] Unregistered block device /dev/sdd")
            # Hot-replug 4th USB drive and verify automatic mount back to /media3
            send_hmp_cmd(sock, "device_add usb-storage,id=usbdev3,bus=xhci.0,port=1.6,drive=usbdisk4", delay=0.6)
            send_keys_str(sock, "echo hotplug-ok > /media3/REPLUG.TXT\n")
            time.sleep(0.3)
            send_keys_str(sock, "cat /media3/REPLUG.TXT\n")
            wait_for_serial(serial_log_path, "hotplug-ok")
            print("[PASS] 6/12 4x USB Mass Storage (/mnt, /media1..3), manual umount/mount, hot-unplug auto-umount & hot-replug auto-mount verified")

            # 7. Execute 32-bit Linux x86 (i386) static & dynamically linked ELF binaries
            send_keys_str(sock, "/mnt/LINUX.ELF\n")
            wait_for_serial(serial_log_path, "[ELF32] Launching Linux i386 binary \"/mnt/LINUX.ELF\"")
            wait_for_serial(serial_log_path, "[LINUX-ELF] execve child running via int 0x80 (PID verified)")
            wait_for_serial(serial_log_path, "[LINUX-ELF] BSD socket syscalls (socket/bind/connect/send/recv) PASSED!")
            wait_for_serial(serial_log_path, "[LINUX-ELF] All 37 Linux i386 syscalls PASSED!")
            send_keys_str(sock, "memmap\n")
            wait_for_serial(serial_log_path, "ACPI RSDP:")
            wait_for_serial(serial_log_path, "Total Usable RAM:")
            send_keys_str(sock, "/mnt/DYN.ELF\n")
            wait_for_serial(serial_log_path, "at 0x01500000")
            wait_for_serial(serial_log_path, "[DYN] PT_INTERP \"/lib/ld-linux.so.2\" loaded at 0x01600000")
            wait_for_serial(serial_log_path, "[LINUX-DYN] Shared library LIBTEST.SO + libc.so.6 + PT_INTERP PASSED!")
            send_keys_str(sock, "cc /mnt/HELLO.C\n")
            wait_for_serial(serial_log_path, "[ELF32] Launching Linux i386 binary \"/mnt/CC.ELF\" in Ring 3")
            wait_for_serial(serial_log_path, "int main() {return 0;}")
            send_keys_str(sock, "as -o /mnt/T.O /mnt/DOCS/HELLO.S\n")
            wait_for_serial(serial_log_path, "[ELF32] Launching Linux i386 binary \"/mnt/AS.ELF\" in Ring 3")
            time.sleep(0.5)
            send_keys_str(sock, "ld -o /mnt/T.ELF /mnt/T.O\n")
            wait_for_serial(serial_log_path, "[ELF32] Launching Linux i386 binary \"/mnt/LD.ELF\" in Ring 3")
            time.sleep(0.5)
            send_keys_str(sock, "/mnt/T.ELF\n")
            wait_for_serial(serial_log_path, "Hello from Oksinix as+ld!")
            send_keys_str(sock, "prottest\n")
            wait_for_serial(serial_log_path, "Hardware task protection test (Ring 3 / paging / TSS) PASSED.")
            print("[PASS] 7/10 Linux x86 32-bit Ring-3 static (/mnt/LINUX.ELF, /mnt/CC.ELF, /mnt/AS.ELF + /mnt/LD.ELF), dynamic (/mnt/DYN.ELF + LIBTEST.SO + libc.so.6) & 'prottest' verified")

            # 7b. Test standalone Ring-3 C90 compiler 'ccia' (/mnt/CCIA via 'ccia' & /media2/CCIA): -v, -E -DVAL=42, -S -DVAL=42, and /mnt/HELLO.C
            if ccia_bin_path:
                send_keys_str(sock, "ccia -v\n")
                out = wait_for_serial_after(serial_log_path, '[SH] exec="ccia -v"', "exited with code 0")
                if '[ELF32] Launching Linux i386 binary "/mnt/CCIA" in Ring 3' not in out or "ccia-i386 version" not in out:
                    raise RuntimeError(f"Unexpected output from ccia -v (/mnt/CCIA):\n{out}")
                send_keys_str(sock, "/media2/CCIA -v\n")
                out = wait_for_serial_after(serial_log_path, '[SH] exec="/media2/CCIA -v"', "exited with code 0")
                if '[ELF32] Launching Linux i386 binary "/media2/CCIA" in Ring 3' not in out or "ccia-i386 version" not in out:
                    raise RuntimeError(f"Unexpected output from /media2/CCIA -v:\n{out}")
                send_keys_str(sock, "/media2/CCIA -E -DVAL=42 /media2/CCIA.C -o /media2/CCIA.I\n")
                wait_for_serial_after(serial_log_path, '[SH] exec="/media2/CCIA -E -DVAL=42 /media2/CCIA.C -o /media2/CCIA.I"', "exited with code 0")
                send_keys_str(sock, "cat /media2/CCIA.I\n")
                wait_for_serial_after(serial_log_path, '[SH] exec="cat /media2/CCIA.I"', "add ( 42 ,")
                send_keys_str(sock, "/media2/CCIA -S -DVAL=42 /media2/CCIA.C -o /media2/CCIA.S\n")
                wait_for_serial_after(serial_log_path, '[SH] exec="/media2/CCIA -S -DVAL=42 /media2/CCIA.C -o /media2/CCIA.S"', "exited with code 0")
                send_keys_str(sock, "cat /media2/CCIA.S\n")
                asm_out = wait_for_serial_after(serial_log_path, '[SH] exec="cat /media2/CCIA.S"', ".section .note.GNU-stack")
                for expected in (".section .rodata", ".globl add", ".globl main", "movl $42, %eax", "call add@PLT"):
                    if expected not in asm_out:
                        raise RuntimeError(f"Missing '{expected}' in ccia -S output:\n{asm_out}")
                send_keys_str(sock, "/media2/CCIA -S /mnt/HELLO.C -o /mnt/HELLO.S\n")
                wait_for_serial_after(serial_log_path, '[SH] exec="/media2/CCIA -S /mnt/HELLO.C -o /mnt/HELLO.S"', "exited with code 0")
                send_keys_str(sock, "cat /mnt/HELLO.S\n")
                hello_asm = wait_for_serial_after(serial_log_path, '[SH] exec="cat /mnt/HELLO.S"', ".section .note.GNU-stack")
                if ".globl main" not in hello_asm or "ret" not in hello_asm:
                    raise RuntimeError(f"Expected compiled assembly for /mnt/HELLO.C, got:\n{hello_asm}")
                send_keys_str(sock, "cd /mnt\n")
                time.sleep(0.15)
                send_keys_str(sock, "as HELLO.S\n")
                wait_for_serial_after(serial_log_path, '[SH] exec="as HELLO.S"', "exited with code 0")
                send_keys_str(sock, "as -o CCIA.O /media2/CCIA.S\n")
                wait_for_serial_after(serial_log_path, '[SH] exec="as -o CCIA.O /media2/CCIA.S"', "exited with code 0")
                send_keys_str(sock, "cd /\n")
                time.sleep(0.15)
                print("[PASS] 7b/14 Ring-3 C90 compiler 'ccia' (/mnt/CCIA & /media2/CCIA: -v, -E -DVAL=42, -S -DVAL=42, /mnt/HELLO.C -> /mnt/HELLO.S -> 'as HELLO.S' & 'as CCIA.S') verified")
            else:
                print("[SKIP] 7b/14 ccia binary not found (set CCIA_BIN or build ../ccia/ccia-i386_stage3)")

            # 8. Test xHCI USB Ethernet (RNDIS) DHCP, ARP + ICMP Echo ping to QEMU SLIRP gateway 10.0.2.2
            send_keys_str(sock, "net\n")
            wait_for_serial(serial_log_path, "eth0: USB RNDIS  MAC 52:54:00:12:34:56")
            send_keys_str(sock, "net dhcp\n")
            wait_for_serial(serial_log_path, "DHCP: leased IP 10.0.2.15  Mask 255.255.255.0  GW 10.0.2.2  DNS 10.0.2.3")
            send_keys_str(sock, "net ping\n")
            wait_for_serial(serial_log_path, "ARP reply: 10.0.2.2 is at")
            wait_for_serial(serial_log_path, "ICMP echo reply from 10.0.2.2: seq=1 OK")
            print("[PASS] 8/13 xHCI USB Ethernet (RNDIS) MAC query, DHCPv4 lease, ARP & ICMP ping to 10.0.2.2 verified")

            # 8b. Test TCP + standalone Ring-3 'telnet [host [port]]' (/mnt/TELNET.ELF) against local TCP server via QEMU SLIRP 10.0.2.2
            telnet_port, telnet_srv, telnet_thr = start_mock_telnet_server()
            try:
                send_keys_str(sock, f"telnet 10.0.2.2 {telnet_port}\n")
                wait_for_serial(serial_log_path, "[ELF32] Launching Linux i386 binary \"/mnt/TELNET.ELF\" in Ring 3")
                wait_for_serial(serial_log_path, f"Connected to 10.0.2.2 (10.0.2.2:{telnet_port}).")
                wait_for_serial(serial_log_path, "WELCOME TO OKSINIX TELNET TEST")
                send_keys_str(sock, "hello-telnet\n")
                wait_for_serial(serial_log_path, "ECHO: hello-telnet")
                wait_for_serial(serial_log_path, "Connection closed by foreign host.")
                send_keys_str(sock, "telnet\n")
                wait_for_serial(serial_log_path, "telnet> ")
                send_keys_str(sock, "quit\n")
                time.sleep(0.15)
            finally:
                telnet_thr.join(timeout=2.0)
            print(f"[PASS] 8b/14 Ring-3 TCP client '/mnt/TELNET.ELF' ('telnet 10.0.2.2 {telnet_port}': IAC negotiation, send/recv, FIN close, REPL) verified")

            # 9. Test xHCI USB Mouse (on Hub port 1.2) movement & button clicks
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
            print(f"[PASS] 9/11 VGA Framebuffer screendump verified ({w}x{h}, {num_colors} active palette colors)")

            # 10. Test ACPI S5 'shutdown' command powers off the QEMU VM cleanly
            send_keys_str(sock, "shutdown\n")
            serial_out = wait_for_serial(serial_log_path, "[ACPI] Entering S5 soft-off")
            rc = proc.wait(timeout=5)
            if rc != 0:
                raise RuntimeError(f"Expected QEMU exit code 0 after ACPI shutdown, got {rc}")
            with open(serial_log_path, "r", errors="replace") as f:
                serial_out = f.read()
            if "[ACPI] ACPI S5 fallback poweroff" in serial_out:
                raise RuntimeError("ACPI S5 fell back instead of powering off via FADT/DSDT _S5")
            print("[PASS] 10/11 ACPI S5 'shutdown' command cleanly powered off VM (exit=0)")

            sock.close()

        finally:
            if proc.poll() is None:
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
