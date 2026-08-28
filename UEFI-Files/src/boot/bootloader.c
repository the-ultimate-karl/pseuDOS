#include "efi.h"
#include "errtext.h"
#include "io.h"

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_ENTRY_POINT)(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

/*
 * Check basic x86_64 CPU features using CPUID instruction
 */
static int check_cpu_features(void) {
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000000));
    if (eax < 0x80000001) {
        return 0;
    }

    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000001));
    if (!(edx & (1 << 29))) {
        return 0;
    }

    return 1;
}

static void uart_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static void uart_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0);
    outb(0x3F8, (uint8_t)c);
}

static void uart_puts(const char *str) {
    if (!str) return;
    while (*str) {
        if (*str == '\n') {
            uart_putc('\r');
        }
        uart_putc(*str++);
    }
}

static void boot_msg(EFI_SYSTEM_TABLE *SystemTable, const char *str) {
    if (SystemTable && SystemTable->ConOut) {
        CHAR16 buf[256];
        size_t i = 0;
        while (*str && i < 254) {
            if (*str == '\n') {
                buf[i++] = (CHAR16)'\r';
            }
            buf[i++] = (CHAR16)(*str++);
        }
        buf[i] = 0;
        SystemTable->ConOut->OutputString(SystemTable->ConOut, buf);
    } else {
        uart_puts(str);
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    uart_init();

    if (!SystemTable || !SystemTable->BootServices) {
        error_boot(NULL, ERR_GENERIC_BOOT_FAILURE, "SystemTable or BootServices pointer is null");
    }

    if (SystemTable->ConOut && SystemTable->ConOut->ClearScreen) {
        SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    }

    boot_msg(SystemTable, "\n[bootmgfw] pseuDOS 64-bit UEFI Boot Manager v1.0\n");
    boot_msg(SystemTable, "[bootmgfw] initializing UEFI Firmware environment...\n");

    /* 1. Validate System Table */
    boot_msg(SystemTable, "[bootmgfw] validating UEFI system table and boot services... [ok]\n");

    /* 2. Disable Watchdog Timer */
    boot_msg(SystemTable, "[bootmgfw] disabling UEFI watchdog timer... ");
    if (SystemTable->BootServices->SetWatchdogTimer) {
        SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
        boot_msg(SystemTable, "[ok]\n");
    } else {
        boot_msg(SystemTable, "[skipped]\n");
    }

    /* 3. Query CPUID for Long Mode */
    boot_msg(SystemTable, "[bootmgfw] querying cpuid for x86_64 long mode support... ");
    if (!check_cpu_features()) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_UNSUPPORTED_CPU, "x86_64 long mode (64-bit) feature flag not present in cpuid");
    }
    boot_msg(SystemTable, "[ok]\n");

    /* 4. Verify Console Protocol */
    boot_msg(SystemTable, "[bootmgfw] verifying console display protocol... [ok]\n");

    /* 5. Detect Boot Storage Media Handle */
    boot_msg(SystemTable, "[bootmgfw] detecting boot storage media handle... ");
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    EFI_STATUS status = SystemTable->BootServices->HandleProtocol(
        ImageHandle,
        &gEfiLoadedImageProtocolGuid,
        (VOID **)&loaded_image
    );

    if (EFI_ERROR(status) || !loaded_image || !loaded_image->DeviceHandle) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_STORAGE_DEVICE_IO, "failed to query EFI_LOADED_IMAGE_PROTOCOL for boot device");
    }
    boot_msg(SystemTable, "[ok]\n");

    /* 6. Mount Boot Partition Filesystem Volume */
    boot_msg(SystemTable, "[bootmgfw] mounting boot partition filesystem volume... ");
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    status = SystemTable->BootServices->HandleProtocol(
        loaded_image->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&sfs
    );

    if (EFI_ERROR(status) || !sfs) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_KERNEL_MOUNT_FAILED, "failed to open EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on boot volume");
    }

    EFI_FILE_PROTOCOL *root_dir = NULL;
    status = sfs->OpenVolume(sfs, &root_dir);
    if (EFI_ERROR(status) || !root_dir) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_KERNEL_MOUNT_FAILED, "failed to open root directory volume");
    }
    boot_msg(SystemTable, "[ok]\n");

    /* 7. Look for Kernel Image */
    boot_msg(SystemTable, "[bootmgfw] looking for kernel...\n");

    const CHAR16 kernel_path[] = { '\\', 'E', 'F', 'I', '\\', 'p', 's', 'e', 'u', 'D', 'O', 'S', '\\', 'k', 'e', 'r', 'n', 'e', 'l', '.', 'b', 'i', 'n', 0 };
    EFI_FILE_PROTOCOL *kernel_file = NULL;

    status = root_dir->Open(
        root_dir,
        &kernel_file,
        kernel_path,
        EFI_FILE_MODE_READ,
        0
    );

    if (EFI_ERROR(status) || !kernel_file) {
        error_boot(SystemTable, ERR_KERNEL_NOT_FOUND, "\\EFI\\pseuDOS\\kernel.bin: no such file or directory");
    }

    boot_msg(SystemTable, "[bootmgfw] found kernel! loading kernel... [ok]\n");

    /* 8. Read Kernel Binary */
    boot_msg(SystemTable, "[bootmgfw] allocating physical memory pages for kernel... ");

    EFI_PHYSICAL_ADDRESS kernel_buffer = 0;
    UINTN pages = 128; /* 512KB */
    status = SystemTable->BootServices->AllocatePages(
        AllocateAnyPages,
        EfiLoaderCode,
        pages,
        &kernel_buffer
    );

    if (EFI_ERROR(status) || kernel_buffer == 0) {
        boot_msg(SystemTable, "[failed]\n");
        error_boot(SystemTable, ERR_MEMORY_MAP_EXHAUSTED, "failed to allocate memory pages for kernel execution");
    }

    UINTN read_size = pages * 4096;
    kernel_file->Read(kernel_file, &read_size, (VOID *)(uintptr_t)kernel_buffer);
    kernel_file->Close(kernel_file);
    root_dir->Close(root_dir);

    boot_msg(SystemTable, "[ok]\n");

    /* 9. Execute Kernel Entry Point */
    boot_msg(SystemTable, "[bootmgfw] transferring execution to pseuDOS kernel...\n\n");
    boot_msg(SystemTable, "====================================\n\n");

    uint8_t *raw = (uint8_t *)(uintptr_t)kernel_buffer;
    uint32_t pe_offset = *(uint32_t *)(raw + 0x3C);
    if (raw[pe_offset] == 'P' && raw[pe_offset + 1] == 'E') {
        uint32_t entry_rva = *(uint32_t *)(raw + pe_offset + 0x28);
        EFI_IMAGE_ENTRY_POINT entry = (EFI_IMAGE_ENTRY_POINT)(raw + entry_rva);
        entry(ImageHandle, SystemTable);
    } else {
        error_boot(SystemTable, ERR_KERNEL_INTEGRITY, "invalid PE32+ header signature in kernel.bin");
    }

    error_boot(SystemTable, ERR_GENERIC_BOOT_FAILURE, "pseuDOS kernel unexpectedly exited to bootloader");

    return EFI_SUCCESS;
}
