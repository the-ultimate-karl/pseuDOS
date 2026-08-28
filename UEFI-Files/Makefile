CC = gcc
LD = ld
PYTHON = python3

CFLAGS = -ffreestanding \
         -fno-stack-protector \
         -fno-stack-check \
         -fno-ident \
         -fno-asynchronous-unwind-tables \
         -fshort-wchar \
         -mno-red-zone \
         -maccumulate-outgoing-args \
         -Wall \
         -Wextra \
         -Iinclude

LDFLAGS_BOOT = -nostdlib \
               -m i386pep \
               --subsystem 10 \
               --image-base 0x10000000 \
               --file-alignment 0x1000 \
               --section-alignment 0x1000 \
               -e efi_main

LDFLAGS_KERNEL = -nostdlib \
                 -m i386pep \
                 --subsystem 10 \
                 --image-base 0x10000000 \
                 --file-alignment 0x1000 \
                 --section-alignment 0x1000 \
                 -e kernel_main

BOOT_SRCS = src/boot/bootloader.c \
            src/boot/errtext.c \
            src/lib/string.c \
            src/lib/guid.c

KERNEL_SRCS = src/kernel/kernel.c \
              src/kernel/shell.c \
              src/fs/vfs.c \
              src/fs/devpath.c \
              src/lib/heap.c \
              src/lib/string.c \
              src/lib/guid.c \
              src/drivers/console.c \
              src/drivers/keyboard.c \
              src/drivers/cpu.c \
              src/drivers/memory.c \
              src/drivers/pci.c \
              src/drivers/acpi.c

BOOT_OBJS = $(patsubst src/%.c, bin/%.o, $(BOOT_SRCS))
KERNEL_OBJS = $(patsubst src/%.c, bin/%.o, $(KERNEL_SRCS))

TARGET_EFI = bin/BOOTX64.EFI
TARGET_KERNEL = bin/kernel.bin
TARGET_ISO = build/pseuDOS.iso

OVMF_BIOS ?= /usr/share/ovmf/OVMF.fd

.PHONY: all iso run run-serial clean

all: $(TARGET_ISO)

$(TARGET_ISO): $(TARGET_EFI) $(TARGET_KERNEL)
	@mkdir -p build
	$(PYTHON) scripts/make_iso.py --efi $(TARGET_EFI) --kernel $(TARGET_KERNEL) --iso $(TARGET_ISO)

$(TARGET_EFI): $(BOOT_OBJS)
	@mkdir -p bin
	$(LD) $(LDFLAGS_BOOT) -o $@ $^

$(TARGET_KERNEL): $(KERNEL_OBJS)
	@mkdir -p bin
	$(LD) $(LDFLAGS_KERNEL) -o $@ $^

bin/boot/%.o: src/boot/%.c
	@mkdir -p bin/boot
	$(CC) $(CFLAGS) -c $< -o $@

bin/lib/%.o: src/lib/%.c
	@mkdir -p bin/lib
	$(CC) $(CFLAGS) -c $< -o $@

bin/fs/%.o: src/fs/%.c
	@mkdir -p bin/fs
	$(CC) $(CFLAGS) -c $< -o $@

bin/drivers/%.o: src/drivers/%.c
	@mkdir -p bin/drivers
	$(CC) $(CFLAGS) -c $< -o $@

bin/kernel/%.o: src/kernel/%.c
	@mkdir -p bin/kernel
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET_ISO)
	qemu-system-x86_64 -bios $(OVMF_BIOS) -cdrom $(TARGET_ISO) -net none

run-serial: $(TARGET_ISO)
	qemu-system-x86_64 -display none -serial stdio -bios $(OVMF_BIOS) -cdrom $(TARGET_ISO) -net none

clean:
	rm -rf bin/boot bin/lib bin/fs bin/drivers bin/kernel bin/*.o bin/*.so bin/*.EFI bin/*.efi bin/*.bin build/*.iso build/*.img
