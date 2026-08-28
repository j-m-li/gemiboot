CC = clang
HOST_CC = clang
HOST_CFLAGS = -Wall -Wextra -ansi -pedantic -Werror -O2

CFLAGS_32 = -m32 -march=i686 -ffreestanding -fno-pic -fno-pie \
            -fno-stack-protector -fno-builtin -nostdlib -Wall -Wextra \
            -ansi -pedantic -Werror -Wno-long-long -Wno-unused-parameter -O2 \
            -Isrc/include

CFLAGS_EFI32 = -target i686-unknown-windows -m32 -ffreestanding \
               -fno-builtin -nostdlib -nostdinc -mno-red-zone \
               -ansi -pedantic -Wall -Wextra -Werror -Wno-long-long -Wno-unused-parameter -O2 \
               -Isrc/include

CFLAGS_EFI64 = -target x86_64-unknown-windows -ffreestanding \
               -fno-builtin -nostdlib -nostdinc -mno-red-zone \
               -ansi -pedantic -Wall -Wextra -Werror -Wno-long-long -Wno-unused-parameter -O2 \
               -Isrc/include

LD = ld.lld
LLD_LINK = lld-link

BUILD_DIR = build
TOOLS_DIR = tools

MKDISK = $(TOOLS_DIR)/mkdisk

MBR_BIN = $(BUILD_DIR)/mbr.bin
VBR_BIN = $(BUILD_DIR)/vbr.bin
IOSYS_BIN = $(BUILD_DIR)/io.sys
BOOTIA32_EFI = $(BUILD_DIR)/BOOTIA32.EFI
BOOTX64_EFI = $(BUILD_DIR)/BOOTX64.EFI
KERNEL_ELF = $(BUILD_DIR)/gemios.elf
DISK_IMG = $(BUILD_DIR)/gemiboot.img

.PHONY: all bios efi32 efi64 disk run-bios run-uefi64 run-uefi32 test clean

all: bios efi32 efi64 disk

# Host Tools
$(MKDISK): tools/mkdisk.c
	$(HOST_CC) $(HOST_CFLAGS) $< -o $@
	@echo "[HOST_CC] $< -> $@"

# Kernel acquisition
$(KERNEL_ELF):
	@mkdir -p $(BUILD_DIR)
	@if [ -f /home/jml/src/gemios/build/gemios.elf ]; then \
		cp -f /home/jml/src/gemios/build/gemios.elf $(KERNEL_ELF); \
	elif [ -d /home/jml/src/gemios ]; then \
		make -C /home/jml/src/gemios; \
		cp -f /home/jml/src/gemios/build/gemios.elf $(KERNEL_ELF); \
	else \
		echo "ERROR: gemios.elf not found"; exit 1; \
	fi
	@echo "[KERNEL] Acquired $(KERNEL_ELF)"

# BIOS Boot Loader Targets
$(BUILD_DIR)/mbr.o: src/bios/mbr.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -m32 -c -nostdlib -ffreestanding -Isrc/include $< -o $@

$(MBR_BIN): $(BUILD_DIR)/mbr.o src/bios/mbr.ld
	$(LD) -m elf_i386 -T src/bios/mbr.ld --oformat binary $< -o $@
	@echo "[BIOS] Created $@"

$(BUILD_DIR)/vbr.o: src/bios/vbr.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -m32 -c -nostdlib -ffreestanding -Isrc/include $< -o $@

$(VBR_BIN): $(BUILD_DIR)/vbr.o src/bios/vbr.ld
	$(LD) -m elf_i386 -T src/bios/vbr.ld --oformat binary $< -o $@
	@echo "[BIOS] Created $@"

$(BUILD_DIR)/io16.o: src/bios/io16.S
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_32) -c $< -o $@

$(BUILD_DIR)/io32.o: src/bios/io32.S
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_32) -c $< -o $@

$(BUILD_DIR)/bios_loader.o: src/bios/bios_loader.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_32) -c $< -o $@

$(BUILD_DIR)/bios_disk.o: src/bios/bios_disk.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_32) -c $< -o $@

$(BUILD_DIR)/fat_bios.o: src/common/fat.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_32) -c $< -o $@

$(BUILD_DIR)/elf_bios.o: src/common/elf.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_32) -c $< -o $@

$(BUILD_DIR)/string_bios.o: src/common/string.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_32) -c $< -o $@

BIOS_OBJS = $(BUILD_DIR)/io16.o $(BUILD_DIR)/io32.o $(BUILD_DIR)/bios_loader.o \
            $(BUILD_DIR)/bios_disk.o $(BUILD_DIR)/fat_bios.o $(BUILD_DIR)/elf_bios.o \
            $(BUILD_DIR)/string_bios.o

$(IOSYS_BIN): $(BIOS_OBJS) src/bios/io.ld
	$(LD) -m elf_i386 -T src/bios/io.ld --oformat binary $(BIOS_OBJS) -o $@
	@echo "[BIOS] Created $@"

bios: $(MBR_BIN) $(VBR_BIN) $(IOSYS_BIN)

# UEFI 32-bit Targets
$(BUILD_DIR)/bootia32_s.o: src/efi32/bootia32.S
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_EFI32) -c $< -o $@

$(BUILD_DIR)/efi32.o: src/efi32/efi32.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_EFI32) -c $< -o $@

$(BUILD_DIR)/elf_efi32.o: src/common/elf.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_EFI32) -c $< -o $@

$(BUILD_DIR)/string_efi32.o: src/common/string.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_EFI32) -c $< -o $@

EFI32_OBJS = $(BUILD_DIR)/bootia32_s.o $(BUILD_DIR)/efi32.o \
             $(BUILD_DIR)/elf_efi32.o $(BUILD_DIR)/string_efi32.o

$(BOOTIA32_EFI): $(EFI32_OBJS)
	$(LLD_LINK) -entry:efi_main -subsystem:efi_application -nodefaultlib -out:$@ $(EFI32_OBJS)
	@echo "[UEFI-32] Created $@"

efi32: $(BOOTIA32_EFI)

# UEFI 64-bit Targets
$(BUILD_DIR)/bootx64_s.o: src/efi64/bootx64.S
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_EFI64) -c $< -o $@

$(BUILD_DIR)/efi64.o: src/efi64/efi64.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_EFI64) -c $< -o $@

$(BUILD_DIR)/elf_efi64.o: src/common/elf.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_EFI64) -c $< -o $@

$(BUILD_DIR)/string_efi64.o: src/common/string.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_EFI64) -c $< -o $@

EFI64_OBJS = $(BUILD_DIR)/bootx64_s.o $(BUILD_DIR)/efi64.o \
             $(BUILD_DIR)/elf_efi64.o $(BUILD_DIR)/string_efi64.o

$(BOOTX64_EFI): $(EFI64_OBJS)
	$(LLD_LINK) -entry:efi_main -subsystem:efi_application -nodefaultlib -out:$@ $(EFI64_OBJS)
	@echo "[UEFI-64] Created $@"

efi64: $(BOOTX64_EFI)

# Bootable Disk Image Target
$(DISK_IMG): $(MKDISK) $(MBR_BIN) $(VBR_BIN) $(IOSYS_BIN) $(BOOTIA32_EFI) $(BOOTX64_EFI) $(KERNEL_ELF)
	$(MKDISK) $(DISK_IMG) $(MBR_BIN) $(VBR_BIN) $(IOSYS_BIN) $(BOOTIA32_EFI) $(BOOTX64_EFI) $(KERNEL_ELF)
	@echo "[DISK] Created bootable image $(DISK_IMG)"

disk: $(DISK_IMG)

# Run Scripts
run-bios: $(DISK_IMG)
	./run-qemu.sh --bios

run-uefi64: $(DISK_IMG)
	./run-qemu.sh --uefi64

run-uefi32: $(DISK_IMG)
	./run-qemu.sh --uefi32

list: $(DISK_IMG)
	@echo "=== Root Directory ==="
	@mdir -i $(DISK_IMG)@@1M ::
	@echo ""
	@echo "=== /EFI/BOOT Directory ==="
	@mdir -i $(DISK_IMG)@@1M ::/EFI/BOOT

test: $(DISK_IMG)
	./test-qemu.sh

clean:
	rm -rf $(BUILD_DIR) $(TOOLS_DIR)/mkdisk
