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

LDFLAGS = -nostdlib \
          -m i386pep \
          --subsystem 10 \
          --image-base 0x10000000 \
          --file-alignment 0x1000 \
          --section-alignment 0x1000 \
          -e efi_main

SRCS = src/boot/bootloader.c \
       src/boot/errtext.c \
       src/kernel/kernel.c

OBJS = $(patsubst src/%.c, bin/%.o, $(SRCS))

TARGET_EFI = bin/BOOTX64.EFI
TARGET_ISO = build/pseuDOS.iso

OVMF_BIOS ?= /usr/share/ovmf/OVMF.fd

.PHONY: all iso run run-serial clean

all: $(TARGET_ISO)

$(TARGET_ISO): $(TARGET_EFI)
	@mkdir -p build
	$(PYTHON) scripts/make_iso.py --efi $(TARGET_EFI) --iso $(TARGET_ISO)

$(TARGET_EFI): $(OBJS)
	@mkdir -p bin
	$(LD) $(LDFLAGS) -o $@ $^

bin/boot/%.o: src/boot/%.c
	@mkdir -p bin/boot
	$(CC) $(CFLAGS) -c $< -o $@

bin/kernel/%.o: src/kernel/%.c
	@mkdir -p bin/kernel
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET_ISO)
	qemu-system-x86_64 -bios $(OVMF_BIOS) -cdrom $(TARGET_ISO) -net none

run-serial: $(TARGET_ISO)
	qemu-system-x86_64 -display none -serial stdio -bios $(OVMF_BIOS) -cdrom $(TARGET_ISO) -net none

clean:
	rm -rf bin/boot bin/kernel bin/*.o bin/*.so bin/*.EFI bin/*.efi build/*.iso build/*.img
