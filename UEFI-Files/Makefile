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
         -mgeneral-regs-only \
         -maccumulate-outgoing-args \
         -Wall \
         -Wextra \
         -MMD \
         -MP \
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
              src/kernel/payloads.c \
              src/fs/vfs.c \
              src/fs/devpath.c \
              src/fs/gpt_fat32.c \
              src/lib/heap.c \
              src/lib/string.c \
              src/lib/guid.c \
              src/drivers/font.c \
              src/drivers/framebuffer.c \
              src/drivers/console.c \
              src/drivers/idt.c \
              src/drivers/keyboard.c \
              src/drivers/cpu.c \
              src/drivers/memory.c \
              src/drivers/pci.c \
              src/drivers/storage.c \
              src/drivers/ahci.c \
              src/drivers/nvme.c \
              src/drivers/usb_storage.c \
              src/drivers/acpi.c

BOOT_OBJS = $(patsubst src/%.c, bin/%.o, $(BOOT_SRCS))
KERNEL_OBJS = $(patsubst src/%.c, bin/%.o, $(KERNEL_SRCS))
KERNEL_EXTRA_OBJS = bin/kernel/bootx64_payload.o

ALL_OBJS = $(sort $(BOOT_OBJS) $(KERNEL_OBJS))
ALL_DEPS = $(ALL_OBJS:.o=.d)

TARGET_EFI = bin/BOOTX64.EFI
TARGET_KERNEL = bin/kernel.bin
TARGET_ISO = build/pseuDOS.iso

OVMF_BIOS ?= /usr/share/ovmf/OVMF.fd

.PHONY: all iso run run-serial clean

all: $(TARGET_ISO)

iso: $(TARGET_ISO)

$(TARGET_ISO): $(TARGET_EFI) $(TARGET_KERNEL)
	@mkdir -p build
	$(PYTHON) scripts/make_iso.py --efi $(TARGET_EFI) --kernel $(TARGET_KERNEL) --iso $(TARGET_ISO)

$(TARGET_EFI): $(BOOT_OBJS)
	@mkdir -p bin
	$(LD) $(LDFLAGS_BOOT) -o $@ $^

bin/kernel/bootx64_payload.o: $(TARGET_EFI)
	@mkdir -p bin/kernel
	printf '.section .rodata\n.global g_payload_bootx64_data\n.global g_payload_bootx64_size\ng_payload_bootx64_data:\n    .incbin "$<"\ng_payload_bootx64_end:\ng_payload_bootx64_size:\n    .quad g_payload_bootx64_end - g_payload_bootx64_data\n' | $(CC) -c -x assembler - -o $@

$(TARGET_KERNEL): $(KERNEL_OBJS) $(KERNEL_EXTRA_OBJS)
	@mkdir -p bin
	$(LD) $(LDFLAGS_KERNEL) -o $@ $^

bin/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(ALL_DEPS)

run: $(TARGET_ISO)
	qemu-system-x86_64 -bios $(OVMF_BIOS) -cdrom $(TARGET_ISO) -net none

run-serial: $(TARGET_ISO)
	qemu-system-x86_64 -display none -serial stdio -bios $(OVMF_BIOS) -cdrom $(TARGET_ISO) -net none

clean:
	rm -rf bin/boot bin/lib bin/fs bin/drivers bin/kernel bin/*.o bin/*.d bin/*.so bin/*.EFI bin/*.efi bin/*.bin build/*.iso build/*.img
