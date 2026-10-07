CC ?= gcc
LD ?= ld
ifeq ($(OS),Windows_NT)
    PYTHON ?= python
    MKDIR_P ?= $(PYTHON) -c "import os, sys; os.makedirs(sys.argv[1], exist_ok=True)"
else
    PYTHON ?= python3
    MKDIR_P ?= mkdir -p
endif

CFLAGS = -O2 \
         -ffreestanding \
         -fno-builtin \
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
            src/userspace/xshss.c \
            src/userspace/superglue.c \
            src/userspace/ntfs.c \
            src/userspace/lack.c \
            src/userspace/ninds.c \
            src/userspace/splash.c \
            src/userspace/gshss.c \
            src/userspace/apps/shell.c \
            src/userspace/apps/sysmon.c \
            src/userspace/apps/calc.c \
            src/userspace/apps/notepad.c \
            src/userspace/apps/paint.c \
            src/userspace/apps/clock.c \
            src/userspace/apps/filemgr.c

BOOT_OBJS = $(patsubst src/%.c, bin/%.o, $(BOOT_SRCS))
KERNEL_OBJS = $(patsubst src/%.c, bin/%.o, $(KERNEL_SRCS))
USER_OBJS = $(patsubst src/%.c, bin/%.o, $(USER_SRCS))

KERNEL_EXTRA_OBJS = bin/kernel/bootx64_payload.o \
                    bin/kernel/autoinit_payload.o \
                    bin/kernel/xshss_payload.o \
                    bin/kernel/superglue_payload.o \
                    bin/kernel/ntfs_payload.o \
                    bin/kernel/lack_payload.o \
                    bin/kernel/ninds_payload.o \
                    bin/kernel/splash_payload.o \
                    bin/kernel/gshss_payload.o \
                    bin/kernel/shell_payload.o \
                    bin/kernel/sysmon_payload.o \
                    bin/kernel/calc_payload.o \
                    bin/kernel/notepad_payload.o \
                    bin/kernel/paint_payload.o \
                    bin/kernel/clock_payload.o \
                    bin/kernel/filemgr_payload.o \
                    bin/kernel/image2_payload.o \
                    bin/kernel/wallpapers_payload.o \
                    bin/drivers/isr_stubs.o \
                    bin/kernel/gdt_flush.o \
                    bin/kernel/syscall_entry.o

ALL_OBJS = $(sort $(BOOT_OBJS) $(KERNEL_OBJS) $(USER_OBJS))
ALL_DEPS = $(ALL_OBJS:.o=.d)

TARGET_EFI = bin/BOOTX64.EFI
TARGET_KERNEL = bin/vpkernel

TARGET_AUTOINIT = bin/autoinit.exe
TARGET_XSHSS = bin/xshss.exe
TARGET_SUPERGLUE = bin/superglue.exe
TARGET_NTFS = bin/ntfs.exe
TARGET_LACK = bin/lack.exe
TARGET_NINDS = bin/ninds.exe
TARGET_SPLASH = bin/splash.exe
TARGET_GSHSS = bin/gshss.exe
TARGET_SHELL = bin/shell.exe
TARGET_SYSMON = bin/sysmon.exe
TARGET_CALC = bin/calc.exe
TARGET_NOTEPAD = bin/notepad.exe
TARGET_PAINT = bin/paint.exe
TARGET_CLOCK = bin/clock.exe
TARGET_FILEMGR = bin/filemgr.exe

TARGET_USER_EXES = $(TARGET_AUTOINIT) \
                   $(TARGET_XSHSS) \
                   $(TARGET_SUPERGLUE) \
                   $(TARGET_NTFS) \
                   $(TARGET_LACK) \
                   $(TARGET_NINDS) \
                   $(TARGET_SPLASH) \
                   $(TARGET_GSHSS) \
                   $(TARGET_SHELL) \
                   $(TARGET_SYSMON) \
                   $(TARGET_CALC) \
                   $(TARGET_NOTEPAD) \
                   $(TARGET_PAINT) \
                   $(TARGET_CLOCK) \
                   $(TARGET_FILEMGR)

TARGET_ISO = build/pseuDOS.iso

OVMF_BIOS ?= /usr/share/ovmf/OVMF.fd

.PHONY: all iso run run-serial clean

all: $(TARGET_ISO)

iso: $(TARGET_ISO)

$(TARGET_ISO): $(TARGET_EFI) $(TARGET_KERNEL) $(TARGET_USER_EXES)
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

$(TARGET_SUPERGLUE): bin/userspace/superglue.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e superglue_main -o $@ $^

$(TARGET_NTFS): bin/userspace/ntfs.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e ntfs_main -o $@ $^

$(TARGET_LACK): bin/userspace/lack.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e lack_main -o $@ $^

$(TARGET_NINDS): bin/userspace/ninds.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e ninds_main -o $@ $^

$(TARGET_SPLASH): bin/userspace/splash.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e splash_main -o $@ $^

$(TARGET_GSHSS): bin/userspace/gshss.o bin/userspace/gshss_fallback.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e gshss_main -o $@ $^

$(TARGET_SHELL): bin/userspace/apps/shell.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e shell_main -o $@ $^

$(TARGET_SYSMON): bin/userspace/apps/sysmon.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e sysmon_main -o $@ $^

$(TARGET_CALC): bin/userspace/apps/calc.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e calc_main -o $@ $^

$(TARGET_NOTEPAD): bin/userspace/apps/notepad.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e notepad_main -o $@ $^

$(TARGET_PAINT): bin/userspace/apps/paint.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e paint_main -o $@ $^

$(TARGET_CLOCK): bin/userspace/apps/clock.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e clock_main -o $@ $^

$(TARGET_FILEMGR): bin/userspace/apps/filemgr.o
	@$(MKDIR_P) bin
	$(LD) $(LDFLAGS_USER) -e filemgr_main -o $@ $^

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

bin/kernel/superglue_payload.o: $(TARGET_SUPERGLUE)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/superglue_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_superglue_data\n.global g_payload_superglue_size\ng_payload_superglue_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_superglue_end:\ng_payload_superglue_size:\n    .quad g_payload_superglue_end - g_payload_superglue_data\n')"
	$(CC) -c bin/kernel/superglue_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/superglue_payload.s') if os.path.exists('bin/kernel/superglue_payload.s') else None"

bin/kernel/ntfs_payload.o: $(TARGET_NTFS)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/ntfs_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_ntfs_data\n.global g_payload_ntfs_size\ng_payload_ntfs_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_ntfs_end:\ng_payload_ntfs_size:\n    .quad g_payload_ntfs_end - g_payload_ntfs_data\n')"
	$(CC) -c bin/kernel/ntfs_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/ntfs_payload.s') if os.path.exists('bin/kernel/ntfs_payload.s') else None"

bin/kernel/lack_payload.o: $(TARGET_LACK)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/lack_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_lack_data\n.global g_payload_lack_size\ng_payload_lack_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_lack_end:\ng_payload_lack_size:\n    .quad g_payload_lack_end - g_payload_lack_data\n')"
	$(CC) -c bin/kernel/lack_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/lack_payload.s') if os.path.exists('bin/kernel/lack_payload.s') else None"

bin/kernel/ninds_payload.o: $(TARGET_NINDS)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/ninds_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_ninds_data\n.global g_payload_ninds_size\ng_payload_ninds_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_ninds_end:\ng_payload_ninds_size:\n    .quad g_payload_ninds_end - g_payload_ninds_data\n')"
	$(CC) -c bin/kernel/ninds_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/ninds_payload.s') if os.path.exists('bin/kernel/ninds_payload.s') else None"

bin/kernel/splash_payload.o: $(TARGET_SPLASH)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/splash_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_splash_data\n.global g_payload_splash_size\ng_payload_splash_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_splash_end:\ng_payload_splash_size:\n    .quad g_payload_splash_end - g_payload_splash_data\n')"
	$(CC) -c bin/kernel/splash_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/splash_payload.s') if os.path.exists('bin/kernel/splash_payload.s') else None"

bin/kernel/gshss_payload.o: $(TARGET_GSHSS)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/gshss_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_gshss_data\n.global g_payload_gshss_size\ng_payload_gshss_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_gshss_end:\ng_payload_gshss_size:\n    .quad g_payload_gshss_end - g_payload_gshss_data\n')"
	$(CC) -c bin/kernel/gshss_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/gshss_payload.s') if os.path.exists('bin/kernel/gshss_payload.s') else None"

bin/kernel/shell_payload.o: $(TARGET_SHELL)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/shell_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_shell_data\n.global g_payload_shell_size\ng_payload_shell_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_shell_end:\ng_payload_shell_size:\n    .quad g_payload_shell_end - g_payload_shell_data\n')"
	$(CC) -c bin/kernel/shell_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/shell_payload.s') if os.path.exists('bin/kernel/shell_payload.s') else None"

bin/kernel/sysmon_payload.o: $(TARGET_SYSMON)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/sysmon_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_sysmon_data\n.global g_payload_sysmon_size\ng_payload_sysmon_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_sysmon_end:\ng_payload_sysmon_size:\n    .quad g_payload_sysmon_end - g_payload_sysmon_data\n')"
	$(CC) -c bin/kernel/sysmon_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/sysmon_payload.s') if os.path.exists('bin/kernel/sysmon_payload.s') else None"

bin/kernel/calc_payload.o: $(TARGET_CALC)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/calc_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_calc_data\n.global g_payload_calc_size\ng_payload_calc_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_calc_end:\ng_payload_calc_size:\n    .quad g_payload_calc_end - g_payload_calc_data\n')"
	$(CC) -c bin/kernel/calc_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/calc_payload.s') if os.path.exists('bin/kernel/calc_payload.s') else None"

bin/kernel/notepad_payload.o: $(TARGET_NOTEPAD)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/notepad_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_notepad_data\n.global g_payload_notepad_size\ng_payload_notepad_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_notepad_end:\ng_payload_notepad_size:\n    .quad g_payload_notepad_end - g_payload_notepad_data\n')"
	$(CC) -c bin/kernel/notepad_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/notepad_payload.s') if os.path.exists('bin/kernel/notepad_payload.s') else None"

bin/kernel/paint_payload.o: $(TARGET_PAINT)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/paint_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_paint_data\n.global g_payload_paint_size\ng_payload_paint_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_paint_end:\ng_payload_paint_size:\n    .quad g_payload_paint_end - g_payload_paint_data\n')"
	$(CC) -c bin/kernel/paint_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/paint_payload.s') if os.path.exists('bin/kernel/paint_payload.s') else None"

bin/kernel/clock_payload.o: $(TARGET_CLOCK)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/clock_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_clock_data\n.global g_payload_clock_size\ng_payload_clock_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_clock_end:\ng_payload_clock_size:\n    .quad g_payload_clock_end - g_payload_clock_data\n')"
	$(CC) -c bin/kernel/clock_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/clock_payload.s') if os.path.exists('bin/kernel/clock_payload.s') else None"

bin/kernel/filemgr_payload.o: $(TARGET_FILEMGR)
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/filemgr_payload.s', 'w') as f: f.write('.section .rodata\n.global g_payload_filemgr_data\n.global g_payload_filemgr_size\ng_payload_filemgr_data:\n    .incbin \"$(subst \,/,$<)\"\ng_payload_filemgr_end:\ng_payload_filemgr_size:\n    .quad g_payload_filemgr_end - g_payload_filemgr_data\n')"
	$(CC) -c bin/kernel/filemgr_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/filemgr_payload.s') if os.path.exists('bin/kernel/filemgr_payload.s') else None"

bin/kernel/image2_payload.o: ../Test-Media/image2_testmedia.png
	@$(MKDIR_P) bin/kernel
	$(PYTHON) -c "with open('bin/kernel/image2_payload.s', 'w') as f: f.write('.section .rodata\n.global g_media_image2_png\n.global g_media_image2_png_size\ng_media_image2_png:\n    .incbin \"$(subst \,/,$<)\"\ng_media_image2_png_end:\ng_media_image2_png_size:\n    .quad g_media_image2_png_end - g_media_image2_png\n')"
	$(CC) -c bin/kernel/image2_payload.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/kernel/image2_payload.s') if os.path.exists('bin/kernel/image2_payload.s') else None"

bin/userspace/gshss_fallback.o: ../Test-Media/Wallpapers/fallback.png
	@$(MKDIR_P) bin/userspace
	$(PYTHON) -c "with open('bin/userspace/gshss_fallback.s', 'w') as f: f.write('.section .rodata\n.global g_fallback_wallpaper_png\n.global g_fallback_wallpaper_png_size\ng_fallback_wallpaper_png:\n    .incbin \"$(subst \,/,$<)\"\ng_fallback_wallpaper_png_end:\ng_fallback_wallpaper_png_size:\n    .quad g_fallback_wallpaper_png_end - g_fallback_wallpaper_png\n')"
	$(CC) -c bin/userspace/gshss_fallback.s -o $@
	@$(PYTHON) -c "import os; os.remove('bin/userspace/gshss_fallback.s') if os.path.exists('bin/userspace/gshss_fallback.s') else None"

bin/kernel/wallpapers_payload.o: scripts/generate_wallpapers_payload.py
	@$(MKDIR_P) bin/kernel
	$(PYTHON) scripts/generate_wallpapers_payload.py
	$(CC) -c bin/kernel/wallpapers_payload.s -o $@

$(TARGET_KERNEL): $(TARGET_USER_EXES) $(KERNEL_OBJS) $(KERNEL_EXTRA_OBJS)
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
	$(PYTHON) -c "import shutil, glob, os; [shutil.rmtree(d, ignore_errors=True) for d in ['bin/boot', 'bin/lib', 'bin/fs', 'bin/drivers', 'bin/kernel', 'bin/userspace']]; [os.remove(f) for pattern in ['bin/*.o', 'bin/*.d', 'bin/*.so', 'bin/*.EFI', 'bin/*.efi', 'bin/*.bin', 'bin/*.exe', 'bin/vpkernel', 'bin/kernel/*.s', 'build/*.iso', 'build/*.img'] for f in glob.glob(pattern)]"
else
	rm -rf bin/boot bin/lib bin/fs bin/drivers bin/kernel bin/userspace bin/*.o bin/*.d bin/*.so bin/*.EFI bin/*.efi bin/*.bin bin/*.exe bin/vpkernel bin/kernel/*.s build/*.iso build/*.img
endif
