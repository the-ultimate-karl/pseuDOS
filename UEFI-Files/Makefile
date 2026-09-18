CC ?= gcc
LD ?= ld
ifeq ($(OS),Windows_NT)
    PYTHON ?= python
    MKDIR_P ?= $(PYTHON) -c "import os, sys; os.makedirs(sys.argv[1], exist_ok=True)"
else
    PYTHON ?= python3
    MKDIR_P ?= mkdir -p
endif

CFLAGS = -ffreestanding \
         -fno-stack-protector \
         -fno-stack-check \
         -mno-stack-arg-probe \
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

LDFLAGS_USER = -nostdlib \
               -m i386pep \
               --subsystem 10 \
               --image-base 0x00400000 \
               --file-alignment 0x1000 \
               --section-alignment 0x1000

BOOT_SRCS = src/boot/bootloader.c \
            src/boot/errtext.c \
            src/lib/string.c \
            src/lib/guid.c

KERNEL_SRCS = src/kernel/kernel.c \
              src/kernel/gdt.c \
              src/kernel/pmm.c \
              src/kernel/vmm.c \
              src/kernel/klog.c \
              src/kernel/process.c \
              src/kernel/scheduler.c \
              src/kernel/syscall.c \
              src/kernel/ipc.c \
              src/kernel/shm.c \
              src/kernel/pe_loader.c \
              src/kernel/shell.c \
              src/kernel/panic.c \
              src/kernel/payloads.c \
              src/fs/vfs.c \
              src/fs/devpath.c \
              src/fs/gpt_fat32.c \
              src/fs/fat32_sync.c \
              src/lib/heap.c \
              src/lib/string.c \
              src/lib/guid.c \
              src/drivers/font.c \
              src/drivers/framebuffer.c \
              src/drivers/console.c \
              src/drivers/idt.c \
              src/drivers/keyboard.c \
              src/drivers/mice.c \
              src/drivers/cpu.c \
              src/drivers/memory.c \
              src/drivers/pci.c \
              src/drivers/storage.c \
              src/drivers/ahci.c \
              src/drivers/nvme.c \
              src/drivers/usb_storage.c \
              src/drivers/rtc.c \
              src/drivers/pit.c \
              src/drivers/acpi.c

USER_SRCS = src/userspace/autoinit.c \
            src/userspace/xshss.c

BOOT_OBJS = $(patsubst src/%.c, bin/%.o, $(BOOT_SRCS))
KERNEL_OBJS = $(patsubst src/%.c, bin/%.o, $(KERNEL_SRCS))
USER_OBJS = $(patsubst src/%.c, bin/%.o, $(USER_SRCS))

KERNEL_EXTRA_OBJS = bin/kernel/bootx64_payload.o \
                    bin/kernel/autoinit_payload.o \
                    bin/kernel/xshss_payload.o \
                    bin/drivers/isr_stubs.o \
                    bin/kernel/gdt_flush.o \
                    bin/kernel/syscall_entry.o

ALL_OBJS = $(sort $(BOOT_OBJS) $(KERNEL_OBJS) $(USER_OBJS))
ALL_DEPS = $(ALL_OBJS:.o=.d)

TARGET_EFI = bin/BOOTX64.EFI
TARGET_KERNEL = bin/kernel.bin
TARGET_AUTOINIT = bin/autoinit.bin
TARGET_XSHSS = bin/xshss.bin
TARGET_ISO = build/pseuDOS.iso

OVMF_BIOS ?= /usr/share/ovmf/OVMF.fd

.PHONY: all iso run run-serial clean

all: $(TARGET_ISO)

iso: $(TARGET_ISO)

$(TARGET_ISO): $(TARGET_EFI) $(TARGET_KERNEL) $(TARGET_AUTOINIT) $(TARGET_XSHSS)
	@$(MKDIR_P) build
	$(PYTHON) scripts/make_iso.py --efi $(TARGET_EFI) --kernel $(TARGET_KERNEL) --iso $(TARGET_ISO)

$(TARGET_EFI): $(BOOT_OBJS)
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_BOOT) -o $@ $^

$(TARGET_AUTOINIT): bin/userspace/autoinit.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e autoinit_main -o $@ $^

$(TARGET_XSHSS): bin/userspace/xshss.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e xshss_main -o $@ $^

bin/kernel/bootx64_payload.o: $(TARGET_EFI)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/bootx64_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_bootx64_data\n.global g_payload_bootx64_size\ng_payload_bootx64_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_bootx64_end:\ng_payload_bootx64_size:\n    .quad g_payload_bootx64_end - g_payload_bootx64_data\n')"
	$(CC) -c bin/kernel/bootx64_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/bootx64_payload.s') if os.path.exists('bin/kernel/bootx64_payload.s') else None"

bin/kernel/autoinit_payload.o: $(TARGET_AUTOINIT)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/autoinit_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_autoinit_data\n.global g_payload_autoinit_size\ng_payload_autoinit_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_autoinit_end:\ng_payload_autoinit_size:\n    .quad g_payload_autoinit_end - g_payload_autoinit_data\n')"
	$(CC) -c bin/kernel/autoinit_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/autoinit_payload.s') if os.path.exists('bin/kernel/autoinit_payload.s') else None"

bin/kernel/xshss_payload.o: $(TARGET_XSHSS)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/xshss_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_xshss_data\n.global g_payload_xshss_size\ng_payload_xshss_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_xshss_end:\ng_payload_xshss_size:\n    .quad g_payload_xshss_end - g_payload_xshss_data\n')"
	$(CC) -c bin/kernel/xshss_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/xshss_payload.s') if os.path.exists('bin/kernel/xshss_payload.s') else None"

$(TARGET_KERNEL): $(TARGET_AUTOINIT) $(TARGET_XSHSS) $(KERNEL_OBJS) $(KERNEL_EXTRA_OBJS)
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_KERNEL) -o $@ $(KERNEL_OBJS) $(KERNEL_EXTRA_OBJS)

bin/%.o: src/%.c
	@$(MKDIR_P) $(patsubst %/,%,$(dir $@))
	$(CC) $(CFLAGS) -c $< -o $@

bin/%.o: src/%.s
	@$(MKDIR_P) $(patsubst %/,%,$(dir $@))
	$(CC) -c $< -o $@

-include $(ALL_DEPS)

run: $(TARGET_ISO)
	qemu-system-x86_64 -bios $(OVMF_BIOS) -cdrom $(TARGET_ISO) -net none

run-serial: $(TARGET_ISO)
	qemu-system-x86_64 -display none -serial stdio -bios $(OVMF_BIOS) -cdrom $(TARGET_ISO) -net none

clean:
ifeq ($(OS),Windows_NT)
	$(PYTHON) -c "import shutil, glob, os; [shutil.rmtree(d, ignore_errors=True) for d in ['bin/boot', 'bin/lib', 'bin/fs', 'bin/drivers', 'bin/kernel', 'bin/userspace']]; [os.remove(f) for pattern in ['bin/*.o', 'bin/*.d', 'bin/*.so', 'bin/*.EFI', 'bin/*.efi', 'bin/*.bin', 'bin/kernel/*.s', 'build/*.iso', 'build/*.img'] for f in glob.glob(pattern)]"
else
	rm -rf bin/boot bin/lib bin/fs bin/drivers bin/kernel bin/userspace bin/*.o bin/*.d bin/*.so bin/*.EFI bin/*.efi bin/*.bin bin/kernel/*.s build/*.iso build/*.img
endif
