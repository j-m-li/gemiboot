# SPDX-License-Identifier: CC0-1.0
# Public Domain (CC0 1.0 Universal)
# Oksinix x86 32-bit C90 Kernel Makefile

CC      := clang
LD      := ld.lld
OBJCOPY := objcopy
QEMU    := qemu-system-i386

CFLAGS  := -m32 -std=c89 -pedantic -Wall -Wextra -Werror \
           -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -mno-sse -mno-mmx -mno-sse2 -mno-80387 -msoft-float \
           -O2 -Iinclude

ASFLAGS := -m32

LDFLAGS := -m elf_i386 -T linker.ld -nostdlib

ASM_SRCS := boot/boot.S \
            arch/x86/io.S \
            arch/x86/isr.S

C_SRCS   := arch/x86/gdt.c \
            arch/x86/pic.c \
            arch/x86/idt.c \
            drivers/acpi.c \
            drivers/pit.c \
            drivers/serial.c \
            drivers/pci.c \
            drivers/ps2.c \
            drivers/keyboard.c \
            drivers/mouse.c \
            drivers/xhci.c \
            drivers/vga.c \
            drivers/console.c \
            fs/vfs.c \
            fs/devfs.c \
            fs/fat32.c \
            user/init.c \
            user/sh.c \
            user/vi.c \
            lib/string.c \
            kernel/syscall.c \
            kernel/main.c

OBJS       := $(ASM_SRCS:.S=.o) $(C_SRCS:.c=.o)
KERNEL_ELF := oksinix.elf
KERNEL_BIN := oksinix.bin
MBR_OBJ    := boot/mbr.o
MBR_BIN    := boot/mbr.bin
LINUX_OBJ  := user/linux_hello.o
LINUX_ELF  := user/linux_hello.elf
SHLIB_OBJ  := user/libtest.o
SHLIB_SO   := user/libtest.so
LIBC_OBJ   := user/libc_stub.o
LIBC_SO    := user/libc.so.6
DYN_OBJ    := user/linux_dyn.o
DYN_ELF    := user/linux_dyn.elf
BOOT_IMG   := oksinix.img
USB_DISK   := usb_disk.img

.PHONY: all clean run run-multiboot test disk

all: $(KERNEL_ELF) $(BOOT_IMG) $(USB_DISK)

$(KERNEL_ELF): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(MBR_OBJ): boot/mbr.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(MBR_BIN): $(MBR_OBJ)
	$(LD) -m elf_i386 --image-base=0x7000 -Ttext=0x7C00 --oformat binary -nostdlib -o $@ $<

$(LINUX_OBJ): user/linux_hello.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(LINUX_ELF): $(LINUX_OBJ)
	$(LD) -m elf_i386 -pie --hash-style=sysv --strip-all -nostdlib -o $@ $<

$(SHLIB_OBJ): user/libtest.c
	$(CC) -m32 -std=c89 -pedantic -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fPIC -O2 -c $< -o $@

$(SHLIB_SO): $(SHLIB_OBJ)
	$(LD) -m elf_i386 -shared -soname=libtest.so --hash-style=sysv --strip-all -nostdlib -o $@ $<

$(LIBC_OBJ): user/libc_stub.c
	$(CC) -m32 -std=c89 -pedantic -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fPIC -O2 -c $< -o $@

$(LIBC_SO): $(LIBC_OBJ)
	$(LD) -m elf_i386 -shared -soname=libc.so.6 --hash-style=sysv --strip-all -nostdlib -o $@ $<

$(DYN_OBJ): user/linux_dyn.c
	$(CC) -m32 -std=c89 -pedantic -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector -fPIC -O2 -c $< -o $@

$(DYN_ELF): $(DYN_OBJ) $(SHLIB_SO) $(LIBC_SO)
	$(LD) -m elf_i386 -pie -dynamic-linker /lib/ld-linux.so.2 --hash-style=sysv --strip-all -nostdlib -o $@ $(DYN_OBJ) $(SHLIB_SO) $(LIBC_SO)

$(BOOT_IMG): $(MBR_BIN) $(KERNEL_BIN) $(LINUX_ELF) $(DYN_ELF) $(SHLIB_SO) scripts/mkdisk.py
	python3 scripts/mkdisk.py $(MBR_BIN) $(KERNEL_BIN) $(BOOT_IMG) $(LINUX_ELF) $(DYN_ELF) $(SHLIB_SO)

$(USB_DISK): $(BOOT_IMG)
	cp $(BOOT_IMG) $(USB_DISK)

disk: $(BOOT_IMG) $(USB_DISK)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

run: $(BOOT_IMG) $(USB_DISK)
	$(QEMU) -drive file=$(BOOT_IMG),format=raw -serial stdio -vga std \
		-device qemu-xhci,id=xhci \
		-device usb-hub,bus=xhci.0,port=1 \
		-device usb-kbd,bus=xhci.0,port=1.1 \
		-device usb-mouse,bus=xhci.0,port=1.2 \
		-drive file=$(USB_DISK),if=none,format=raw,id=usbdisk \
		-device usb-storage,bus=xhci.0,port=1.3,drive=usbdisk

run-multiboot: $(KERNEL_ELF) $(USB_DISK)
	$(QEMU) -kernel $(KERNEL_ELF) -serial stdio -vga std \
		-device qemu-xhci,id=xhci \
		-device usb-hub,bus=xhci.0,port=1 \
		-device usb-kbd,bus=xhci.0,port=1.1 \
		-device usb-mouse,bus=xhci.0,port=1.2 \
		-drive file=$(USB_DISK),if=none,format=raw,id=usbdisk \
		-device usb-storage,bus=xhci.0,port=1.3,drive=usbdisk

test: $(KERNEL_ELF) $(BOOT_IMG) $(USB_DISK)
	python3 tests/test_qemu.py $(BOOT_IMG) $(USB_DISK) $(KERNEL_ELF)

clean:
	rm -f $(OBJS) *.img $(MBR_OBJ) $(MBR_BIN) $(LINUX_OBJ) $(LINUX_ELF) $(SHLIB_OBJ) $(SHLIB_SO) $(LIBC_OBJ) $(LIBC_SO) $(DYN_OBJ) $(DYN_ELF) $(KERNEL_ELF) $(KERNEL_BIN) tests/*.ppm
